# Sub-Epic 1 — Input Recording Infrastructure: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implementar gravação de inputs por frame em arquivo `.bsm`, com save state inicial embutido, ativada via flag `-R <arquivo>` na CLI e keybinding `ui.movie_record`.

**Architecture:** Um módulo `movie.c/h` independente que captura o estado dos botões a cada frame via hook no loop do genesis. A serialização do estado inicial usa o `system_header.serialize` já existente. O arquivo `.bsm` tem header binário fixo de 64 bytes + save state serializado + array de inputs.

**Tech Stack:** C (gnu99), zlib (já linkado via `png.c`), APIs internas `io.h`, `serialize.h`, `system.h`, `genesis.h`.

---

## Mapa de Arquivos

| Arquivo | Ação | Responsabilidade |
|---------|------|-----------------|
| `io.c` | Modificar | Adicionar `io_read_pad_buttons` |
| `io.h` | Modificar | Declarar `io_read_pad_buttons` |
| `movie.h` | Criar | Tipos, constantes, API pública |
| `movie.c` | Criar | Implementação completa do módulo |
| `testmovie.c` | Criar | Testa write/read do header `.bsm` |
| `genesis.c` | Modificar | Hook `movie_update` no frame boundary |
| `blastem.c` | Modificar | Flag `-R`, chamadas start/stop |
| `bindings.c` | Modificar | Ação `UI_MOVIE_RECORD` |
| `Makefile` | Modificar | `movie.o` em MAINOBJS, alvo `testmovie` |

---

## Task 1: Adicionar `io_read_pad_buttons` em `io.c` / `io.h`

**Files:**
- Modify: `io.c` (após linha 165, função `io_port_gamepad_up`)
- Modify: `io.h` (após linha 161, declaração de `io_port_gamepad_up`)

- [ ] **Step 1.1: Adicionar declaração em `io.h`**

Abrir `io.h`. Após a linha:
```c
void io_port_gamepad_up(io_port *port, uint8_t button);
```
Adicionar:
```c
uint16_t io_read_pad_buttons(io_port *port);
```

- [ ] **Step 1.2: Implementar em `io.c`**

Abrir `io.c`. Após a função `io_port_gamepad_up` (após linha ~165):

```c
uint16_t io_read_pad_buttons(io_port *port)
{
	uint16_t state = 0;
	for (int btn = DPAD_UP; btn < NUM_GAMEPAD_BUTTONS; btn++) {
		gp_button_def *def = button_defs + btn;
		if (port->input[def->states[0]] & def->value) {
			state |= (1 << (btn - DPAD_UP));
		}
	}
	return state;
}
```

> Esta função itera pelos 12 botões e mapeia cada um para um bit (DPAD_UP → bit 0 … BUTTON_MODE → bit 11), usando a mesma `button_defs` estática já usada por `io_port_gamepad_down/up`.

- [ ] **Step 1.3: Compilar para verificar**

```bash
cd /Users/afonsof/Projects/retro/blastem && make blastem 2>&1 | tail -5
```
Esperado: sem erros de compilação.

- [ ] **Step 1.4: Commit**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add io.c io.h
git commit -m "feat(io): add io_read_pad_buttons helper

Returns a 16-bit bitmask of currently-pressed buttons for a given
io_port, used by the upcoming movie recording module.

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 2: Criar `movie.h`

**Files:**
- Create: `movie.h`

- [ ] **Step 2.1: Criar o arquivo**

```c
/* movie.h — BlastEm gameplay recording (.bsm format)
 * Sub-epic 1: input recording infrastructure
 */
#ifndef MOVIE_H_
#define MOVIE_H_

#include <stdint.h>
#include <stdio.h>
#include "system.h"

/* ---- File format constants ---- */
#define BSM_MAGIC            0x1a4d5342u  /* "BSM\x1a" little-endian */
#define BSM_VERSION          1u
#define BSM_HEADER_SIZE      64u
#define BSM_INPUT_FLUSH_FRAMES 4096u

/* Flags (byte 20 of header) */
#define BSM_FLAG_FROM_RESET  (1 << 0)    /* starts from hard reset, no embedded save state */
#define BSM_FLAG_PAL         (1 << 1)    /* system is running in PAL (50 Hz) mode */

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

#endif /* MOVIE_H_ */
```

- [ ] **Step 2.2: Verificar que `sizeof(bsm_header) == 64`**

