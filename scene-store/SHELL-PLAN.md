# Shell Layer Plan — Pass 11

## Architecture

The shell is a **scene store client** — it creates nodes via `scene_client_*` APIs over the
loopback transport, exactly like the automation tests do. This means:

- Zero marginal memory (no separate process — loopback transport, in-process)
- Zero IPC overhead (same process, FIFO pair)
- Fully testable with existing harness pattern
- Re-themeable via the scene store's own style table
- Window list is queried from the store itself (scene_store_walk / search)

The shell is NOT a Wayland layer-shell client. It is a native scene store consumer.

## Components

### 1. Desktop Background
- A single WINDOW role node at the lowest z-order (created first)
- Full-screen rect (0, 0, output_width, output_height)
- SCENE_FLAG_VISIBLE, not focusable
- Style: solid fill color (configurable), no border, no radius
- On resize: `scene_client_set_rect` to match new output dimensions

### 2. Panel (Taskbar)
- A PANEL role node anchored to the bottom edge
- Height configurable (default 32px), full width
- Children (all BUTTON or LABEL role):
  - **Start button** (left): BUTTON, labeled "▶" or config label, focusable
  - **Task list** (center): dynamically created/destroyed BUTTON nodes, one per
    visible window. Text = window title (read from store via node_texts).
    Created when INPUT_ACTIVATE fires on a new window, destroyed when window
    is destroyed.
  - **Clock** (right): LABEL, updated on a timer tick (scene_client_set_text)

### 3. App Launcher (Start Menu)
- A WINDOW role node (floating menu), initially invisible (flags = 0)
- Position: above the start button
- Children: one BUTTON per app entry, with text = app name
- Shown/hidden by toggling SCENE_FLAG_VISIBLE on the menu node
- On BUTTON activation: the shell calls `execvp` (or system()) to launch the app
- App list: built from a config file or hardcoded minimal set

### 4. Window Management Feedback
- When the shell receives INPUT_ACTIVATE for a node, it checks the node's role
- If the activated node is a task-button in the panel, the shell sends focus to
  the corresponding window (via scene_client_focus)
- The shell tracks window creation/destruction by walking the store after each
  frame and comparing against its cached task list

## Config Format

Custom `Option=Value` text file (`shell.conf`), parsed in ~100 lines of C.
No dependencies. Live-reload via SIGUSR1 (or re-read on next frame).

```
# shell.conf — desktop shell configuration
background_color=0xFF1A1A2E
panel_height=32
panel_color=0xFF16213E
panel_position=bottom
panel_border=0xFF0F3460
button_color=0xFF1A1A2E
button_border=0xFF533483
button_text=0xFFFFFFFF
label_text=0xFFE0E0E0
font_size=8
launcher_apps=terminal,file manager,browser
```

## Files

### New files
| File | Purpose | Est. lines |
|------|---------|------------|
| `include/scene_shell.h` | Shell public API | ~60 |
| `src/scene_shell.c` | Shell implementation | ~400 |
| `tests/test_shell.c` | Shell test suite | ~300 |
| `shell.conf` | Default config | ~15 |

### Modified files
| File | Change |
|------|--------|
| `Makefile` | Add scene_shell.o, test_shell.exe targets |

## API

```c
/* scene_shell.h */

typedef struct scene_shell scene_shell;

/* Configuration (parsed from file or in-memory defaults) */
typedef struct scene_shell_config {
    uint32_t bg_color;          /* background fill ARGB */
    uint32_t panel_height;      /* px */
    uint32_t panel_color;       /* panel fill ARGB */
    uint32_t panel_border;      /* panel border ARGB */
    uint8_t  panel_radius;      /* corner radius */
    uint32_t button_color;      /* task button fill */
    uint32_t button_border;     /* task button border */
    uint32_t button_text;       /* task button text color */
    uint32_t label_text;        /* label text color */
    /* launcher entries stored separately */
} scene_shell_config;

/* Lifecycle */
scene_shell *scene_shell_new(scene_client *client, const scene_shell_config *cfg);
void         scene_shell_free(scene_shell *sh);

/* Build the initial shell node tree (background + panel + start button + clock) */
int scene_shell_build(scene_shell *sh, int32_t width, int32_t height);

/* Called after each frame — updates clock, reconciles task list with store */
int scene_shell_tick(scene_shell *sh);

/* Handle an activation event from the store (returns 1 if consumed) */
int scene_shell_handle_activate(scene_shell *sh, scene_node_id activated_id);

/* Reconfigure from file (returns 0 on success, -1 on parse error) */
int scene_shell_load_config(scene_shell *sh, const char *path);

/* Resize output — repositions background and panel */
int scene_shell_resize(scene_shell *sh, int32_t width, int32_t height);
```

## Test Strategy

Tests use the existing automation harness pattern (loopback transport + compositor + client).

| Test | What it verifies |
|------|-----------------|
| `test_shell_build` | After build, correct node tree exists: background (1 WINDOW), panel (1 PANEL), start button (1 BUTTON), clock (1 LABEL) |
| `test_shell_panel_rect` | Panel rect matches config (y = height - panel_height, h = panel_height, w = width) |
| `test_shell_background_rect` | Background covers full screen (0, 0, width, height) |
| `test_shell_start_click` | Clicking start button creates launcher menu nodes |
| `test_shell_task_list` | Creating a window via client adds a task button to the panel |
| `test_shell_task_destroy` | Destroying a window removes its task button |
| `test_shell_clock_update` | Clock label text changes between ticks |
| `test_shell_config_reload` | Loading new config changes panel color |
| `test_shell_resize` | Resize updates background and panel rects |
| `test_shell_determinism` | Two shells with identical configs produce identical node trees |

## Implementation Order

1. `scene_shell.h` — API and config struct
2. `scene_shell.c` — config parser + shell_new/free + build (background + panel + start button + clock)
3. `test_shell.c` — test_shell_build, test_shell_panel_rect, test_shell_background_rect
4. Wire into Makefile, get green
5. Add task list management (tick reconciles with store walk)
6. Add start menu (show/hide on start button click)
7. Add config file parser + load_config
8. Add resize handling
9. Add determinism test
10. Full green, update AGENTS.md
