/*
 * scene_settings.h — settings GUI for the desktop shell.
 *
 * The settings app is a scene-store-native application: its UI is built from
 * scene store nodes, rendered by the compositor, and driven by the same input
 * loop as any other app. It provides a tabbed interface for editing the shell
 * config (colors, layout, effects) and applies changes live via the shell's
 * re-theme API.
 *
 * Construction order:
 *   st = scene_settings_new(client, store, &callbacks)
 *   scene_settings_open(st, x, y, width, height)
 *   loop { scene_settings_tick(st); scene_settings_handle_pointer(st,...); }
 *   scene_settings_close(st)
 *   scene_settings_free(st)
 */
#ifndef SCENE_SETTINGS_H
#define SCENE_SETTINGS_H

#include "scene_client.h"
#include "scene_store.h"
#include "scene_shell.h"

typedef struct scene_settings scene_settings;

/* ---- callbacks -------------------------------------------------------- */

typedef struct scene_settings_cbs {
    /* Called when the user clicks Apply. cfg = current config with edits. */
    void (*on_apply)(const scene_shell_config *cfg, void *userdata);
    /* Called when the user clicks Reset. Restores defaults. */
    void (*on_reset)(void *userdata);
    /* Called when the window is closed. */
    void (*on_close)(void *userdata);
    void *userdata;
} scene_settings_cbs;

/* ---- lifecycle -------------------------------------------------------- */

/* Create a settings app attached to the given client and store. */
scene_settings *scene_settings_new(scene_client *client, scene_store *store,
                                   const scene_settings_cbs *cbs);

/* Free the settings app and its internal state. Does NOT destroy nodes. */
void scene_settings_free(scene_settings *st);

/* ---- window management ------------------------------------------------ */

/* Open the settings window at (x, y) with the given dimensions.
 * Creates the full tabbed UI. Returns 0 on success.                      */
int scene_settings_open(scene_settings *st, int32_t x, int32_t y,
                        int32_t width, int32_t height);

/* Close the settings window. Destroys all nodes.                         */
int scene_settings_close(scene_settings *st);

/* Returns 1 if the settings window is open.                              */
int scene_settings_is_open(const scene_settings *st);

/* ---- per-frame updates ------------------------------------------------ */

/* Rebuild the content area if dirty (after color edits, tab switches).   */
int scene_settings_tick(scene_settings *st);

/* ---- input handling --------------------------------------------------- */

/* Handle a pointer click. Returns 1 if consumed, 0 if not.              */
int scene_settings_handle_pointer(scene_settings *st, scene_node_id id);

/* Handle a key event. Returns 1 if consumed, 0 if passed.               */
int scene_settings_handle_key(scene_settings *st, uint32_t key_code,
                              uint8_t state, uint8_t modifiers);

/* ---- configuration ---------------------------------------------------- */

/* Get the current config (with any user edits applied).                  */
const scene_shell_config *scene_settings_get_config(const scene_settings *st);

/* Set the config (e.g. from shell's current config).                     */
void scene_settings_set_config(scene_settings *st, const scene_shell_config *cfg);

/* ---- tab identifiers -------------------------------------------------- */

#define SCENE_SETTINGS_TAB_COLORS   0
#define SCENE_SETTINGS_TAB_LAYOUT   1
#define SCENE_SETTINGS_TAB_EFFECTS  2
#define SCENE_SETTINGS_TAB_APPS     3
#define SCENE_SETTINGS_TAB_THEME    4
#define SCENE_SETTINGS_TAB_WALLPAPER 5
#define SCENE_SETTINGS_TAB_COUNT    6

/* ---- node ID layout --------------------------------------------------- */
/* Settings window IDs: 50000+ (high range, no collision with shell/app) */
#define STG_ID_WINDOW      50000
#define STG_ID_TITLEBAR    50001
#define STG_ID_TITLE_LABEL 50002
#define STG_ID_CLOSE_BTN   50003
#define STG_ID_TAB_BASE    50010  /* TAB_COUNT tabs: 50010..50014 */
#define STG_ID_CONTENT     50020
#define STG_ID_APPLY_BTN   50030
#define STG_ID_RESET_BTN   50031
#define STG_ID_FIELD_BASE  50040  /* up to 32 fields: 50040..50071 */
#define STG_ID_LABEL_BASE  50080  /* up to 32 labels: 50080..50111 */
#define STG_ID_LIST_BASE   50120  /* launcher list items: 50120..50151 */

#endif /* SCENE_SETTINGS_H */
