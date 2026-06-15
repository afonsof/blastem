# Sub-Epic 5 — UI e UX: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expor record/play/export via menu Nuklear e confirmar CLI flags, para que o usuário não precise de linha de comando.

**Architecture:** Três novos itens no `view_pause` — "Record Movie" (toggle), "Play Movie" (toggle), "Export Movie". Cada um abre uma view dedicada que reusa `view_file_browser` para escolha de paths. OSD renderizado via `render_video_loop` mostra estado durante o jogo. Long-form flags (`--record`, `--play`, `--export-movie`) adicionados ao parser CLI.

**Tech Stack:** C (gnu99), Nuklear UI (`nuklear_ui/blastem_nuklear.c`), `movie.h/c`, `blastem.c`, `bindings.c/h`

---

## Mapa de Arquivos

| Arquivo | Ação | Responsabilidade |
|---------|------|-----------------|
| `nuklear_ui/blastem_nuklear.c` | Modificar `view_pause`, adicionar views | Menu items Record/Play/Export |
| `nuklear_ui/blastem_nuklear.h` | Adicionar `show_play_view` (já existe) | Declarações das novas views |
| `blastem.c` | Modificar parser CLI | Long-form flags `--record`, `--play`, `--export-movie` |
| `bindings.c` | Verificar | `ui.movie_record` já existe, verificar bind |
| `movie.c` | Modificar `movie_update` (opcional) | Hook para OSD update |
| `render_sdl.c` | Modificar `render_video_loop` | Render OSD de estado movie |
| `default.cfg` | Verificar | Bind `r ui.movie_record` já existe |

---

## Task 1: Menu item "Record Movie" (toggle)

**Files:**
- Modify: `nuklear_ui/blastem_nuklear.c` — adicionar `view_record_movie`, handler, item no `view_pause`

- [ ] **Step 1.1: Adicionar `view_record_movie` function**

Em `nuklear_ui/blastem_nuklear.c`, antes de `view_pause`, inserir:

```c
static char record_path[1024];

static void record_start(const char *path)
{
	if (!current_system) return;
	strncpy(record_path, path, sizeof(record_path) - 1);
	record_path[sizeof(record_path) - 1] = 0;
	if (movie_record_start(current_system, record_path) == 0) {
		show_play_view();  /* back to game */
	} else {
		warning("Failed to start recording to %s\n", record_path);
	}
}

static void view_record_movie(struct nk_context *context)
{
	if (movie_get_state() == BSM_STATE_RECORD) {
		/* Recording is active — show stop button + info */
		if (nk_begin(context, "Record Movie",
		    nk_rect(0, 0, render_width(), render_height()), 0)) {
			nk_layout_row_dynamic(context, context->style.font->height * 2, 1);
			nk_label(context, "Recording in progress...", NK_TEXT_CENTERED);

			nk_layout_row_dynamic(context, context->style.font->height * 1.75, 1);
			if (nk_button_label(context, "Stop Recording")) {
				movie_record_stop();
				show_play_view();
			}

			nk_layout_row_dynamic(context, context->style.font->height * 1.75, 1);
			if (nk_button_label(context, "Back")) {
				pop_view();
			}
			nk_end(context);
		}
	} else {
		/* Not recording — show file browser to choose save path */
		view_file_browser(context, 0);  /* 0 = save/new file */
	}
}
```

- [ ] **Step 1.2: Adicionar handler para o file browser confirmar gravação**

Em `view_file_browser`, o botão "Open" (linha 324) chama o handler de abertura. Para gravação, precisamos que o file browser chame `record_start` em vez de `load_rom`. Modificar `view_file_browser` para aceitar um callback:

Na verdade, a abordagem mais simples é estender `view_file_browser` com um parâmetro de callback. Mas para manter o escopo mínimo, vamos usar uma variável estática de estado:

Após `view_file_browser`, adicionar hook no path de retorno. No `view_record_movie`, após `view_file_browser(context, 0)`, o file browser volta chamando `pop_view` e o controller (caller) precisa chamar `record_start`.

