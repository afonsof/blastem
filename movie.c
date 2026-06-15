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

/* ---- Recording state stubs (completed in Task 4) ---- */

bsm_state movie_get_state(void) { return BSM_STATE_NONE; }
int  movie_record_start(system_header *system, const char *filename) { (void)system; (void)filename; return -1; }
void movie_record_stop(void) {}
void movie_update(system_header *system) { (void)system; }
