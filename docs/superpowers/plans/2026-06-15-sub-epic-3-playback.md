# Sub-Epic 3 — Playback: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Abrir um arquivo `.bsm`, restaurar o save state inicial embutido e, a cada frame, injetar os inputs gravados nos ports de IO do emulador em vez dos inputs reais do jogador; ao esgotar o buffer, vira "live".

**Architecture:** Nova função `io_port_set_pad_state` em `io.c` (simétrica a `io_read_pad_buttons`). Campo `play_frame` adicionado à struct interna `bsm_movie`. Três novas funções públicas em `movie.c/h`: `movie_play_start` (abre arquivo, restaura save state via `system->deserialize`, carrega todos os inputs em RAM), `movie_play_stop`, `movie_get_play_frame`. `movie_update` ganha um `if (BSM_STATE_PLAY)` no topo que injeta o frame e avança o ponteiro. Flag `-P FILE` em `blastem.c`. Nenhum novo hook em `genesis.c` — `movie_update` já é chamado a cada frame.

**Tech Stack:** C (gnu99), `io.h/io.c`, `movie.h/movie.c`, `blastem.c`, `testmovie.c`, `zlib/zlib.h` (para crc32, já incluída).

---

## Mapa de Arquivos

| Arquivo | Ação | Responsabilidade |
|---------|------|-----------------|
| `io.h` | Modificar linha 161 | Declarar `io_port_set_pad_state` |
| `io.c` | Modificar após linha 177 | Implementar `io_port_set_pad_state` |
| `movie.h` | Modificar linha 92 | Declarar `movie_play_start`, `movie_play_stop`, `movie_get_play_frame` |
| `movie.c` | Modificar linha 101 | Adicionar `play_frame` à struct `bsm_movie` |
| `movie.c` | Modificar após linha 125 | Implementar `movie_get_play_frame`, `movie_play_stop`, `movie_play_start` |
| `movie.c` | Modificar linhas 214-238 | Estender `movie_update` com case PLAY |
| `blastem.c` | Modificar linhas 424, 492, 583, 736 | Flag `-P`, variável, help, chamada |
| `testmovie.c` | Modificar linhas 182, 190 | `test_playback_input_buffer_roundtrip`, chamar em main |

---

## Task 1: `io_port_set_pad_state` em `io.h` e `io.c`

**Files:**
- Modify: `io.h` (linha 161, após `uint16_t io_read_pad_buttons(io_port *port);`)
- Modify: `io.c` (linha 177, após o `}` de `io_read_pad_buttons`)

- [ ] **Step 1.1: Escrever o teste que falha em `testmovie.c`**

Em `testmovie.c`, antes de `int main(void)` (linha 183), adicionar:

```c
static void test_io_port_set_pad_state(void)
{
	/* Build a minimal io_port with a 6-button gamepad */
	io_port port;
	memset(&port, 0, sizeof(port));
	port.device_type = IO_GAMEPAD6;

	/* All buttons released: read should return 0 */
	uint16_t state = io_read_pad_buttons(&port);
	assert(state == 0);

	/* Set A+B+START pressed (bits 4,5,7 = 0b10110000 = 0x00B0) */
	uint16_t want = (1u << (BUTTON_A    - DPAD_UP))
	              | (1u << (BUTTON_B    - DPAD_UP))
	              | (1u << (BUTTON_START - DPAD_UP));
	io_port_set_pad_state(&port, want);

	state = io_read_pad_buttons(&port);
	assert(state == want);

	/* Release all — set 0 */
	io_port_set_pad_state(&port, 0);
	state = io_read_pad_buttons(&port);
	assert(state == 0);

	printf("test_io_port_set_pad_state: PASSED\n");
}
```

Adicionar a chamada em `main`:
```c
	test_io_port_set_pad_state();
```

- [ ] **Step 1.2: Rodar o teste para confirmar que falha**

```bash
cd /Users/afonsof/Projects/retro/blastem && make testmovie 2>&1 | tail -5
```

Esperado: erro de compilação — `io_port_set_pad_state` undeclared.

- [ ] **Step 1.3: Declarar em `io.h`**

Em `io.h`, na linha 161, após `uint16_t io_read_pad_buttons(io_port *port);`, inserir:

```c
void io_port_set_pad_state(io_port *port, uint16_t buttons);
```

- [ ] **Step 1.4: Implementar em `io.c`**

Em `io.c`, após o `}` de `io_read_pad_buttons` (linha 177), inserir:

```c
void io_port_set_pad_state(io_port *port, uint16_t buttons)
{
	for (int btn = DPAD_UP; btn < NUM_GAMEPAD_BUTTONS; btn++) {
		uint16_t mask = 1u << (btn - DPAD_UP);
		if (buttons & mask)
			io_port_gamepad_down(port, btn);
		else
			io_port_gamepad_up(port, btn);
	}
}
```

- [ ] **Step 1.5: Rodar o teste para confirmar que passa**

```bash
cd /Users/afonsof/Projects/retro/blastem && make testmovie 2>&1 | tail -3 && ./testmovie
```

Esperado: `test_io_port_set_pad_state: PASSED` e `All tests passed.`

- [ ] **Step 1.6: Commit**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add io.h io.c testmovie.c
git commit -m "feat(io): add io_port_set_pad_state — write bitmask to pad (sub-epic 3)

Symmetric to io_read_pad_buttons. Used by movie playback to inject
recorded inputs frame by frame. Tested via testmovie."
```

---

## Task 2: Campo `play_frame`, declarações e implementações de suporte

**Files:**
- Modify: `movie.c` (struct `bsm_movie`, linha 101)
- Modify: `movie.h` (após linha 92)
- Modify: `movie.c` (após `movie_get_state` na linha 125)

- [ ] **Step 2.1: Adicionar `play_frame` à struct `bsm_movie`**

Em `movie.c`, na struct `bsm_movie` (linhas 94-102), após a linha `uint8_t freeze_seen;`, inserir:

```c
	uint32_t         play_frame;       /* next frame index to inject during BSM_STATE_PLAY */
```

A struct fica:
```c
typedef struct {
	bsm_state        state;
	FILE             *file;
	bsm_header       header;
	bsm_frame_input  *input_buffer;
	uint32_t         input_buffer_cap;
	uint32_t         input_buffer_used;
	uint8_t          freeze_seen;
	uint32_t         play_frame;
} bsm_movie;
```

- [ ] **Step 2.2: Declarar as 3 funções de playback em `movie.h`**

Em `movie.h`, antes de `#endif /* MOVIE_H_ */` (linha 94), inserir:

```c
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
```

- [ ] **Step 2.3: Implementar `movie_get_play_frame` e `movie_play_stop`**

Em `movie.c`, após `movie_get_state` (linha 125), inserir:

```c
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
```

- [ ] **Step 2.4: Build para confirmar sem erros**

```bash
cd /Users/afonsof/Projects/retro/blastem && make blastem 2>&1 | grep -E "error:|warning:" | head -10
```

Esperado: zero linhas de erro. (Warnings de `movie_play_start` undeclared são esperados — ainda não implementado.)