Abordagem alternativa mais simples: usar o `view_file_browser` existente e adicionar o callback no handler. Como o `view_file_browser` atual chama `load_rom` internamente, vamos criar uma versão simplificada inline.

**Abordagem final (mais simples):** Criar `view_record_movie` com file browser inline que chama `movie_record_start` diretamente, seguindo o padrão do `view_file_browser` mas simplificado.

```c
static void view_record_movie(struct nk_context *context)
{
	if (movie_get_state() == BSM_STATE_RECORD) {
		/* ... stop button as above ... */
		return;
	}

	/* File browser for .bsm save */
	static int selected_entry = -1;
	static dir_entry *entries = NULL;
	static int num_entries = 0;
	static int old_selected = -1;

	if (!entries) {
		entries = get_dir_list(get_extension_list("bsm"), &num_entries);
		selected_entry = -1;
	}

	if (nk_begin(context, "Record Movie - Choose File",
	    nk_rect(0, 0, render_width(), render_height()), 0)) {

		nk_layout_row_dynamic(context, context->style.font->height * 10, 1);
		if (nk_group_begin(context, "Files", NK_WINDOW_BORDER)) {
			nk_layout_row_dynamic(context, context->style.font->height, 1);
			for (int i = 0; i < num_entries; i++) {
				if (nk_button_label(context, entries[i].name)) {
					selected_entry = i;
				}
			}
			nk_group_end(context);
		}

		nk_layout_row_dynamic(context, context->style.font->height * 1.75, 2);
		if (nk_button_label(context, "Back")) {
			free_dir_list(entries, num_entries);
			entries = NULL;
			pop_view();
		}
		if (nk_button_label(context, "Record") || (old_selected >= 0 && selected_entry < 0)) {
			if (selected_entry >= 0) {
				char *path = path_join(entries[selected_entry].path, "");
				record_start(path);
				free(path);
				free_dir_list(entries, num_entries);
				entries = NULL;
			}
		}
		old_selected = selected_entry;
		nk_end(context);
	}
}
```

- [ ] **Step 1.3: Adicionar item em `view_pause`**

Em `view_pause`, na array `items[]` (linha 2567), adicionar após `{"Resume", view_play}`:

```c
		{"Record Movie", view_record_movie},
```

A array fica:
```c
	static menu_item items[] = {
		{"Resume", view_play},
		{"Record Movie", view_record_movie},
		{"Load ROM", view_load},
		{"Lock On", view_lock_on},
		{"Save State", view_save_state},
		{"Load State", view_load_state},
		{"Settings", view_settings},
#ifndef __EMSCRIPTEN__
		{"Exit", NULL}
#endif
	};
```

E em `sc3k_items[]` também adicionar `{"Record Movie", view_record_movie}`.

- [ ] **Step 1.4: Build**

```bash
cd /Users/afonsof/Projects/retro/blastem && make blastem 2>&1 | grep -E "error:" | head -10
```

Esperado: zero erros.

- [ ] **Step 1.5: Commit**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add nuklear_ui/blastem_nuklear.c
git commit -m "feat(ui): add Record Movie menu item with file browser (sub-epic 5)"
```

---

## Task 2: Menu item "Play Movie" (toggle)

**Files:**
- Modify: `nuklear_ui/blastem_nuklear.c` — adicionar `view_play_movie`, item no `view_pause`

- [ ] **Step 2.1: Adicionar `view_play_movie` function**

Em `nuklear_ui/blastem_nuklear.c`, após `view_record_movie`, inserir:

```c
static void play_start(const char *path)
{
	if (!current_system) return;
	if (movie_play_start(current_system, path) == 0) {
		movie_play_pre_inject(current_system);
		show_play_view();  /* back to game */
	} else {
		warning("Failed to start playback from %s\n", path);
	}
}

