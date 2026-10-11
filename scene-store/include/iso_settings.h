/*
 * iso_settings.h — first-party settings GUI guest app.
 *
 * Edits the small text config files the desktop owns and writes every
 * change back atomically (a "<path>.tmp" is written, fsync'd by close,
 * then renamed over the target; on Windows the rename is preceded by a
 * remove() because the MSVCRT rename does not overwrite an existing
 * file, POSIX relies on rename's atomic replace).
 *
 * Files, by directory:
 *   base  shell.conf                                Option=Value, comments kept
 *   base  conf.d                                    Option=Value, comments kept
 *   base  wpa_supplicant/wpa_supplicant.conf        network={ ssid= psk= }
 *   state scene-volume                              0..100
 *
 * The base and state directories default to "/etc" and "/run"; the
 * tests point both at a scratch dir, so the whole core is exercised
 * without touching the live system.
 *
 * The GUI speaks the scene client wire API only: it creates child nodes
 * under a caller-provided content node and reports edits through the
 * optional applied() callback. It never pumps the client (the app loop
 * owns the pump: re-entering the client pump from an input callback
 * recurses, see iso_files.c notes).
 */
#ifndef ISO_SETTINGS_H
#define ISO_SETTINGS_H

#include <stddef.h>
#include <stdint.h>

#include "scene_client.h"
#include "scene_fmt.h"

#define ISO_SETTINGS_PATH_MAX 256
#define ISO_SETTINGS_KEY_MAX 48
#define ISO_SETTINGS_VAL_MAX 128
#define ISO_SETTINGS_KV_MAX 48
#define ISO_SETTINGS_RAW_MAX 16384
#define ISO_SETTINGS_NET_MAX 16
#define ISO_SETTINGS_SSID_MAX 64
#define ISO_SETTINGS_PSK_MAX 128

/* ---- views ------------------------------------------------------------ */

#define ISO_SETTINGS_VIEW_LIST 0
#define ISO_SETTINGS_VIEW_SHELL 1
#define ISO_SETTINGS_VIEW_NETWORK 2
#define ISO_SETTINGS_VIEW_VOLUME 3
#define ISO_SETTINGS_VIEW_CLOCK 4

#define ISO_SETTINGS_LIST_ITEMS 4
#define ISO_SETTINGS_SHELL_FIELDS 6

/* ---- widget ids --------------------------------------------------------
 * Fixed so the layout is deterministic and tests can address a row
 * without a search round-trip. The network ids are spaced 40 apart and
 * the three per-network id blocks stride by 1 over ISO_SETTINGS_NET_MAX
 * (< 40), so the blocks never collide. All ids sit far above the
 * scene_app window block (40000..40006). */
#define ISO_SETTINGS_ID_BACK 62001
#define ISO_SETTINGS_ID_STATUS 62002
#define ISO_SETTINGS_ID_ITEM 62010      /* +0..3   nav rows              */
#define ISO_SETTINGS_ID_FIELD 62020     /* +0..7   shell/clock/volume    */
#define ISO_SETTINGS_ID_NET_SSID 62100  /* +0..15  ssid rows             */
#define ISO_SETTINGS_ID_NET_PSK 62140   /* +0..15  psk rows              */
#define ISO_SETTINGS_ID_NET_DEL 62180   /* +0..15  remove rows           */
#define ISO_SETTINGS_ID_NET_ADD 62220
#define ISO_SETTINGS_ID_VOL_DEC 62221
#define ISO_SETTINGS_ID_VOL_INC 62222

/* ---- paths ------------------------------------------------------------ */

typedef struct iso_settings_paths {
    char base[ISO_SETTINGS_PATH_MAX];
    char state[ISO_SETTINGS_PATH_MAX];
} iso_settings_paths;

void iso_settings_paths_default(iso_settings_paths *p);
/* Sets base AND state to `base` (the tests' scratch-dir shortcut);
 * NULL/empty restores the defaults. Returns 0/-1. */
int iso_settings_paths_set_base(iso_settings_paths *p, const char *base);
int iso_settings_paths_set_state(iso_settings_paths *p, const char *state);
int iso_settings_path(const iso_settings_paths *p, const char *name,
                      char *out, size_t cap);
int iso_settings_state_path(const iso_settings_paths *p, const char *name,
                            char *out, size_t cap);

