# Sub-épico 4: Exportação de Vídeo

**Épico pai:** [2026-06-13-gameplay-recording-epic.md](./2026-06-13-gameplay-recording-epic.md)  
**Data:** 2026-06-15  
**Status:** Spec — aguardando plano de implementação  

---

## Objetivo

Exportar um arquivo `.bsm` para vídeo MP4 (H.264) rodando o emulador em modo headless (sem janela), capturando o framebuffer VDP frame a frame e alimentando o ffmpeg CLI via pipe.

```bash
./blastem --export-movie gravacao.bsm saida.mp4
```

---

## Decisões de Design

| Decisão | Escolha | Motivo |
|---|---|---|
| FFmpeg | Pipe CLI (`popen`) | Zero dependência de build; CLI do ffmpeg é contrato público estável |
| Renderização | Framebuffer VDP em RAM (`render_get_framebuffer`) | Pixels já estão em system memory antes do upload GL; sem round-trip GPU |
| Loop | Headless, sem janela SDL | Export offline não precisa de display; roda o mais rápido possível |
| Áudio | Fora do escopo | Complexidade de sincronização YM2612/PSG postergada |
| Codec | H.264, CRF 18, preset medium | Qualidade quase-lossless com compressão razoável; hardcoded por simplicidade |
| Framerate | Detectado do header `.bsm` | `BSM_OPT_PAL` → 50 fps; default → 60 fps |

---

## Arquitetura

```
movie_export_start(system, bsm_path, output_path)
        │
        ├─ movie_play_start()           // Sub-épico 3: carrega .bsm, restaura save state
        ├─ popen("ffmpeg ... pipe:0")   // Abre pipe pra stdin do ffmpeg
        └─ movie_export_loop(system, pipe)
                │
                └─ for cada frame:
                     ├─ run_one_frame(system)              // Avança 1 frame (CPU + VDP + movie_update)
                     ├─ vdp_force_update_framebuffer()     // Garante framebuffer completo
                     ├─ fb = render_get_framebuffer()      // Captura pixels (RAM, via texture_buf)
                     └─ movie_export_write_frame(fb, pipe)  // ARGB→RGB24 → pipe
```

---

## Pipeline FFmpeg

```
[BlastEm headless] → raw RGB24 frames (stdin pipe) → ffmpeg → H.264 MP4
```

**Comando:**

```
ffmpeg -y -f rawvideo -vcodec rawvideo -s WxH -r FPS -pix_fmt rgb24 -i pipe:0 \
  -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p saida.mp4
```

---

## Novas Funções em `movie.h` / `movie.c`

### `movie_export_start`

```c
/* Exporta um .bsm para vídeo MP4 via ffmpeg CLI (pipe).
 * Roda o emulador em modo headless, sem abrir janela.
 * Retorna 0 em sucesso, não-zero em erro. */
int movie_export_start(system_header *system, const char *bsm_path, const char *output_path);
```

### `movie_export_write_frame` (interna)

```c
/* Converte framebuffer ARGB8888 para RGB24 e escreve no pipe do ffmpeg.
 * Retorna 0 em sucesso, -1 em erro de escrita. */
static int movie_export_write_frame(pixel_t *fb, int pitch,
                                     uint32_t w, uint32_t h, FILE *out);
```

---

## Implementação

### `movie_export_start`

```c
int movie_export_start(system_header *system, const char *bsm_path, const char *output_path)
{
    // 1. Inicia playback (sub-épico 3)
    if (movie_play_start(system, bsm_path) != 0) {
        warning("movie_export_start: failed to open %s for playback\n", bsm_path);
        return -1;
    }

    genesis_context *gen = (genesis_context *)system;

    // 2. Detecta framerate do header
    int fps = (movie.header.flags & BSM_OPT_PAL) ? 50 : 60;

    // 3. Detecta resolução
    uint32_t width  = gen->vdp.h40 ? 320 : 256;
    uint32_t height = gen->vdp.output_lines; // 224 NTSC, 240 PAL

    // 4. Abre pipe pro ffmpeg
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -f rawvideo -vcodec rawvideo "
        "-s %dx%d -r %d -pix_fmt rgb24 -i pipe:0 "
        "-c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p \"%s\"",
        width, height, fps, output_path);

    FILE *pipe = popen(cmd, "w");
    if (!pipe) {
        warning("movie_export_start: ffmpeg not found or failed to start: %s\n", strerror(errno));
        movie_play_stop();
        return -1;
    }

    // 5. Loop headless
    int ret = movie_export_loop(system, pipe);

    // 6. Finaliza
    int pclose_ret = pclose(pipe);
    if (pclose_ret != 0) {
        warning("movie_export_start: ffmpeg exited with status %d\n", pclose_ret);
        if (ret == 0) ret = -1;
    }

    movie_play_stop();
    return ret;
}
```

### `movie_export_loop`

O BlastEm não tem uma função única de "executar 1 frame". O loop de exportação precisa
orquestrar a execução da máquina e capturar o framebuffer quando o VDP sinalizar que
um frame completo foi renderizado. O mecanismo exato será definido no plano de
implementação, mas o pseudo-código é:

