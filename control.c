#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define CTL_CLOSE closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#define CTL_CLOSE close
#endif
#include "control.h"
#include "blastem.h"
#include "system.h"
#include "util.h"
#include "version.inc"
#include "io.h"
#include "genesis.h"
#include "vdp.h"
#include "ym2612.h"
#include "psg.h"
#include "png.h"
#include "backend.h"
#include "serialize.h"

int control_enabled = 0;
static int control_sock = -1;
static char rx[4096];
static int rx_len = 0;

void control_send_event(const char *json_body)
{
	if (control_sock < 0) return;
	char line[1024];
	int n = snprintf(line, sizeof(line), "{%s}\n", json_body);
	if (n < 0) n = 0;
	else if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
	send(control_sock, line, n, 0);
}

static void control_send_ok(const char *result_body)
{
	char line[1200];
	int n = snprintf(line, sizeof(line), "{\"ok\":true,\"result\":{%s}}\n",
		result_body ? result_body : "");
	if (n < 0) n = 0;
	else if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
	send(control_sock, line, n, 0);
}

static void control_send_err(const char *msg)
{
	char line[512];
	int n = snprintf(line, sizeof(line), "{\"ok\":false,\"error\":\"%s\"}\n", msg);
	if (n < 0) n = 0;
	else if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
	send(control_sock, line, n, 0);
}

// Returns 1 if rx currently holds a complete newline-terminated line.
static int rx_has_line(void)
{
	for (int i = 0; i < rx_len; i++) {
		if (rx[i] == '\n') return 1;
	}
	return 0;
}

// Pops the first buffered line out of rx into out (without the newline).
// Only valid to call when rx_has_line() is true.
static void rx_take_line(char *out, int cap)
{
	for (int i = 0; i < rx_len; i++) {
		if (rx[i] == '\n') {
			int len = i < cap - 1 ? i : cap - 1;
			memcpy(out, rx, len);
			out[len] = 0;
			// strip trailing CR
			if (len && out[len-1] == '\r') out[len-1] = 0;
			memmove(rx, rx + i + 1, rx_len - i - 1);
			rx_len -= i + 1;
			return;
		}
	}
}

// Read one newline-terminated request line into out (without the newline).
// Returns 1 on success, 0 on disconnect.
static int control_read_line(char *out, int cap)
{
	for (;;) {
		if (rx_has_line()) {
			rx_take_line(out, cap);
			return 1;
		}
		int space = (int)sizeof(rx) - rx_len;
		if (space <= 0) { rx_len = 0; space = sizeof(rx); }
		int got = recv(control_sock, rx + rx_len, space, 0);
		if (got <= 0) return 0;
		rx_len += got;
	}
}

int control_active(void) { return control_sock >= 0; }

void control_init(int port)
{
#ifdef _WIN32
	WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif
	int lsock = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(lsock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		fatal_error("control: bind on port %d failed\n", port);
	}
	if (listen(lsock, 1) < 0) {
		fatal_error("control: listen failed\n");
	}
	control_sock = accept(lsock, NULL, NULL);
	CTL_CLOSE(lsock);
	if (control_sock < 0) {
		fatal_error("control: accept failed\n");
	}
	control_enabled = 1;
	char hello[128];
	int n = snprintf(hello, sizeof(hello),
		"{\"event\":\"hello\",\"version\":\"%s\",\"protocol\":1}\n", BLASTEM_VERSION);
	if (n < 0) n = 0;
	else if (n >= (int)sizeof(hello)) n = (int)sizeof(hello) - 1;
	send(control_sock, hello, n, 0);
}

static unsigned long control_frame = 0;   // frames since boot
static int step_remaining = 0;            // >0 while a step is in progress
static int running = 0;                   // free-run mode

static uint16_t pad_desired = 0;   // bitmask of buttons requested
static uint16_t pad_applied = 0;   // bitmask currently pressed on io

typedef struct { const char *name; uint8_t button; uint16_t bit; } btn_map;
static const btn_map BTN_MAP[] = {
	{"UP", DPAD_UP, 1<<0}, {"DOWN", DPAD_DOWN, 1<<1},
	{"LEFT", DPAD_LEFT, 1<<2}, {"RIGHT", DPAD_RIGHT, 1<<3},
	{"A", BUTTON_A, 1<<4}, {"B", BUTTON_B, 1<<5}, {"C", BUTTON_C, 1<<6},
	{"START", BUTTON_START, 1<<7}, {"X", BUTTON_X, 1<<8}, {"Y", BUTTON_Y, 1<<9},
	{"Z", BUTTON_Z, 1<<10}, {"MODE", BUTTON_MODE, 1<<11},
};
#define BTN_COUNT (int)(sizeof(BTN_MAP)/sizeof(BTN_MAP[0]))

static const btn_map *find_btn(const char *name, int len)
{
	for (int i = 0; i < BTN_COUNT; i++) {
		if ((int)strlen(BTN_MAP[i].name) == len && !strncmp(BTN_MAP[i].name, name, len))
			return &BTN_MAP[i];
	}
	return NULL;
}

