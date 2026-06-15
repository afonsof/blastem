# Sub-épico 3: Reprodução (Playback)

**Épico pai:** [2026-06-13-gameplay-recording-epic.md](./2026-06-13-gameplay-recording-epic.md)  
**Data:** 2026-06-15  
**Status:** Spec — aguardando plano de implementação  
**Referência:** Snes9x `movie.cpp` (`S9xMovieOpen`, `S9xMovieUpdate`, `MovieSetJoypad` em `controls.cpp`)

---

## Objetivo

Abrir um arquivo `.bsm` gravado pelo sub-épico 1, restaurar o save state inicial embutido e, a cada frame, injetar os inputs gravados no lugar dos inputs reais do jogador. Ao esgotar o buffer de inputs, o emulador continua rodando normalmente com os inputs reais ("vira live").

```bash
./blastem --play gravacao.bsm jogo.bin
```

---

## Referência Técnica — Snes9x

O Snes9x (`movie.cpp` + `controls.cpp`) usa o mesmo padrão desta spec:

| Passo | Snes9x | BlastEm (este sub-épico) |
|-------|--------|--------------------------|
| Abrir arquivo | `S9xMovieOpen` | `movie_play_start` |
| Restaurar estado | `S9xUnfreezeFromStream` | `system->deserialize` |
| Carregar inputs em RAM | `fread` de todo `InputBuffer` | `fread` de todo `input_buffer` |
| Injetar por frame | `MovieSetJoypad(i, Read16(ptr))` | `io_port_set_pad_state(port, frame->pad)` |
| Fim do buffer | `change_state(MOVIE_STATE_NONE)` | `movie.state = BSM_STATE_NONE` |

---

## Contexto do BlastEm Relevante

| Componente | Arquivo | Papel |
|---|---|---|
| Movie state | `movie.c` / `movie.h` | Sub-épico 1: struct `bsm_movie`, `movie_update`, `BSM_STATE_*` |
| Re-recording | `movie.c` / `movie.h` | Sub-épico 2: `movie_freeze/unfreeze`, `play_frame` ainda não existe |
| IO | `io.c` / `io.h` | `io_read_pad_buttons`, `io_port_gamepad_down/up`, `find_gamepad` |
| Sistema | `system.h` | `system->serialize` (retorna `uint8_t*`) e `system->deserialize` (recebe `uint8_t*, size_t`) |
| Loop principal | `genesis.c` | `movie_update(&gen->header)` já é chamado a cada frame |
| CLI | `blastem.c` | Ponto de entrada para `--play` |

---

## Arquitetura

```
movie_play_start(system, filename)
        │
        ├─ bsm_read_header()          valida magic + version
        ├─ fread save state bytes     do savestate_offset + 4
        ├─ system->deserialize()      restaura máquina ao estado inicial
        ├─ fread input_buffer         todos frame_count × 4 bytes em RAM
        └─ state = BSM_STATE_PLAY, play_frame = 0

        ┌─────── a cada frame (movie_update já hookado em genesis.c) ───────┐
        │                                                                    │
        │  BSM_STATE_PLAY                                                    │
        │    if play_frame >= frame_count → BSM_STATE_NONE (vira live)      │
        │    else:                                                            │
        │      frame = input_buffer[play_frame]                             │
        │      io_port_set_pad_state(port1, frame.pad1)                     │
        │      io_port_set_pad_state(port2, frame.pad2)                     │
        │      play_frame++                                                  │
        └────────────────────────────────────────────────────────────────────┘
```

---

## Nova Função: `io_port_set_pad_state`

Simétrica ao `io_read_pad_buttons` existente. Escreve o bitmask inteiro no pad usando a API existente de `io_port_gamepad_down/up`, sem duplicar a lógica interna do `input[]` array.

### Declaração (`io.h`)

```c
/* Define o estado completo de um pad a partir de um bitmask.
 * Simétrico a io_read_pad_buttons. Usado pelo playback de movie. */
void io_port_set_pad_state(io_port *port, uint16_t buttons);
```

### Implementação (`io.c`)

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

> **Nota:** internamente delega para `io_port_gamepad_down/up` — não há acoplamento ao layout interno dos campos `input[]` de `io_port`. Qualquer refatoração futura do IO não quebra o playback.

---

## Novos Símbolos em `movie.h` / `movie.c`

### Campo interno adicional em `bsm_movie`

```c
uint32_t play_frame;   /* índice do próximo frame a injetar durante BSM_STATE_PLAY */
```

### API pública (`movie.h`)

```c
/* Abre filename, restaura save state inicial e prepara playback.
 * Retorna 0 em sucesso, não-zero em erro (arquivo inválido, CRC errado, etc). */
int movie_play_start(system_header *system, const char *filename);

/* Para o playback imediatamente. Safe to call when not playing. */
void movie_play_stop(void);

/* Retorna o índice do frame atualmente sendo reproduzido.
 * Retorna 0 se não estiver em playback. */
uint32_t movie_get_play_frame(void);
```

---

## Implementação de `movie_play_start`

