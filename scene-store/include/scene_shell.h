/*
 * scene_shell.h — desktop shell layer (built on scene store client API).
 *
 * The shell creates native scene store nodes for the desktop background,
 * panel/taskbar, app launcher, and window-tracking task buttons. It is a
 * scene store client — zero IPC overhead (loopback), zero marginal memory,
 * fully testable.
 *
 * Construction order:
 *   sh = scene_shell_new(client, &config)
 *   scene_shell_build(sh, width, height)
 *   loop { scene_shell_tick(sh); ... compositor frame ... }
 *   scene_shell_free(sh)
 */
#ifndef SCENE_SHELL_H
#define SCENE_SHELL_H

#include "scene_client.h"
#include "scene_store.h"
#include "scene_compositor.h"
#include "scene_wallpaper.h"
#include "scene_fmt.h"

typedef struct scene_shell scene_shell;

/* ---- configuration --------------------------------------------------- */

#define SCENE_SHELL_MAX_APPS 32

typedef struct scene_shell_config {
    uint32_t bg_color;            /* background fill ARGB               */
    uint32_t panel_height;        /* panel height in pixels             */
    uint32_t panel_color;         /* panel fill ARGB                    */
    uint32_t panel_border;        /* panel border ARGB (0 = no border)  */
    uint8_t  panel_border_w;      /* panel border width (0 = none)      */
    uint8_t  panel_radius;        /* panel corner radius                */
    uint32_t button_color;        /* task button fill ARGB              */
    uint32_t button_border;       /* task button border ARGB            */
    uint32_t button_text;         /* task button text color ARGB        */
    uint32_t hover_color;         /* button hover fill ARGB             */
    uint32_t label_text;          /* label/clock text color ARGB        */
    uint32_t menu_color;          /* launcher menu fill ARGB            */
    uint32_t menu_border;         /* launcher menu border ARGB          */
    uint32_t menu_item_color;     /* launcher item fill ARGB            */
    uint32_t menu_item_text;      /* launcher item text ARGB            */
    uint8_t  clock_12h;           /* 1 = 12-hour clock, 0 = 24-hour    */
    char     launcher_apps[SCENE_SHELL_MAX_APPS][64];
    uint32_t launcher_app_count;

    /* wallpaper */
    char     wallpaper_path[256]; /* image path for static/ken_burns     */
    uint8_t  wallpaper_mode;      /* SCENE_WP_* enum value               */
    char     wallpaper_slideshow_dir[256]; /* slideshow image directory   */
    float    wallpaper_slideshow_sec;      /* seconds per slide (0=15)    */
    float    wallpaper_slideshow_fade;     /* crossfade seconds (0=1)     */
    float    wallpaper_speed;              /* procedural speed (0=1.0)    */
} scene_shell_config;

/* Fill config with default dark-theme values. */
void scene_shell_config_defaults(scene_shell_config *cfg);

/* Parse a "Option=Value" text file into cfg. Returns 0 on success. */
int  scene_shell_config_load(scene_shell_config *cfg, const char *path);

/* ---- lifecycle ------------------------------------------------------- */

/* Create a shell attached to the given client and store.
 * client = for creating/modifying nodes (scene_client_* API)
 * store  = for reading current state (scene_store_walk, search, etc.)
 * Both must outlive the shell.                                     */
scene_shell *scene_shell_new(scene_client *client, scene_store *store,
                             scene_compositor *cp,
                             const scene_shell_config *cfg);

/* Set the hover style ref (from scene_compositor_setup_hover_style).
 * Must be called after build if hover effects are desired.              */
void scene_shell_set_hover_style(scene_shell *sh, scene_style_ref ref);

/* Set the active-focus style ref (from scene_compositor_setup_active_style).
 * The focused window's task button gets this style.                     */
void scene_shell_set_active_style(scene_shell *sh, scene_style_ref ref);

/* Launch hook for the launcher menu. Called when a menu item is
 * activated (idx = item index, name = configured app name). The host
 * owns the app factory (e.g. scene_launcher_spawn); without a hook the
 * shell falls back to shelling out: system("name &").                    */
typedef void (*scene_shell_launch_fn)(void *ud, uint32_t idx,
                                      const char *name);
void scene_shell_set_launch_cb(scene_shell *sh, scene_shell_launch_fn fn,
                               void *ud);

/* Network tray probe: fills the tray label text ("net" / "no net" /
 * "NA" on Windows). The shell calls it at most every 2 seconds and only
 * updates the label when the cached text changes. Tests set this to a
 * stub; the default probe reads the Linux sysfs net-carrier files and
 * returns "NA" on Windows.                                             */
extern const char *(*scene_shell_tray_probe)(void);

/* Free the shell and its internal state. Does NOT destroy nodes (the
 * client session handles that on close). */
void scene_shell_free(scene_shell *sh);

/* ---- tree construction ----------------------------------------------- */

/* Build the initial shell node tree: background + panel + start button +
 * clock label. Must be called once after connect; subsequent frames call
 * tick. width/height = output dimensions in pixels.                    */
int scene_shell_build(scene_shell *sh, int32_t width, int32_t height);

/* ---- per-frame updates ----------------------------------------------- */

/* Update clock text, reconcile task list with store state. Call once per
 * frame after the compositor has ingested the latest store state.        */
int scene_shell_tick(scene_shell *sh);

/* ---- input handling -------------------------------------------------- */

/* Handle an INPUT_ACTIVATE event. Returns 1 if the shell consumed the
 * event (clicked a shell node), 0 if not (passed to app).              */
int scene_shell_handle_activate(scene_shell *sh, scene_node_id activated_id);

/* Handle a pointer motion/button event for hover tracking. Call on every
 * INPUT_POINTER from the compositor. Returns the hit shell-owned node ID
 * (nonzero) or 0 if the pointer is not over a shell-owned node.         */
scene_node_id scene_shell_handle_pointer(scene_shell *sh, int32_t x, int32_t y,
                                uint8_t buttons);

/* Handle a key event. Returns 1 if the shell consumed it, 0 if passed.
 * Keys: Tab/Shift-Tab = focus cycle, Enter = activate focused,
 * Escape = close menu, Alt+Tab/Alt+Shift-Tab = task cycle.              */
int scene_shell_handle_key(scene_shell *sh, uint32_t key_code,
                           uint8_t state, uint8_t modifiers);

/* ---- notifications ---------------------------------------------------- */

/* Toast lifetime in ticks (~4 s at 60 fps). Deterministic, frame-based. */
#define SCENE_SHELL_TOAST_TICKS 240u

/* Raise a toast notification: a small OS window (top-right, role
 * NOTIFICATION) with a title + body label, auto-hidden after
 * TOAST_TICKS ticks (frame-count based, deterministic under test).
 * The toast is shell-session state; the host calls this from its
 * launcher/notify callbacks. Returns 0 on success. */
int scene_shell_notify(scene_shell *sh, const char *title, const char *body);

/* ---- reconfiguration ------------------------------------------------- */

/* Reload config from file and apply live (re-theme panel, buttons, etc.) */
int scene_shell_load_config(scene_shell *sh, const char *path);

/* Apply a config directly (for settings app, no file needed).
 * Destroys and rebuilds all shell nodes with the new config.             */
int scene_shell_apply_config(scene_shell *sh, const scene_shell_config *cfg);

/* ---- output changes -------------------------------------------------- */

/* Notify the shell of an output resize. Repositions background + panel. */
int scene_shell_resize(scene_shell *sh, int32_t width, int32_t height);

#endif /* SCENE_SHELL_H */
