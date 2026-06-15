/*
 * movie.c — BlastEm gameplay recording (.bsm format)
 * Sub-epic 1: input recording infrastructure
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include "movie.h"
#include "io.h"
#include "genesis.h"
#include "zlib/zlib.h"
#include "util.h"
#include "vdp.h"
#include "render.h"

_Static_assert(sizeof(bsm_header) == BSM_HEADER_SIZE,
               "bsm_header layout changed — file format broken");

/* ---- Low-level format I/O ---- */

/* Write all 64 header bytes to f at position 0. */
void bsm_write_header(FILE *f, const bsm_header *h)
{
	uint8_t buf[BSM_HEADER_SIZE];
	uint8_t *p = buf;

#define W32(v) do { uint32_t _v = (v); memcpy(p, &_v, 4); p += 4; } while(0)
#define W16(v) do { uint16_t _v = (v); memcpy(p, &_v, 2); p += 2; } while(0)
#define W8(v)  do { *p++ = (uint8_t)(v); } while(0)

	W32(h->magic);
	W32(h->version);
	W32(h->movie_id);
	W32(h->rerecord_count);
	W32(h->frame_count);
	W8(h->flags);
	W8(h->pad_mask);
	W16(h->reserved);
	W32(h->rom_crc32);
	memcpy(p, h->rom_name, 16); p += 16;
	W32(h->savestate_offset);
	W32(h->input_offset);
	memcpy(p, h->reserved2, 12); p += 12;

#undef W32
#undef W16
#undef W8

	fseek(f, 0, SEEK_SET);
	fwrite(buf, 1, BSM_HEADER_SIZE, f);
}

/* Read 64 header bytes from f at current position.
 * Returns 0 on success, -1 if magic or version is wrong. */
int bsm_read_header(FILE *f, bsm_header *h)
{
	uint8_t buf[BSM_HEADER_SIZE];
	if (fread(buf, 1, BSM_HEADER_SIZE, f) != BSM_HEADER_SIZE) {
		return -1;
	}
	uint8_t *p = buf;

#define R32(dst) do { memcpy(&(dst), p, 4); p += 4; } while(0)
#define R16(dst) do { memcpy(&(dst), p, 2); p += 2; } while(0)
#define R8(dst)  do { (dst) = *p++; } while(0)

	R32(h->magic);
	R32(h->version);
	R32(h->movie_id);
	R32(h->rerecord_count);
	R32(h->frame_count);
	R8(h->flags);
	R8(h->pad_mask);
	R16(h->reserved);
	R32(h->rom_crc32);
	memcpy(h->rom_name, p, 16); p += 16;
	R32(h->savestate_offset);
	R32(h->input_offset);
	memcpy(h->reserved2, p, 12); p += 12;

#undef R32
#undef R16
#undef R8

	if (h->magic != BSM_MAGIC || h->version != BSM_VERSION) {
		return -1;
	}
	return 0;
}

/* ---- Internal state ---- */

typedef struct {
	bsm_state        state;
	FILE             *file;
	bsm_header       header;
	bsm_frame_input  *input_buffer;
	uint32_t         input_buffer_cap;  /* allocated slots */
	uint32_t         input_buffer_used; /* filled slots since last flush */
	uint8_t          freeze_seen;   /* setado por movie_unfreeze, checado por movie_check_after_load */
	uint32_t         play_frame;       /* next frame index to inject during BSM_STATE_PLAY */
} bsm_movie;

static bsm_movie movie;

/* ---- Internal helpers ---- */

static void flush_inputs(void)
{
	if (!movie.file || !movie.input_buffer_used) {
		return;
	}
	fseek(movie.file, movie.header.input_offset +
	      (movie.header.frame_count - movie.input_buffer_used) * sizeof(bsm_frame_input),
	      SEEK_SET);
	fwrite(movie.input_buffer, sizeof(bsm_frame_input), movie.input_buffer_used, movie.file);
	movie.input_buffer_used = 0;
}