// Parse "A,DOWN,START" into a bitmask. Returns -1 on unknown button.
static int parse_pad(const char *args)
{
	uint16_t mask = 0;
	const char *p = args;
	while (*p) {
		const char *start = p;
		while (*p && *p != ',') p++;
		const btn_map *b = find_btn(start, (int)(p - start));
		if (!b) return -1;
		mask |= b->bit;
		if (*p == ',') p++;
	}
	return mask;
}

// gamepad_num for controller port 1. io_gamepad_down/up resolve the port via
// find_gamepad(io, gamepad_num) matching port->device.pad.gamepad_num — NOT the
// port array index. The default io config maps port 1 to "gamepad*.1", so its
// gamepad_num is 1; passing 0 finds no port and the input is silently dropped.
#define CONTROL_PAD1 1

// Apply pad_desired vs pad_applied via io_gamepad_down/up on controller port 1.
static void control_apply_pad(void *sys)
{
	genesis_context *gen = (genesis_context *)sys;
	uint16_t changed = pad_desired ^ pad_applied;
	for (int i = 0; i < BTN_COUNT; i++) {
		if (!(changed & BTN_MAP[i].bit)) continue;
		if (pad_desired & BTN_MAP[i].bit) io_gamepad_down(&gen->io, CONTROL_PAD1, BTN_MAP[i].button);
		else io_gamepad_up(&gen->io, CONTROL_PAD1, BTN_MAP[i].button);
	}
	pad_applied = pad_desired;
}

// Sound chip state in the fields of sound_state (see control_dispatch).
typedef struct {
	uint8_t  ym[2][256];
	uint8_t  keyon[6];
	uint8_t  dac_enabled, dac_value;
	uint8_t  psg_volume[4];
	uint16_t psg_tone[3];
	uint8_t  psg_noise, psg_latch;
} sound_snapshot;

static sound_snapshot vint_snapshot;
static unsigned long vint_count = 0;

static int read_sound(void *sys, sound_snapshot *s)
{
	genesis_context *gen = (genesis_context *)sys;
	ym2612_context *ym = gen->ym;
	psg_context *psg = gen->psg;
	if (!ym || !psg) return 0;
	for (int part = 0; part < 2; part++) {
		const uint8_t *regs = part ? ym->part2_regs : ym->part1_regs;
		int start = part ? YM_PART2_START : YM_PART1_START;
		for (int r = 0; r < 256; r++) {
			s->ym[part][r] = (r >= start && r < YM_REG_END) ? regs[r - start] : 0;
		}
	}
	for (int c = 0; c < 6; c++) s->keyon[c] = ym->channels[c].keyon >> 4;
	s->dac_enabled = ym->dac_enable ? 1 : 0;
	s->dac_value = ym->part1_regs[0x2A - YM_PART1_START];
	for (int i = 0; i < 4; i++) s->psg_volume[i] = psg->volume[i];
	for (int i = 0; i < 3; i++) s->psg_tone[i] = psg->counter_load[i];
	// noise register reconstructed from the chip state, not the raw byte written
	// (bit 2 = white, bits 0-1 = rate, rate 3 = follows tone 3)
	int noise_rate = psg->noise_use_tone ? 3 : 0;
	if (!psg->noise_use_tone) {
		while (noise_rate < 2 && psg->counter_load[3] > (0x10u << noise_rate)) noise_rate++;
	}
	s->psg_noise = (psg->noise_type ? 4 : 0) | noise_rate;
	s->psg_latch = psg->latch;
	return 1;
}

static int format_sound(char *b, int cap, const sound_snapshot *s)
{
	int n = snprintf(b, cap, "\"ym\":[[");
	for (int part = 0; part < 2; part++) {
		for (int r = 0; r < 256; r++) {
			n += snprintf(b + n, cap - n, "%s%d", r ? "," : "", s->ym[part][r]);
		}
		n += snprintf(b + n, cap - n, part ? "]]" : "],[");
	}
	n += snprintf(b + n, cap - n, ",\"keyon\":[");
	for (int c = 0; c < 6; c++) {
		n += snprintf(b + n, cap - n, "%s%d", c ? "," : "", s->keyon[c]);
	}
	n += snprintf(b + n, cap - n, "],\"dac_enabled\":%d,\"dac_value\":%d,\"psg\":{\"volume\":[%d,%d,%d,%d],\"tone\":[%d,%d,%d],\"noise\":%d,\"latch\":%d}",
		s->dac_enabled, s->dac_value,
		s->psg_volume[0], s->psg_volume[1], s->psg_volume[2], s->psg_volume[3],
		s->psg_tone[0], s->psg_tone[1], s->psg_tone[2], s->psg_noise, s->psg_latch);
	return n;
}