static void view_play_movie(struct nk_context *context)
{
	if (movie_get_state() == BSM_STATE_PLAY) {
		/* Playback active — show stop button + frame info */
		if (nk_begin(context, "Play Movie",
		    nk_rect(0, 0, render_width(), render_height()), 0)) {
			nk_layout_row_dynamic(context, context->style.font->height * 2, 1);
			char buf[64];
			snprintf(buf, sizeof(buf), "Playing: frame %u / %u",
				movie_get_play_frame(),
				(uint32_t)0 /* TODO: expose movie_get_frame_count */);
			nk_label(context, buf, NK_TEXT_CENTERED);

			nk_layout_row_dynamic(context, context->style.font->height * 1.75, 1);
			if (nk_button_label(context, "Stop Playback")) {
				movie_play_stop();
				show_play_view();
			}

			nk_layout_row_dynamic(context, context->style.font->height * 1.75, 1);
			if (nk_button_label(context, "Back")) {
				pop_view();
			}
			nk_end(context);
		}
	} else {
		/* Not playing — show file browser for .bsm */
		static int selected_entry = -1;
		static dir_entry *entries = NULL;
		static int num_entries = 0;
		static int old_selected = -1;

		if (!entries) {
			entries = get_dir_list(get_extension_list("bsm"), &num_entries);
			selected_entry = -1;
		}

		if (nk_begin(context, "Play Movie - Choose File",
		    nk_rect(0, 0, render_width(), render_height()), 0)) {

			nk_layout_row_dynamic(context, context->style.font->height * 10, 1);
			if (nk_group_begin(context, "Files", NK_WINDOW_BORDER)) {
				nk_layout_row_dynamic(context, context->style.font->height, 1);
				for (int i = 0; i < num_entries; i++) {
					if (nk_button_label(context, entries[i].name)) {
						selected_entry = i;
					}
				}
				nk_group_end(context);
			}

			nk_layout_row_dynamic(context, context->style.font->height * 1.75, 2);
			if (nk_button_label(context, "Back")) {
				free_dir_list(entries, num_entries);
				entries = NULL;
				pop_view();
			}
			if (nk_button_label(context, "Play")) {
				if (selected_entry >= 0) {
					char *path = path_join(entries[selected_entry].path, "");
					play_start(path);
					free(path);
					free_dir_list(entries, num_entries);
					entries = NULL;
				}
			}
			old_selected = selected_entry;
			nk_end(context);
		}
	}
}
```

- [ ] **Step 2.2: Adicionar `movie_get_frame_count` helper (opcional)**

Em `movie.h`, após `movie_get_play_frame`:
```c
uint32_t movie_get_frame_count(void);
```

Em `movie.c`, após `movie_get_play_frame`:
```c
uint32_t movie_get_frame_count(void)
{
	return movie.header.frame_count;
}
```

Isso permite mostrar `frame / total` no menu de playback.

- [ ] **Step 2.3: Adicionar item em `view_pause`**

Na array `items[]` de `view_pause`, adicionar após `{"Record Movie", view_record_movie}`:

```c
		{"Play Movie", view_play_movie},
```

- [ ] **Step 2.4: Build**

```bash
cd /Users/afonsof/Projects/retro/blastem && make blastem 2>&1 | grep -E "error:" | head -10
```

- [ ] **Step 2.5: Commit**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add nuklear_ui/blastem_nuklear.c movie.h movie.c
git commit -m "feat(ui): add Play Movie menu item with file browser (sub-epic 5)"
```

---

## Task 3: Menu item "Export Movie"

**Files:**
- Modify: `nuklear_ui/blastem_nuklear.c` — adicionar `view_export_movie`

- [ ] **Step 3.1: Adicionar `view_export_movie` function**

