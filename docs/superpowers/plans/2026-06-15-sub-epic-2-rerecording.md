# Sub-Epic 2 — Re-recording with Save States: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Embutir movie state em save states de modo que carregar um save state durante gravação trunca o input buffer naquele frame e incrementa o rerecord_count.

**Architecture:** Dois hooks em `genesis.c`: `movie_freeze` ao final de `genesis_serialize` (escreve `SECTION_MOVIE`) e `movie_unfreeze` registrado como handler em `genesis_deserialize` (lê e trunca). `movie_check_after_load` (chamado no final de `genesis_deserialize`) para a gravação se o save state não tinha movie data. Tudo contido em `movie.c/h` + modificações pontuais em `serialize.h` e `genesis.c`.

**Tech Stack:** C (gnu99), `serialize.h` (section I/O já existente), `ftruncate`/`_chsize` (POSIX/Win32).

---

## Mapa de Arquivos

| Arquivo | Ação | Responsabilidade |
|---------|------|-----------------|
| `serialize.h` | Modificar | Adicionar `SECTION_MOVIE = 24` ao enum |
| `movie.h` | Modificar | Declarar `movie_freeze`, `movie_unfreeze`, `movie_check_after_load`, `movie_prepare_for_load` |
| `movie.c` | Modificar | Implementar as 4 novas funções + campo `freeze_seen` em `bsm_movie` |
| `testmovie.c` | Modificar | Adicionar 3 novos testes TDD |
| `genesis.c` | Modificar | Hook `movie_freeze` no final de `genesis_serialize`; registrar handler + prepare + check em `genesis_deserialize` |

---

## Task 1: Adicionar `SECTION_MOVIE` a `serialize.h`

**Files:**
- Modify: `serialize.h` (linha 54, após `SECTION_COLECO_IO`)

- [ ] **Step 1.1: Adicionar o enum value**

Abrir `serialize.h`. Mudar:
```c
	SECTION_COLECO_IO
```
Para:
```c
	SECTION_COLECO_IO,
	SECTION_MOVIE
```

- [ ] **Step 1.2: Verificar que o valor é 24**

```bash
cd /Users/afonsof/Projects/retro/blastem && python3 -c "
import subprocess, sys
# SECTION_MOVIE deve ser o 25º enum (0-indexed = 24)
out = subprocess.check_output(['grep', '-c', 'SECTION_', 'serialize.h']).decode().strip()
print('Entries:', out, '(expect 25)')
"
```

- [ ] **Step 1.3: Build para confirmar sem erros**

```bash
cd /Users/afonsof/Projects/retro/blastem && make blastem 2>&1 | tail -3
```
Esperado: sem erros.

- [ ] **Step 1.4: Commit**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add serialize.h
git commit -m "feat(serialize): add SECTION_MOVIE enum value (24)

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 2: Declarar novas funções em `movie.h`

**Files:**
- Modify: `movie.h`

- [ ] **Step 2.1: Adicionar declarações à seção Public API**

Em `movie.h`, após a declaração de `bsm_read_header`, adicionar:

```c
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
```

- [ ] **Step 2.2: Build**

```bash
cd /Users/afonsof/Projects/retro/blastem && make blastem 2>&1 | tail -3
```
Esperado: sem erros (as funções ainda não estão implementadas; se o linker reclamar, é esperado na próxima etapa quando as implementarmos em movie.c).

- [ ] **Step 2.3: Commit**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add movie.h
git commit -m "feat(movie): declare re-recording API in movie.h

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 3: Escrever testes em `testmovie.c` (TDD — falham primeiro)

**Files:**
- Modify: `testmovie.c`

- [ ] **Step 3.1: Adicionar função fake_serialize e os 3 novos testes**

Em `testmovie.c`, após os includes existentes, adicionar o helper:

```c
/* Helper para testes que precisam de uma gravação ativa */
#include "genesis.h"   /* para SYSTEM_GENESIS */

static uint8_t *fake_serialize(system_header *s, size_t *sz)
{
	*sz = 16;
	uint8_t *ret = malloc(16);
	memset(ret, 0, 16);
	return ret;
}

static system_header make_fake_system(void)
{
	static uint8_t fake_rom[1] = {0};
	system_header s;
	memset(&s, 0, sizeof(s));
	s.serialize     = fake_serialize;
	s.info.rom      = fake_rom;
	s.info.rom_size = 1;
	s.type          = SYSTEM_GENESIS;
	return s;
}
```