// Frame latched at the VInt ack: a copy of the VDP framebuffer, same layout as
// the "screenshot" PNG (indexed, 347x240 in H40 / 283x240 in H32, with borders).
// The headless VDP draws every line of the frame into one buffer before VInt,
// so at the ack the buffer holds the whole frame that was just displayed.
static pixel_t *vint_fb = NULL;
static size_t vint_fb_cap = 0;
static uint32_t vint_fb_w, vint_fb_h, vint_fb_pitch;

static void latch_frame(void)
{
	if (!current_system || !current_system->get_vdp) return;
	vdp_context *v = current_system->get_vdp(current_system);
	if (!v || !v->fb) return;
	uint32_t w = ((v->regs[REG_MODE_4] & BIT_H40) ? 320 : 256) + HORIZ_BORDER;
	uint32_t h = v->inactive_start + v->border_bot + v->border_top;
	size_t bytes = (size_t)v->output_pitch * h;
	if (bytes > vint_fb_cap) {
		pixel_t *n = realloc(vint_fb, bytes);
		if (!n) return;
		vint_fb = n;
		vint_fb_cap = bytes;
	}
	memcpy(vint_fb, v->fb, bytes);
	vint_fb_w = w; vint_fb_h = h; vint_fb_pitch = v->output_pitch;
}

static int save_latched_frame(const char *path)
{
	if (!vint_fb) return 0;
	FILE *f = fopen(path, "wb");
	if (!f) return 0;
	save_png(f, vint_fb, vint_fb_w, vint_fb_h, vint_fb_pitch);
	fclose(f);
	return 1;
}

void control_vint_ack(void *sys)
{
	if (control_sock < 0) return;
	vint_count++;
	read_sound(sys, &vint_snapshot);
	latch_frame();
}

unsigned long control_vint_count(void)
{
	return vint_count;
}

static void send_frame_result(void)
{
	char body[64];
	snprintf(body, sizeof(body), "\"frame\":%lu", control_frame);
	control_send_ok(body);
}

static char *b64_encode_file(const char *path, int *out_len)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	if (sz < 0) { fclose(f); return NULL; }
	unsigned char *buf = malloc(sz);
	if (!buf) { fclose(f); return NULL; }
	fread(buf, 1, sz, f); fclose(f);
	static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char *out = malloc(((sz + 2) / 3) * 4 + 1);
	if (!out) { free(buf); return NULL; }
	int o = 0;
	for (long i = 0; i < sz; i += 3) {
		int n = (buf[i] << 16) | (i+1 < sz ? buf[i+1] << 8 : 0) | (i+2 < sz ? buf[i+2] : 0);
		out[o++] = T[(n >> 18) & 63];
		out[o++] = T[(n >> 12) & 63];
		out[o++] = (i+1 < sz) ? T[(n >> 6) & 63] : '=';
		out[o++] = (i+2 < sz) ? T[n & 63] : '=';
	}
	out[o] = 0; *out_len = o; free(buf);
	return out;
}

// Longest request line the server reads (control_pause_loop/control_poll);
// longer lines are truncated, so poke rejects a line that fills it.
#define CONTROL_LINE_MAX 2048
#define PEEK_MAX 65536

// Parses one unsigned number (decimal or 0x hex) at *p, then skips the spaces
// after it. Returns 0 if there is no number or it is followed by junk.
static int parse_u32(const char **p, uint32_t *out)
{
	char *end;
	while (**p == ' ') (*p)++;
	if (**p < '0' || **p > '9') return 0;
	unsigned long v = strtoul(*p, &end, 0);
	if (*end && *end != ' ') return 0;
	*out = (uint32_t)v;
	*p = end;
	while (**p == ' ') (*p)++;
	return 1;
}

// Work RAM: 64 KB mirrored over 0xE00000-0xFFFFFF.
static int is_ram_range(uint32_t addr, uint32_t len)
{
	return addr >= 0xE00000 && addr <= 0xFFFFFF && len <= 0x1000000 - addr;
}

static uint8_t *ram_ptr(genesis_context *gen, uint32_t addr)
{
	// work_ram holds native-endian words: byte n of the bus is at n ^ 1
	// (the same access as the Z80 banked write in genesis.c)
	return ((uint8_t *)gen->work_ram) + ((addr & 0xFFFF) ^ 1);
}

// 1 if addr is backed by a plain memory buffer (ROM, or SRAM mapped as one),
// so reading it calls no handler and has no side effect.
static int is_direct_readable(m68k_context *m, uint32_t addr)
{
	memmap_chunk const *chunk = find_map_chunk(addr, &m->opts->gen, 0, NULL);
	if (!chunk || !(chunk->flags & MMAP_READ)) return 0;
	void *base = (chunk->flags & MMAP_PTR_IDX) ? m->mem_pointers[chunk->ptr_index] : chunk->buffer;
	return base != NULL;
}