Adicionar temporariamente ao início de `movie.c` (remover após confirmar):
```c
_Static_assert(sizeof(bsm_header) == BSM_HEADER_SIZE, "bsm_header must be 64 bytes");
```

- [ ] **Step 2.3: Commit**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add movie.h
git commit -m "feat(movie): add movie.h with .bsm format types and API

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 3: Criar `testmovie.c` e as funções de formato em `movie.c`

**Files:**
- Create: `movie.c` (parcial — apenas format helpers)
- Create: `testmovie.c`

- [ ] **Step 3.1: Escrever o teste que vai falhar**

Criar `testmovie.c`:

```c
/* testmovie.c — testa write/read roundtrip do header .bsm */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "movie.h"

static void test_header_roundtrip(void)
{
	bsm_header expected = {
		.magic            = BSM_MAGIC,
		.version          = BSM_VERSION,
		.movie_id         = 0xDEADBEEFu,
		.rerecord_count   = 7,
		.frame_count      = 3600,
		.flags            = BSM_FLAG_PAL,
		.pad_mask         = 0x03,
		.reserved         = 0,
		.rom_crc32        = 0x12345678u,
		.savestate_offset = BSM_HEADER_SIZE,
		.input_offset     = BSM_HEADER_SIZE + 4 + 1024,
	};
	strncpy(expected.rom_name, "Sonic The Hedgehog", sizeof(expected.rom_name) - 1);
	memset(expected.reserved2, 0, sizeof(expected.reserved2));

	FILE *f = tmpfile();
	assert(f != NULL);

	bsm_write_header(f, &expected);

	rewind(f);

	bsm_header actual;
	int result = bsm_read_header(f, &actual);
	assert(result == 0);

	assert(actual.magic            == expected.magic);
	assert(actual.version          == expected.version);
	assert(actual.movie_id         == expected.movie_id);
	assert(actual.rerecord_count   == expected.rerecord_count);
	assert(actual.frame_count      == expected.frame_count);
	assert(actual.flags            == expected.flags);
	assert(actual.pad_mask         == expected.pad_mask);
	assert(actual.rom_crc32        == expected.rom_crc32);
	assert(memcmp(actual.rom_name, expected.rom_name, sizeof(expected.rom_name)) == 0);
	assert(actual.savestate_offset == expected.savestate_offset);
	assert(actual.input_offset     == expected.input_offset);

	fclose(f);
	printf("test_header_roundtrip: PASSED\n");
}

static void test_bad_magic(void)
{
	FILE *f = tmpfile();
	assert(f != NULL);
	uint32_t bad = 0xDEADBEEF;
	fwrite(&bad, 4, 1, f);
	rewind(f);
	bsm_header h;
	int result = bsm_read_header(f, &h);
	assert(result == -1);
	fclose(f);
	printf("test_bad_magic: PASSED\n");
}

int main(void)
{
	test_header_roundtrip();
	test_bad_magic();
	printf("All tests passed.\n");
	return 0;
}
```

- [ ] **Step 3.2: Adicionar alvo `testmovie` no Makefile**

Em `Makefile`, após a definição de `UPDDISOBJS` (~linha 331), adicionar:
```makefile
TESTMOVIEOBJS:=testmovie.o movie.o
```

Após o alvo `upddis$(EXE)` (~linha 411), adicionar:
```makefile
testmovie : $(TESTMOVIEOBJS:%.o=$(OBJDIR)/%.o)
	$(CC) -o $@ $^ $(OPT)
```

- [ ] **Step 3.3: Tentar compilar para confirmar que falha por falta de `movie.c`**

```bash
cd /Users/afonsof/Projects/retro/blastem && make testmovie 2>&1 | head -10
```
Esperado: erro de compilação sobre `movie.c` não existir ou funções não definidas.

- [ ] **Step 3.4: Criar `movie.c` com as funções de formato**

Criar `movie.c`:

```c
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

_Static_assert(sizeof(bsm_header) == BSM_HEADER_SIZE, "bsm_header must be 64 bytes");

/* ---- Low-level format I/O ---- */

/* Write all 64 header bytes to f at current position. */
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

/* ---- Recording state (stubs, completed in Task 4) ---- */

bsm_state movie_get_state(void) { return BSM_STATE_NONE; }
int  movie_record_start(system_header *system, const char *filename) { return -1; }
void movie_record_stop(void) {}
void movie_update(system_header *system) {}
```

- [ ] **Step 3.5: Compilar e rodar o teste**

