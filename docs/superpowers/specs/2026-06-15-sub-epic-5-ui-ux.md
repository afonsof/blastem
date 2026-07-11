# Sub-Epic 5 — UI e UX para Gravação de Gameplay

**Data:** 2026-06-15  
**Status:** Spec aprovado — aguardando plano de implementação  
**Referência:** Snes9x (`S9xMovieToggleRecState`, `S9xMovieOpen`, `S9xMovieStop`)

---

## Visão Geral

Expor as funções de record/play/export construídas nos sub-épicos 1-4 através do menu Nuklear existente e confirmar as flags CLI. O usuário deve conseguir gravar, reproduzir e exportar vídeo sem usar linha de comando.

---

## O que já existe

| Funcionalidade | API | CLI | Keybinding | Menu |
|---|---|---|---|---|
| Gravar inputs | `movie_record_start/stop` | `-R FILE` | `ui.movie_record` (default `r`) | ❌ |
| Reproduzir .bsm | `movie_play_start/stop` | `-P FILE` | ❌ | ❌ |
| Exportar vídeo | `movie_export_start` (sub-epic 4) | ❌ | ❌ | ❌ |
| Estado recording/play | `movie_get_state()` | — | — | ❌ |

---

## Requisitos

### R1: Menu Item — "Record Movie" (toggle)

Adicionar item **"Record Movie"** ao `view_pause`. Comportamento:

- Se **não está gravando**: ao selecionar, abre file browser para escolher onde salvar o `.bsm`. Após confirmar, chama `movie_record_start()` e retorna ao jogo.
- Se **está gravando**: o item muda para **"Stop Recording"**. Ao selecionar, chama `movie_record_stop()` e volta ao label original.

O file browser deve filtrar por extensão `.bsm` no diálogo de save.

**Critério de aceite:** usuário abre o menu, seleciona "Record Movie", escolhe um path, joga, reabre o menu e seleciona "Stop Recording" — o `.bsm` é gerado corretamente e reproduzível.

### R2: Menu Item — "Play Movie"

Adicionar item **"Play Movie"** ao `view_pause`. Comportamento:

- Se **não está reproduzindo**: ao selecionar, abre file browser para escolher um `.bsm`. Após confirmar, chama `movie_play_start()` + `movie_play_pre_inject()` e retorna ao jogo.
- Se **está reproduzindo**: o item muda para **"Stop Playback"**. Ao selecionar, chama `movie_play_stop()`.

Se `movie_play_start` falhar (arquivo inválido, ROM errada), mostrar warning e voltar ao menu.

O file browser deve filtrar por extensão `.bsm`.

**Critério de aceite:** usuário abre o menu, seleciona "Play Movie", escolhe um `.bsm`, a reprodução inicia e roda frame a frame. Ao reabrir o menu e selecionar "Stop Playback", para e o jogo continua live.

### R3: Menu Item — "Export Movie" (sub-epic 4)

Adicionar item **"Export Movie"** ao `view_pause`. Comportamento:

- Abre file browser para escolher um `.bsm` de entrada.
- Depois abre file browser para escolher o path do `.mp4` de saída.
- Chama `movie_export_start(system, bsm_path, output_path)`.
- Se a exportação falhar ou ffmpeg não estiver no PATH, mostra warning.

Este item só deve aparecer se compilado com suporte a exportação de vídeo (`#ifndef DISABLE_MOVIE_EXPORT` ou similar). Se sub-epic 4 não estiver completo, pode ficar como placeholder desabilitado.

**Critério de aceite:** usuário seleciona "Export Movie", escolhe `.bsm` de entrada, escolhe `.mp4` de saída, e o MP4 é gerado com sucesso.

### R4: Indicador visual de estado

Quando o emulador está gravando ou reproduzindo, mostrar indicador textual no canto da tela durante o jogo (não no menu):

- Gravação: "● REC" em vermelho no canto superior direito, com o frame count atual.
- Reprodução: "▶ PLAY" em verde no canto superior direito, com `play_frame / frame_count`.
- Quando não está nem gravando nem reproduzindo: não mostrar nada.

Usar o sistema de renderização de texto existente (não Nuklear — deve ser visível durante o jogo).

Flag de config opcional para desabilitar: `ui\0movie_osd\0` = "off".

**Critério de aceite:** durante gravação, "● REC" visível no canto. Durante playback, "▶ PLAY 123/6396" visível. Parou, some.

### R5: CLI flags consistentes

As flags CLI já existem (`-R`, `-P`). Verificar que:

- `-h` lista todas as flags de movie (`-R`, `-P`).
- `--record`, `--play` (long form) são reconhecidos como aliases.
- Se `--export-movie` for implementado (sub-epic 4), adicionar `-E` e `--export-movie`.

**Critério de aceite:** `./blastem -h` mostra `-R`, `-P` e `-E` (se export implementado).

### R6: Keybindings no default.cfg

Já existe `r ui.movie_record`. Verificar que:

- O binding funciona corretamente (toggle record).
- Não há conflitos com outros bindings.

**Critério de aceite:** apertar `r` durante o jogo inicia/para gravação; o OSD aparece/desaparece.

---

## Decisões de Design

| Decisão | Escolha | Motivo |
|---|---|---|
| Menu items como toggle | Record vira "Stop Recording" quando ativo | Padrão Snes9x (`S9xMovieToggleRecState`); evita poluir o menu |
| File browser para escolher paths | Reuso do `view_file_browser` existente | Já implementado no Nuklear, filtro por extensão |
| OSD durante o jogo | Texto renderizado via `render_video_loop` | Precisa ser visível fora do menu; Nuklear não renderiza durante gameplay |
| Export no menu | File browser duplo (entrada .bsm, saída .mp4) | UX mais simples que digitar paths |
| Config OSD | `ui.movie_osd = "off"` para desabilitar | Consistente com convenções existentes (`ui.style`, etc.) |

---

## Não está no escopo

- Diálogo de confirmação "deseja sobrescrever?"
- Barra de progresso durante exportação
- Thumbnail ou preview do .bsm no file browser
- Edição de metadata do movie (nome do autor, descrição)
- Menu "Recent Movies" ou histórico de arquivos
- Suporte a path via teclado no file browser (só seleção por lista)