// Sends {"ok":true,"result":{<head>,"hex":"<data>"}}; head has no braces.
static void send_hex_result(const char *head, const uint8_t *data, uint32_t len)
{
	static const char *H = "0123456789abcdef";
	size_t cap = strlen(head) + (size_t)len * 2 + 64;
	char *msg = malloc(cap);
	if (!msg) { control_send_err("out of memory"); return; }
	int n = snprintf(msg, cap, "{\"ok\":true,\"result\":{%s,\"hex\":\"", head);
	for (uint32_t i = 0; i < len; i++) {
		msg[n++] = H[data[i] >> 4];
		msg[n++] = H[data[i] & 0xF];
	}
	n += snprintf(msg + n, cap - n, "\"}}\n");
	send(control_sock, msg, n, 0);
	free(msg);
}

// step_until: condition checked at every frame boundary while stepping.
static int until_active = 0;
static uint32_t until_addr;
static uint8_t until_mask, until_value;

static void send_until_result(int matched)
{
	char body[96];
	snprintf(body, sizeof(body), "\"frame\":%lu,\"matched\":%s", control_frame, matched ? "true" : "false");
	control_send_ok(body);
}

// save_state/load_state request, carried out by control_instruction_boundary.
enum { STATE_NONE, STATE_SAVE, STATE_LOAD };
static int state_op = STATE_NONE;
static char state_path[1024];

// Runs a pending save_state/load_state with BlastEm's native serializer
// (genesis_serialize/genesis_deserialize, the format of its .state slots) and
// sends the reply.
static void do_state_op(genesis_context *gen, int op)
{
	char body[1100];
	if (op == STATE_SAVE) {
		serialize_buffer state;
		init_serialize(&state);
		// the NEW_CORE 68000 saves its own pc; the argument is only used by the JIT core
		genesis_serialize(gen, &state, gen->m68k->pc, 1);
		int ok = save_to_file(&state, state_path);
		free(state.data);
		if (!ok) { control_send_err("save_state failed to write the file"); return; }
	} else {
		deserialize_buffer state;
		if (!load_from_file(&state, state_path)) {
			control_send_err("load_state failed: not a BlastEm native .state file");
			return;
		}
		genesis_deserialize(&state, gen);
		free(state.data);
		// the VDP frame counter came from the file: rebase so the next
		// boundary sees the frames run since the load, not the difference
		gen->last_frame = gen->vdp->frame;
		gen->last_flush_cycle = gen->m68k->cycles;
	}
	snprintf(body, sizeof(body), "\"path\":\"%s\",\"frame\":%lu", state_path, control_frame);
	control_send_ok(body);
}

