# Épico: Gravação de Gameplay e Exportação de Vídeo

**Data:** 2026-06-13  
**Status:** Spec aprovado — aguardando planos de implementação por sub-épico

---

## Visão Geral

Implementar um sistema completo de gravação determinística de gameplay no BlastEm, composto de três capacidades:

1. **Record**: gravar os inputs do jogador frame a frame durante a jogatina.
2. **Replay**: reproduzir a gravação dentro do emulador de forma idêntica à sessão original.
3. **Export**: renderizar a gravação para um arquivo de vídeo (MP4 ou similar) com áudio.

O sistema deve integrar-se ao mecanismo de save states existente, de modo que carregar um save state durante uma gravação **trunca** a timeline de inputs naquele ponto e continua gravando a partir daí, descartando a parte "desfeita".

---

## Referência Técnica

O Snes9x (formato `.smv`, `movie.cpp`) foi usado como referência principal. Pontos relevantes observados:

- Inputs são gravados por amostra (`BytesPerSample` × estado dos controladores por frame).
- O arquivo de movie contém: **header** + **save state inicial** + **buffer de inputs**.
- Ao carregar um save state durante gravação (`S9xMovieUnfreeze`), o buffer é truncado até `CurrentFrame` e `RerecordCount` é incrementado; o estado retorna a `MOVIE_STATE_RECORD`.
- Export de vídeo (AVI) é feito via `AVIAddVideoFrame` / `AVIAddSoundSamples` na plataforma Win32; usaremos FFmpeg via `libavcodec`/`libavformat` para portabilidade no BlastEm.

---

## Contexto do BlastEm Relevante

| Componente | Arquivo | Papel |
|---|---|---|
| Save states | `saves.c`, `serialize.c` | Serialização/deserialização de estado completo da máquina |
| Input | `io.c` | `io_gamepad_down/up`, estado dos ports por frame |
| Loop principal | `render_sdl.c` | Processa eventos SDL e chama bindings |
| Bindings | `bindings.c` | Mapeia eventos a ações de gamepad/UI |
| Áudio | `render_audio.c`, `ym2612.c`, `psg.c` | Samples de áudio por frame |
| Sistema geral | `genesis.c`, `system.h` | `system_header` com hooks de save_state, vgm_logging |
| Event log | `event_log.h` | Infraestrutura de log de eventos (referência para extensão) |

---

## Arquitetura do Sistema

```
┌─────────────────────────────────────────────────────────┐
│                      BlastEm Runtime                    │
│                                                         │
│  render_sdl.c ──► bindings.c ──► io.c                  │
│       │                              │                  │
│       │                    [io_gamepad_down/up]         │
│       │                              │                  │
│       ▼                              ▼                  │
│  [movie_hook]──────────────►  movie_record.c            │
│       │                         │         │             │
│  save state trigger         buffer      .bsm file       │
│       │                      inputs      (disco)        │
│       ▼                         │                       │
│  saves.c ──────────────────► truncate_at_frame()        │
│                                                         │
│  Playback:                                              │
│  movie_record.c ──► io.c (injeta inputs gravados)       │
│                                                         │
│  Export:                                                │
│  playback loop + frame capture ──► ffmpeg encode ──► MP4│
└─────────────────────────────────────────────────────────┘
```

---

## Formato de Arquivo de Movie (`.bsm` — BlastEm Save Movie)

Formato binário próprio, inspirado no SMV do Snes9x, mas adaptado ao Mega Drive:

| Seção | Conteúdo |
|---|---|
| **Header** (64 bytes fixos) | Magic (`BSM\x1a`), versão, flags, frame count, rerecord count, CRC32 da ROM, nome da ROM, offset do save state, offset dos inputs |
| **Save State** | Estado completo da máquina no início da gravação (formato existente do BlastEm) |
| **Input Buffer** | Array de frames: `[frame_n: pad1_buttons (2 bytes) + pad2_buttons (2 bytes)]` |

### Flags
- `BSM_OPT_FROM_RESET` — gravação inicia do reset, sem save state embutido
- `BSM_OPT_PAL` — sistema em modo PAL (50 Hz)

---

## Sub-épicos (implementação em fases)

Cada sub-épico terá seu próprio documento de spec e plano de implementação.

### Sub-épico 1 — Infraestrutura de Gravação de Inputs
**Escopo:** estrutura `BsmMovie`, captura de inputs por frame (`movie_update` chamado a cada frame no loop genesis), escrita do buffer em arquivo `.bsm`.  
**Critério de aceite:** jogar uma ROM e gerar um `.bsm` válido com os inputs corretos.

### Sub-épico 2 — Integração com Save States (Re-recording)
**Escopo:** hookar `saves.c` para que, ao carregar um save state enquanto `BSM_STATE_RECORD` está ativo, a timeline de inputs seja truncada naquele frame e `RerecordCount` seja incrementado. Ao salvar um save state durante gravação, embeddar snapshot da posição do buffer.  
**Critério de aceite:** jogar, voltar num save state, continuar jogando — o `.bsm` final reproduz apenas o caminho "final" sem os segmentos descartados.

### Sub-épico 3 — Reprodução (Playback)
**Escopo:** abrir um `.bsm`, restaurar o save state inicial, e a cada frame injetar os inputs gravados no lugar dos inputs reais do jogador. Modo `BSM_STATE_PLAY` no loop de io.  
**Critério de aceite:** abrir um `.bsm` e a reprodução ser frame-a-frame idêntica à gravação original.

### Sub-épico 4 — Exportação de Vídeo
**Escopo:** modo headless de playback que captura frames do framebuffer VDP e samples de áudio, e os passa para FFmpeg (`libavformat`/`libavcodec`) para gerar um MP4. Framerate fixo de 60 fps (NTSC) ou 50 fps (PAL).  
**Critério de aceite:** `./blastem --export-movie gravacao.bsm saida.mp4` gera um MP4 assistível, sincronizado, com qualidade razoável.

### Sub-épico 5 — UI e UX
**Escopo:** expor as funções de record/play/export via menu Nuklear existente e via CLI flags (`--record`, `--play`, `--export-movie`).  
**Critério de aceite:** usuário consegue iniciar gravação, parar, abrir replay e exportar vídeo sem precisar de linha de comando.

---

## Decisões de Design

| Decisão | Escolha | Motivo |
|---|---|---|
| Formato de arquivo | `.bsm` próprio | Controle total, adaptado ao MD de 2 ports × 6/3 botões |
| Granularidade de captura | Por frame (não por evento SDL) | Garante determinismo independente de timing de host |
| Biblioteca de vídeo | FFmpeg (`libavformat`) | Portável em macOS/Linux, suporta MP4/H.264 |
| Save state no movie | Sempre embedded no início | Permite reprodução sem depender de ROM específica carregada |
| Re-recording | Truncar buffer + incrementar contador | Idêntico ao SMV do Snes9x; comportamento claro e simples |

---

## Não está no escopo deste épico

- Suporte a múltiplos branches de timeline (árvore de savestates)
- Edição de inputs frame a frame (TAS editor)
- Streaming em tempo real
- Suporte a periféricos além de gamepad de 6 botões (mouse, lightgun, etc.)
