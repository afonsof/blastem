# Sub-épico 1: Infraestrutura de Gravação de Inputs

**Épico pai:** [2026-06-13-gameplay-recording-epic.md](./2026-06-13-gameplay-recording-epic.md)  
**Data:** 2026-06-13  
**Status:** Spec — aguardando plano de implementação

---

## Objetivo

Implementar a capacidade de gravar os inputs do jogador frame a frame durante uma sessão de jogo e persistir essa gravação em um arquivo `.bsm` no disco.

Ao final deste sub-épico, deve ser possível:

```bash
./blastem --record gravacao.bsm jogo.bin
```

…jogar normalmente, pressionar stop (keybinding ou menu), e obter um arquivo `.bsm` válido com os inputs de cada frame e o estado inicial da máquina.

---

## Contexto do Código Existente

### Onde os inputs vivem

O estado dos botões é mantido em `io_port.device.pad` (ver `io.h`). As funções `io_port_gamepad_down/up` e `io_gamepad_down/up` em `io.c` atualizam bits nos arrays `input[]` da porta. O `system_header` expõe `gamepad_down` / `gamepad_up` como function pointers que apontam para as implementações de cada sistema.

### Onde fica o frame boundary

Em `genesis.c`, a função `genesis_frame` (chamada pelo loop principal via `m68k_sync`) detecta o fim de frame via `vdp_cycles_to_frame_end`. O ponto exato para chamar `movie_update()` é logo após `gen->frame_end = vdp_cycles_to_frame_end(v_context)` ser calculado e o frame ser renderizado — o mesmo lugar onde o VGM log e o event log são flushed.

### Serialização existente

`serialize.c` / `serialize.h` fornecem `serialize_buffer` com `save_int8/16/32`, `save_buffer8`, `save_to_file`. A função `genesis_serialize` em `genesis.c` serializa o estado completo da máquina. Vamos reutilizar ambas.

### Infraestrutura de event log

`event_log.c` já tem a estrutura de compressão zlib para streams de eventos. Não vamos reutilizar o formato diretamente (é orientado a eventos de hardware, não a inputs por frame), mas o padrão de `serialize_buffer` + flush para arquivo é a referência.

---

## Formato do Arquivo `.bsm`

### Header (64 bytes fixos)

| Offset | Tamanho | Campo | Descrição |
|--------|---------|-------|-----------|
| 0 | 4 | `magic` | `BSM\x1a` |
| 4 | 4 | `version` | Versão do formato (atualmente `1`) |
| 8 | 4 | `movie_id` | ID único gerado no momento da criação |
| 12 | 4 | `rerecord_count` | Número de vezes que um save state foi carregado durante a gravação |
| 16 | 4 | `frame_count` | Total de frames gravados |
| 20 | 1 | `flags` | Ver flags abaixo |
| 21 | 1 | `pad_mask` | Bitmask dos gamepads ativos (bit 0 = pad1, bit 1 = pad2) |
| 22 | 2 | `reserved` | Reservado, deve ser zero |
| 24 | 4 | `rom_crc32` | CRC32 da ROM |
| 28 | 16 | `rom_name` | Nome da ROM (ASCII, null-padded) |
| 44 | 4 | `savestate_offset` | Offset em bytes do save state dentro do arquivo |
| 48 | 4 | `input_offset` | Offset em bytes do início do buffer de inputs |
| 52 | 12 | `reserved2` | Reservado para uso futuro |

**Flags:**
- `BSM_FLAG_FROM_RESET (1 << 0)` — gravação inicia do hard reset; save state não está presente
- `BSM_FLAG_PAL (1 << 1)` — sistema em modo PAL (50 Hz)

### Save State

- Imediatamente após o header (em `savestate_offset`)
- Formato nativo do BlastEm: `serialize_buffer` produzido por `genesis_serialize`
- Precedido por 4 bytes indicando o tamanho em bytes do bloco

### Buffer de Inputs

- Começa em `input_offset`
- Array de frames, cada frame com **4 bytes**:

```c
typedef struct {
    uint16_t pad1;  // bitmask dos botões do pad 1
    uint16_t pad2;  // bitmask dos botões do pad 2
} bsm_frame_input;
```

**Bitmask dos botões** (mesmo enum que `io.h`):

| Bit | Botão |
|-----|-------|
| 0 | `DPAD_UP` |
| 1 | `DPAD_DOWN` |
| 2 | `DPAD_LEFT` |
| 3 | `DPAD_RIGHT` |
| 4 | `BUTTON_A` |
| 5 | `BUTTON_B` |
| 6 | `BUTTON_C` |
| 7 | `BUTTON_START` |
| 8 | `BUTTON_X` |
| 9 | `BUTTON_Y` |
| 10 | `BUTTON_Z` |
| 11 | `BUTTON_MODE` |
| 12–15 | reservado |

---

## Novos Arquivos

### `movie.h`

Declara a API pública do módulo de gravação:

```c
typedef enum {
    BSM_STATE_NONE = 0,
    BSM_STATE_RECORD,
    BSM_STATE_PLAY       // usado nos sub-épicos 2 e 3
} bsm_state;

// Ciclo de vida
int  movie_record_start(system_header *system, const char *filename);
void movie_record_stop(void);
bsm_state movie_get_state(void);

// Chamado a cada frame pelo loop do emulador
void movie_update(system_header *system);

// Lê o estado atual dos botões de um pad (para snapshot por frame)
uint16_t movie_get_pad_state(system_header *system, uint8_t pad_num);
```

### `movie.c`