// Dispatch a single request line. Returns:
//   0 = handled inline (reply already sent), stay paused
//   1 = advance the emulator (step/run); reply is sent at the next boundary
//   (Tasks 9-10 extend this for pad/screenshot/reset)
int control_dispatch(char *line)
{
	if (!strcmp(line, "ping")) { control_send_ok("\"pong\":true"); return 0; }
	if (!strcmp(line, "version")) {
		char body[128];
		snprintf(body, sizeof(body), "\"version\":\"%s\",\"protocol\":1", BLASTEM_VERSION);
		control_send_ok(body);
		return 0;
	}
	if (!strncmp(line, "step ", 5)) {
		if (running) { control_send_err("cannot step while running; pause first"); return 0; }
		int n = atoi(line + 5);
		if (n <= 0) { control_send_err("step needs a positive frame count"); return 0; }
		step_remaining = n;
		return 1;  // break pause loop so frames advance; reply sent at boundary
	}
	if (!strcmp(line, "run")) {
		running = 1;
		control_send_ok("\"mode\":\"run\"");
		return 1;  // break pause loop; free-run
	}
	if (!strcmp(line, "pause")) {
		// only meaningful mid-run; handled in the boundary poll. If paused, just report.
		running = 0;
		send_frame_result();
		return 0;
	}
	if (!strcmp(line, "pad") || !strncmp(line, "pad ", 4)) {
		const char *args = (line[3] == ' ') ? line + 4 : "";
		int mask = args[0] ? parse_pad(args) : 0;
		if (mask < 0) { control_send_err("unknown button"); return 0; }
		pad_desired = (uint16_t)mask;
		// echo the normalized set
		char body[256]; int n = snprintf(body, sizeof(body), "\"buttons\":[");
		int first = 1;
		for (int i = 0; i < BTN_COUNT; i++) if (pad_desired & BTN_MAP[i].bit) {
			n += snprintf(body+n, sizeof(body)-n, "%s\"%s\"", first?"":",", BTN_MAP[i].name);
			first = 0;
		}
		snprintf(body+n, sizeof(body)-n, "]");
		control_send_ok(body);
		return 0;
	}
	// "screenshot vint [path]": the frame latched at the last VInt ack, with that
	// VInt's count ("vint" in the reply); 0 = none yet. Plain "screenshot [path]"
	// is the live framebuffer, unchanged.
	if (!strcmp(line, "screenshot") || !strncmp(line, "screenshot ", 11)) {
		int latched = !strcmp(line, "screenshot vint") || !strncmp(line, "screenshot vint ", 16);
		const char *path = latched ? (line[15] == ' ' ? line + 16 : NULL) : ((line[10] == ' ') ? line + 11 : NULL);
		if (latched && !vint_fb) { control_send_err("no latched frame yet"); return 0; }
		#define SHOT(p) (latched ? save_latched_frame(p) : save_screenshot(p))
		if (path && *path) {
			if (SHOT(path)) {
				char body[1100];
				if (latched) snprintf(body, sizeof(body), "\"vint\":%lu,\"path\":\"%s\"", vint_count, path);
				else snprintf(body, sizeof(body), "\"path\":\"%s\"", path);
				control_send_ok(body);
			} else {
				control_send_err("screenshot failed");
			}
		} else {
			// one file per process: several emulators run side by side
			char tmp[64]; snprintf(tmp, sizeof(tmp), "/tmp/emocre_ctl_shot_%d.png", (int)getpid());
			if (!SHOT(tmp)) { control_send_err("screenshot failed"); return 0; }
			int len; char *b64 = b64_encode_file(tmp, &len);
			remove(tmp);
			if (!b64) { control_send_err("screenshot read failed"); return 0; }
			char *msg = malloc(len + 96);
			if (!msg) { free(b64); control_send_err("out of memory"); return 0; }
			int n = latched
				? snprintf(msg, len + 96, "{\"ok\":true,\"result\":{\"vint\":%lu,\"png_base64\":\"%s\"}}\n", vint_count, b64)
				: snprintf(msg, len + 96, "{\"ok\":true,\"result\":{\"png_base64\":\"%s\"}}\n", b64);
			send(control_sock, msg, n, 0);
			free(b64); free(msg);
		}
		#undef SHOT
		return 0;
	}
	// layers <mask>: hide VDP layers in the rendered frame (screenshots included).
	// Same bits as vdp->layer_disabled: 1 = BGA, 2 = BGB, 4 = sprites; 0 shows all.
	if (!strncmp(line, "layers ", 7)) {
		if (!current_system || current_system->type != SYSTEM_GENESIS) {
			control_send_err("layers needs a Genesis system");
			return 0;
		}
		char *end;
		long mask = strtol(line + 7, &end, 0);
		if (end == line + 7 || *end || mask < 0 || mask > 7) {
			control_send_err("layers needs a mask 0-7 (1=BGA 2=BGB 4=sprites)");
			return 0;
		}
		genesis_context *gen = (genesis_context *)current_system;
		gen->vdp->layer_disabled = (uint8_t)mask;
		char body[32]; snprintf(body, sizeof(body), "\"layers_disabled\":%ld", mask);
		control_send_ok(body);
		return 0;
	}
	// vram [pal] [path]: the whole VRAM as a 512x256 PNG (vdp_save_vram_image:
	// 2048 tiles of 8x8, 64 per row, in address order), colored with CRAM
	// palette line `pal` (0-3, default 0). Same reply as screenshot.
	if (!strcmp(line, "vram") || !strncmp(line, "vram ", 5)) {
		if (!current_system || current_system->type != SYSTEM_GENESIS) {
			control_send_err("vram needs a Genesis system");
			return 0;
		}
		const char *args = (line[4] == ' ') ? line + 5 : "";
		int pal = 0;
		if (args[0] >= '0' && args[0] <= '3' && (args[1] == 0 || args[1] == ' ')) {
			pal = args[0] - '0';
			args += args[1] ? 2 : 1;
		}
		genesis_context *gen = (genesis_context *)current_system;
		uint8_t saved = gen->vdp->debug_modes[DEBUG_VRAM];
		gen->vdp->debug_modes[DEBUG_VRAM] = pal;
		char tmp[64]; snprintf(tmp, sizeof(tmp), "/tmp/emocre_ctl_vram_%d.png", (int)getpid());
		const char *path = args[0] ? args : tmp;
		vdp_save_vram_image(gen->vdp, path);
		gen->vdp->debug_modes[DEBUG_VRAM] = saved;
		if (args[0]) {
			char body[1100]; snprintf(body, sizeof(body), "\"path\":\"%s\",\"pal\":%d", path, pal);
			control_send_ok(body);
			return 0;
		}
		int len; char *b64 = b64_encode_file(path, &len);
		remove(path);
		if (!b64) { control_send_err("vram read failed"); return 0; }
		char *msg = malloc(len + 64);
		if (!msg) { free(b64); control_send_err("out of memory"); return 0; }
		int n = snprintf(msg, len + 64, "{\"ok\":true,\"result\":{\"png_base64\":\"%s\"}}\n", b64);
		send(control_sock, msg, n, 0);
		free(b64); free(msg);
		return 0;
	}
	// sound_state: last value written to every YM2612 register (256 per part,
	// indexed by register number), per-channel key-on mask, DAC and PSG state.
	// Pure read, safe while paused.
	// - ym: a register never written and one written with 0 both read 0; only
	//   the registers BlastEm stores are filled (part 0: 0x21..0xB7, part 1: 0x30..0xB7).
	// - keyon: keyon>>4 of BlastEm's channel keyon, in the order of reg 0x28
	//   slot bits (keyon_bits {0x10,0x40,0x20,0x80}): bit0 = op1 (0x28 bit4),
	//   bit1 = op3 (0x28 bit5), bit2 = op2 (0x28 bit6), bit3 = op4 (0x28 bit7).
	//   That is the 0x28 slot order, not operator order. Channel 3's value
	//   reflects reg 0x28 only; CSM-driven key-on is not included.
	// - dac_value: last write to reg 0x2A, 0 before the first one.
	// "sound_state" reads the chips now (result starts with "frame"); "sound_state
	// vint" returns the same fields latched when the 68000 last acknowledged VInt
	// (result starts with "vint", the count of that VInt; 0 = none yet). A step
	// ends a few lines after VInt, so the latch is the state at the frame's
	// interrupt, before the VInt handler and the next frame's code run.
	if (!strcmp(line, "sound_state") || !strcmp(line, "sound_state vint")) {
		if (!current_system || current_system->type != SYSTEM_GENESIS) { control_send_err("sound_state needs a genesis system"); return 0; }
		int latched = line[11] != 0;
		sound_snapshot live;
		if (!latched && !read_sound(current_system, &live)) { control_send_err("no sound chips"); return 0; }
		const int cap = 8192;
		char *b = malloc(cap);
		if (!b) { control_send_err("out of memory"); return 0; }
		int n = latched
			? snprintf(b, cap, "{\"ok\":true,\"result\":{\"vint\":%lu,", vint_count)
			: snprintf(b, cap, "{\"ok\":true,\"result\":{\"frame\":%lu,", control_frame);
		n += format_sound(b + n, cap - n, latched ? &vint_snapshot : &live);
		n += snprintf(b + n, cap - n, "}}\n");
		send(control_sock, b, n, 0);
		free(b);
		return 0;
	}
	// peek <addr> <len>: read len (1-65536) bytes of the 68000 address space
	// with no side effects. Allowed: work RAM (0xE00000-0xFFFFFF, 64 KB
	// mirrored) and cartridge space backed by a plain buffer (ROM). I/O, Z80,
	// VDP and handler-mapped ranges are refused. Reply {"addr","hex"}.
	if (!strncmp(line, "peek ", 5)) {
		if (!current_system || current_system->type != SYSTEM_GENESIS) { control_send_err("peek needs a Genesis system"); return 0; }
		const char *p = line + 5;
		uint32_t addr, len;
		if (!parse_u32(&p, &addr) || !parse_u32(&p, &len) || *p) { control_send_err("peek needs <addr> <len>"); return 0; }
		if (!len || len > PEEK_MAX) { control_send_err("peek len must be 1-65536"); return 0; }
		genesis_context *gen = (genesis_context *)current_system;
		uint8_t *buf = malloc(len);
		if (!buf) { control_send_err("out of memory"); return 0; }
		if (is_ram_range(addr, len)) {
			for (uint32_t i = 0; i < len; i++) buf[i] = *ram_ptr(gen, addr + i);
		} else if (addr < 0x400000 && len <= 0x400000 - addr) {
			for (uint32_t i = 0; i < len; i++) {
				if (!is_direct_readable(gen->m68k, addr + i)) {
					free(buf);
					control_send_err("peek range is not plain ROM/RAM (I/O or mapper handler)");
					return 0;
				}
				buf[i] = read_byte(addr + i, (void **)gen->m68k->mem_pointers, &gen->m68k->opts->gen, gen->m68k);
			}
		} else {
			free(buf);
			control_send_err("peek only reads cartridge ROM (0x000000-0x3FFFFF) or work RAM (0xE00000-0xFFFFFF)");
			return 0;
		}
		char head[32]; snprintf(head, sizeof(head), "\"addr\":%u", addr);
		send_hex_result(head, buf, len);
		free(buf);
		return 0;
	}
	// poke <addr> <hexbytes>: write bytes into work RAM (0xE00000-0xFFFFFF)
	// only. The whole line must fit in CONTROL_LINE_MAX (about 1000 bytes per
	// poke). Reply {"addr","len"}.
	if (!strncmp(line, "poke ", 5)) {
		if (!current_system || current_system->type != SYSTEM_GENESIS) { control_send_err("poke needs a Genesis system"); return 0; }
		if (strlen(line) >= CONTROL_LINE_MAX - 1) { control_send_err("poke line too long; split the write"); return 0; }
		const char *p = line + 5;
		uint32_t addr;
		if (!parse_u32(&p, &addr) || !*p) { control_send_err("poke needs <addr> <hexbytes>"); return 0; }
		size_t hexlen = strlen(p);
		if (hexlen % 2 || strspn(p, "0123456789abcdefABCDEF") != hexlen) { control_send_err("poke bytes must be an even number of hex digits"); return 0; }
		uint32_t len = (uint32_t)(hexlen / 2);
		if (!is_ram_range(addr, len)) { control_send_err("poke only writes work RAM (0xE00000-0xFFFFFF)"); return 0; }
		genesis_context *gen = (genesis_context *)current_system;
		for (uint32_t i = 0; i < len; i++) {
			char byte[3] = {p[2*i], p[2*i+1], 0};
			*ram_ptr(gen, addr + i) = (uint8_t)strtoul(byte, NULL, 16);
		}
		// RAM may hold 68000 code (a no-op on the interpreter core)
		m68k_invalidate_code_range(gen->m68k, 0xE00000, 0x1000000);
		char body[48]; snprintf(body, sizeof(body), "\"addr\":%u,\"len\":%u", addr, len);
		control_send_ok(body);
		return 0;
	}
	// save_state <path> / load_state <path>: the whole machine to/from a file in
	// BlastEm's native .state format (genesis_serialize/genesis_deserialize).
	// Both run at a 68000 instruction boundary: when paused at a step's frame
	// boundary that is the same sync point or the end of the instruction in
	// flight, before the next frame. The reply comes from there and the
	// emulator stays paused. Reply {"path","frame"}; frame is this server's
	// step counter, which a load does not rewind. The live framebuffer is not
	// in the state: step before a screenshot. Interpreter (NEW_CORE) only.
	if (!strncmp(line, "save_state ", 11) || !strncmp(line, "load_state ", 11)) {
		if (!current_system || current_system->type != SYSTEM_GENESIS) { control_send_err("save_state/load_state need a Genesis system"); return 0; }
#ifndef NEW_CORE
		control_send_err("save_state/load_state need a NEW_CORE (interpreter) build");
		return 0;
#endif
		const char *path = line + 11;
		if (!*path || strlen(path) >= sizeof(state_path)) { control_send_err("save_state/load_state need a path (max 1023 chars)"); return 0; }
		if (state_op != STATE_NONE) { control_send_err("a save_state/load_state is already pending"); return 0; }
		strcpy(state_path, path);
		state_op = line[0] == 's' ? STATE_SAVE : STATE_LOAD;
		return 1;  // leave the pause loop; control_instruction_boundary replies
	}
	// vdp_mem <vram|cram|vsram> <addr> <len>: raw VDP memory as hex. Byte
	// addresses. vram: 64 KB in bus order. cram: 64 words (128 bytes), each the
	// hardware 9-bit 0000BBB0GGG0RRR0, big-endian. vsram: vsram_size words
	// (40, 80 bytes) of 11 bits, big-endian. Reply {"kind","addr","hex"}.
	if (!strncmp(line, "vdp_mem ", 8)) {
		if (!current_system || current_system->type != SYSTEM_GENESIS) { control_send_err("vdp_mem needs a Genesis system"); return 0; }
		vdp_context *v = ((genesis_context *)current_system)->vdp;
		const char *p = line + 8;
		const char *kind;
		uint32_t size;
		if (!strncmp(p, "vram ", 5)) { kind = "vram"; size = VRAM_SIZE; p += 5; }
		else if (!strncmp(p, "cram ", 5)) { kind = "cram"; size = CRAM_SIZE * 2; p += 5; }
		else if (!strncmp(p, "vsram ", 6)) { kind = "vsram"; size = v->vsram_size * 2; p += 6; }
		else { control_send_err("vdp_mem needs vram, cram or vsram"); return 0; }
		uint32_t addr, len;
		if (!parse_u32(&p, &addr) || !parse_u32(&p, &len) || *p) { control_send_err("vdp_mem needs <kind> <addr> <len>"); return 0; }
		if (!len || addr >= size || len > size - addr) { control_send_err("vdp_mem range outside that memory"); return 0; }
		uint8_t *buf = malloc(len);
		if (!buf) { control_send_err("out of memory"); return 0; }
		for (uint32_t i = 0; i < len; i++) {
			uint32_t a = addr + i;
			if (!strcmp(kind, "vram")) {
				buf[i] = v->vdpmem[a];
			} else {
				uint16_t w = !strcmp(kind, "cram") ? v->cram[a >> 1] & CRAM_BITS : v->vsram[a >> 1] & VSRAM_BITS;
				buf[i] = (a & 1) ? w & 0xFF : w >> 8;
			}
		}
		char head[48]; snprintf(head, sizeof(head), "\"kind\":\"%s\",\"addr\":%u", kind, addr);
		send_hex_result(head, buf, len);
		free(buf);
		return 0;
	}
	// step_until <addr> <mask> <value> <max_steps>: step like "step" (same
	// unit: one frame boundary, half a video frame) until
	// (byte at addr & mask) == value at a boundary, or max_steps have run.
	// Always advances at least one step. addr must be work RAM. Reply
	// {"frame","matched"}.
	if (!strncmp(line, "step_until ", 11)) {
		if (running) { control_send_err("cannot step while running; pause first"); return 0; }
		if (!current_system || current_system->type != SYSTEM_GENESIS) { control_send_err("step_until needs a Genesis system"); return 0; }
		const char *p = line + 11;
		uint32_t addr, mask, value, n;
		if (!parse_u32(&p, &addr) || !parse_u32(&p, &mask) || !parse_u32(&p, &value) || !parse_u32(&p, &n) || *p) {
			control_send_err("step_until needs <addr> <mask> <value> <max_steps>");
			return 0;
		}
		if (!is_ram_range(addr, 1)) { control_send_err("step_until addr must be work RAM (0xE00000-0xFFFFFF)"); return 0; }
		if (mask > 0xFF || value > 0xFF) { control_send_err("step_until mask and value are bytes (0-255)"); return 0; }
		if (!n || n > 0x7FFFFFFF) { control_send_err("step_until needs a positive step count"); return 0; }
		until_active = 1;
		until_addr = addr;
		until_mask = (uint8_t)mask;
		until_value = (uint8_t)value;
		step_remaining = (int)n;
		return 1;
	}
	if (!strcmp(line, "reset") || !strcmp(line, "reset hard")) {
		if (current_system && current_system->soft_reset) {
			current_system->soft_reset(current_system);
		}
		control_send_ok("\"ok\":true");
		return 0;
	}
	control_send_err("unknown command");
	return 0;
}

