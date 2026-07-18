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

// Apply pad_desired vs pad_applied via io_gamepad_down/up on pad 1 (index 0).
static void control_apply_pad(void *sys)
{
	genesis_context *gen = (genesis_context *)sys;
	uint16_t changed = pad_desired ^ pad_applied;
	for (int i = 0; i < BTN_COUNT; i++) {
		if (!(changed & BTN_MAP[i].bit)) continue;
		if (pad_desired & BTN_MAP[i].bit) io_gamepad_down(&gen->io, 0, BTN_MAP[i].button);
		else io_gamepad_up(&gen->io, 0, BTN_MAP[i].button);
	}
	pad_applied = pad_desired;
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
	if (!strcmp(line, "screenshot") || !strncmp(line, "screenshot ", 11)) {
		const char *path = (line[10] == ' ') ? line + 11 : NULL;
		if (path && *path) {
			if (save_screenshot(path)) {
				char body[1100]; snprintf(body, sizeof(body), "\"path\":\"%s\"", path);
				control_send_ok(body);
			} else {
				control_send_err("screenshot failed");
			}
		} else {
			char tmp[] = "/tmp/emocre_ctl_shot.png";
			if (!save_screenshot(tmp)) { control_send_err("screenshot failed"); return 0; }
			int len; char *b64 = b64_encode_file(tmp, &len);
			if (!b64) { control_send_err("screenshot read failed"); return 0; }
			char *msg = malloc(len + 64);
			if (!msg) { free(b64); control_send_err("out of memory"); return 0; }
			int n = snprintf(msg, len + 64, "{\"ok\":true,\"result\":{\"png_base64\":\"%s\"}}\n", b64);
			send(control_sock, msg, n, 0);
			free(b64); free(msg);
		}
		return 0;
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
	char line[2048];
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

	if (step_remaining > 0) {
		step_remaining -= (int)elapsed;
		if (step_remaining <= 0) {
			step_remaining = 0;
			send_frame_result();
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
	char line[2048];
	for (;;) {
		int got = recv(control_sock, rx + rx_len, (int)sizeof(rx) - rx_len, 0);
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
