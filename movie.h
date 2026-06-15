/* movie.h — BlastEm gameplay recording (.bsm format)
 * Sub-epic 1: input recording infrastructure
 */
#ifndef MOVIE_H_
#define MOVIE_H_

#include <stdint.h>
#include <stdio.h>
#include "system.h"
#include "serialize.h"
#include "pixel.h"

/* ---- File format constants ---- */
#define BSM_MAGIC            0x1a4d5342u  /* "BSM\x1a" little-endian */
#define BSM_VERSION          1u
#define BSM_HEADER_SIZE      64u
#define BSM_INPUT_FLUSH_FRAMES 4096u

/* Flags (byte 20 of header) */
#define BSM_FLAG_FROM_RESET  (1u << 0)   /* starts from hard reset, no embedded save state */
#define BSM_FLAG_PAL         (1u << 1)   /* system is running in PAL (50 Hz) mode */

/* ---- Types ---- */

typedef enum {
	BSM_STATE_NONE = 0,
	BSM_STATE_RECORD,
	BSM_STATE_PLAY   /* used in sub-epics 3+ */
} bsm_state;

/* One entry per frame — 4 bytes */
typedef struct {
	uint16_t pad1;  /* bitmask: bit0=UP bit1=DOWN bit2=LEFT bit3=RIGHT
	                             bit4=A bit5=B bit6=C bit7=START
	                             bit8=X bit9=Y bit10=Z bit11=MODE */
	uint16_t pad2;  /* same layout for pad 2 */
} bsm_frame_input;

/* In-memory representation of the .bsm header */
typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t movie_id;
	uint32_t rerecord_count;
	uint32_t frame_count;
	uint8_t  flags;
	uint8_t  pad_mask;          /* bit0=pad1 active, bit1=pad2 active */
	uint16_t reserved;
	uint32_t rom_crc32;
	char     rom_name[16];      /* null-padded ASCII */
	uint32_t savestate_offset;  /* offset of save state block in file */
	uint32_t input_offset;      /* offset of first bsm_frame_input in file */
	uint8_t  reserved2[12];
} bsm_header;   /* must be BSM_HEADER_SIZE (64) bytes */
_Static_assert(sizeof(bsm_header) == BSM_HEADER_SIZE,
               "bsm_header layout changed — file format broken");

/* ---- Public API ---- */

/* Start recording. Opens/creates filename, serializes initial machine state.
 * Returns 0 on success, non-zero on error. */
int  movie_record_start(system_header *system, const char *filename);

/* Stop recording. Flushes remaining inputs, rewrites header with final
 * frame_count, closes file. Safe to call when not recording. */
void movie_record_stop(void);

/* Called once per frame from genesis.c frame boundary.
 * When recording: snapshots pad state and appends to buffer. */
void movie_update(system_header *system);

/* Current recording state */
bsm_state movie_get_state(void);

/* Low-level format helpers (also used by testmovie.c and future playback) */
void bsm_write_header(FILE *f, const bsm_header *h);
int  bsm_read_header(FILE *f, bsm_header *h);   /* returns 0 ok, -1 bad magic/version */

/* ---- Re-recording (sub-epic 2) ---- */

/* Chamado ao final de genesis_serialize: embute movie state em SECTION_MOVIE.
 * No-op se não estiver gravando. */
void movie_freeze(serialize_buffer *buf);

/* Handler para SECTION_MOVIE em genesis_deserialize: trunca timeline ao frame salvo.
 * Se gravando e seção ausente: movie_check_after_load() para a gravação. */
void movie_unfreeze(deserialize_buffer *buf, void *vgen);

/* Chamar ANTES de cada genesis_deserialize para resetar a flag de detecção. */
void movie_prepare_for_load(void);

/* Chamar APÓS genesis_deserialize: para gravação se SECTION_MOVIE não apareceu. */
void movie_check_after_load(void);

/* ---- Playback (sub-epic 3) ---- */

/* Opens filename, restores the embedded save state via system->deserialize,
 * loads all input frames into RAM and switches to BSM_STATE_PLAY.
 * Returns 0 on success, -1 on error. Safe to call while recording (stops it). */
int movie_play_start(system_header *system, const char *filename);

/* Stops playback immediately, resets play_frame to 0.
 * Safe to call when not playing. */
void movie_play_stop(void);

/* Returns the index of the next frame to inject (0 when not playing). */
uint32_t movie_get_play_frame(void);

/* Called immediately after movie_play_start to pre-inject frame 0's input
 * into the genesis io ports. Fixes the 1-frame playback offset caused by
 * movie_update being called at the end of each frame (after io_run).
 * Must only be called when movie.state == BSM_STATE_PLAY && frame_count > 0. */
void movie_play_pre_inject(system_header *system);

/* ---- Video export (sub-epic 4) ---- */

/* Converte framebuffer ARGB8888 para RGB24 e escreve no pipe.
 * Pula BORDER_LEFT pixels de borda à esquerda.
 * Retorna 0 em sucesso, -1 em erro de escrita. */
int movie_export_write_frame(pixel_t *fb, int pitch,
                             uint32_t vis_width, uint32_t vis_height, FILE *out);

/* Exporta um .bsm para video MP4 via ffmpeg CLI (pipe).
 * Requer ffmpeg no PATH. Roda o emulador em modo headless.
 * Retorna 0 em sucesso, nao-zero em erro. */
int movie_export_start(system_header *system, const char *bsm_path, const char *output_path);

#endif /* MOVIE_H_ */
