/*
 * movie.c — BlastEm gameplay recording (.bsm format)
 * Sub-epic 1: input recording infrastructure
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "movie.h"
#include "io.h"
#include "genesis.h"
#include "zlib/zlib.h"
#include "util.h"

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

/* ---- Public API ---- */

bsm_state movie_get_state(void)
{
	return movie.state;
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
			free(state_data);
			return -1;
		}
		movie.input_buffer_cap = BSM_INPUT_FLUSH_FRAMES;
	}
	movie.input_buffer_used = 0;

	movie.file  = f;
	movie.state = BSM_STATE_RECORD;

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
	if (movie.state != BSM_STATE_RECORD) {
		return;
	}

	if (system->type != SYSTEM_GENESIS && system->type != SYSTEM_SEGACD) {
		return;
	}

	genesis_context *gen = (genesis_context *)system;
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