```bash
cd /Users/afonsof/Projects/retro/blastem && make testmovie 2>&1 && ./testmovie
```
Esperado:
```
test_header_roundtrip: PASSED
test_bad_magic: PASSED
All tests passed.
```

- [ ] **Step 3.6: Commit**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add movie.h movie.c testmovie.c Makefile
git commit -m "feat(movie): add .bsm header format read/write with tests

testmovie verifies roundtrip serialization of bsm_header.
movie.c contains stub implementations for remaining API.

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 4: Implementar `movie_record_start`, `movie_record_stop` e `movie_update`

**Files:**
- Modify: `movie.c` (substituir stubs pelas implementações reais)

- [ ] **Step 4.1: Substituir o bloco "Recording state" em `movie.c` pelo seguinte**

Substituir tudo a partir do comentário `/* ---- Recording state (stubs...` até o fim do arquivo por:

```c
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
	uint32_t rom_crc = (uint32_t)crc32(0, system->info.rom, system->info.rom_size);

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
		movie.input_buffer     = malloc(BSM_INPUT_FLUSH_FRAMES * sizeof(bsm_frame_input));
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
```

- [ ] **Step 4.2: Compilar**

```bash
cd /Users/afonsof/Projects/retro/blastem && make blastem 2>&1 | tail -10
```
Esperado: sem erros. Linker deve encontrar `crc32` via zlib já linkado.

- [ ] **Step 4.3: Confirmar que `testmovie` ainda passa**

```bash
cd /Users/afonsof/Projects/retro/blastem && make testmovie && ./testmovie
```
Esperado: `All tests passed.`

- [ ] **Step 4.4: Commit**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add movie.c
git commit -m "feat(movie): implement movie_record_start/stop/update

- movie_record_start: serializes machine state, writes .bsm header
  and save state block, then starts capturing inputs per frame
- movie_update: snapshots pad1/pad2 state once per frame
- movie_record_stop: flushes remaining inputs, rewrites final header

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 5: Hook `movie_update` no loop do Genesis

**Files:**
- Modify: `genesis.c`

O ponto de inserção é dentro do bloco `if (v_context->frame != gen->last_frame)` em `genesis_sync_components`, logo após `event_flush(mclks)` (linha ~596).

- [ ] **Step 5.1: Adicionar include de `movie.h` em `genesis.c`**

No topo de `genesis.c`, junto aos outros `#include`, adicionar:
```c
#include "movie.h"
```

- [ ] **Step 5.2: Inserir chamada a `movie_update` no frame boundary**

Localizar o bloco (linhas ~587–596 em `genesis.c`):
```c
	if (v_context->frame != gen->last_frame) {
#ifndef IS_LIB
		if (gen->ym->scope) {
			scope_render(gen->ym->scope);
		}
#endif
		//printf(...)
		uint32_t elapsed = v_context->frame - gen->last_frame;
		gen->last_frame = v_context->frame;
		event_flush(mclks);
		gen->last_flush_cycle = mclks;
```

Após `event_flush(mclks);` e `gen->last_flush_cycle = mclks;`, adicionar:
```c
#ifndef IS_LIB
		movie_update(&gen->header);
#endif
```

O bloco completo fica:
```c
	if (v_context->frame != gen->last_frame) {
#ifndef IS_LIB
		if (gen->ym->scope) {
			scope_render(gen->ym->scope);
		}
#endif
		uint32_t elapsed = v_context->frame - gen->last_frame;
		gen->last_frame = v_context->frame;
		event_flush(mclks);
		gen->last_flush_cycle = mclks;
#ifndef IS_LIB
		movie_update(&gen->header);
#endif
```

- [ ] **Step 5.3: Compilar**

```bash
cd /Users/afonsof/Projects/retro/blastem && make blastem 2>&1 | tail -5
```
Esperado: sem erros.

- [ ] **Step 5.4: Commit**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add genesis.c
git commit -m "feat(genesis): hook movie_update at frame boundary

Called once per rendered frame inside genesis_sync_components,
guarded by IS_LIB to keep the shared library clean.

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 6: Adicionar flag `-R` em `blastem.c`

**Files:**
- Modify: `blastem.c`

- [ ] **Step 6.1: Declarar variável `record_file` em `main`**

Em `blastem.c`, dentro da função `main`, logo após as outras declarações de variáveis locais (~linha 416), adicionar:
```c
char *record_file = NULL;
```