- [ ] **Step 2.5: Commit**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add movie.h movie.c
git commit -m "feat(movie): play_frame field + playback API stubs (sub-epic 3)"
```

---

## Task 3: Implementar `movie_play_start`

**Files:**
- Modify: `movie.c` (após `movie_play_stop`)

- [ ] **Step 3.1: Escrever o teste que falha**

Em `testmovie.c`, antes de `int main(void)`, adicionar:

```c
static void test_playback_input_buffer_roundtrip(void)
{
	/* Phase 1: record 5 frames with known input patterns to a temp file */
	system_header sys = make_fake_system();
	int r = movie_record_start(&sys, "/tmp/testmovie_pb.bsm");
	assert(r == 0);

	/* Directly write known frames into the internal buffer.
	 * We access movie internals through the public input_buffer_used path:
	 * call movie_update is impossible without a real genesis_context.
	 * Instead we rely on movie_record_stop + file inspection. */

	/* Simulate 5 known frames by calling movie_update with a fake system.
	 * Since movie_update guards on SYSTEM_GENESIS and casts to genesis_context,
	 * we need a minimal genesis_context. Build one on the stack. */
	genesis_context fake_gen;
	memset(&fake_gen, 0, sizeof(fake_gen));
	fake_gen.header.type = SYSTEM_GENESIS;

	/* Set up two fake gamepads in fake_gen.io */
	fake_gen.io.ports[0].device_type = IO_GAMEPAD6;
	fake_gen.io.ports[0].device.pad.gamepad_num = 1;
	fake_gen.io.ports[1].device_type = IO_GAMEPAD6;
	fake_gen.io.ports[1].device.pad.gamepad_num = 2;

	/* Define 5 known input patterns */
	uint16_t pad1_inputs[5] = {0x0001, 0x0003, 0x0007, 0x000F, 0x001F};
	uint16_t pad2_inputs[5] = {0x0010, 0x0030, 0x0070, 0x00F0, 0x01F0};

	for (int i = 0; i < 5; i++) {
		io_port_set_pad_state(&fake_gen.io.ports[0], pad1_inputs[i]);
		io_port_set_pad_state(&fake_gen.io.ports[1], pad2_inputs[i]);
		movie_update(&fake_gen.header);
	}

	movie_record_stop();
	assert(movie_get_state() == BSM_STATE_NONE);

	/* Phase 2: read the file back manually and verify inputs */
	FILE *f = fopen("/tmp/testmovie_pb.bsm", "rb");
	assert(f != NULL);

	bsm_header h;
	r = bsm_read_header(f, &h);
	assert(r == 0);
	assert(h.frame_count == 5);

	fseek(f, h.input_offset, SEEK_SET);
	bsm_frame_input frames[5];
	size_t read = fread(frames, sizeof(bsm_frame_input), 5, f);
	assert(read == 5);
	fclose(f);

	for (int i = 0; i < 5; i++) {
		assert(frames[i].pad1 == pad1_inputs[i]);
		assert(frames[i].pad2 == pad2_inputs[i]);
	}

	/* Phase 3: movie_play_start loads the file, reads header + inputs into RAM.
	 * We cannot call system->deserialize (fake_serialize returns 16 zeroed bytes —
	 * movie_play_start will call fake_deserialize which is a no-op stub).
	 * Add a fake_deserialize stub to make_fake_system. */
	assert(movie_get_state() == BSM_STATE_NONE);
	r = movie_play_start(&sys, "/tmp/testmovie_pb.bsm");
	assert(r == 0);
	assert(movie_get_state() == BSM_STATE_PLAY);
	assert(movie_get_play_frame() == 0);

	/* Verify input buffer was loaded correctly */
	/* We read back via movie_get_play_frame advancing through movie_update calls */
	for (int i = 0; i < 5; i++) {
		assert(movie_get_play_frame() == (uint32_t)i);
		movie_update(&fake_gen.header);
	}
	/* After 5 frames the buffer is exhausted — state must flip to NONE */
	assert(movie_get_state() == BSM_STATE_NONE);

	printf("test_playback_input_buffer_roundtrip: PASSED\n");
}
```

Adicionar `fake_deserialize` a `make_fake_system` em `testmovie.c`:

```c
static void fake_deserialize(system_header *s, uint8_t *buf, size_t sz)
{
	(void)s; (void)buf; (void)sz;
}

static system_header make_fake_system(void)
{
	static uint8_t fake_rom[1] = {0};
	system_header s;
	memset(&s, 0, sizeof(s));
	s.serialize   = fake_serialize;
	s.deserialize = fake_deserialize;
	s.info.rom      = fake_rom;
	s.info.rom_size = 1;
	s.type          = SYSTEM_GENESIS;
	return s;
}
```

Adicionar a chamada em `main`:
```c
	test_playback_input_buffer_roundtrip();
```

- [ ] **Step 3.2: Rodar o teste para confirmar que falha**

```bash
cd /Users/afonsof/Projects/retro/blastem && make testmovie 2>&1 | tail -5
```

Esperado: erro de compilação ou link — `movie_play_start` undefined.

- [ ] **Step 3.3: Implementar `movie_play_start` em `movie.c`**

Em `movie.c`, após `movie_play_stop`, inserir:

```c
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
```

- [ ] **Step 3.4: Estender `movie_update` com case `BSM_STATE_PLAY`**

Em `movie.c`, substituir a função `movie_update` inteira (linhas 214-238):

```c
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
```

- [ ] **Step 3.5: Rodar os testes para confirmar que passam**

```bash
cd /Users/afonsof/Projects/retro/blastem && make testmovie 2>&1 | tail -5 && ./testmovie
```

Esperado:
```
test_header_roundtrip: PASSED
test_bad_magic: PASSED
test_check_noop_when_not_recording: PASSED
test_stop_on_missing_section: PASSED
test_freeze_writes_section: PASSED
test_freeze_unfreeze_roundtrip: PASSED
test_io_port_set_pad_state: PASSED
test_playback_input_buffer_roundtrip: PASSED
All tests passed.
```

- [ ] **Step 3.6: Build do binário principal**

```bash
cd /Users/afonsof/Projects/retro/blastem && make blastem 2>&1 | grep -E "error:" | head -10
```

Esperado: zero linhas de erro.

- [ ] **Step 3.7: Commit**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add movie.c movie.h testmovie.c
git commit -m "feat(movie): implement movie_play_start + BSM_STATE_PLAY in movie_update (sub-epic 3)

- movie_play_start: opens .bsm, restores save state via system->deserialize,
  loads all inputs into RAM, sets BSM_STATE_PLAY
- movie_update: injects recorded inputs per frame; flips to NONE when exhausted
- Tested via test_playback_input_buffer_roundtrip (5-frame roundtrip)"
```