```c
int movie_play_start(system_header *system, const char *filename)
{
    if (movie.state != BSM_STATE_NONE)
        movie_record_stop();   /* para gravação se estiver ativa */

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

    /* Verificação de ROM — warning apenas, não bloqueia */
    uint32_t rom_crc = (uint32_t)crc32(0,
        (const Bytef *)system->info.rom, system->info.rom_size);
    if (h.rom_crc32 != rom_crc) {
        warning("movie_play_start: ROM CRC mismatch "
                "(movie=0x%08x current=0x%08x) — proceeding anyway\n",
                h.rom_crc32, rom_crc);
    }

    /* Ler save state embutido */
    fseek(f, h.savestate_offset, SEEK_SET);
    uint32_t ss_size;
    fread(&ss_size, 4, 1, f);
    uint8_t *ss_buf = malloc(ss_size);
    if (!ss_buf || fread(ss_buf, 1, ss_size, f) != ss_size) {
        warning("movie_play_start: failed to read embedded save state\n");
        free(ss_buf);
        fclose(f);
        return -1;
    }
    system->deserialize(system, ss_buf, ss_size);
    free(ss_buf);

    /* Carregar todo o buffer de inputs em RAM */
    uint32_t input_bytes = h.frame_count * sizeof(bsm_frame_input);
    if (!movie.input_buffer || movie.input_buffer_cap < h.frame_count) {
        free(movie.input_buffer);
        movie.input_buffer = malloc(input_bytes);
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

---

## Implementação de `movie_play_stop`

```c
void movie_play_stop(void)
{
    if (movie.state != BSM_STATE_PLAY)
        return;
    movie.state      = BSM_STATE_NONE;
    movie.play_frame = 0;
    debug_message("Movie playback stopped\n");
}
```

---

## Extensão de `movie_update`

`movie_update` em `genesis.c` já é chamado a cada frame. Adicionar o case de PLAY antes do early-return existente de RECORD:

```c
void movie_update(system_header *system)
{
    if (system->type != SYSTEM_GENESIS && system->type != SYSTEM_SEGACD)
        return;

    genesis_context *gen = (genesis_context *)system;

    if (movie.state == BSM_STATE_PLAY) {
        if (movie.play_frame >= movie.header.frame_count) {
            /* Buffer esgotado: vira live */
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

    if (movie.state != BSM_STATE_RECORD)
        return;

    /* ... lógica de record existente ... */
}
```

---

## Integração com CLI (`blastem.c`)

```c
if (strcmp(argv[i], "--play") == 0 && i + 1 < argc) {
    movie_play_start(system, argv[++i]);
}
```

---

## Teste (`testmovie.c`)

### `test_playback_roundtrip`

Verifica que gravar e depois reproduzir produz exatamente a mesma sequência de inputs:

1. Iniciar gravação em modo record (sem arquivo real — usar buffer em memória via arquivo temporário)
2. Simular 30 frames com inputs conhecidos (alternando botões predefinidos)
3. Chamar `movie_record_stop` → obter um `.bsm` em arquivo temporário
4. Ler o header do arquivo e verificar `frame_count == 30`
5. Ler o `input_buffer` do arquivo e comparar byte a byte com os inputs originais
6. Verificar que `play_frame` avança corretamente simulando o loop de `movie_update`

> **Nota:** o teste não instancia um sistema real, portanto não testa a restauração do save state. Essa verificação fica no critério de aceite end-to-end.

---

## Arquivos Modificados

| Arquivo | Tipo | Mudança |
|---------|------|---------|
| `io.h` | Modificar | Declarar `io_port_set_pad_state` |
| `io.c` | Modificar | Implementar `io_port_set_pad_state` |
| `movie.h` | Modificar | Declarar `movie_play_start`, `movie_play_stop`, `movie_get_play_frame` |
| `movie.c` | Modificar | Implementar as 3 funções; adicionar `play_frame` a `bsm_movie`; estender `movie_update` com case PLAY |
| `blastem.c` | Modificar | Parsear `--play <arquivo>`, chamar `movie_play_start` |
| `testmovie.c` | Modificar | Adicionar `test_playback_roundtrip` |

---

## Critério de Aceite

1. `./blastem --play gravacao.bsm jogo.bin` abre o emulador, restaura o save state e começa a reprodução sem crash.
2. Gravar com `--record`, gerar `gravacao.bsm`, abrir com `--play` — a reprodução é frame-a-frame idêntica à gravação original (mesmas telas, mesmos movimentos).
3. Ao chegar no último frame gravado, o emulador continua rodando normalmente aceitando inputs do jogador.
4. Se o arquivo `.bsm` for inválido (magic errado), `movie_play_start` retorna não-zero sem crash.
5. Se o CRC da ROM for diferente do gravado, o emulador emite warning mas tenta reproduzir.
6. `test_playback_roundtrip` passa sem falhas.

---

## Fora do Escopo

- Exportação de vídeo headless (Sub-épico 4)
- UI no menu Nuklear para abrir `.bsm` (Sub-épico 5)
- Modo read-only vs read-write (conceito do Snes9x — não aplicável aqui pois playback não modifica o arquivo)
- Suporte a periféricos além de gamepad de 6 botões