Depois, adicionar os três testes antes de `main`:

```c
/* Testa que movie_check_after_load é no-op quando não está gravando */
static void test_check_noop_when_not_recording(void)
{
	assert(movie_get_state() == BSM_STATE_NONE);
	movie_prepare_for_load();
	movie_check_after_load(); /* não deve travar nem mudar estado */
	assert(movie_get_state() == BSM_STATE_NONE);
	printf("test_check_noop_when_not_recording: PASSED\n");
}

/* Testa que carregar state sem SECTION_MOVIE para a gravação */
static void test_stop_on_missing_section(void)
{
	system_header sys = make_fake_system();
	int r = movie_record_start(&sys, "/tmp/testmovie_stop.bsm");
	assert(r == 0);
	assert(movie_get_state() == BSM_STATE_RECORD);

	movie_prepare_for_load();
	/* Não chamamos movie_unfreeze — simula save state sem SECTION_MOVIE */
	movie_check_after_load();

	assert(movie_get_state() == BSM_STATE_NONE);
	printf("test_stop_on_missing_section: PASSED\n");
}

/* Testa que movie_freeze escreve algo na serialize_buffer quando gravando */
static void test_freeze_writes_section(void)
{
	system_header sys = make_fake_system();
	int r = movie_record_start(&sys, "/tmp/testmovie_freeze.bsm");
	assert(r == 0);

	serialize_buffer sbuf;
	init_serialize(&sbuf);
	size_t size_before = sbuf.size;

	movie_freeze(&sbuf);

	assert(sbuf.size > size_before); /* algo foi escrito */
	/* Os primeiros 2 bytes devem ser SECTION_MOVIE (=24) big-endian */
	uint16_t section_id = (sbuf.data[0] << 8) | sbuf.data[1];
	assert(section_id == SECTION_MOVIE);

	movie_record_stop();
	free(sbuf.data);
	printf("test_freeze_writes_section: PASSED\n");
}
```

- [ ] **Step 3.2: Adicionar as chamadas em `main`**

No `main` de `testmovie.c`, adicionar as três chamadas antes de `return 0`:

```c
	test_check_noop_when_not_recording();
	test_stop_on_missing_section();
	test_freeze_writes_section();
```

- [ ] **Step 3.3: Confirmar que os testes falham (funções não implementadas)**

```bash
cd /Users/afonsof/Projects/retro/blastem && make testmovie 2>&1 | head -15
```
Esperado: erro de linker (`undefined reference to movie_freeze`, etc.) ou crash em runtime.

- [ ] **Step 3.4: Commit dos testes (ainda falhando)**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add testmovie.c
git commit -m "test(movie): add failing tests for re-recording API

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 4: Implementar as 4 novas funções em `movie.c`

**Files:**
- Modify: `movie.c`

- [ ] **Step 4.1: Adicionar campo `freeze_seen` a `bsm_movie`**

Em `movie.c`, na struct `bsm_movie` (logo após `input_buffer_used`), adicionar:

```c
	uint8_t          freeze_seen;   /* setado por movie_unfreeze, checado por movie_check_after_load */
```

- [ ] **Step 4.2: Adicionar `#include "unistd.h"` para `ftruncate` (Unix)**

No bloco de includes de `movie.c`, após `#include "util.h"`:

```c
#ifndef _WIN32
#include <unistd.h>
#endif
```

- [ ] **Step 4.3: Implementar as 4 funções**

Adicionar ao final de `movie.c` (após `movie_update`):