```c
static char export_input_path[1024];
static uint8_t export_phase; /* 0=pick bsm, 1=pick output */

static void export_start(void)
{
	if (!current_system) return;
	if (movie_export_start(current_system, export_input_path, export_input_path) == 0) {
		/* Note: movie_export_start blocks until done, then returns.
		 * Use the output path from the second file picker. */
		show_play_view();
	} else {
		warning("Export failed. Is ffmpeg installed?\n");
	}
}

static void view_export_movie(struct nk_context *context)
{
	static int selected_entry = -1;
	static dir_entry *entries = NULL;
	static int num_entries = 0;

	if (!entries) {
		if (export_phase == 0) {
			entries = get_dir_list(get_extension_list("bsm"), &num_entries);
		} else {
			entries = get_dir_list(get_extension_list("mp4"), &num_entries);
		}
		selected_entry = -1;
	}

	const char *title = export_phase == 0 ?
		"Export Movie - Choose .bsm" : "Export Movie - Save As .mp4";

	if (nk_begin(context, title,
	    nk_rect(0, 0, render_width(), render_height()), 0)) {

		nk_layout_row_dynamic(context, context->style.font->height * 10, 1);
		if (nk_group_begin(context, "Files", NK_WINDOW_BORDER)) {
			nk_layout_row_dynamic(context, context->style.font->height, 1);
			for (int i = 0; i < num_entries; i++) {
				if (nk_button_label(context, entries[i].name)) {
					selected_entry = i;
				}
			}
			nk_group_end(context);
		}

		nk_layout_row_dynamic(context, context->style.font->height * 1.75, 2);
		if (nk_button_label(context, "Back")) {
			free_dir_list(entries, num_entries);
			entries = NULL;
			export_phase = 0;
			pop_view();
		}

		const char *action = export_phase == 0 ? "Next" : "Export";
		if (nk_button_label(context, action)) {
			if (selected_entry >= 0) {
				if (export_phase == 0) {
					strncpy(export_input_path,
						entries[selected_entry].path,
						sizeof(export_input_path) - 1);
					free_dir_list(entries, num_entries);
					entries = NULL;
					export_phase = 1;  /* next: pick output */
				} else {
					/* Use output path, start export */
					char output_path[1024];
					strncpy(output_path, entries[selected_entry].path,
						sizeof(output_path) - 1);
					free_dir_list(entries, num_entries);
					entries = NULL;
					export_phase = 0;
					/* Call export with input .bsm and output .mp4 */
					if (movie_export_start(current_system,
					    export_input_path, output_path) != 0) {
						warning("Export failed\n");
					}
					show_play_view();
				}
			}
		}
		nk_end(context);
	}
}
```

- [ ] **Step 3.2: Adicionar item em `view_pause`**

Na array `items[]`, adicionar após `{"Play Movie", view_play_movie}`:

```c
		{"Export Movie", view_export_movie},
```

- [ ] **Step 3.3: Build**

```bash
cd /Users/afonsof/Projects/retro/blastem && make blastem 2>&1 | grep -E "error:" | head -10
```

- [ ] **Step 3.4: Commit**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add nuklear_ui/blastem_nuklear.c
git commit -m "feat(ui): add Export Movie menu item (sub-epic 5)"
```

---

## Task 4: OSD indicator (on-screen display durante gameplay)

**Files:**
- Modify: `nuklear_ui/blastem_nuklear.c` — adicionar `render_movie_osd()`
- Modify: `render_sdl.c` ou `blastem.c` — chamar `render_movie_osd` no loop

- [ ] **Step 4.1: Adicionar função de renderização do OSD**

Em `nuklear_ui/blastem_nuklear.c`, antes de `show_pause_menu`:

```c
void render_movie_osd(void)
{
	bsm_state state = movie_get_state();
	if (state == BSM_STATE_NONE) return;

	/* Check config override */
	char *osd_setting = tern_find_path_default(config,
		"ui\0movie_osd\0", (tern_val){.ptrval = "on"}, TVAL_PTR).ptrval;
	if (osd_setting && !strcmp(osd_setting, "off")) return;

	char buf[64];
	if (state == BSM_STATE_RECORD) {
		snprintf(buf, sizeof(buf), "● REC");
	} else if (state == BSM_STATE_PLAY) {
		snprintf(buf, sizeof(buf), "▶ PLAY %u/%u",
			movie_get_play_frame(), movie_get_frame_count());
	} else {
		return;
	}

	/* Render at top-right corner using Nuklear overlay */
	struct nk_context *ctx = context;
	if (!ctx) return;

	/* Save current style, set up overlay */
	struct nk_style old_style = ctx->style;
	ctx->style.window.fixed_background = nk_style_item_color(nk_rgba(0, 0, 0, 0));
	ctx->style.window.background = nk_rgba(0, 0, 0, 0);

	float text_width = strlen(buf) * ctx->style.font->width * 0.6f;
	float x = render_width() - text_width - 20;
	float y = 10;

	if (nk_begin(ctx, "movie_osd",
	    nk_rect(x, y, text_width + 16, ctx->style.font->height + 8),
	    NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BORDER)) {
		nk_layout_row_dynamic(ctx, ctx->style.font->height, 1);
		if (state == BSM_STATE_RECORD) {
			nk_label_colored(ctx, buf, NK_TEXT_RIGHT, nk_rgb(255, 60, 60));
		} else {
			nk_label_colored(ctx, buf, NK_TEXT_RIGHT, nk_rgb(60, 255, 60));
		}
		nk_end(ctx);
	}

	ctx->style = old_style;
}
```

- [ ] **Step 4.2: Exportar função em `blastem_nuklear.h`**

Em `nuklear_ui/blastem_nuklear.h`, após `show_pause_menu`:

```c
void render_movie_osd(void);
```

- [ ] **Step 4.3: Chamar no loop principal**

Em `blastem.c`, no `render_video_loop` (ou no loop principal após `render_update`), adicionar:

```c
#ifndef DISABLE_NUKLEAR
		if (use_nuklear) {
			render_movie_osd();
		}