/* ---- Video export helpers (sub-epic 4) ---- */

/* Converte framebuffer ARGB8888 para RGB24 e escreve no pipe do ffmpeg.
 * Pula BORDER_LEFT pixels de borda à esquerda e lê apenas a área visível.
 * Retorna 0 em sucesso, -1 em erro de escrita.
 * NOTA: não-static para acesso por testmovie.c */
int movie_export_write_frame(pixel_t *fb, int pitch,
                                     uint32_t vis_width, uint32_t vis_height, FILE *out)
{
	uint8_t *row_base = (uint8_t *)fb;
	size_t rgb_row_size = vis_width * 3;
	uint8_t *rgb_row = malloc(rgb_row_size);
	if (!rgb_row) {
		warning("movie_export_write_frame: out of memory for row buffer\n");
		return -1;
	}

	for (uint32_t y = 0; y < vis_height; y++) {
		/* Pula colunas de borda esquerda */
		uint32_t *src = (uint32_t *)(row_base + (size_t)y * pitch) + BORDER_LEFT;
		for (uint32_t x = 0; x < vis_width; x++) {
			uint32_t argb = src[x];
			rgb_row[x * 3 + 0] = (argb >> 16) & 0xFF; /* R */
			rgb_row[x * 3 + 1] = (argb >> 8)  & 0xFF; /* G */
			rgb_row[x * 3 + 2] = argb         & 0xFF; /* B */
		}
		if (fwrite(rgb_row, 1, rgb_row_size, out) != rgb_row_size) {
			warning("movie_export_write_frame: write failed (pipe broken?)\n");
			free(rgb_row);
			return -1;
		}
	}
	free(rgb_row);
	return 0;
}

/* Executa o loop headless de exportacao.
 * Avanca a maquina frame a frame, captura o framebuffer VDP
 * e escreve cada frame convertido no pipe do ffmpeg. */
static int movie_export_loop(system_header *system, FILE *pipe_out)
{
	genesis_context *gen = (genesis_context *)system;
	int is_h40 = gen->vdp->h40_lines > gen->vdp->output_lines / 2;
	uint32_t vis_width  = is_h40 ? 320 : 256;
	uint32_t vis_height = gen->vdp->output_lines;

	for (uint32_t f = 0; f < movie.header.frame_count; f++) {
		/* Run one frame of emulation.
		 * resume_context advances the machine by one "quantum"
		 * (typically one frame). movie_update is hooked inside
		 * the normal flow and injects recorded inputs. */
		system->resume_context(system);

		/* Ensure VDP has finished the current framebuffer */
		vdp_force_update_framebuffer(gen->vdp);

		/* Capture framebuffer */
		int pitch;
		pixel_t *fb = render_export_get_fb(&pitch);
		if (!fb) {
			warning("movie_export_loop: failed to get framebuffer at frame %u\n", f);
			return -1;
		}

		/* Convert and write to ffmpeg pipe */
		if (movie_export_write_frame(fb, pitch, vis_width, vis_height, pipe_out) != 0) {
			warning("movie_export_loop: write failed at frame %u\n", f);
			return -1;
		}
	}

	return 0;
}