// Read+dispatch commands until a command asks the emulator to advance
// (step/run) or the client disconnects. Tasks 8+ give control_dispatch a
// nonzero return to break this loop.
void control_pause_loop(void *sys)
{
	char line[CONTROL_LINE_MAX];
	while (control_sock >= 0) {
		if (!control_read_line(line, sizeof(line))) {
			CTL_CLOSE(control_sock);
			control_sock = -1;
			exit(0);
		}
		if (control_dispatch(line)) return;
	}
}

// Called at every Genesis frame boundary while the control server is active.
// Drives the step/run/pause state machine.
void control_frame_boundary(void *sys, unsigned int elapsed)
{
	if (control_sock < 0) return;
	control_frame += elapsed;
	control_apply_pad(sys);
	// a save_state/load_state is about to run at the next instruction
	// boundary (same sync or the next); it replies and pauses there
	if (state_op != STATE_NONE) return;

	if (step_remaining > 0) {
		step_remaining -= (int)elapsed;
		int matched = until_active
			&& (*ram_ptr((genesis_context *)sys, until_addr) & until_mask) == until_value;
		if (matched || step_remaining <= 0) {
			step_remaining = 0;
			if (until_active) {
				until_active = 0;
				send_until_result(matched);
			} else {
				send_frame_result();
			}
			control_pause_loop(sys);   // block for the next command
		}
		return;
	}

	if (running) {
		// non-blocking drain of any pending commands (pad/pause/screenshot)
		control_poll(sys);
		return;
	}

	// paused and not stepping: block until told to advance
	control_pause_loop(sys);
}