#endif
```

- [ ] **Step 4.4: Build**

```bash
cd /Users/afonsof/Projects/retro/blastem && make blastem 2>&1 | grep -E "error:" | head -10
```

- [ ] **Step 4.5: Commit**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add nuklear_ui/blastem_nuklear.c nuklear_ui/blastem_nuklear.h blastem.c
git commit -m "feat(ui): add OSD indicator for movie record/play state (sub-epic 5)"
```

---

## Task 5: Long-form CLI flags

**Files:**
- Modify: `blastem.c` — adicionar `--record`, `--play`, `--export-movie`

- [ ] **Step 5.1: Adicionar long-form flags ao parser**

Em `blastem.c`, no switch de parsing (linha ~486), após o case `'P'`:

```c
		case 'E':
			i++;
			if (i >= argc) {
				fatal_error("-E must be followed by an output .mp4 path\n");
			}
			/* -E needs both a .bsm input and an .mp4 output.
			 * For now, require --export-movie INPUT.bsm OUTPUT.mp4 */
			export_output = argv[i];
			break;
```

Wait — a flag `-E` para export precisa de 2 argumentos (input .bsm e output .mp4), ou o input .bsm é o `play_file`? Vamos simplificar: usar `-P` para especificar o .bsm de entrada, e `-E` para o .mp4 de saída. Se ambos estiverem presentes, chama `movie_export_start`.

Abordagem mais limpa: long-form `--export-movie` que recebe o .bsm de input, e `-E` para output. Ou simplemente: `--export INPUT.bsm OUTPUT.mp4`.

Para manter escopo mínimo, vamos usar a abordagem que o spec pede: long-form aliases.

```c
	/* After existing -R and -P blocks, around line 500 */

	/* Long-form --record and --play aliases */
	/* The existing getopt-like loop handles single-char flags.
	 * For long-form, we check the full argument string. */

	/* In the argument parsing loop, before the switch: */
	if (!strcmp(argv[i], "--record")) {
		i++;
		if (i >= argc) fatal_error("--record must be followed by a filename\n");
		record_file = argv[i];
		continue;
	}
	if (!strcmp(argv[i], "--play")) {
		i++;
		if (i >= argc) fatal_error("--play must be followed by a filename\n");
		play_file = argv[i];
		continue;
	}
	if (!strcmp(argv[i], "--export-movie")) {
		/* --export-movie INPUT.bsm OUTPUT.mp4 */
		i++;
		if (i + 1 >= argc) fatal_error("--export-movie requires INPUT.bsm OUTPUT.mp4\n");
		/* Store both: use play_file for input, export_output for output */
		play_file = argv[i];
		i++;
		export_output = argv[i];
		continue;
	}
```

- [ ] **Step 5.2: Declarar `export_output` e integrar com playback**