int movie_export_start(system_header *system, const char *bsm_path, const char *output_path)
{
	/* 1. Start playback (sub-epic 3) */
	if (movie_play_start(system, bsm_path) != 0) {
		warning("movie_export_start: failed to open %s for playback\n", bsm_path);
		return -1;
	}

	genesis_context *gen = (genesis_context *)system;

	/* 2. Initialize headless framebuffer */
	render_export_init();

	/* 3. Detect framerate from header */
	int fps = (movie.header.flags & BSM_FLAG_PAL) ? 50 : 60;

	/* 4. Detect visible resolution */
	int is_h40 = gen->vdp->h40_lines > gen->vdp->output_lines / 2;
	uint32_t vis_width  = is_h40 ? 320 : 256;
	uint32_t vis_height = gen->vdp->output_lines;

	/* 5. Open ffmpeg pipe */
	char cmd[1024];
	snprintf(cmd, sizeof(cmd),
		"ffmpeg -y -f rawvideo -vcodec rawvideo "
		"-s %dx%d -r %d -pix_fmt rgb24 -i pipe:0 "
		"-c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p \"%s\"",
		vis_width, vis_height, fps, output_path);

	debug_message("movie_export_start: %s\n", cmd);

	FILE *pipe_out = popen(cmd, "w");
	if (!pipe_out) {
		warning("movie_export_start: ffmpeg not found or failed to start: %s\n",
		        strerror(errno));
		movie_play_stop();
		return -1;
	}

	/* 6. Headless export loop */
	int ret = movie_export_loop(system, pipe_out);

	/* 7. Close pipe */
	int pclose_ret = pclose(pipe_out);
	if (pclose_ret != 0) {
		warning("movie_export_start: ffmpeg exited with status %d\n", pclose_ret);
		if (ret == 0) ret = -1;
	}

	movie_play_stop();
	return ret;
}

/* ---- Public API ---- */

bsm_state movie_get_state(void)
{
	return movie.state;
}

uint32_t movie_get_play_frame(void)
{
	return movie.play_frame;
}

void movie_play_stop(void)
{
	if (movie.state != BSM_STATE_PLAY)
		return;
	movie.state      = BSM_STATE_NONE;
	movie.play_frame = 0;
	debug_message("Movie playback stopped\n");
}

void movie_play_pre_inject(system_header *system)
{
	if (movie.state != BSM_STATE_PLAY || movie.header.frame_count == 0)
		return;
	if (system->type != SYSTEM_GENESIS && system->type != SYSTEM_SEGACD)
		return;

	genesis_context *gen = (genesis_context *)system;
	bsm_frame_input *frame0 = &movie.input_buffer[0];
	io_port *port1 = find_gamepad(&gen->io, 1);
	io_port *port2 = find_gamepad(&gen->io, 2);
	if (port1) io_port_set_pad_state(port1, frame0->pad1);
	if (port2) io_port_set_pad_state(port2, frame0->pad2);

	movie.play_frame = 1;  /* frame 0 already injected, next is frame 1 */
}

int movie_play_start(system_header *system, const char *filename)
{
	if (movie.state != BSM_STATE_NONE)
		movie_record_stop();

	FILE *f = fopen(filename, "rb");
	if (!f) {
		warning("movie_play_start: cannot open %s\n", filename);
		return -1;
	}

	bsm_header h;
	if (bsm_read_header(f, &h) != 0) {
		warning("movie_play_start: invalid .bsm file\n");
		fclose(f);
		return -1;
	}

	/* ROM CRC check — warning only, never blocks playback */
	uint32_t rom_crc = (uint32_t)crc32(0,
		(const Bytef *)system->info.rom, system->info.rom_size);
	if (h.rom_crc32 != rom_crc) {
		warning("movie_play_start: ROM CRC mismatch "
			"(movie=0x%08x current=0x%08x) — proceeding anyway\n",
			h.rom_crc32, rom_crc);
	}

	/* Read and restore embedded save state */
	fseek(f, h.savestate_offset, SEEK_SET);
	uint32_t ss_size;
	if (fread(&ss_size, 4, 1, f) != 1) {
		warning("movie_play_start: failed to read save state size\n");
		fclose(f);
		return -1;
	}
	uint8_t *ss_buf = malloc(ss_size);
	if (!ss_buf || fread(ss_buf, 1, ss_size, f) != ss_size) {
		warning("movie_play_start: failed to read embedded save state\n");
		free(ss_buf);
		fclose(f);
		return -1;
	}
	system->deserialize(system, ss_buf, ss_size);
	free(ss_buf);

	/* Load all input frames into RAM */
	if (!movie.input_buffer || movie.input_buffer_cap < h.frame_count) {
		free(movie.input_buffer);
		movie.input_buffer = malloc(h.frame_count * sizeof(bsm_frame_input));
		if (!movie.input_buffer) {
			warning("movie_play_start: out of memory for input buffer\n");
			fclose(f);
			return -1;
		}
		movie.input_buffer_cap = h.frame_count;
	}

	fseek(f, h.input_offset, SEEK_SET);
	if (fread(movie.input_buffer, sizeof(bsm_frame_input),
		  h.frame_count, f) != h.frame_count) {
		warning("movie_play_start: failed to read input buffer\n");
		fclose(f);
		return -1;
	}
	fclose(f);

	movie.header     = h;
	movie.play_frame = 0;
	movie.state      = BSM_STATE_PLAY;

	debug_message("Movie playback started: %s (%u frames)\n",
		      filename, h.frame_count);
	return 0;
}