/* ---- Option=Value files ------------------------------------------------ */

typedef struct iso_settings_kvent {
    char key[ISO_SETTINGS_KEY_MAX];
    char val[ISO_SETTINGS_VAL_MAX];
} iso_settings_kvent;

typedef struct iso_settings_kvfile {
    char path[ISO_SETTINGS_PATH_MAX];
    char raw[ISO_SETTINGS_RAW_MAX];
    size_t raw_len;
    iso_settings_kvent kv[ISO_SETTINGS_KV_MAX];
    int count;
} iso_settings_kvfile;

/* Reads the whole file; missing file is not an error for the caller
 * (returns -1 but leaves path set and the kv set empty, so a later
 * set+save creates the file). Parses `key=value`, skipping blank and
 * #/; comment lines; later duplicates win. */
int iso_settings_kv_load(iso_settings_kvfile *k, const char *path);
int iso_settings_kv_get(const iso_settings_kvfile *k, const char *key,
                        const char **out);
int iso_settings_kv_get_int(const iso_settings_kvfile *k, const char *key,
                            int deflt);
int iso_settings_kv_set(iso_settings_kvfile *k, const char *key,
                        const char *val);
/* Rewrites the file line-by-line: modeled keys take their current
 * value, every other line (comments, unmodeled keys, blanks) is kept
 * verbatim; keys not present in the original are appended. */
int iso_settings_kv_save(iso_settings_kvfile *k);

/* ---- wpa_supplicant.conf ---------------------------------------------- */

typedef struct iso_settings_net {
    char ssid[ISO_SETTINGS_SSID_MAX];
    char psk[ISO_SETTINGS_PSK_MAX];
} iso_settings_net;

typedef struct iso_settings_wpa {
    char path[ISO_SETTINGS_PATH_MAX];
    char head[512];                 /* preamble before the first network */
    iso_settings_net nets[ISO_SETTINGS_NET_MAX];
    int count;
} iso_settings_wpa;

int iso_settings_wpa_load(iso_settings_wpa *w, const char *path);
int iso_settings_wpa_add(iso_settings_wpa *w, const char *ssid,
                         const char *psk);
int iso_settings_wpa_set(iso_settings_wpa *w, int idx, const char *ssid,
                         const char *psk);
int iso_settings_wpa_remove(iso_settings_wpa *w, int idx);
int iso_settings_wpa_save(iso_settings_wpa *w);

/* ---- atomic write / volume -------------------------------------------- */

int iso_settings_write_atomic(const char *path, const char *data,
                              size_t len);
/* Loads a 0..100 integer; a missing or unparsable file yields 100 and
 * returns -1 (the desktop default). */
int iso_settings_volume_load(const char *path, int *out);
int iso_settings_volume_save(const char *path, int vol);

/* ---- GUI --------------------------------------------------------------- */

typedef struct iso_settings_gui iso_settings_gui;

typedef struct iso_settings_gui_cbs {
    void (*applied)(void *ud, const char *what);
} iso_settings_gui_cbs;

iso_settings_gui *iso_settings_gui_new(scene_client *cl, const char *base_dir);
void iso_settings_gui_free(iso_settings_gui *g);
void iso_settings_gui_set_cbs(iso_settings_gui *g,
                              const iso_settings_gui_cbs *cbs, void *ud);

/* Build the widget tree under `content` (a scene_app window content id).
 * x/y/w/h is the content node's absolute session-space rect. */
int iso_settings_gui_open(iso_settings_gui *g, scene_node_id content,
                          int32_t x, int32_t y, int32_t w, int32_t h);
int iso_settings_gui_tick(iso_settings_gui *g);
int iso_settings_gui_handle_activate(iso_settings_gui *g, scene_node_id id);
/* KEY_* evdev scancode + state (1=press) + modifier bits. Only consumed
 * while a text field is being edited. */
int iso_settings_gui_handle_key(iso_settings_gui *g, uint32_t key_code,
                                uint8_t state, uint8_t modifiers);

int iso_settings_gui_view(const iso_settings_gui *g);
const char *iso_settings_gui_status(const iso_settings_gui *g);
int iso_settings_gui_editing(const iso_settings_gui *g);

#endif /* ISO_SETTINGS_H */