- [ ] **Step 6.2: Adicionar include de `movie.h`**

No topo de `blastem.c`, após os outros `#include`, adicionar:
```c
#include "movie.h"
```

- [ ] **Step 6.3: Parsear o case `-R` no switch de argumentos**

No `switch(argv[i][1])` (~linha 425), adicionar após o case `'r'` (região) e antes de `'m'`:
```c
		case 'R':
			i++;
			if (i >= argc) {
				fatal_error("-R must be followed by a movie filename\n");
			}
			record_file = argv[i];
			break;
```

- [ ] **Step 6.4: Atualizar a mensagem de `-h`**

No bloco de `-h` (~linha 548), antes de `);`, adicionar a linha:
```c
				"	-R FILE     Record gameplay to FILE in .bsm format\n"
```

- [ ] **Step 6.5: Iniciar gravação após criação do sistema**

Localizar o ponto onde `start_context` é chamado pela primeira vez (~após `alloc_config_system`). Buscar por `current_system->start_context` no arquivo. Antes desta chamada, adicionar:

```c
	if (record_file && current_system) {
		if (movie_record_start(current_system, record_file)) {
			warning("Failed to start movie recording to %s\n", record_file);
			record_file = NULL;
		}
	}
```

- [ ] **Step 6.6: Parar gravação ao sair**

Localizar o final do loop principal em `main` (onde `current_system->request_exit` é chamado ou logo antes de `return 0`). Adicionar:
```c
	movie_record_stop();
```

- [ ] **Step 6.7: Compilar**

```bash
cd /Users/afonsof/Projects/retro/blastem && make blastem 2>&1 | tail -5
```
Esperado: sem erros.

- [ ] **Step 6.8: Commit**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add blastem.c
git commit -m "feat(blastem): add -R flag for gameplay recording

./blastem -R output.bsm game.bin starts recording on launch.
Recording stops automatically when the emulator exits.

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 7: Adicionar `UI_MOVIE_RECORD` em `bindings.c`

**Files:**
- Modify: `bindings.c`

Esta tarefa permite mapear uma tecla (ex.: `F9`) para `ui.movie_record` no arquivo de config.

- [ ] **Step 7.1: Adicionar enum `UI_MOVIE_RECORD`**

Em `bindings.c`, no enum `ui_action` (~linha 26), adicionar após `UI_VGM_LOG`:
```c
	UI_MOVIE_RECORD,
```

- [ ] **Step 7.2: Adicionar include de `movie.h`**

No topo de `bindings.c`, após os outros `#include`:
```c
#include "movie.h"
```

- [ ] **Step 7.3: Adicionar handler no switch de ações**

No `switch` de ações UI (~linha 457, após `case UI_VGM_LOG`), adicionar:
```c
		case UI_MOVIE_RECORD:
			if (allow_content_binds) {
				if (movie_get_state() == BSM_STATE_RECORD) {
					movie_record_stop();
				} else {
					char *path = get_content_config_path(
						"ui\0movie_path\0",
						"ui\0movie_template\0",
						"blastem_%c.bsm"
					);
					movie_record_start(current_system, path);
					free(path);
				}
			}
			break;
```

- [ ] **Step 7.4: Adicionar mapeamento de string para o enum**

Na função de parse de bindings (~linha 766, após `} else if (!strcmp(target + 3, "vgm_log"))`), adicionar:
```c
		} else if (!strcmp(target + 3, "movie_record")) {
			*subtype_a = UI_MOVIE_RECORD;
```

- [ ] **Step 7.5: Compilar**

```bash
cd /Users/afonsof/Projects/retro/blastem && make blastem 2>&1 | tail -5
```
Esperado: sem erros.

- [ ] **Step 7.6: Commit**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add bindings.c
git commit -m "feat(bindings): add ui.movie_record keybinding action

Toggle recording on/off. Maps to UI_MOVIE_RECORD enum.
Configurable in blastem.cfg as: ui.movie_record = F9

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 8: Atualizar Makefile

**Files:**
- Modify: `Makefile`

- [ ] **Step 8.1: Adicionar `movie.o` em `MAINOBJS`**

Em `Makefile`, localizar a linha que define `MAINOBJS` (~linha 295):
```makefile
MAINOBJS:=$(COREOBJS) blastem.o $(RENDEROBJS) zip.o  menu.o debug.o gdb_remote.o bindings.o oscilloscope.o
```