Em `blastem.c`, linha 425, após `char *play_file = NULL;`:

```c
	char *export_output = NULL;
```

Após o bloco de playback (linha 753), adicionar:

```c
	if (export_output && play_file && current_system) {
		/* Export mode: playback + ffmpeg pipe */
		if (movie_export_start(current_system, play_file, export_output)) {
			warning("Failed to export movie from %s to %s\n",
				play_file, export_output);
		}
		exit(0);
	}
```

- [ ] **Step 5.3: Atualizar help text**

Após a linha `-P FILE` no help, adicionar:

```c
					"\t-E FILE     Export .bsm movie to .mp4 video file\n"
					"\t--record FILE     Same as -R\n"
					"\t--play FILE       Same as -P\n"
					"\t--export-movie INPUT.bsm OUTPUT.mp4\n"
```

- [ ] **Step 5.4: Build e smoke test**

```bash
cd /Users/afonsof/Projects/retro/blastem && make blastem 2>&1 | grep -E "error:" | head -10
./blastem -h 2>&1 | grep -E "\-E|\-\-record|\-\-play|\-\-export"
```

- [ ] **Step 5.5: Commit**

```bash
cd /Users/afonsof/Projects/retro/blastem
git add blastem.c
git commit -m "feat(cli): add --record, --play, -E, --export-movie flags (sub-epic 5)"
```

---

## Task 6: Verificação End-to-End

- [ ] **Step 6.1: Testar menu Record**

```bash
cd /Users/afonsof/Projects/retro/blastem
./blastem /caminho/para/jogo.bin
# 1. Abrir menu (ESC)
# 2. Selecionar "Record Movie"
# 3. Navegar e escolher path (ex: /tmp/ui_test.bsm)
# 4. Clicar "Record" → volta ao jogo
# 5. Verificar OSD "● REC" no canto superior direito
# 6. Jogar alguns segundos
# 7. Abrir menu → "Record Movie" agora deve mostrar "Recording in progress..."
# 8. Clicar "Stop Recording"
# 9. Verificar que OSD sumiu
# 10. Sair e verificar que /tmp/ui_test.bsm existe e é válido
```

- [ ] **Step 6.2: Testar menu Play**

```bash
./blastem /caminho/para/jogo.bin
# 1. Abrir menu
# 2. Selecionar "Play Movie"
# 3. Escolher /tmp/ui_test.bsm
# 4. Clicar "Play" → reprodução inicia
# 5. Verificar OSD "▶ PLAY 123/6396"
# 6. Abrir menu → "Play Movie" mostra "Stop Playback"
# 7. Clicar "Stop Playback" → volta ao live
```

- [ ] **Step 6.3: Testar CLI long-form**

```bash
./blastem --record /tmp/cli_test.bsm /caminho/para/jogo.bin
# jogar alguns segundos, fechar
python3 -c "import struct; f=open('/tmp/cli_test.bsm','rb'); assert f.read(4)==b'BSM\x1a'; print('OK')"

./blastem --play /tmp/cli_test.bsm /caminho/para/jogo.bin
# verificar que reproduz sem crash
```

- [ ] **Step 6.4: Commit de validação**

```bash
cd /Users/afonsof/Projects/retro/blastem
git commit --allow-empty -m "chore: sub-epic 5 UI/UX e2e validated"
```

---

## Critério de Aceite

1. Menu do Nuklear tem itens "Record Movie", "Play Movie", "Export Movie".
2. "Record Movie" inicia gravação via file browser; reabrir menu mostra "Stop Recording".
3. "Play Movie" inicia reprodução via file browser; reabrir menu mostra "Stop Playback".
4. "Export Movie" seleciona .bsm de entrada e .mp4 de saída, gera vídeo.
5. OSD "● REC" visível durante gravação; "▶ PLAY N/M" durante playback; some ao parar.
6. `./blastem -h` mostra `--record`, `--play`, `--export-movie`.
7. `./blastem --record FILE rom.bin` inicia gravação; `--play FILE rom.bin` inicia playback.
8. Keybinding `r` (`ui.movie_record`) funciona como toggle de gravação.