Implementa a struct interna:

```c
typedef struct {
    bsm_state   state;
    FILE        *file;
    char        filename[PATH_MAX + 1];
    uint32_t    movie_id;
    uint32_t    frame_count;
    uint32_t    rerecord_count;
    uint32_t    savestate_offset;
    uint32_t    input_offset;
    uint8_t     flags;
    uint8_t     pad_mask;
    uint32_t    rom_crc32;
    char        rom_name[16];

    // buffer em memória, flushed periodicamente
    bsm_frame_input *input_buffer;
    uint32_t         input_buffer_size;   // frames alocados
    uint32_t         input_buffer_used;   // frames escritos
} bsm_movie;
```

---

## Lógica de `movie_update`

Chamado **uma vez por frame** no loop de `genesis.c`, após o frame ser completamente renderizado:

1. Se `state != BSM_STATE_RECORD`, retorna imediatamente.
2. Lê o estado atual dos botões via `movie_get_pad_state` para pad1 e pad2.
3. Escreve um `bsm_frame_input` no `input_buffer`.
4. Incrementa `frame_count`.
5. Se `input_buffer_used` atingir um threshold (ex.: 4096 frames), faz flush do buffer para o arquivo e reseta o ponteiro.

### `movie_get_pad_state`

Lê o bitmask atual dos botões direto da estrutura `io_port`:

```c
uint16_t movie_get_pad_state(system_header *system, uint8_t pad_num) {
    genesis_context *gen = (genesis_context *)system;
    io_port *port = find_gamepad(&gen->io, pad_num);
    if (!port) return 0;
    // reconstrói bitmask a partir do estado interno do port
    uint16_t state = 0;
    for (int btn = DPAD_UP; btn < NUM_GAMEPAD_BUTTONS; btn++) {
        if (/* botão btn está pressionado no port */)
            state |= (1 << (btn - DPAD_UP));
    }
    return state;
}
```

> **Nota de implementação:** o estado dos botões em `io_port` é codificado nos bits de `input[GAMEPAD_TH0]` e `input[GAMEPAD_TH1]` (ver `button_defs` em `io.c`). A função deve inverter essa lógica ou, preferencialmente, o módulo de IO deve expor uma função auxiliar `io_read_pad_buttons(io_port *port) -> uint16_t`.

---

## Ponto de Hook no Loop Principal

Em `genesis.c`, na função responsável pelo fim de frame (onde `gen->frame_end` é atribuído e o VDP é sincronizado), adicionar:

```c
#ifndef IS_LIB
    movie_update(&gen->header);
#endif
```

Isso garante que `movie_update` seja chamado exatamente uma vez por frame, no mesmo ritmo do emulador.

---

## Inicialização da Gravação (`movie_record_start`)

1. Serializar o estado atual da máquina via `genesis_serialize` → `state_buf`.
2. Calcular `savestate_offset = BSM_HEADER_SIZE`.
3. Calcular `input_offset = savestate_offset + 4 (tamanho) + state_buf.size`.
4. Abrir arquivo em `"wb"`.
5. Escrever header provisório (com `frame_count = 0`).
6. Escrever `state_buf.size` (uint32_t) + `state_buf.data`.
7. Setar `state = BSM_STATE_RECORD`.

---

## Finalização da Gravação (`movie_record_stop`)

1. Flush do `input_buffer` restante para o arquivo.
2. Reescrever o header no início do arquivo com `frame_count` final.
3. Fechar o arquivo.
4. Setar `state = BSM_STATE_NONE`.

---

## Integração com CLI

Em `blastem.c`, adicionar suporte ao flag `--record <arquivo>`:

```c
if (strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
    movie_record_start(system, argv[++i]);
}
```

A gravação é parada via keybinding (mapeável em `bindings.c`) ou automaticamente quando o emulador encerra.

---

## Arquivos Modificados

| Arquivo | Tipo | Mudança |
|---------|------|---------|
| `movie.h` | novo | API pública do módulo |
| `movie.c` | novo | Implementação completa |
| `io.c` / `io.h` | modificado | Adicionar `io_read_pad_buttons(io_port *) -> uint16_t` |
| `genesis.c` | modificado | Chamar `movie_update` no fim de cada frame |
| `blastem.c` | modificado | Parsear `--record`, chamar `movie_record_start/stop` |
| `bindings.c` | modificado | Adicionar ação `movie.record_stop` |
| `Makefile` | modificado | Incluir `movie.o` em `MAINOBJS` |

---

## Critério de Aceite

1. `./blastem --record saida.bsm jogo.bin` inicia o emulador em modo record sem crash.
2. Ao fechar o emulador (ou acionar `movie.record_stop`), o arquivo `saida.bsm` é criado.
3. O header do arquivo contém magic `BSM\x1a`, `frame_count > 0` e os offsets corretos.
4. O save state embutido pode ser deserializado com o mecanismo existente do BlastEm sem erros.
5. O buffer de inputs contém exatamente `frame_count` entradas de 4 bytes.
6. Jogar 60 segundos (≈3600 frames NTSC) gera um arquivo de tamanho esperado: 64 (header) + tamanho_save_state + 3600 × 4 bytes.

---

## Fora do Escopo deste Sub-épico

- Reprodução (playback) — Sub-épico 3
- Truncar buffer ao carregar save state (re-recording) — Sub-épico 2
- Exportação de vídeo — Sub-épico 4
- UI no menu Nuklear — Sub-épico 5
- Suporte a outros periféricos além de gamepad (mouse, lightgun)