```c
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
	uint32_t saved_frame_count = load_int32(buf);
	uint32_t saved_buffer_used = load_int32(buf);

	movie.freeze_seen = 1;

	if (movie.state != BSM_STATE_RECORD) {
		/* Não gravando: ignorar dados. load_section já avança o cursor do pai. */
		return;
	}

	if (saved_buffer_used > movie.input_buffer_cap) {
		saved_buffer_used = movie.input_buffer_cap;
	}
	load_buffer8(buf, (uint8_t *)movie.input_buffer,
	             saved_buffer_used * sizeof(bsm_frame_input));

	/* Flush do buffer pendente antes de truncar o arquivo */
	flush_inputs();

	/* Truncar o .bsm no disco até o frame do save state */
	uint32_t trunc_pos = movie.header.input_offset +
	                     saved_frame_count * sizeof(bsm_frame_input);
#ifdef _WIN32
	_chsize(fileno(movie.file), trunc_pos);
#else
	ftruncate(fileno(movie.file), trunc_pos);
#endif
	fseek(movie.file, trunc_pos, SEEK_SET);

	movie.input_buffer_used     = saved_buffer_used;
	movie.header.frame_count    = saved_frame_count;
	movie.header.rerecord_count++;

	bsm_write_header(movie.file, &movie.header);
	debug_message("Movie rerecord: truncado para frame %u (rerecord_count=%u)\n",
	              saved_frame_count, movie.header.rerecord_count);
}
```

- [ ] **Step 4.4: Build e rodar testes**

```bash
cd /Users/afonsof/Projects/retro/blastem && make testmovie && ./testmovie
```
Esperado:
```
test_header_roundtrip: PASSED
test_bad_magic: PASSED
test_check_noop_when_not_recording: PASSED
test_stop_on_missing_section: PASSED
test_freeze_writes_section: PASSED
All tests passed.
```

- [ ] **Step 4.5: Commit**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add movie.c
git commit -m "feat(movie): implement re-recording API

- movie_freeze: embute frame_count + input buffer em SECTION_MOVIE
- movie_unfreeze: restaura estado e trunca .bsm ao carregar save state
- movie_prepare_for_load / movie_check_after_load: detectam load sem movie data

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 5: Hook `movie_freeze` em `genesis_serialize`

**Files:**
- Modify: `genesis.c` (linha 132–136, final de `genesis_serialize`)

- [ ] **Step 5.1: Adicionar chamada ao final de `genesis_serialize`**

Localizar o final da função `genesis_serialize` em `genesis.c` (linha ~132–136):

```c
	if (gen->expansion) {
		segacd_context *cd = gen->expansion;
		segacd_serialize(cd, buf, all);
	}
}
```

Substituir por:

```c
	if (gen->expansion) {
		segacd_context *cd = gen->expansion;
		segacd_serialize(cd, buf, all);
	}
#ifndef IS_LIB
	movie_freeze(buf);
#endif
}
```

- [ ] **Step 5.2: Build**

```bash
cd /Users/afonsof/Projects/retro/blastem && make blastem 2>&1 | tail -3
```
Esperado: sem erros.

- [ ] **Step 5.3: Confirmar que testmovie ainda passa**

```bash
cd /Users/afonsof/Projects/retro/blastem && make testmovie && ./testmovie
```
Esperado: `All tests passed.`

- [ ] **Step 5.4: Commit**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add genesis.c
git commit -m "feat(genesis): hook movie_freeze at end of genesis_serialize

Writes SECTION_MOVIE into save state when recording is active.

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 6: Hook `movie_prepare_for_load`, handler e `movie_check_after_load` em `genesis_deserialize`

**Files:**
- Modify: `genesis.c` (função `genesis_deserialize`, linhas ~234–299)

- [ ] **Step 6.1: Adicionar `movie_prepare_for_load` e registrar o handler**

Na função `genesis_deserialize` (linha ~234), após todos os `register_section_handler` existentes e antes do `while (buf->cur_pos < buf->size)` loop (~linha 260), adicionar:

```c
#ifndef IS_LIB
	movie_prepare_for_load();
	register_section_handler(buf,
		(section_handler){.fun = movie_unfreeze, .data = gen},
		SECTION_MOVIE);
#endif
```

O bloco deve ficar assim após a mudança:

```c
	if (gen->expansion) {
		segacd_context *cd = gen->expansion;
		segacd_register_section_handlers(cd, buf);
	}
#ifndef IS_LIB
	movie_prepare_for_load();
	register_section_handler(buf,
		(section_handler){.fun = movie_unfreeze, .data = gen},
		SECTION_MOVIE);
#endif
	uint8_t tmss_old = gen->tmss;
```