---

## Task 4: Flag `-P FILE` em `blastem.c`

**Files:**
- Modify: `blastem.c`

- [ ] **Step 4.1: Declarar `play_file` junto a `record_file` (linha 424)**

Em `blastem.c`, na linha 424, após `char *record_file = NULL;`, inserir:

```c
	char *play_file = NULL;
```

- [ ] **Step 4.2: Parsear `-P` no switch (após o case `'R':`, linha 492)**

Em `blastem.c`, após o `break;` do case `'R':` (linha 492), inserir:

```c
		case 'P':
			i++;
			if (i >= argc) {
				fatal_error("-P must be followed by a movie filename\n");
			}
			play_file = argv[i];
			break;
```

- [ ] **Step 4.3: Adicionar ao help text (linha 583)**

Em `blastem.c`, após a linha `"\t-R FILE     Record gameplay to FILE in .bsm format\n"`, inserir:

```c
					"\t-P FILE     Play back gameplay from FILE in .bsm format\n"
```

- [ ] **Step 4.4: Chamar `movie_play_start` (após o bloco record, linha 736)**

Em `blastem.c`, após o bloco `if (record_file && current_system) { ... }` (linha 736), inserir:

```c
	if (play_file && current_system) {
		if (movie_play_start(current_system, play_file)) {
			warning("Failed to start movie playback from %s\n", play_file);
			play_file = NULL;
		}
	}
```

- [ ] **Step 4.5: Build**

```bash
cd /Users/afonsof/Projects/retro/blastem && make blastem 2>&1 | grep -E "error:" | head -10
```

Esperado: zero linhas de erro.

- [ ] **Step 4.6: Smoke test da flag de ajuda**

```bash
cd /Users/afonsof/Projects/retro/blastem && ./blastem -h 2>&1 | grep "\-P"
```

Esperado:
```
	-P FILE     Play back gameplay from FILE in .bsm format
```

- [ ] **Step 4.7: Commit**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add blastem.c
git commit -m "feat(blastem): add -P FILE flag for .bsm movie playback (sub-epic 3)"
```

---

## Task 5: Verificação End-to-End

- [ ] **Step 5.1: Gravar ~10 segundos**

```bash
cd /Users/afonsof/Projects/retro/blastem
./blastem -R /tmp/e2e_play_test.bsm /path/to/sonic.bin
# Jogar ~10 segundos e fechar o emulador (Ctrl+Q ou fechar janela)
```

- [ ] **Step 5.2: Verificar header do arquivo gravado**

```bash
python3 -c "
import struct
with open('/tmp/e2e_play_test.bsm', 'rb') as f:
    magic = f.read(4)
    assert magic == b'BSM\x1a', f'bad magic: {magic!r}'
    f.seek(16)
    fc = struct.unpack('<I', f.read(4))[0]
    print(f'frame_count: {fc} (~{fc/60:.1f}s NTSC)')
    assert fc > 0, 'frame_count must be > 0'
print('Header OK')
"
```

Esperado: `frame_count: NNN (~10.0s NTSC)` e `Header OK`.

- [ ] **Step 5.3: Reproduzir e verificar identidade visual**

```bash
./blastem -P /tmp/e2e_play_test.bsm /path/to/sonic.bin
```

Verificar manualmente:
- A reprodução começa do mesmo estado em que foi gravada.
- Os movimentos são idênticos à sessão original.
- Ao terminar o buffer, o emulador continua rodando e aceita inputs do teclado/controle.

- [ ] **Step 5.4: Testar arquivo inválido**

```bash
echo "JUNK" > /tmp/bad.bsm
./blastem -P /tmp/bad.bsm /path/to/sonic.bin 2>&1 | grep "movie_play_start"
```

Esperado: warning `movie_play_start: invalid .bsm file` e emulador abre normalmente.

- [ ] **Step 5.5: Commit de validação**

```bash
cd /Users/afonsof/Projects/retro/blastem
git commit --allow-empty -m "chore: sub-epic 3 playback e2e validated"
```

---

## Critério de Aceite

1. `./testmovie` exibe `All tests passed.` com todos os 8 testes.
2. `./blastem -P gravacao.bsm jogo.bin` reproduz frame a frame sem crash.
3. Reprodução visualmente idêntica à gravação original.
4. Ao esgotar o buffer, emulador vira "live" sem interrupção perceptível.
5. Arquivo `.bsm` inválido gera warning no stderr e não crasha.
6. CRC mismatch gera warning mas não bloqueia o playback.