Adicionar `movie.o` no final:
```makefile
MAINOBJS:=$(COREOBJS) blastem.o $(RENDEROBJS) zip.o  menu.o debug.o gdb_remote.o bindings.o oscilloscope.o movie.o
```

- [ ] **Step 8.2: Compilar tudo**

```bash
cd /Users/afonsof/Projects/retro/blastem && make 2>&1 | tail -5
```
Esperado: `dis`, `zdis` e `blastem` compilados sem erros.

- [ ] **Step 8.3: Confirmar que `testmovie` ainda compila**

```bash
cd /Users/afonsof/Projects/retro/blastem && make testmovie && ./testmovie
```
Esperado: `All tests passed.`

- [ ] **Step 8.4: Commit**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add Makefile
git commit -m "build: add movie.o to MAINOBJS and testmovie target

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 9: Validação End-to-End

**Files:** nenhum arquivo novo — apenas execução e verificação.

- [ ] **Step 9.1: Build final completo**

```bash
cd /Users/afonsof/Projects/retro/blastem && make clean && make 2>&1 | tail -5
```
Esperado: sem erros ou warnings novos.

- [ ] **Step 9.2: Executar com uma ROM de teste por ~5 segundos**

```bash
# Substitua jogo.bin pelo caminho de qualquer ROM .bin disponível
cd /Users/afonsof/Projects/retro/blastem
./blastem -R /tmp/test_recording.bsm /caminho/para/jogo.bin &
sleep 5
kill %1
```

- [ ] **Step 9.3: Verificar o arquivo gerado**

```bash
# Verificar magic bytes e tamanho
xxd /tmp/test_recording.bsm | head -5
# Esperado: primeira linha começa com "4253 4d1a" (= "BSM\x1a" little-endian: 42 53 4d 1a)

# Verificar tamanho (5s a 60fps ≈ 300 frames = header + save_state + 300*4 bytes)
wc -c /tmp/test_recording.bsm
```

- [ ] **Step 9.4: Verificar frame_count no header**

```python
# Script rápido para inspecionar o header
python3 - << 'EOF'
import struct, sys
with open('/tmp/test_recording.bsm', 'rb') as f:
    data = f.read(64)
magic, version, movie_id, rerecord, frames = struct.unpack_from('<IIIII', data, 0)
flags, pad_mask = struct.unpack_from('<BB', data, 20)
rom_crc = struct.unpack_from('<I', data, 24)[0]
rom_name = data[28:44].rstrip(b'\x00').decode('ascii', errors='replace')
ss_off, inp_off = struct.unpack_from('<II', data, 44)

print(f"Magic:    0x{magic:08X} (expect 0x1A4D5342)")
print(f"Version:  {version}")
print(f"Frames:   {frames}")
print(f"ROM name: {rom_name!r}")
print(f"SS offset:    {ss_off}")
print(f"Input offset: {inp_off}")
expected_size = inp_off + frames * 4
print(f"Expected file size: {expected_size}")
import os
actual = os.path.getsize('/tmp/test_recording.bsm')
print(f"Actual file size:   {actual}")
print("SIZE OK" if actual == expected_size else "SIZE MISMATCH!")
EOF
```

Esperado:
```
Magic:    0x1A4D5342 (expect 0x1A4D5342)
Version:  1
Frames:   (> 0)
SIZE OK
```

- [ ] **Step 9.5: Commit final de validação**

```bash
cd /Users/afonsof/Projects/retro/blastem
git commit --allow-empty -m "chore: sub-epic 1 end-to-end validation passed

Recording produces valid .bsm with correct magic, frame count,
and file size matching header offsets.

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Checklist de Cobertura do Spec

| Requisito do Spec | Tarefa |
|---|---|
| `io_read_pad_buttons` helper | Task 1 |
| `movie.h` com tipos e API pública | Task 2 |
| Formato `.bsm`: magic, header 64 bytes | Tasks 2, 3 |
| `bsm_write_header` / `bsm_read_header` testáveis | Task 3 |
| `movie_record_start` serializa estado inicial | Task 4 |
| `movie_update` captura pad state por frame | Task 4 |
| `movie_record_stop` flush + reescreve header | Task 4 |
| Hook em `genesis.c` no frame boundary | Task 5 |
| Flag `-R` na CLI | Task 6 |
| Ação `ui.movie_record` configurável | Task 7 |
| `movie.o` no Makefile | Task 8 |
| Validação: magic, frame_count, tamanho correto | Task 9 |
