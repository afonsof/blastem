# Instant Screenshot Mode (-S) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a new command-line flag `-S` that runs BlastEm in headless mode, loads a ROM and a save state, captures a PNG screenshot, and exits.

**Architecture:**
- Global flag `screenshot_and_exit` to control the behavior.
- Argument parsing in `main` to handle `-S <rom> <state>`.
- Screenshot trigger at the end of the first frame after state load.
- Re-use `get_content_config_path` for standard screenshot naming.

**Tech Stack:** C, SDL2, zlib (for PNG).

---

### Task 1: Define the Global Flag

**Files:**
- Modify: `blastem.h:1-20`
- Modify: `blastem.c:45-55`

- [ ] **Step 1: Add global variable declaration in `blastem.h`**
```c
extern int screenshot_and_exit;
```

- [ ] **Step 2: Initialize global variable in `blastem.c`**
```c
int screenshot_and_exit = 0;
```

- [ ] **Step 3: Commit**
```bash
git add blastem.h blastem.c
git commit -m "feat: add global flag for screenshot_and_exit"
```

---

### Task 2: Handle Command-Line Flag `-S`

**Files:**
- Modify: `blastem.c:380-500`

- [ ] **Step 1: Add `-S` case to the argument parsing loop in `main`**
```c
			case 'S':
				i++;
				if (i >= argc) {
					fatal_error("-S must be followed by a savestate filename\n");
				}
				statefile = argv[i];
				screenshot_and_exit = 1;
				headless = 1;
				exit_after = 1; // Exit after 1 frame
				break;
```

- [ ] **Step 2: Update help text to include `-S`**
```c
					"	-S FILE     Headless mode: load state from FILE, save screenshot and exit\n"
```

- [ ] **Step 3: Commit**
```bash
git add blastem.c
git commit -m "feat: implement -S flag in main argument parsing"
```

---

### Task 4: Implement Headless Screenshot Logic

**Files:**
- Modify: `blastem.c:60-100` (new helper function)
- Modify: `genesis.c:600-620` (or appropriate frame sync point)
- Modify: `vdp.c:3050-3100`

- [ ] **Step 1: Add `save_screenshot_and_exit` helper in `blastem.c`**
```c
#include "png.h"
#include "render.h"

void save_screenshot_and_exit()
{
	if (!current_system || !current_system->get_vdp) {
		exit(0);
	}
	vdp_context *v_context = current_system->get_vdp(current_system);
	if (!v_context || !v_context->fb) {
		exit(0);
	}
	char *path = get_content_config_path("ui\0screenshot_path\0", "ui\0screenshot_template\0", "blastem_%c.png");
	FILE *f = fopen(path, "wb");
	if (f) {
		uint32_t width = v_context->h40_lines > 0 ? 320 : 256;
		width += HORIZ_BORDER;
		uint32_t height = v_context->output_lines;
		save_png(f, v_context->fb, width, height, v_context->output_pitch);
		fclose(f);
		debug_message("Screenshot saved to %s\n", path);
	} else {
		warning("Failed to open screenshot file %s for writing\n", path);
	}
	free(path);
	exit(0);
}
```

- [ ] **Step 2: Call `save_screenshot_and_exit` in `genesis.c` when `exit_after` hits 0 and `screenshot_and_exit` is set**
```c
		if(exit_after){
			if (elapsed >= exit_after) {
				if (screenshot_and_exit) {
					save_screenshot_and_exit();
				}
				exit(0);
			} else {
				exit_after -= elapsed;
			}
		}
```
*(Apply similar changes to `sms.c` and `coleco.c` if needed)*

- [ ] **Step 3: Commit**
```bash
git add blastem.c genesis.c
git commit -m "feat: implement screenshot saving logic for headless mode"
```

---

### Task 4: Verification

- [ ] **Step 1: Build BlastEm**
Run: `make`

- [ ] **Step 2: Test with a ROM and a state**
Run: `./blastem -S rom.bin quicksave.state`
Expected: Program exits quickly, no window appears, and a PNG file is created in `$HOME` (or configured path).

- [ ] **Step 3: Verify PNG content**
Check the generated PNG file to ensure it's not black and contains the game screen.
```bash
ls -rt *.png | tail -n 1
```

- [ ] **Step 4: Commit**
```bash
git commit --allow-empty -m "test: verify -S flag works as expected"
```