int movie_record_start(system_header *system, const char *filename)
{
	if (movie.state != BSM_STATE_NONE) {
		movie_record_stop();
	}

	FILE *f = fopen(filename, "wb");
	if (!f) {
		warning("movie_record_start: failed to open %s\n", filename);
		return -1;
	}

	/* Serialize current machine state */
	size_t state_size = 0;
	uint8_t *state_data = system->serialize(system, &state_size);
	if (!state_data || !state_size) {
		warning("movie_record_start: serialize failed\n");
		fclose(f);
		return -1;
	}

	/* Compute ROM CRC32 */
	uint32_t rom_crc = (uint32_t)crc32(0, (const Bytef *)system->info.rom, system->info.rom_size);

	/* Build header */
	memset(&movie.header, 0, sizeof(movie.header));
	movie.header.magic            = BSM_MAGIC;
	movie.header.version          = BSM_VERSION;
	movie.header.movie_id         = (uint32_t)time(NULL);
	movie.header.rerecord_count   = 0;
	movie.header.frame_count      = 0;
	movie.header.flags            = 0;
	movie.header.pad_mask         = 0x03; /* pad1 and pad2 */
	movie.header.rom_crc32        = rom_crc;
	if (system->info.name) {
		strncpy(movie.header.rom_name, system->info.name, sizeof(movie.header.rom_name) - 1);
	}
	movie.header.savestate_offset = BSM_HEADER_SIZE;
	movie.header.input_offset     = BSM_HEADER_SIZE + 4 + (uint32_t)state_size;

	/* Write header (placeholder — will be rewritten on stop) */
	bsm_write_header(f, &movie.header);

	/* Write save state: 4-byte size prefix + raw data */
	uint32_t ss_size = (uint32_t)state_size;
	fwrite(&ss_size, 4, 1, f);
	fwrite(state_data, 1, state_size, f);
	free(state_data);

	/* Allocate input buffer */
	if (!movie.input_buffer) {
		movie.input_buffer = malloc(BSM_INPUT_FLUSH_FRAMES * sizeof(bsm_frame_input));
		if (!movie.input_buffer) {
			warning("movie_record_start: out of memory for input buffer\n");
			fclose(f);
			return -1;
		}
		movie.input_buffer_cap = BSM_INPUT_FLUSH_FRAMES;
	}
	movie.input_buffer_used = 0;

	movie.file  = f;
	movie.state = BSM_STATE_RECORD;
	static uint8_t atexit_registered = 0;
	if (!atexit_registered) {
		atexit(movie_record_stop);
		atexit_registered = 1;
	}

	debug_message("Movie recording started: %s\n", filename);
	return 0;
}

void movie_record_stop(void)
{
	if (movie.state != BSM_STATE_RECORD || !movie.file) {
		return;
	}
	flush_inputs();
	/* Rewrite header with final frame_count */
	bsm_write_header(movie.file, &movie.header);
	fclose(movie.file);
	movie.file  = NULL;
	movie.state = BSM_STATE_NONE;
	debug_message("Movie recording stopped. Frames: %u\n", movie.header.frame_count);
}