- [ ] **Step 6.2: Adicionar `movie_check_after_load` após o loop de load_section**

Logo após o `while (buf->cur_pos < buf->size) { load_section(buf); }` (~linha 262), adicionar:

```c
#ifndef IS_LIB
	movie_check_after_load();
#endif
```

O bloco fica:

```c
	while (buf->cur_pos < buf->size)
	{
		load_section(buf);
	}
#ifndef IS_LIB
	movie_check_after_load();
#endif
	if (gen->header.type == SYSTEM_GENESIS && (gen->version_reg & 0xF)) {
```

- [ ] **Step 6.3: Build**

```bash
cd /Users/afonsof/Projects/retro/blastem && make blastem 2>&1 | tail -3
```
Esperado: sem erros.

- [ ] **Step 6.4: Confirmar que testmovie ainda passa**

```bash
cd /Users/afonsof/Projects/retro/blastem && make testmovie && ./testmovie
```
Esperado: `All tests passed.`

- [ ] **Step 6.5: Commit**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add genesis.c
git commit -m "feat(genesis): hook movie re-recording into genesis_deserialize

- movie_prepare_for_load resets detection flag before each load
- movie_unfreeze registered as SECTION_MOVIE handler
- movie_check_after_load stops recording if section was absent

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Task 7: Validação End-to-End

**Files:** nenhum novo — execução e verificação.

- [ ] **Step 7.1: Build limpo completo**

```bash
cd /Users/afonsof/Projects/retro/blastem && make clean && make 2>&1 | tail -5
```
Esperado: sem erros.

- [ ] **Step 7.2: Jogar, salvar, retroceder e verificar**

```bash
# Iniciar gravação
./blastem -R /tmp/rerecord_test.bsm /Users/afonsof/Projects/retro/ltdk-sgdk/out/genesis/release/rom.bin
```

Dentro do emulador:
1. Jogar ~5 segundos (≈300 frames)
2. Pressionar a tecla de Quick Save (padrão: `F5` ou binding de `ui.save_state`)
3. Jogar mais ~3 segundos (≈180 frames, total ≈480)
4. Pressionar Quick Load (`F8` ou `ui.load_state`)
5. Jogar mais ~2 segundos (≈120 frames)
6. Fechar a janela

- [ ] **Step 7.3: Inspecionar o arquivo**

```bash
python3 - << 'EOF'
import struct, os

path = '/tmp/rerecord_test.bsm'
with open(path, 'rb') as f:
    data = f.read(64)

_, _, _, rerecord, frames = struct.unpack_from('<IIIII', data, 0)
inp_off = struct.unpack_from('<I', data, 48)[0]
actual = os.path.getsize(path)
expected = inp_off + frames * 4

print(f"Frames:        {frames}  (esperado ~420: 300+120, não 600)")
print(f"Rerecord:      {rerecord}  (esperado 1)")
print(f"Consistência:  {'OK' if actual == expected else 'MISMATCH'}")
EOF
```

Esperado:
- `Frames` ≈ 420 (não ≈ 600 — os 180 frames descartados não aparecem)
- `Rerecord: 1`
- `Consistência: OK`

- [ ] **Step 7.4: Commit de validação**

```bash
cd /Users/afonsof/Projects/retro/blastem
git commit --allow-empty -m "chore: sub-epic 2 end-to-end validation passed

Load state durante gravação trunca timeline corretamente.
rerecord_count incrementado, tamanho do arquivo consistente.

Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
```

---

## Checklist de Cobertura do Spec

| Requisito do Spec | Tarefa |
|---|---|
| `SECTION_MOVIE` adicionado a `serialize.h` | Task 1 |
| `movie_freeze` / `movie_unfreeze` / `movie_prepare_for_load` / `movie_check_after_load` declarados | Task 2 |
| `freeze_seen` field em `bsm_movie` | Task 4 |
| `ftruncate` com guarda `#ifdef _WIN32` | Task 4 |
| `movie_freeze` hookado em `genesis_serialize` | Task 5 |
| Handler + prepare + check hookados em `genesis_deserialize` | Task 6 |
| Testes: check no-op, stop on missing, freeze writes section | Task 3–4 |
| Validação: frame_count truncado, rerecord_count=1, tamanho consistente | Task 7 |