```c
static int movie_export_loop(system_header *system, FILE *pipe_out)
{
    genesis_context *gen = (genesis_context *)system;
    uint32_t width  = gen->vdp.h40 ? 320 : 256;
    uint32_t height = gen->vdp.output_lines;

    for (uint32_t f = 0; f < movie.header.frame_count; f++) {
        // Avança a máquina até o próximo frame
        // (via system->resume_context ou loop inline do 68K)
        run_one_frame(system);

        // Garante que o VDP finalizou o framebuffer corrente
        vdp_force_update_framebuffer(&gen->vdp);

        // Captura framebuffer (já está em RAM — texture_buf)
        int pitch;
        pixel_t *fb = render_get_framebuffer(gen->vdp.cur_buffer, &pitch);
        if (!fb) {
            warning("movie_export_loop: failed to get framebuffer at frame %u\n", f);
            return -1;
        }

        // Converte ARGB → RGB24 e escreve no pipe do ffmpeg
        if (movie_export_write_frame(fb, pitch, width, height, pipe_out) != 0) {
            warning("movie_export_loop: write failed at frame %u\n", f);
            return -1;
        }
    }

    return 0;
}
```

> **Nota de implementação:** `run_one_frame()` não existe no código atual.
> O plano de implementação definirá como avançar a máquina frame a frame em modo
> headless. A abordagem mais provável é usar `system->resume_context()` após
> configurar o sync source para um modo que produza frames sem bloquear em SDL.

### `movie_export_write_frame`

```c
static int movie_export_write_frame(pixel_t *fb, int pitch,
                                     uint32_t w, uint32_t h, FILE *out)
{
    uint8_t *row = (uint8_t *)fb;
    size_t rgb_row_size = w * 3;
    uint8_t *rgb_row = malloc(rgb_row_size);
    if (!rgb_row) return -1;

    for (uint32_t y = 0; y < h; y++) {
        uint32_t *src = (uint32_t *)(row + (size_t)y * pitch);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t argb = src[x];
            rgb_row[x * 3 + 0] = (argb >> 16) & 0xFF; // R
            rgb_row[x * 3 + 1] = (argb >> 8)  & 0xFF; // G
            rgb_row[x * 3 + 2] = argb         & 0xFF; // B
        }
        if (fwrite(rgb_row, 1, rgb_row_size, out) != rgb_row_size) {
            free(rgb_row);
            return -1;
        }
    }
    free(rgb_row);
    return 0;
}
```

---

## Integração CLI (`blastem.c`)

```c
if (strcmp(argv[i], "--export-movie") == 0 && i + 2 < argc) {
    const char *bsm_path = argv[++i];
    const char *mp4_path = argv[++i];

    if (movie_export_start(system, bsm_path, mp4_path) != 0) {
        fprintf(stderr, "Failed to export movie\n");
        return 1;
    }
    return 0;
}
```

---

## Tratamento de Erros

| Condição | Comportamento |
|---|---|
| `ffmpeg` não está no PATH | `popen()` retorna NULL → warning, retorna -1 |
| `popen()` falha (outro motivo) | `perror("ffmpeg")`, retorna -1 |
| `fwrite()` falha (pipe quebrado) | `movie_export_write_frame` retorna -1, loop aborta |
| `pclose()` retorna ≠ 0 | Warning, retorna -1 |
| `.bsm` inválido | `movie_play_start` já retorna erro (sub-épico 3) |
| CRC da ROM diferente | Warning via `movie_play_start`, prossegue |
| Framebuffer indisponível | `render_get_framebuffer` retorna NULL, aborta |

---

## Testes (`testmovie.c`)

### `test_export_frame_write`

Testa a conversão ARGB → RGB24 isoladamente:

1. Cria framebuffer sintético ARGB de 4×4 pixels com valores conhecidos
2. Abre pipe com `popen("cat", "r")` ou usa `tmpfile()`
3. Chama `movie_export_write_frame`
4. Lê de volta e verifica: RGB24 correto, alpha descartado
5. Verifica tamanho: 4 × 4 × 3 = 48 bytes

### `test_export_pipe`

Testa o pipeline completo:

1. Gera `.bsm` sintético com 30 frames de inputs conhecidos
2. Chama `movie_export_start()` com saída `/dev/null` (ou pipe que descarta)
3. Verifica retorno 0 (ffmpeg completou encode)
4. Se `ffmpeg` não está no PATH, pula com mensagem (SKIP, não FAIL)

---

## Critério de Aceite

1. `./blastem --export-movie gravacao.bsm saida.mp4` gera `saida.mp4` sem crash.
2. `ffprobe saida.mp4` confirma: codec H.264, framerate 60 (NTSC) ou 50 (PAL), resolução 320×224 ou 256×224, duração compatível com `frame_count / fps`.
3. O vídeo exportado é visualmente idêntico ao que seria visto na tela durante playback normal.
4. Se `ffmpeg` não estiver instalado, o comando falha com mensagem clara (não crash).
5. Se o `.bsm` for inválido, o comando falha com mensagem clara (herdado do sub-épico 3).
6. `test_export_frame_write` passa sem falhas.
7. `test_export_pipe` passa (ou SKIP se ffmpeg ausente).

---

## Arquivos Modificados

| Arquivo | Tipo | Mudança |
|---|---|---|
| `movie.h` | Modificar | Declarar `movie_export_start` |
| `movie.c` | Modificar | Implementar `movie_export_start`, `movie_export_loop`, `movie_export_write_frame` |
| `blastem.c` | Modificar | Parsear `--export-movie <bsm> <saida.mp4>` |
| `testmovie.c` | Modificar | Adicionar `test_export_frame_write`, `test_export_pipe` |

---

## Fora do Escopo

- Áudio (YM2612 + PSG) — será tratado em sub-épico futuro
- Parâmetros de codec expostos na CLI (CRF, preset, codec alternativo)
- Suporte a resoluções upscaled (será sempre resolução nativa VDP)
- Progresso em tempo real (barra de progresso) — apenas `fprintf(stderr, ...)` básico
- UI no menu Nuklear (Sub-épico 5)
- Suporte a periféricos além de gamepad de 6 botões