// Called from sync_components when the 68000 is at an instruction boundary
// (address known). Runs a pending save_state/load_state, replies, and stays
// paused there (unless free-running) so a following command, including
// another save/load, sees exactly that point.
void control_instruction_boundary(void *sys)
{
	if (control_sock < 0) return;
	while (state_op != STATE_NONE) {
		int op = state_op;
		do_state_op((genesis_context *)sys, op);
		state_op = STATE_NONE;
		if (running || step_remaining) break;
		control_pause_loop(sys);
	}
}

static void set_nonblocking(int s, int on)
{
#ifdef _WIN32
	u_long m = on; ioctlsocket(s, FIONBIO, &m);
#else
	int fl = fcntl(s, F_GETFL, 0);
	fcntl(s, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

// Non-blocking drain of any commands the client sent while free-running.
void control_poll(void *sys)
{
	set_nonblocking(control_sock, 1);
	char line[CONTROL_LINE_MAX];
	for (;;) {
		int space = (int)sizeof(rx) - rx_len;
		if (space <= 0) { rx_len = 0; break; }  // buffer full with no newline: drop and stop polling this cycle
		int got = recv(control_sock, rx + rx_len, space, 0);
		if (got > 0) { rx_len += got; continue; }
		if (got == 0) {          // peer closed the connection
			CTL_CLOSE(control_sock);
			control_sock = -1;
			exit(0);
		}
		break;                    // got < 0: no data available right now
	}
	set_nonblocking(control_sock, 0);
	// dispatch any complete lines currently buffered
	while (rx_has_line()) {
		rx_take_line(line, sizeof(line));
		if (!strcmp(line, "pause")) { running = 0; send_frame_result(); control_pause_loop(sys); return; }
		control_dispatch(line);
	}
}