void movie_update(system_header *system)
{
	if (system->type != SYSTEM_GENESIS && system->type != SYSTEM_SEGACD) {
		return;
	}

	genesis_context *gen = (genesis_context *)system;

	if (movie.state == BSM_STATE_PLAY) {
		if (movie.play_frame >= movie.header.frame_count) {
			/* Input buffer exhausted — go live */
			movie.state = BSM_STATE_NONE;
			debug_message("Movie playback ended at frame %u — going live\n",
				      movie.play_frame);
			return;
		}
		bsm_frame_input *frame = &movie.input_buffer[movie.play_frame];
		io_port *port1 = find_gamepad(&gen->io, 1);
		io_port *port2 = find_gamepad(&gen->io, 2);
		if (port1) io_port_set_pad_state(port1, frame->pad1);
		if (port2) io_port_set_pad_state(port2, frame->pad2);
		movie.play_frame++;
		return;
	}

	if (movie.state != BSM_STATE_RECORD) {
		return;
	}

	io_port *port1 = find_gamepad(&gen->io, 1);
	io_port *port2 = find_gamepad(&gen->io, 2);

	bsm_frame_input frame;
	frame.pad1 = port1 ? io_read_pad_buttons(port1) : 0;
	frame.pad2 = port2 ? io_read_pad_buttons(port2) : 0;

	movie.input_buffer[movie.input_buffer_used++] = frame;
	movie.header.frame_count++;

	if (movie.input_buffer_used >= movie.input_buffer_cap) {
		flush_inputs();
	}
}

/* ---- Re-recording (sub-epic 2) ---- */

void movie_prepare_for_load(void)
{
	movie.freeze_seen = 0;
}

void movie_check_after_load(void)
{
	if (movie.state != BSM_STATE_RECORD) {
		movie.freeze_seen = 0;
		return;
	}
	if (!movie.freeze_seen) {
		warning("movie_check_after_load: save state sem SECTION_MOVIE — parando gravação\n");
		movie_record_stop();
	}
	movie.freeze_seen = 0;
}

void movie_freeze(serialize_buffer *buf)
{
	if (movie.state != BSM_STATE_RECORD) {
		return;
	}
	start_section(buf, SECTION_MOVIE);
	save_int32(buf, movie.header.frame_count);
	save_int32(buf, movie.input_buffer_used);
	save_buffer8(buf, (uint8_t *)movie.input_buffer,
	             movie.input_buffer_used * sizeof(bsm_frame_input));
	end_section(buf);
}

void movie_unfreeze(deserialize_buffer *buf, void *vgen)
{
	(void)vgen;
	uint32_t saved_frame_count = load_int32(buf);
	uint32_t saved_buffer_used = load_int32(buf);

	movie.freeze_seen = 1;

	if (movie.state != BSM_STATE_RECORD) {
		/* Not recording: ignore data. load_section already advances the parent cursor. */
		return;
	}

	if (saved_buffer_used > movie.input_buffer_cap) {
		saved_buffer_used = movie.input_buffer_cap;
	}
	load_buffer8(buf, (uint8_t *)movie.input_buffer,
	             saved_buffer_used * sizeof(bsm_frame_input));

	/* Update counters BEFORE flush so flush seeks to the correct position */
	movie.input_buffer_used  = saved_buffer_used;
	movie.header.frame_count = saved_frame_count;

	/* Flush the restored buffer to disk at the correct position */
	flush_inputs();

	/* Truncate .bsm on disk to remove frames after the save point */
	uint32_t trunc_pos = movie.header.input_offset +
	                     saved_frame_count * sizeof(bsm_frame_input);
#ifdef _WIN32
	_chsize(fileno(movie.file), trunc_pos);
#else
	ftruncate(fileno(movie.file), trunc_pos);
#endif
	fseek(movie.file, trunc_pos, SEEK_SET);

	/* flush_inputs() zeroes input_buffer_used; frame_count stays at saved_frame_count */
	movie.header.rerecord_count++;

	bsm_write_header(movie.file, &movie.header);
	debug_message("Movie rerecord: truncado para frame %u (rerecord_count=%u)\n",
	              saved_frame_count, movie.header.rerecord_count);
}
