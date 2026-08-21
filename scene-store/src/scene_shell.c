/*
 * scene_shell.c — desktop shell layer.
 *
 * Creates and manages native scene store nodes for the desktop background,
 * panel/taskbar, app launcher, and per-window task buttons. All node
 * creation goes through the scene_client API; state queries go through
 * the scene_store read API.
 */
#include "scene_shell.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#if !defined(_WIN32)
#include <dirent.h>
#include <crypt.h>
#endif

/* ---- internal IDs ---------------------------------------------------- */
#define ID_BACKGROUND  10000u
#define ID_PANEL       10001u
#define ID_START_BTN   10002u
#define ID_CLOCK       10003u
#define ID_MENU        10004u
#define ID_TRAY        10005u   /* network tray label (right of clock)    */
#define ID_VOL_BTN     10006u   /* volume toggle button (left of tray)    */
#define ID_MENU_BASE   20000u   /* menu items: ID_MENU_BASE + i */
#define ID_RESTART_ITEM  (ID_MENU_BASE + SCENE_SHELL_MAX_APPS)
#define ID_POWEROFF_ITEM (ID_MENU_BASE + SCENE_SHELL_MAX_APPS + 1)
#define ID_TASK_BASE   30000u   /* task buttons: ID_TASK_BASE + seq */
#define ID_APP_TASK_BASE 40000u /* app task buttons: base + slot */
#define APP_TASK_MAX   64u      /* app task slots (fixed-size array) */

/* Cross-app search overlay (Super+S). Backdrop + query label + up to
 * 8 hit rows, all shell-session nodes (layer 0). */
#define ID_OVL_BG      50000u
#define ID_OVL_QUERY   50001u
#define ID_OVL_HITS    50010u   /* hit rows: ID_OVL_HITS + i */
#define SCENE_SHELL_OVL_HITS 8u
#define SCENE_SHELL_OVL_W    480
#define SCENE_SHELL_OVL_H    320
#define OVL_HOTKEY_CODE 31u     /* set-1 scancode of 's' (Super+S) */

/* Toast notifications (lazy, created on first notify). */
#define ID_TOAST       60000u
#define ID_TOAST_TITLE 60001u
#define ID_TOAST_BODY  60002u

/* Desktop lock screen (lazy, created on lock). */
#define ID_LOCK_BG      61000u   /* full-screen backdrop                   */
#define ID_LOCK_TITLE   61001u   /* "screen locked"                        */
#define ID_LOCK_PWD     61002u   /* password dots                          */
#define ID_LOCK_HINT    61003u   /* hint / wrong-password notice           */
#define LOCK_HOTKEY_CODE 38u     /* set-1 scancode of 'l' (Super+L)        */
#define LOCK_PWD_MAX     64u

/* Compositor style slots owned by the shell theme. Slots 1 (hover) and
 * 2 (active) belong to iso_drm (scene_compositor_setup_hover_style /
 * setup_active_style); the shell uses 3..7 and grows the table to 8.
 * Style 0 = role default, so themed nodes must never be reset to 0. */
#define SHELL_STYLE_BG      3u
#define SHELL_STYLE_PANEL   4u
#define SHELL_STYLE_BUTTON  5u
#define SHELL_STYLE_LABEL   6u
#define SHELL_STYLE_MENU    7u
#define SHELL_STYLE_LOCK    8u   /* lock screen backdrop                    */
#define SHELL_STYLE_SLOTS   9u

/* Window resize gesture: edge band width in px, edge flags (corner =
 * both bits), and the minimum window size. */
#define RESIZE_BAND        6
#define RESIZE_EDGE_RIGHT  1u
#define RESIZE_EDGE_BOTTOM 2u
#define RESIZE_MIN_W       96
#define RESIZE_MIN_H       64

/* ---- internal state -------------------------------------------------- */
#define MAX_TASKS 256

/* One cross-app search hit: layer + node + a copy of the matched text
 * (scene_store_node_texts views point into store memory, so the hit
 * keeps its own copy for the row label). */
typedef struct ovl_hit {
    int           layer;
    scene_node_id node_id;
    scene_text_id text_id;
    char          text[64];
} ovl_hit;

typedef struct task_entry {
    scene_node_id window_id;    /* the WINDOW node in the store            */
    scene_node_id button_id;    /* the BUTTON node in the panel            */
    uint8_t       active;       /* 1 = present in current reconciliation   */
} task_entry;

/* App-layer task entry: one per (layer, WINDOW) on layers 1..n. The
 * window stays in the taskbar while minimized (hidden), so it can be
 * restored. button_id is always ID_APP_TASK_BASE + slot.                */
typedef struct app_task_entry {
    uint8_t       used;         /* slot occupied (freed slots reused)      */
    uint8_t       active;       /* present in current reconciliation       */
    int           layer;        /* app layer index (>= 1)                 */
    scene_node_id window_id;
    scene_node_id button_id;
} app_task_entry;

struct scene_shell {
    scene_client        *client;
    scene_store         *store;
    scene_compositor    *cp;         /* for texture registration (may be NULL) */
    scene_shell_config   cfg;
    int32_t              width, height;
    int                  built;

    /* task list reconciliation */
    task_entry           tasks[MAX_TASKS];
    uint32_t             task_count;
    /* app-layer task buttons (ID_APP_TASK_BASE + slot) */
    app_task_entry       app_tasks[APP_TASK_MAX];

    /* launcher state */
    uint8_t              menu_open;
    uint32_t             last_clock_min;  /* for clock update debounce */
    scene_shell_launch_fn launch_fn;     /* host app factory hook      */
    void                 *launch_ud;

    /* hover tracking */
    scene_node_id        hovered_id;      /* current node under cursor   */
    scene_style_ref      hover_style;     /* compositor style slot for hover */
    scene_style_ref      active_style;    /* compositor style slot for focused */
    scene_node_id        active_task_id;  /* task btn with active style  */

    /* window move (drag title bar) */
    scene_node_id        moving_titlebar; /* titlebar being dragged      */
    int32_t              move_off_x, move_off_y; /* offset from pointer to window origin */

    /* window resize (drag window edge/corner) */
    scene_node_id        resizing_window;  /* WINDOW node being resized  */
    uint8_t              resize_edges;     /* RESIZE_EDGE_* bitmask      */
    int32_t              resize_orig_w, resize_orig_h; /* rect at press  */
    int32_t              resize_orig_px, resize_orig_py; /* pointer at press */

    /* wallpaper */
    scene_wallpaper     *wp;
    uint32_t             wp_tex_ref;      /* texture ref for bg wallpaper */
    int                  wp_tex_registered;

    /* network tray label */
    char                 tray_text[16];   /* cached label text            */
    time_t               last_tray_probe; /* 0 = probe immediately        */

    /* cross-app search overlay (Super+S) */
    uint8_t              ovl_open;        /* overlay showing              */
    uint8_t              ovl_built;       /* overlay nodes created        */
    int32_t              ovl_y;           /* overlay top (centered)       */
    char                 ovl_query[64];
    uint32_t             ovl_hit_count;   /* live hits (<= 8)             */
    uint32_t             ovl_sel;         /* selected hit row             */
    ovl_hit              ovl_hits[SCENE_SHELL_OVL_HITS];

    /* toast notifications (lazily created, top-right) */
    scene_node_id        toast_id;     /* 0 = never created                */
    uint32_t             toast_life;   /* ticks until hide (0 = hidden)    */
    uint8_t              toast_up;     /* 1 = currently visible            */

    /* volume toggle button */
    uint8_t              vol_muted;    /* 1 = muted (vol file "0")         */

    /* desktop lock (Super+L / autolock) */
    uint8_t              locked;       /* 1 = desktop locked                */
    uint8_t              lock_built;   /* lock screen nodes created         */
    char                 lock_pwd[LOCK_PWD_MAX];
    uint32_t             lock_pwd_len;
    time_t               last_activity; /* sec of last input (autolock)     */
    scene_shell_lock_check_fn lock_check;
    void                 *lock_check_ud;

    /* system menu (Restart / Power Off) host hook */
    scene_shell_power_fn power_fn;
    void                 *power_ud;
};

/* ---- helpers --------------------------------------------------------- */

static int emit_create(scene_shell *sh, scene_node_id parent,
                       scene_node_id id, uint16_t role,
                       int32_t x, int32_t y, int32_t w, int32_t h,
                       uint8_t flags)
{
    scene_rect r = { x, y, w, h };
    return scene_client_create_node(sh->client, parent, id, role, &r, flags);
}

static int emit_text(scene_shell *sh, scene_node_id id,
                     scene_text_id slot, const char *text, uint32_t len)
{
    return scene_client_set_text(sh->client, id, slot, text, len);
}

static int emit_rect(scene_shell *sh, scene_node_id id,
                     int32_t x, int32_t y, int32_t w, int32_t h)
{
    scene_rect r = { x, y, w, h };
    return scene_client_set_rect(sh->client, id, &r);
}

static int emit_flags(scene_shell *sh, scene_node_id id, uint8_t flags)
{
    return scene_client_set_flags(sh->client, id, flags);
}

static int emit_destroy(scene_shell *sh, scene_node_id id)
{
    return scene_client_destroy_node(sh->client, id);
}

/* ---- network tray probe ------------------------------------------------ */

#if !defined(_WIN32)
/* Linux: any non-loopback interface with carrier = up. */
static const char *shell_tray_probe_impl(void)
{
    static char res[8];
    DIR *d = opendir("/sys/class/net");
    if (!d) return "no net";
    struct dirent *e;
    int up = 0;
    while ((e = readdir(d)) && !up) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 ||
            strcmp(e->d_name, "lo") == 0)
            continue;
        char path[300];
        if (strlen(e->d_name) > 240)
            continue;
        snprintf(path, sizeof(path), "/sys/class/net/%s/carrier",
                 e->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            int c = fgetc(f);
            fclose(f);
            if (c == '1') up = 1;
        }
    }
    closedir(d);
    strcpy(res, up ? "net" : "no net");
    return res;
}
#else
static const char *shell_tray_probe_impl(void)
{
    return "NA";
}
#endif

const char *(*scene_shell_tray_probe)(void) = shell_tray_probe_impl;

/* Idle clock for the autolock timeout (NULL = time(NULL)). */
time_t (*scene_shell_clock_probe)(void) = NULL;

static time_t now_sec(void)
{
    return scene_shell_clock_probe ? scene_shell_clock_probe() : time(NULL);
}

/* Default /etc/shadow checker (defined with the desktop lock below). */
static int lock_check_shadow(void *ud, const char *password);
/* Lock screen layout (defined with the desktop lock below). */
static void lock_layout(scene_shell *sh);

/* Volume state file: the OS audio service reads this. Windows builds
 * (tests, preview) write into build/; the ISO uses the tmpfs run dir. */
static const char *vol_file_path(void)
{
#if defined(_WIN32)
    return "build/scene-volume";
#else
    return "/run/scene-volume";
#endif
}

/* ---- config ---------------------------------------------------------- */

void scene_shell_config_defaults(scene_shell_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->bg_color        = 0xFF1A1A2E;
    cfg->panel_height    = 32;
    cfg->panel_color     = 0xFF16213E;
    cfg->panel_border    = 0xFF0F3460;
    cfg->panel_border_w  = 1;
    cfg->panel_radius    = 4;
    cfg->button_color    = 0xFF1A1A2E;
    cfg->button_border   = 0xFF533483;
    cfg->button_text     = 0xFFFFFFFF;
    cfg->hover_color     = 0xFF2A2A4E;
    cfg->label_text      = 0xFFE0E0E0;
    cfg->menu_color      = 0xFF16213E;
    cfg->menu_border     = 0xFF0F3460;
    cfg->menu_item_color = 0xFF1A1A2E;
    cfg->menu_item_text  = 0xFFFFFFFF;
    cfg->autolock_sec    = 300;    /* 5 min idle -> lock (0 = never) */
    cfg->launcher_app_count = 0;
    cfg->wallpaper_path[0] = '\0';
    cfg->wallpaper_mode = SCENE_WP_STATIC;
    cfg->wallpaper_slideshow_dir[0] = '\0';
    cfg->wallpaper_slideshow_sec = 15.0f;
    cfg->wallpaper_slideshow_fade = 1.0f;
    cfg->wallpaper_speed = 1.0f;
}

/* Parse hex string "0xAARRGGBB" or "0xRRGGBB" to uint32_t.
 * Returns the parsed value, or 0 on error. */
static uint32_t parse_hex(const char *s)
{
    if (!s) return 0;
    unsigned long v = strtoul(s, NULL, 16);
    return (uint32_t)v;
}

int scene_shell_config_load(scene_shell_config *cfg, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    while (fgets(line, (int)sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        /* strip trailing newline */
        char *nl = strchr(val, '\n');
        if (nl) *nl = '\0';

        if (strcmp(key, "background_color") == 0 ||
            strcmp(key, "bg_color") == 0)
            cfg->bg_color = parse_hex(val);
        else if (strcmp(key, "panel_height") == 0)
            cfg->panel_height = (uint32_t)strtoul(val, NULL, 10);
        else if (strcmp(key, "panel_color") == 0)
            cfg->panel_color = parse_hex(val);
        else if (strcmp(key, "panel_border") == 0)
            cfg->panel_border = parse_hex(val);
        else if (strcmp(key, "panel_border_w") == 0)
            cfg->panel_border_w = (uint8_t)strtoul(val, NULL, 10);
        else if (strcmp(key, "panel_radius") == 0)
            cfg->panel_radius = (uint8_t)strtoul(val, NULL, 10);
        else if (strcmp(key, "button_color") == 0)
            cfg->button_color = parse_hex(val);
        else if (strcmp(key, "button_border") == 0)
            cfg->button_border = parse_hex(val);
        else if (strcmp(key, "button_text") == 0)
            cfg->button_text = parse_hex(val);
        else if (strcmp(key, "hover_color") == 0)
            cfg->hover_color = parse_hex(val);
        else if (strcmp(key, "label_text") == 0)
            cfg->label_text = parse_hex(val);
        else if (strcmp(key, "clock_12h") == 0)
            cfg->clock_12h = (uint8_t)strtoul(val, NULL, 10) & 1;
        else if (strcmp(key, "autolock_sec") == 0)
            cfg->autolock_sec = (uint32_t)strtoul(val, NULL, 10);
        else if (strcmp(key, "menu_color") == 0)
            cfg->menu_color = parse_hex(val);
        else if (strcmp(key, "menu_border") == 0)
            cfg->menu_border = parse_hex(val);
        else if (strcmp(key, "menu_item_color") == 0)
            cfg->menu_item_color = parse_hex(val);
        else if (strcmp(key, "menu_item_text") == 0)
            cfg->menu_item_text = parse_hex(val);
        else if (strcmp(key, "launcher_apps") == 0) {
            /* comma-separated list */
            cfg->launcher_app_count = 0;
            char *tok = strtok(val, ",");
            while (tok && cfg->launcher_app_count < SCENE_SHELL_MAX_APPS) {
                while (*tok == ' ') tok++;
                strncpy(cfg->launcher_apps[cfg->launcher_app_count],
                        tok, 63);
                cfg->launcher_apps[cfg->launcher_app_count][63] = '\0';
                cfg->launcher_app_count++;
                tok = strtok(NULL, ",");
            }
        }
        else if (strcmp(key, "wallpaper_path") == 0)
            strncpy(cfg->wallpaper_path, val, 255);
        else if (strcmp(key, "wallpaper_mode") == 0) {
            if (strcmp(val, "static") == 0) cfg->wallpaper_mode = SCENE_WP_STATIC;
            else if (strcmp(val, "ken_burns") == 0) cfg->wallpaper_mode = SCENE_WP_KEN_BURNS;
            else if (strcmp(val, "slideshow") == 0) cfg->wallpaper_mode = SCENE_WP_SLIDESHOW;
            else if (strcmp(val, "plasma") == 0) cfg->wallpaper_mode = SCENE_WP_PLASMA;
            else if (strcmp(val, "aurora") == 0) cfg->wallpaper_mode = SCENE_WP_AURORA;
            else if (strcmp(val, "gradient_mesh") == 0) cfg->wallpaper_mode = SCENE_WP_GRADIENT_MESH;
        }
        else if (strcmp(key, "wallpaper_slideshow_dir") == 0)
            strncpy(cfg->wallpaper_slideshow_dir, val, 255);
        else if (strcmp(key, "wallpaper_slideshow_sec") == 0)
            cfg->wallpaper_slideshow_sec = (float)atof(val);
        else if (strcmp(key, "wallpaper_slideshow_fade") == 0)
            cfg->wallpaper_slideshow_fade = (float)atof(val);
        else if (strcmp(key, "wallpaper_speed") == 0)
            cfg->wallpaper_speed = (float)atof(val);
    }
    fclose(f);
    return 0;
}

/* ---- lifecycle ------------------------------------------------------- */

scene_shell *scene_shell_new(scene_client *client, scene_store *store,
                             scene_compositor *cp,
                             const scene_shell_config *cfg)
{
    scene_shell *sh = (scene_shell *)calloc(1, sizeof(scene_shell));
    if (!sh) return NULL;
    sh->client = client;
    sh->store  = store;
    sh->cp     = cp;
    if (cfg) sh->cfg = *cfg;
    else     scene_shell_config_defaults(&sh->cfg);
    sh->last_activity = now_sec();
#if !defined(_WIN32)
    sh->lock_check = lock_check_shadow;
#else
    sh->lock_check = NULL;   /* Windows: no /etc/shadow; tests inject */
#endif
    return sh;
}

void scene_shell_free(scene_shell *sh)
{
    if (!sh) return;
    if (sh->wp) {
        scene_wallpaper_free(sh->wp);
        if (sh->cp && sh->wp_tex_registered)
            scene_compositor_release_texture(sh->cp, sh->wp_tex_ref);
    }
    free(sh);
}

void scene_shell_set_hover_style(scene_shell *sh, scene_style_ref ref)
{
    if (sh) sh->hover_style = ref;
}

void scene_shell_set_active_style(scene_shell *sh, scene_style_ref ref)
{
    if (sh) sh->active_style = ref;
}

void scene_shell_set_launch_cb(scene_shell *sh, scene_shell_launch_fn fn,
                               void *ud)
{
    if (!sh) return;
    sh->launch_fn = fn;
    sh->launch_ud = ud;
}

void scene_shell_set_power_cb(scene_shell *sh, scene_shell_power_fn fn,
                              void *ud)
{
    if (!sh) return;
    sh->power_fn = fn;
    sh->power_ud = ud;
}

/* ---- tree construction ----------------------------------------------- */

/* The style a node reverts to when its hover/active style is removed.
 * Menu and menu items use the menu theme, everything else the button
 * theme. Never 0 (role default) — that would lose the shell theme. */
static scene_style_ref base_style_for(scene_node_id id)
{
    if (id == ID_MENU || (id >= ID_MENU_BASE &&
                          id < ID_MENU_BASE + SCENE_SHELL_MAX_APPS) ||
        id == ID_RESTART_ITEM || id == ID_POWEROFF_ITEM)
        return SHELL_STYLE_MENU;
    return SHELL_STYLE_BUTTON;
}

/* Push the config colors into the compositor style table and pin the
 * shell nodes to those slots. Re-running this with a changed config
 * re-themes live: scene_compositor_set_style dirties every visible node
 * referencing the slot, so the next frame repaints with the new colors. */
static void apply_theme(scene_shell *sh)
{
    if (!sh || !sh->cp) return;
    scene_compositor_set_style_count(sh->cp, SHELL_STYLE_SLOTS);

    scene_style st;

    memset(&st, 0, sizeof st);
    st.fill = sh->cfg.bg_color;
    st.text = sh->cfg.label_text;
    scene_compositor_set_style(sh->cp, SHELL_STYLE_BG, &st);

    memset(&st, 0, sizeof st);
    st.fill     = sh->cfg.panel_color;
    st.border   = sh->cfg.panel_border;
    st.border_w = sh->cfg.panel_border_w;
    st.text     = sh->cfg.label_text;
    st.radius   = sh->cfg.panel_radius;
    scene_compositor_set_style(sh->cp, SHELL_STYLE_PANEL, &st);

    memset(&st, 0, sizeof st);
    st.fill     = sh->cfg.button_color;
    st.border   = sh->cfg.button_border;
    st.border_w = 1;
    st.text     = sh->cfg.button_text;
    st.radius   = 4;
    scene_compositor_set_style(sh->cp, SHELL_STYLE_BUTTON, &st);

    memset(&st, 0, sizeof st);
    st.text = sh->cfg.label_text;
    scene_compositor_set_style(sh->cp, SHELL_STYLE_LABEL, &st);

    memset(&st, 0, sizeof st);
    st.fill     = sh->cfg.menu_color;
    st.border   = sh->cfg.menu_border;
    st.border_w = 1;
    st.text     = sh->cfg.menu_item_text;
    st.radius   = 4;
    scene_compositor_set_style(sh->cp, SHELL_STYLE_MENU, &st);

    memset(&st, 0, sizeof st);
    st.fill     = 0xFF0A0A14;   /* lock backdrop: darker than the desktop */
    st.border   = 0xFF2A2A4E;
    st.border_w = 1;
    st.text     = 0xFFE0E0E0;
    scene_compositor_set_style(sh->cp, SHELL_STYLE_LOCK, &st);

    if (!sh->client) return;
    scene_client_set_style(sh->client, ID_BACKGROUND, SHELL_STYLE_BG);
    scene_client_set_style(sh->client, ID_PANEL, SHELL_STYLE_PANEL);
    scene_client_set_style(sh->client, ID_START_BTN, SHELL_STYLE_BUTTON);
    scene_client_set_style(sh->client, ID_VOL_BTN, SHELL_STYLE_BUTTON);
    scene_client_set_style(sh->client, ID_CLOCK, SHELL_STYLE_LABEL);
    scene_client_set_style(sh->client, ID_TRAY, SHELL_STYLE_LABEL);
    scene_client_set_style(sh->client, ID_MENU, SHELL_STYLE_MENU);
    scene_client_set_style(sh->client, ID_RESTART_ITEM, SHELL_STYLE_MENU);
    scene_client_set_style(sh->client, ID_POWEROFF_ITEM, SHELL_STYLE_MENU);
    uint32_t i;
    for (i = 0; i < sh->cfg.launcher_app_count && i < SCENE_SHELL_MAX_APPS; i++)
        scene_client_set_style(sh->client, ID_MENU_BASE + i, SHELL_STYLE_MENU);

}

int scene_shell_build(scene_shell *sh, int32_t width, int32_t height)
{
    if (!sh || sh->built) return -1;
    sh->width  = width;
    sh->height = height;

    int r;
    uint32_t ph = sh->cfg.panel_height;

    /* Desktop background — full screen, lowest z-order */
    r = emit_create(sh, SCENE_NO_PARENT, ID_BACKGROUND,
                    SCENE_ROLE_WINDOW, 0, 0, width, height,
                    SCENE_FLAG_VISIBLE);
    if (r != 0) return -1;

    /* Panel — bottom edge, full width */
    r = emit_create(sh, SCENE_NO_PARENT, ID_PANEL,
                    SCENE_ROLE_PANEL, 0, height - (int32_t)ph,
                    width, (int32_t)ph,
                    SCENE_FLAG_VISIBLE);
    if (r != 0) return -1;

    /* Start button — left side of panel (absolute screen coords) */
    int32_t panel_y = height - (int32_t)ph;
    r = emit_create(sh, ID_PANEL, ID_START_BTN,
                    SCENE_ROLE_BUTTON, 2, panel_y + 2,
                    (int32_t)ph - 4, (int32_t)ph - 4,
                    SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    if (r != 0) return -1;
    r = emit_text(sh, ID_START_BTN, 1, "Menu", 4);
    if (r != 0) return -1;

    /* Clock label — right side of panel (absolute screen coords) */
    r = emit_create(sh, ID_PANEL, ID_CLOCK,
                    SCENE_ROLE_LABEL, width - 100, panel_y + 2,
                    96, (int32_t)ph - 4,
                    SCENE_FLAG_VISIBLE);
    if (r != 0) return -1;

    /* Network tray label — just left of the clock (not interactive) */
    r = emit_create(sh, ID_PANEL, ID_TRAY,
                    SCENE_ROLE_LABEL, width - 100 - 56, panel_y + 2,
                    52, (int32_t)ph - 4,
                    SCENE_FLAG_VISIBLE);
    if (r != 0) return -1;
    r = emit_text(sh, ID_TRAY, 1, "NA", 2);
    if (r != 0) return -1;

    /* Volume button — left of the tray/clock cluster (40x22, vertically
     * centered in the panel; 6px gap to the tray label) */
    r = emit_create(sh, ID_PANEL, ID_VOL_BTN,
                    SCENE_ROLE_BUTTON, width - 202,
                    panel_y + ((int32_t)ph - 22) / 2,
                    40, 22,
                    SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    if (r != 0) return -1;
    r = emit_text(sh, ID_VOL_BTN, 1, "vol", 3);
    if (r != 0) return -1;

    /* Launcher menu — initially hidden, positioned above start button.
     * Height covers launcher items + the two system items (Restart,
     * Power Off). */
    uint32_t menu_items = sh->cfg.launcher_app_count;
    if (menu_items > SCENE_SHELL_MAX_APPS) menu_items = SCENE_SHELL_MAX_APPS;
    int32_t menu_h = 8 + (int32_t)(menu_items + 2) * 28;
    int32_t menu_y = height - (int32_t)ph - menu_h;
    r = emit_create(sh, SCENE_NO_PARENT, ID_MENU,
                    SCENE_ROLE_MENU, 0, menu_y,
                    160, menu_h,
                    0);  /* not visible initially */
    if (r != 0) return -1;

    /* Pre-create launcher item slots (hidden until menu is opened) */
    uint32_t i;
    for (i = 0; i < menu_items; i++) {
        uint32_t item_id = ID_MENU_BASE + i;
        r = emit_create(sh, ID_MENU, item_id,
                        SCENE_ROLE_BUTTON, 0 + 4,
                        menu_y + 4 + (int32_t)i * 28,
                        152, 24,
                        SCENE_FLAG_FOCUSABLE);  /* no VISIBLE yet */
        if (r != 0) return -1;
        r = emit_text(sh, item_id, 1,
                      sh->cfg.launcher_apps[i],
                      (uint32_t)strlen(sh->cfg.launcher_apps[i]));
        if (r != 0) return -1;
    }

    /* System menu items: Restart, Power Off (below the launcher list) */
    r = emit_create(sh, ID_MENU, ID_RESTART_ITEM,
                    SCENE_ROLE_BUTTON, 4,
                    menu_y + 4 + (int32_t)menu_items * 28,
                    152, 24,
                    SCENE_FLAG_FOCUSABLE);
    if (r != 0) return -1;
    r = emit_text(sh, ID_RESTART_ITEM, 1, "Restart", 7);
    if (r != 0) return -1;
    r = emit_create(sh, ID_MENU, ID_POWEROFF_ITEM,
                    SCENE_ROLE_BUTTON, 4,
                    menu_y + 4 + (int32_t)(menu_items + 1) * 28,
                    152, 24,
                    SCENE_FLAG_FOCUSABLE);
    if (r != 0) return -1;
    r = emit_text(sh, ID_POWEROFF_ITEM, 1, "Power Off", 9);
    if (r != 0) return -1;

    /* --- Wallpaper --- */
    if (sh->cp) {
        sh->wp = scene_wallpaper_new((uint32_t)width, (uint32_t)height);
        if (sh->wp) {
            sh->wp_tex_ref = 0xFF000001; /* unique ref */
            sh->wp_tex_registered = 0;
            scene_wp_params p;
            memset(&p, 0, sizeof(p));
            p.speed = sh->cfg.wallpaper_speed;

            switch (sh->cfg.wallpaper_mode) {
            case SCENE_WP_STATIC:
                if (sh->cfg.wallpaper_path[0])
                    scene_wallpaper_set_static(sh->wp, sh->cfg.wallpaper_path);
                break;
            case SCENE_WP_KEN_BURNS:
                if (sh->cfg.wallpaper_path[0])
                    scene_wallpaper_set_ken_burns(sh->wp, sh->cfg.wallpaper_path,
                                                  0, 0);
                break;
            case SCENE_WP_PLASMA:
            case SCENE_WP_AURORA:
            case SCENE_WP_GRADIENT_MESH:
                scene_wallpaper_set_procedural(sh->wp, sh->cfg.wallpaper_mode, &p);
                break;
            default:
                break;
            }
        }
    }

    sh->built = 1;
    apply_theme(sh);

    /* OS key grabs: Super+S opens the cross-app search overlay and PrtSc
     * (SYSRQ, no mods) captures the screen — both from anywhere, even
     * while an app session has keyboard focus. Grabs are registered once
     * at build time, not per gesture. */
    if (sh->cp) {
        scene_compositor_key_grab(sh->cp, OVL_HOTKEY_CODE,
                                  SCENE_MOD_SUPER);
        scene_compositor_key_grab(sh->cp, SCENE_KEY_SYSRQ, 0);
        scene_compositor_key_grab(sh->cp, LOCK_HOTKEY_CODE,
                                  SCENE_MOD_SUPER);
    }
    return 0;
}

/* ---- task list reconciliation ---------------------------------------- */

struct walk_ctx {
    scene_store *store;
    task_entry  *tasks;
    uint32_t     task_count;
    uint32_t     task_cap;
};

static int walk_cb(scene_node_id id, void *ud)
{
    struct walk_ctx *ctx = ud;
    /* Skip shell-owned nodes (IDs >= ID_BACKGROUND) */
    if (id >= ID_BACKGROUND) return 0;
    scene_node_vis v;
    if (scene_store_node_vis(ctx->store, id, &v) != 0) return 0;
    if (!(v.flags & SCENE_FLAG_VISIBLE)) return 0;
    if (v.role != SCENE_ROLE_WINDOW) return 0;
    if (ctx->task_count >= ctx->task_cap) return 0;

    /* Check if already tracked */
    uint32_t i;
    for (i = 0; i < ctx->task_count; i++) {
        if (ctx->tasks[i].window_id == id) {
            ctx->tasks[i].active = 1;
            return 0;
        }
    }

    /* New window — add to list */
    ctx->tasks[ctx->task_count].window_id = id;
    ctx->tasks[ctx->task_count].button_id = 0;
    ctx->tasks[ctx->task_count].active = 1;
    ctx->task_count++;
    return 0;
}

/* ---- app-layer task reconciliation ------------------------------------ */

struct app_walk_ctx {
    scene_shell     *sh;
    app_task_entry  *tasks;
    int              layer;
};

/* Track every WINDOW node of one app layer. Unlike layer-0 windows,
 * minimized (hidden) ones stay tracked so their task button persists
 * and can restore the window. */
static int app_walk_cb(scene_node_id id, void *ud)
{
    struct app_walk_ctx *aw = ud;
    /* App layers live in their own session namespace (scene_app windows
     * are id >= 40000); the shell's own ids (>= ID_BACKGROUND) exist only
     * in layer 0, so no id filter applies here — every WINDOW role in the
     * layer is a tracked task (stale nodes are filtered by node_vis).   */
    scene_store *lst = NULL;
    if (aw->sh->cp)
        lst = scene_compositor_layer_store(aw->sh->cp, aw->layer);
    if (!lst) return 0;
    scene_node_vis v;
    if (scene_store_node_vis(lst, id, &v) != 0) return 0;
    if (v.stale) return 0;
    if (v.role != SCENE_ROLE_WINDOW) return 0;
    uint32_t k;
    for (k = 0; k < APP_TASK_MAX; k++) {
        if (aw->tasks[k].used && aw->tasks[k].window_id == id) {
            aw->tasks[k].active = 1;
            return 0;
        }
    }
    for (k = 0; k < APP_TASK_MAX; k++) {
        if (!aw->tasks[k].used) {
            aw->tasks[k].used = 1;
            aw->tasks[k].active = 1;
            aw->tasks[k].layer = aw->layer;
            aw->tasks[k].window_id = id;
            aw->tasks[k].button_id = 0;   /* the tick's create phase owns */
            return 0;                     /* the button id, as layer 0   */
        }
    }
    return 0;
}

int scene_shell_tick(scene_shell *sh)
{
    fprintf(stderr, "DEBUG: shell_tick sh=%p store=%p cp=%p built=%d\n", (void*)sh, (void*)(sh?sh->store:0), (void*)(sh?sh->cp:0), sh?sh->built:0); fflush(stderr);
    if (!sh || !sh->built) return -1;

    /* --- Clock update --- */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    unsigned int cur_min = (unsigned int)tm->tm_hour * 60 +
                           (unsigned int)tm->tm_min;
    if (cur_min != sh->last_clock_min) {
        char buf[16];
        int h = tm->tm_hour;
        if (sh->cfg.clock_12h) {
            int h12 = h % 12;
            if (h12 == 0) h12 = 12;
            snprintf(buf, sizeof(buf), "%2d:%02d%c",
                     h12, tm->tm_min, h < 12 ? 'a' : 'p');
        } else {
            snprintf(buf, sizeof(buf), "%02d:%02d", h, tm->tm_min);
        }
        emit_text(sh, ID_CLOCK, 1, buf, (uint32_t)strlen(buf));
        sh->last_clock_min = cur_min;
    }

    /* --- Auto-lock: idle timeout (config; 0 = never) --- */
    if (!sh->locked && sh->cfg.autolock_sec > 0 &&
        now_sec() - sh->last_activity >= (time_t)sh->cfg.autolock_sec)
        scene_shell_lock(sh);

    /* --- Toast lifetime (frame-counted, deterministic) --- */
    if (sh->toast_life > 0) {
        sh->toast_life--;
        if (sh->toast_life == 0 && sh->toast_up) {
            emit_flags(sh, ID_TOAST, 0);
            emit_flags(sh, ID_TOAST_TITLE, 0);
            emit_flags(sh, ID_TOAST_BODY, 0);
            sh->toast_up = 0;   /* idempotent: hide emitted once */
        }
    }

    /* --- Network tray text (probe at most every 2 s, emit on change) --- */
    if (sh->last_tray_probe == 0 || now - sh->last_tray_probe >= 2) {
        sh->last_tray_probe = now;
        const char *probe = scene_shell_tray_probe ?
            scene_shell_tray_probe() : "NA";
        size_t plen = strlen(probe);
        if (plen >= sizeof(sh->tray_text))
            plen = sizeof(sh->tray_text) - 1;
        if (plen != strlen(sh->tray_text) ||
            memcmp(sh->tray_text, probe, plen) != 0) {
            memcpy(sh->tray_text, probe, plen);
            sh->tray_text[plen] = '\0';
            emit_text(sh, ID_TRAY, 1, sh->tray_text, (uint32_t)plen);
        }
    }

    /* --- Task list reconciliation --- */
    /* Mark all existing tasks inactive, then walk store to find windows */
    uint32_t i;
    for (i = 0; i < sh->task_count; i++)
        sh->tasks[i].active = 0;

    struct walk_ctx ctx;
    ctx.store      = sh->store;
    ctx.tasks      = sh->tasks;
    ctx.task_count = sh->task_count;
    ctx.task_cap   = MAX_TASKS;
    scene_store_walk(sh->store, walk_cb, &ctx);
    sh->task_count = ctx.task_count;

    /* App-layer reconciliation: walk every attached app layer store. */
    uint32_t j;
    for (j = 0; j < APP_TASK_MAX; j++)
        sh->app_tasks[j].active = 0;
    if (sh->cp) {
        int nlay = scene_compositor_layer_count(sh->cp);
        for (i = 1; i < (uint32_t)nlay; i++) {
            struct app_walk_ctx aw;
            aw.sh    = sh;
            aw.tasks = sh->app_tasks;
            aw.layer = (int)i;
            scene_store_walk(scene_compositor_layer_store(sh->cp, (int)i),
                             app_walk_cb, &aw);
        }
    }

    /* Create buttons for new windows, destroy buttons for gone windows,
     * and refresh text for existing buttons (window title may change). */
    uint32_t next_task_id = ID_TASK_BASE;
    uint32_t ph = sh->cfg.panel_height;
    uint32_t btn_x = (ph - 4) + 8;  /* after start button */
    for (i = 0; i < sh->task_count; i++) {
        if (!sh->tasks[i].active) {
            if (sh->tasks[i].button_id != 0) {
                emit_destroy(sh, sh->tasks[i].button_id);
                sh->tasks[i].button_id = 0;
            }
            continue;
        }
        if (sh->tasks[i].button_id == 0) {
            /* New window — create task button (absolute screen coords) */
            scene_node_id btn_id = next_task_id++;
            int32_t bw = 100;
            int32_t bx = (int32_t)btn_x +
                         (int32_t)(i * (uint32_t)(bw + 4));
            int32_t panel_y = sh->height - (int32_t)sh->cfg.panel_height;
            emit_create(sh, ID_PANEL, btn_id,
                        SCENE_ROLE_BUTTON, bx, panel_y + 2, bw,
                        (int32_t)ph - 4,
                        SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
            scene_client_set_style(sh->client, btn_id, SHELL_STYLE_BUTTON);
            sh->tasks[i].button_id = btn_id;
        }
        /* Refresh button text from window title */
        scene_node_text_vis tv[16];
        int nt = scene_store_node_texts(sh->store,
                                        sh->tasks[i].window_id,
                                        tv, 16);
        if (nt > 0 && tv[0].len > 0) {
            uint32_t len = tv[0].len;
            if (len > 30) len = 30;
            emit_text(sh, sh->tasks[i].button_id, 1, tv[0].data, len);
        }
    }

    /* App task buttons: position after the layer-0 buttons; a minimized
     * (hidden) window keeps its button so it can be restored. */
    uint32_t app_off = sh->task_count;
    for (j = 0; j < APP_TASK_MAX; j++) {
        app_task_entry *at = &sh->app_tasks[j];
        if (!at->used) continue;
        if (!at->active) {
            if (at->button_id != 0) {
                emit_destroy(sh, at->button_id);
                at->button_id = 0;
            }
            at->used = 0;   /* slot is free for reuse */
            continue;
        }
        if (at->button_id == 0) {
            at->button_id = ID_APP_TASK_BASE + j;
            int32_t bx = (int32_t)btn_x +
                         (int32_t)(app_off * (uint32_t)(100 + 4));
            int32_t panel_y = sh->height - (int32_t)sh->cfg.panel_height;
            emit_create(sh, ID_PANEL, at->button_id,
                        SCENE_ROLE_BUTTON, bx, panel_y + 2, 100,
                        (int32_t)ph - 4,
                        SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
            scene_client_set_style(sh->client, at->button_id,
                                   SHELL_STYLE_BUTTON);
        }
        app_off++;
        /* Refresh button text from the window title on the app layer */
        scene_store *lst = sh->cp ?
            scene_compositor_layer_store(sh->cp, at->layer) : NULL;
        if (lst) {
            scene_node_text_vis tv[16];
            int nt = scene_store_node_texts(lst, at->window_id, tv, 16);
            if (nt > 0 && tv[0].len > 0) {
                uint32_t len = tv[0].len;
                if (len > 30) len = 30;
                emit_text(sh, at->button_id, 1, tv[0].data, len);
            }
        }
    }

    /* --- Active task highlighting --- */
    if (sh->active_style != 0) {
        scene_node_id focused = scene_store_focus(sh->store);
        scene_node_id new_active_btn = 0;
        for (i = 0; i < sh->task_count; i++) {
            if (sh->tasks[i].window_id == focused &&
                sh->tasks[i].button_id != 0) {
                new_active_btn = sh->tasks[i].button_id;
                break;
            }
        }
        /* App layers: focus is per-layer engine state (host WM) */
        if (new_active_btn == 0 && sh->cp) {
            for (j = 0; j < APP_TASK_MAX; j++) {
                app_task_entry *at = &sh->app_tasks[j];
                if (!at->used || at->button_id == 0) continue;
                scene_store *lst =
                    scene_compositor_layer_store(sh->cp, at->layer);
                if (lst && scene_store_focus(lst) == at->window_id) {
                    new_active_btn = at->button_id;
                    break;
                }
            }
        }
        if (new_active_btn != sh->active_task_id) {
            /* Revert old active button */
            if (sh->active_task_id != 0)
                scene_client_set_style(sh->client, sh->active_task_id,
                                       SHELL_STYLE_BUTTON);
            /* Apply active style to new focused button */
            if (new_active_btn != 0)
                scene_client_set_style(sh->client, new_active_btn,
                                       sh->active_style);
            sh->active_task_id = new_active_btn;
        }
    }

    /* --- Wallpaper tick --- */
    if (sh->wp && sh->cp) {
        if (scene_wallpaper_tick(sh->wp)) {
            uint32_t rw, rh;
            scene_wallpaper_render_size(sh->wp, &rw, &rh);
            const uint32_t *pixels = scene_wallpaper_pixels(sh->wp);
            scene_compositor_register_texture(sh->cp, sh->wp_tex_ref,
                                              rw, rh,
                                              SCENE_TEX_FMT_ARGB, 0, pixels);
            sh->wp_tex_registered = 1;
            scene_rect src = { 0, 0, (int32_t)rw, (int32_t)rh };
            scene_client_set_texture(sh->client, ID_BACKGROUND,
                                     sh->wp_tex_ref, &src, 1, 255);
        }
    }

    return 0;
}

/* ---- cross-app search overlay ------------------------------------------
 *
 * Super+S (OS key grab, registered at build) opens a shell-session
 * search overlay. Typing searches committed texts LIVE across every
 * layer (layer ascending, then the engine's document order); Enter or a
 * click on a hit row activates the hit through the host WM APIs on that
 * layer's store (host_focus, host_set_visible after restore) — zero
 * wire bytes into the app session. Escape closes; Backspace edits the
 * query; Up/Down move the selection (highlighted with the hover style
 * slot when one is set).                                         ---------- */

/* set-1 scancode -> lowercase letter; 0 = not a letter. */
static char ovl_letter(uint32_t code)
{
    switch (code) {
    case 30: return 'a'; case 48: return 'b'; case 46: return 'c';
    case 32: return 'd'; case 18: return 'e'; case 33: return 'f';
    case 34: return 'g'; case 35: return 'h'; case 23: return 'i';
    case 36: return 'j'; case 37: return 'k'; case 38: return 'l';
    case 50: return 'm'; case 49: return 'n'; case 24: return 'o';
    case 25: return 'p'; case 16: return 'q'; case 19: return 'r';
    case 31: return 's'; case 20: return 't'; case 22: return 'u';
    case 47: return 'v'; case 17: return 'w'; case 45: return 'x';
    case 21: return 'y'; case 44: return 'z';
    default: return 0;
    }
}

/* Printable scancodes for the query: letters/digits/space/period; shift
 * maps letters to uppercase. Returns 0 when the key is not printable. */
static char ovl_printable_char(uint32_t code, uint8_t mods)
{
    char c;

    if (code >= 2 && code <= 10) c = (char)('1' + (code - 2));
    else if (code == 11) c = '0';
    else if (code == 57) c = ' ';
    else if (code == 52) c = '.';
    else {
        c = ovl_letter(code);
        if (c == 0) return 0;
        if (mods & SCENE_MOD_SHIFT) c = (char)(c - 'a' + 'A');
    }
    return c;
}

/* Lazily create the overlay nodes (hidden until the first open). */
static int ovl_build(scene_shell *sh)
{
    int32_t x = (sh->width - SCENE_SHELL_OVL_W) / 2;
    int r;
    uint32_t i;

    sh->ovl_y = (sh->height - SCENE_SHELL_OVL_H) / 2;
    r = emit_create(sh, SCENE_NO_PARENT, ID_OVL_BG, SCENE_ROLE_MENU,
                    x, sh->ovl_y, SCENE_SHELL_OVL_W, SCENE_SHELL_OVL_H, 0);
    if (r != 0) return -1;
    scene_client_set_style(sh->client, ID_OVL_BG, SHELL_STYLE_MENU);

    r = emit_create(sh, ID_OVL_BG, ID_OVL_QUERY, SCENE_ROLE_LABEL,
                    x + 16, sh->ovl_y + 14, SCENE_SHELL_OVL_W - 32, 24, 0);
    if (r != 0) return -1;
    scene_client_set_style(sh->client, ID_OVL_QUERY, SHELL_STYLE_LABEL);

    for (i = 0; i < SCENE_SHELL_OVL_HITS; i++) {
        r = emit_create(sh, ID_OVL_BG, ID_OVL_HITS + i, SCENE_ROLE_BUTTON,
                        x + 16, sh->ovl_y + 44 + (int32_t)i * 28,
                        SCENE_SHELL_OVL_W - 32, 24, 0);
        if (r != 0) return -1;
        scene_client_set_style(sh->client, ID_OVL_HITS + i,
                               SHELL_STYLE_BUTTON);
    }
    return 0;
}

/* Reposition the overlay nodes (output resize). */
static void ovl_layout(scene_shell *sh)
{
    int32_t x = (sh->width - SCENE_SHELL_OVL_W) / 2;
    uint32_t i;

    sh->ovl_y = (sh->height - SCENE_SHELL_OVL_H) / 2;
    emit_rect(sh, ID_OVL_BG, x, sh->ovl_y, SCENE_SHELL_OVL_W,
              SCENE_SHELL_OVL_H);
    emit_rect(sh, ID_OVL_QUERY, x + 16, sh->ovl_y + 14,
              SCENE_SHELL_OVL_W - 32, 24);
    for (i = 0; i < SCENE_SHELL_OVL_HITS; i++)
        emit_rect(sh, ID_OVL_HITS + i, x + 16,
                  sh->ovl_y + 44 + (int32_t)i * 28,
                  SCENE_SHELL_OVL_W - 32, 24);
}

/* Selected row gets the hover style slot, the others their base theme.
 * Without a hover style (slot unset) no highlighting is applied. */
static void ovl_highlight(scene_shell *sh)
{
    uint32_t i;

    if (!sh->hover_style) return;
    for (i = 0; i < SCENE_SHELL_OVL_HITS; i++) {
        int sel = (sh->ovl_open && sh->ovl_hit_count > 0
                   && i == sh->ovl_sel);
        scene_client_set_style(sh->client, ID_OVL_HITS + i,
                               sel ? sh->hover_style
                                   : base_style_for(ID_OVL_HITS + i));
    }
}

/* Search every layer's committed texts for the current query and rebuild
 * the hit rows. Deterministic: layer ascending, then the engine's
 * document order. App-layer rows are labelled "[L%d] <text>", layer-0
 * rows carry no prefix. */
static void ovl_search(scene_shell *sh)
{
    uint32_t i;
    uint32_t qlen = (uint32_t)strlen(sh->ovl_query);
    int nlay = sh->cp ? scene_compositor_layer_count(sh->cp) : 1;
    char buf[96];

    sh->ovl_hit_count = 0;
    sh->ovl_sel = 0;
    for (i = 0; i < (uint32_t)nlay
         && sh->ovl_hit_count < SCENE_SHELL_OVL_HITS; i++) {
        scene_store *st = (i == 0) ? sh->store
            : scene_compositor_layer_store(sh->cp, (int)i);
        scene_node_id ids[48];
        scene_text_id tids[48];
        size_t cap = sizeof(ids) / sizeof(ids[0]);
        size_t tcap = cap;
        size_t n;
        uint32_t k;

        if (!st || qlen == 0) continue;
        n = scene_store_search(st, sh->ovl_query, qlen, ids, cap, tids,
                               &tcap);
        for (k = 0; k < n && k < cap
             && sh->ovl_hit_count < SCENE_SHELL_OVL_HITS; k++) {
            ovl_hit *hit = &sh->ovl_hits[sh->ovl_hit_count];
            scene_node_text_vis tv[16];
            int nt, ti;

            /* The overlay's own nodes (backdrop, query label, hit rows)
             * are not search targets: their committed texts would echo
             * back into the rows on the next keystroke. Layer 0
             * contributes only interactive nodes — passive labels
             * (clock, tray status) are not activatable targets. */
            if (ids[k] >= ID_OVL_BG) continue;
            if (i == 0) {
                scene_node_vis nv;
                if (scene_store_node_vis(st, ids[k], &nv) != 0) continue;
                if (!(nv.flags & SCENE_FLAG_FOCUSABLE)) continue;
            }

            hit->layer = (int)i;
            hit->node_id = ids[k];
            hit->text_id = tids[k];
            hit->text[0] = '\0';
            nt = scene_store_node_texts(st, ids[k], tv, 16);
            for (ti = 0; ti < nt; ti++) {
                if (tv[ti].text_id == tids[k] && tv[ti].len > 0) {
                    size_t cl = tv[ti].len;
                    if (cl >= sizeof(hit->text)) cl = sizeof(hit->text) - 1;
                    memcpy(hit->text, tv[ti].data, cl);
                    hit->text[cl] = '\0';
                    break;
                }
            }
            sh->ovl_hit_count++;
        }
    }
    for (i = 0; i < SCENE_SHELL_OVL_HITS; i++) {
        if (i < sh->ovl_hit_count) {
            const ovl_hit *hit = &sh->ovl_hits[i];

            if (hit->layer == 0)
                snprintf(buf, sizeof(buf), "%s", hit->text);
            else
                snprintf(buf, sizeof(buf), "[L%d] %s", hit->layer,
                         hit->text);
            emit_text(sh, ID_OVL_HITS + i, 1, buf, (uint32_t)strlen(buf));
            emit_flags(sh, ID_OVL_HITS + i,
                       SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
        } else {
            emit_flags(sh, ID_OVL_HITS + i, 0);
        }
    }
    ovl_highlight(sh);
}

static void ovl_query_changed(scene_shell *sh)
{
    char buf[68];
    size_t qlen = strlen(sh->ovl_query);
    size_t take = qlen;

    if (take > 62) take = 62;
    buf[0] = '>';
    buf[1] = ' ';
    memcpy(buf + 2, sh->ovl_query, take);
    buf[2 + take] = '\0';
    emit_text(sh, ID_OVL_QUERY, 1, buf, (uint32_t)strlen(buf));
    ovl_search(sh);
}

static int ovl_open_shell(scene_shell *sh)
{
    uint32_t i;

    if (sh->ovl_open) return 1;
    sh->ovl_open = 1;
    sh->ovl_query[0] = '\0';
    sh->ovl_hit_count = 0;
    sh->ovl_sel = 0;
    if (!sh->ovl_built) {
        if (ovl_build(sh) != 0) {
            sh->ovl_open = 0;
            return 0;
        }
        sh->ovl_built = 1;
    }
    ovl_layout(sh);
    emit_flags(sh, ID_OVL_BG, SCENE_FLAG_VISIBLE);
    emit_flags(sh, ID_OVL_QUERY, SCENE_FLAG_VISIBLE);
    emit_text(sh, ID_OVL_QUERY, 1, "> ", 2);
    for (i = 0; i < SCENE_SHELL_OVL_HITS; i++)
        emit_flags(sh, ID_OVL_HITS + i, 0);
    ovl_search(sh);
    /* The overlay owns the keyboard: route keys to the shell session
     * even if a click left focus on an app layer. */
    if (sh->cp)
        scene_compositor_set_focus_layer(sh->cp, 0);
    scene_store_host_focus(sh->store, ID_BACKGROUND);
    return 1;
}

static void ovl_close(scene_shell *sh)
{
    uint32_t i;

    if (!sh->ovl_open) return;
    sh->ovl_open = 0;
    emit_flags(sh, ID_OVL_BG, 0);
    emit_flags(sh, ID_OVL_QUERY, 0);
    for (i = 0; i < SCENE_SHELL_OVL_HITS; i++)
        emit_flags(sh, ID_OVL_HITS + i, 0);
    sh->ovl_query[0] = '\0';
    sh->ovl_hit_count = 0;
    sh->ovl_sel = 0;
}

static void ovl_sel_move(scene_shell *sh, int dir)
{
    int32_t s;

    if (sh->ovl_hit_count == 0) return;
    s = (int32_t)sh->ovl_sel + dir;
    if (s < 0) s = 0;
    if (s >= (int32_t)sh->ovl_hit_count) s = (int32_t)sh->ovl_hit_count - 1;
    sh->ovl_sel = (uint32_t)s;
    ovl_highlight(sh);
}

/* Activate the chosen hit: restore visibility + focus on ITS layer's
 * store (host WM, no wire bytes). Layer 0 hits only need the focus. */
static void ovl_activate(scene_shell *sh, uint32_t idx)
{
    const ovl_hit *hit;
    scene_store *lst;

    if (!sh->ovl_open || idx >= sh->ovl_hit_count) return;
    hit = &sh->ovl_hits[idx];
    lst = (hit->layer == 0) ? sh->store
        : (sh->cp ? scene_compositor_layer_store(sh->cp, hit->layer)
                  : NULL);
    if (lst) {
        if (hit->layer != 0)
            scene_store_host_set_visible(lst, hit->node_id, 1);
        scene_store_host_focus(lst, hit->node_id);
    }
    ovl_close(sh);
}

static void ovl_enter(scene_shell *sh)
{
    uint32_t idx;

    if (sh->ovl_hit_count == 0) return;
    idx = sh->ovl_sel < sh->ovl_hit_count ? sh->ovl_sel : 0;
    ovl_activate(sh, idx);
}

static void ovl_backspace(scene_shell *sh)
{
    size_t len = strlen(sh->ovl_query);

    if (len == 0) return;
    sh->ovl_query[len - 1] = '\0';
    ovl_query_changed(sh);
}

/* Append a printable key to the query. Returns 1 when the key was
 * printable (consumed), 0 otherwise. */
static int ovl_printable(scene_shell *sh, uint32_t code, uint8_t mods)
{
    char c = ovl_printable_char(code, mods);
    size_t len;

    if (c == 0) return 0;
    len = strlen(sh->ovl_query);
    if (len + 1 >= sizeof(sh->ovl_query)) return 1;  /* full: consumed */
    sh->ovl_query[len] = c;
    sh->ovl_query[len + 1] = '\0';
    ovl_query_changed(sh);
    return 1;
}

/* ---- input handling -------------------------------------------------- */

/* Show/hide the start menu and all its items (launcher + system). */
static void menu_set_visible(scene_shell *sh, int open)
{
    uint32_t i;
    uint8_t flags = open ? (SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE) : 0;
    sh->menu_open = open ? 1 : 0;
    emit_flags(sh, ID_MENU, flags);
    for (i = 0; i < sh->cfg.launcher_app_count; i++)
        emit_flags(sh, ID_MENU_BASE + i, flags);
    emit_flags(sh, ID_RESTART_ITEM, flags);
    emit_flags(sh, ID_POWEROFF_ITEM, flags);
}

int scene_shell_handle_activate(scene_shell *sh, scene_node_id activated_id)
{
    if (!sh || !sh->built) return 0;
    sh->last_activity = now_sec();

    /* Start button — toggle launcher menu */
    if (activated_id == ID_START_BTN) {
        menu_set_visible(sh, !sh->menu_open);
        return 1;
    }

    /* Volume button — toggle mute: write the OS volume file and flip the
     * button text. */
    if (activated_id == ID_VOL_BTN) {
        sh->vol_muted = !sh->vol_muted;
        const char *label = sh->vol_muted ? "muted" : "vol";
        FILE *vf = fopen(vol_file_path(), "w");
        if (vf) {
            fputs(sh->vol_muted ? "0" : "100", vf);
            fclose(vf);
        }
        emit_text(sh, ID_VOL_BTN, 1, label, (uint32_t)strlen(label));
        return 1;
    }

    /* Menu item — launch app and close menu */
    if (activated_id >= ID_MENU_BASE &&
        activated_id < ID_MENU_BASE + SCENE_SHELL_MAX_APPS) {
        uint32_t idx = activated_id - ID_MENU_BASE;
        if (idx < sh->cfg.launcher_app_count) {
            /* Close menu first */
            menu_set_visible(sh, 0);

            /* Launch the app: host hook if set, else shell out */
            if (sh->launch_fn)
                sh->launch_fn(sh->launch_ud, idx,
                              sh->cfg.launcher_apps[idx]);
            else {
                char cmd[128];
                snprintf(cmd, sizeof(cmd), "%s &",
                         sh->cfg.launcher_apps[idx]);
                (void)system(cmd);
            }
        }
        return 1;
    }

    /* System menu items — Restart / Power Off */
    if (activated_id == ID_RESTART_ITEM ||
        activated_id == ID_POWEROFF_ITEM) {
        int action = activated_id == ID_POWEROFF_ITEM ?
                     SCENE_SHELL_ACTION_POWEROFF : SCENE_SHELL_ACTION_RESTART;
        menu_set_visible(sh, 0);
        if (sh->power_fn)
            sh->power_fn(sh->power_ud, action);
        else {
            const char *cmd = action == SCENE_SHELL_ACTION_POWEROFF ?
                              "poweroff &" : "reboot &";
            (void)system(cmd);
        }
        return 1;
    }

    /* App task button — host WM: focus / minimize / restore the window
     * on its own layer's store (an OS intervention: direct host API, no
     * wire bytes into the app session). */
    if (activated_id >= ID_APP_TASK_BASE &&
        activated_id < ID_APP_TASK_BASE + APP_TASK_MAX) {
        uint32_t slot = activated_id - ID_APP_TASK_BASE;
        app_task_entry *at = &sh->app_tasks[slot];
        if (!at->used || !sh->cp) return 0;
        scene_store *lst = scene_compositor_layer_store(sh->cp, at->layer);
        if (!lst) return 0;
        scene_node_vis v;
        int visible = (scene_store_node_vis(lst, at->window_id, &v) == 0 &&
                       (v.flags & SCENE_FLAG_VISIBLE));
        if (!visible) {
            /* Minimized: restore and focus. */
            scene_store_host_set_visible(lst, at->window_id, 1);
            scene_store_host_focus(lst, at->window_id);
        } else if (scene_store_focus(lst) == at->window_id) {
            /* Focused: minimize (button stays in the taskbar). */
            scene_store_host_set_visible(lst, at->window_id, 0);
        } else {
            /* Visible, not focused: focus the window. */
            scene_store_host_focus(lst, at->window_id);
        }
        return 1;
    }

    /* Search overlay hit row — activate the chosen cross-app hit. */
    if (activated_id >= ID_OVL_HITS &&
        activated_id < ID_OVL_HITS + SCENE_SHELL_OVL_HITS) {
        uint32_t idx = activated_id - ID_OVL_HITS;
        if (idx < sh->ovl_hit_count)
            ovl_activate(sh, idx);
        return 1;
    }

    /* Task button — focus the corresponding window */
    if (activated_id >= ID_TASK_BASE) {
        uint32_t i;
        for (i = 0; i < sh->task_count; i++) {
            if (sh->tasks[i].button_id == activated_id) {
                scene_client_focus(sh->client, sh->tasks[i].window_id);
                return 1;
            }
        }
    }

    /* Close button: walk up titlebar → window, then destroy the window.
     * We detect close buttons by role=BUTTON with parent role=TITLEBAR. */
    {
        scene_node_vis v;
        if (scene_store_node_vis(sh->store, activated_id, &v) == 0 &&
            v.role == SCENE_ROLE_BUTTON && v.parent != SCENE_NO_PARENT) {
            scene_node_vis pv;
            if (scene_store_node_vis(sh->store, v.parent, &pv) == 0 &&
                pv.role == SCENE_ROLE_TITLEBAR &&
                pv.parent != SCENE_NO_PARENT) {
                /* Found: pv.parent is the WINDOW node. Destroy it. */
                scene_client_destroy_node(sh->client, pv.parent);
                return 1;
            }
        }
    }

    return 0;  /* not a shell node */
}

/* ---- notifications --------------------------------------------------- */

int scene_shell_notify(scene_shell *sh, const char *title, const char *body)
{
    if (!sh || !title || !body) return -1;

    /* Lazy creation once: a top-right NOTIFICATION toast owned by the
     * shell session (role default style, style_ref 0). Rects are
     * absolute session space (spec §3): children anchored at the
     * toast's origin. Never focusable. */
    if (sh->toast_id == 0) {
        int32_t tx = sh->width - 272 - 12;
        int32_t ty = 12;

        if (emit_create(sh, SCENE_NO_PARENT, ID_TOAST,
                        SCENE_ROLE_NOTIFICATION, tx, ty, 272, 56,
                        SCENE_FLAG_VISIBLE) != 0)
            return -1;
        if (emit_create(sh, ID_TOAST, ID_TOAST_TITLE,
                        SCENE_ROLE_LABEL, tx + 8, ty + 4, 256, 16,
                        SCENE_FLAG_VISIBLE) != 0)
            return -1;
        if (emit_create(sh, ID_TOAST, ID_TOAST_BODY,
                        SCENE_ROLE_LABEL, tx + 8, ty + 22, 256, 24,
                        SCENE_FLAG_VISIBLE) != 0)
            return -1;
        sh->toast_id = ID_TOAST;
    } else {
        /* Re-raise an existing toast: show + replace, no recreate. */
        emit_flags(sh, ID_TOAST, SCENE_FLAG_VISIBLE);
        emit_flags(sh, ID_TOAST_TITLE, SCENE_FLAG_VISIBLE);
        emit_flags(sh, ID_TOAST_BODY, SCENE_FLAG_VISIBLE);
    }
    emit_text(sh, ID_TOAST_TITLE, 1, title, (uint32_t)strlen(title));
    emit_text(sh, ID_TOAST_BODY, 1, body, (uint32_t)strlen(body));
    sh->toast_life = SCENE_SHELL_TOAST_TICKS;
    sh->toast_up = 1;
    return 0;
}

/* ---- pointer / hover ------------------------------------------------- */

static int point_in_rect(int32_t px, int32_t py, const int32_t r[4])
{
    return px >= r[0] && px < r[0] + r[2] && py >= r[1] && py < r[1] + r[3];
}

/* Get absolute rect for a node by walking up the parent chain and
 * accumulating parent-relative offsets. */
static int get_abs_rect(scene_store *store, scene_node_id id, int32_t out[4])
{
    scene_node_vis v;
    if (scene_store_node_vis(store, id, &v) != 0) return -1;
    out[0] = v.rect[0];
    out[1] = v.rect[1];
    out[2] = v.rect[2];
    out[3] = v.rect[3];
    return 0;
}

/* Which resize edges the point (x,y) is within RESIZE_BAND of on the
 * window rect `wr` (absolute session space). 0 = none (move instead);
 * corner = both bits. */
static uint8_t resize_edges_at(int32_t x, int32_t y, const int32_t wr[4])
{
    uint8_t e = 0;
    if (x >= wr[0] + wr[2] - RESIZE_BAND && x < wr[0] + wr[2])
        e |= RESIZE_EDGE_RIGHT;
    if (y >= wr[1] + wr[3] - RESIZE_BAND && y < wr[1] + wr[3])
        e |= RESIZE_EDGE_BOTTOM;
    return e;
}

/* Depth of a node in the store (hops to the root; 0 for a root). */
static uint32_t node_depth(scene_store *store, scene_node_id id)
{
    uint32_t d = 0;
    scene_node_vis v;
    while (scene_store_node_vis(store, id, &v) == 0 &&
           v.parent != SCENE_NO_PARENT) {
        d++;
        id = v.parent;
    }
    return d;
}

struct tb_ctx {
    scene_store   *store;
    int32_t        x, y;
    scene_node_id  best;
    uint32_t       best_depth;
};

static int titlebar_walk_cb(scene_node_id id, void *ud)
{
    struct tb_ctx *c = ud;
    scene_node_vis v;
    if (scene_store_node_vis(c->store, id, &v) != 0) return 0;
    if (v.role != SCENE_ROLE_TITLEBAR) return 0;
    if (!(v.flags & SCENE_FLAG_VISIBLE) || v.stale) return 0;
    if (!point_in_rect(c->x, c->y, v.rect)) return 0;
    uint32_t d = node_depth(c->store, id);
    if (d >= c->best_depth) { c->best = id; c->best_depth = d; }
    return 0;
}

/* Innermost (deepest) titlebar node under (x,y), or 0. The shell's own
 * nodes are never TITLEBARs, so this finds app window titlebars. */
static scene_node_id titlebar_at(scene_store *store, int32_t x, int32_t y)
{
    struct tb_ctx c = { store, x, y, 0, 0 };
    scene_store_walk(store, titlebar_walk_cb, &c);
    return c.best;
}

/* ---- window resize (re-derive the window tree) ----------------------- */

struct resize_ctx {
    scene_client   *cl;
    scene_store    *store;
    scene_node_id   window_id;
    scene_node_id   titlebar_id;
    uint8_t         content_set;   /* first non-TITLEBAR child is CONTENT */
    int32_t         wx, wy, nw, nh;
};

/* Re-derive the window tree for the new size. TITLEBAR spans the new
 * width at the top; the first other direct child is CONTENT and fills
 * below the 32px titlebar; the titlebar's own label and close button
 * follow the scene_app layout (label at +4,+4 w-40x24, close at
 * w-28,+4 24x24 — the "X" must track the new width). Unknown roles are
 * left alone. All rects are absolute session space (spec §3). */
static int resize_child_cb(scene_node_id id, void *ud)
{
    struct resize_ctx *rc = ud;
    scene_node_vis v;
    scene_rect r;

    if (scene_store_node_vis(rc->store, id, &v) != 0) return 0;
    if (v.parent == rc->window_id) {
        if (v.role == SCENE_ROLE_TITLEBAR) {
            rc->titlebar_id = id;
            r.x = rc->wx; r.y = rc->wy; r.w = rc->nw; r.h = 32;
            scene_client_set_rect(rc->cl, id, &r);
        } else if (!rc->content_set) {
            rc->content_set = 1;
            r.x = rc->wx; r.y = rc->wy + 32; r.w = rc->nw; r.h = rc->nh - 32;
            scene_client_set_rect(rc->cl, id, &r);
        }
    } else if (rc->titlebar_id != 0 && v.parent == rc->titlebar_id) {
        if (v.role == SCENE_ROLE_LABEL) {
            r.x = rc->wx + 4; r.y = rc->wy + 4;
            r.w = rc->nw - 40; r.h = 24;
            scene_client_set_rect(rc->cl, id, &r);
        } else if (v.role == SCENE_ROLE_BUTTON) {
            r.x = rc->wx + rc->nw - 28; r.y = rc->wy + 4; r.w = 24; r.h = 24;
            scene_client_set_rect(rc->cl, id, &r);
        }
    }
    return 0;
}

static void resize_children(scene_shell *sh, scene_node_id window_id,
                            int32_t wx, int32_t wy, int32_t nw, int32_t nh)
{
    struct resize_ctx rc;

    rc.cl = sh->client;
    rc.store = sh->store;
    rc.window_id = window_id;
    rc.titlebar_id = 0;
    rc.content_set = 0;
    rc.wx = wx; rc.wy = wy; rc.nw = nw; rc.nh = nh;
    scene_store_walk(sh->store, resize_child_cb, &rc);
}

scene_node_id scene_shell_handle_pointer(scene_shell *sh, int32_t x, int32_t y,
                                uint8_t buttons)
{
    if (!sh || !sh->built) return 0;
    sh->last_activity = now_sec();

    /* Window resize: if resizing, update the WINDOW rect and re-derive
     * its children (titlebar/content/label/close follow the new size).
     * Size = original + pointer delta along the active edges, clamped
     * to the minimum; position is preserved. */
    if (sh->resizing_window != 0) {
        if (!(buttons & 0x01)) {
            /* Button released — end resize. */
            sh->resizing_window = 0;
            return 0;
        }
        int32_t nw = sh->resize_orig_w, nh = sh->resize_orig_h;
        if (sh->resize_edges & RESIZE_EDGE_RIGHT)
            nw += x - sh->resize_orig_px;
        if (sh->resize_edges & RESIZE_EDGE_BOTTOM)
            nh += y - sh->resize_orig_py;
        /* Clamp only the dimensions being dragged (an edge-only drag
         * must leave the other dimension untouched). */
        if ((sh->resize_edges & RESIZE_EDGE_RIGHT) && nw < RESIZE_MIN_W)
            nw = RESIZE_MIN_W;
        if ((sh->resize_edges & RESIZE_EDGE_BOTTOM) && nh < RESIZE_MIN_H)
            nh = RESIZE_MIN_H;
        scene_node_vis wv;
        if (scene_store_node_vis(sh->store, sh->resizing_window, &wv) == 0) {
            scene_rect wr = {wv.rect[0], wv.rect[1], nw, nh};
            scene_client_set_rect(sh->client, sh->resizing_window, &wr);
            resize_children(sh, sh->resizing_window,
                            wv.rect[0], wv.rect[1], nw, nh);
        }
        return sh->resizing_window;
    }

    /* Window move: if dragging a title bar, update the window rect. */
    if (sh->moving_titlebar != 0) {
        if (!(buttons & 0x01)) {
            /* Button released — end move. */
            sh->moving_titlebar = 0;
            return 0;
        }
        /* Move the parent WINDOW. */
        scene_node_vis v;
        if (scene_store_node_vis(sh->store, sh->moving_titlebar, &v) == 0 &&
            v.parent != SCENE_NO_PARENT) {
            int32_t wx = x - sh->move_off_x;
            int32_t wy = y - sh->move_off_y;
            scene_node_vis wv;
            if (scene_store_node_vis(sh->store, v.parent, &wv) == 0) {
                scene_rect wr = {wx, wy, wv.rect[2], wv.rect[3]};
                scene_client_set_rect(sh->client, v.parent, &wr);
            }
        }
        return sh->moving_titlebar;
    }

    /* Hit-test: find the topmost shell-owned node under (x,y).
     * Check fixed candidates first (panel children, menu items),
     * then dynamic task buttons. */
    scene_node_id hit = 0;
    scene_node_id candidates[] = {
        ID_START_BTN, ID_CLOCK, ID_VOL_BTN,
        ID_MENU,
        ID_MENU_BASE, ID_MENU_BASE+1, ID_MENU_BASE+2, ID_MENU_BASE+3,
        ID_MENU_BASE+4, ID_MENU_BASE+5, ID_MENU_BASE+6, ID_MENU_BASE+7,
    };
    uint32_t nc = sizeof(candidates) / sizeof(candidates[0]);
    uint32_t i;
    int32_t ar[4];

    for (i = 0; i < nc; i++) {
        if (get_abs_rect(sh->store, candidates[i], ar) != 0) continue;
        scene_node_vis v;
        if (scene_store_node_vis(sh->store, candidates[i], &v) != 0) continue;
        if (!(v.flags & SCENE_FLAG_VISIBLE)) continue;
        if (point_in_rect(x, y, ar)) {
            hit = candidates[i];
            break;
        }
    }

    if (hit == 0) {
        for (i = 0; i < sh->task_count; i++) {
            scene_node_id btn = sh->tasks[i].button_id;
            if (btn == 0) continue;
            if (get_abs_rect(sh->store, btn, ar) != 0) continue;
            scene_node_vis v;
            if (scene_store_node_vis(sh->store, btn, &v) != 0) continue;
            if (!(v.flags & SCENE_FLAG_VISIBLE)) continue;
            if (point_in_rect(x, y, ar)) {
                hit = btn;
                break;
            }
        }
    }

    /* Update hover state */
    if (hit != sh->hovered_id) {
        if (sh->hovered_id != 0 && sh->hover_style != 0)
            scene_client_set_style(sh->client, sh->hovered_id,
                                   base_style_for(sh->hovered_id));
        if (hit != 0 && sh->hover_style != 0)
            scene_client_set_style(sh->client, hit, sh->hover_style);
        sh->hovered_id = hit;
    }

    /* Start window move or resize: a press on a titlebar drags its
     * parent WINDOW; presses within RESIZE_BAND of the window's right
     * edge, bottom edge, or bottom-right corner resize instead. */
    if (buttons & 0x01) {
        scene_node_id tb = titlebar_at(sh->store, x, y);
        if (tb != 0) {
            scene_node_vis v;
            if (scene_store_node_vis(sh->store, tb, &v) == 0 &&
                v.parent != SCENE_NO_PARENT) {
                int32_t wr[4];
                if (get_abs_rect(sh->store, v.parent, wr) == 0) {
                    uint8_t edges = resize_edges_at(x, y, wr);
                    if (edges) {
                        sh->resizing_window = v.parent;
                        sh->resize_edges     = edges;
                        sh->resize_orig_w    = wr[2];
                        sh->resize_orig_h    = wr[3];
                        sh->resize_orig_px   = x;
                        sh->resize_orig_py   = y;
                    } else {
                        sh->moving_titlebar = tb;
                        /* Offset from pointer to window origin. */
                        sh->move_off_x = x - wr[0];
                        sh->move_off_y = y - wr[1];
                    }
                    return tb;
                }
            }
        }
    }

    return hit;
}

/* ---- reconfiguration ------------------------------------------------- */

int scene_shell_load_config(scene_shell *sh, const char *path)
{
    if (!sh) return -1;
    scene_shell_config new_cfg;
    scene_shell_config_defaults(&new_cfg);
    if (scene_shell_config_load(&new_cfg, path) != 0) return -1;
    sh->cfg = new_cfg;

    /* Live re-theme: destroy all shell nodes and rebuild.
     * This is the nuclear approach but it's correct and simple.
     * The performance doesn't matter — this is triggered by user action,
     * not per-frame. */
    if (sh->built) {
        /* Destroy all shell-owned nodes in reverse creation order.
         * Menu items first, then menu, clock, start btn, panel, background. */
        uint32_t i;
        for (i = 0; i < sh->cfg.launcher_app_count && i < SCENE_SHELL_MAX_APPS; i++)
            emit_destroy(sh, ID_MENU_BASE + i);
        /* Destroy any extra task buttons */
        for (i = 0; i < sh->task_count; i++) {
            if (sh->tasks[i].button_id != 0)
                emit_destroy(sh, sh->tasks[i].button_id);
        }
        sh->task_count = 0;
        emit_destroy(sh, ID_MENU);
        /* Destroy the search overlay (lazily created, may not exist) */
        if (sh->ovl_built) {
            for (i = 0; i < SCENE_SHELL_OVL_HITS; i++)
                emit_destroy(sh, ID_OVL_HITS + i);
            emit_destroy(sh, ID_OVL_QUERY);
            emit_destroy(sh, ID_OVL_BG);
        }
        sh->ovl_built = 0;
        sh->ovl_open = 0;
        sh->ovl_hit_count = 0;
        sh->ovl_sel = 0;
        sh->ovl_query[0] = '\0';
        /* Destroy the toast (lazily created, may not exist) */
        if (sh->toast_id != 0) {
            emit_destroy(sh, ID_TOAST_BODY);
            emit_destroy(sh, ID_TOAST_TITLE);
            emit_destroy(sh, ID_TOAST);
            sh->toast_id = 0;
            sh->toast_life = 0;
            sh->toast_up = 0;
        }
        /* Destroy the volume button (exists after every build) */
        {
            scene_node_vis tbv;
            if (scene_store_node_vis(sh->store, ID_VOL_BTN, &tbv) == 0)
                emit_destroy(sh, ID_VOL_BTN);
        }
        emit_destroy(sh, ID_CLOCK);
        emit_destroy(sh, ID_TRAY);
        emit_destroy(sh, ID_START_BTN);
        emit_destroy(sh, ID_PANEL);
        emit_destroy(sh, ID_BACKGROUND);
        /* Free old wallpaper */
        if (sh->wp) {
            scene_wallpaper_free(sh->wp);
            sh->wp = NULL;
            if (sh->cp && sh->wp_tex_registered) {
                scene_compositor_release_texture(sh->cp, sh->wp_tex_ref);
                sh->wp_tex_registered = 0;
            }
        }
        sh->built = 0;
        sh->menu_open = 0;
        sh->hovered_id = 0;
        sh->active_task_id = 0;
        sh->moving_titlebar = 0;
        sh->resizing_window = 0;
        memset(sh->app_tasks, 0, sizeof(sh->app_tasks));
        sh->tray_text[0] = '\0';
        sh->last_tray_probe = 0;
        /* Rebuild with new config */
        return scene_shell_build(sh, sh->width, sh->height);
    }
    return 0;
}

int scene_shell_apply_config(scene_shell *sh, const scene_shell_config *cfg)
{
    if (!sh || !cfg) return -1;
    sh->cfg = *cfg;
    if (sh->built) {
        uint32_t i;
        for (i = 0; i < sh->cfg.launcher_app_count && i < SCENE_SHELL_MAX_APPS; i++)
            emit_destroy(sh, ID_MENU_BASE + i);
        for (i = 0; i < sh->task_count; i++) {
            if (sh->tasks[i].button_id != 0)
                emit_destroy(sh, sh->tasks[i].button_id);
        }
        sh->task_count = 0;
        emit_destroy(sh, ID_MENU);
        /* Destroy the search overlay (lazily created, may not exist) */
        if (sh->ovl_built) {
            for (i = 0; i < SCENE_SHELL_OVL_HITS; i++)
                emit_destroy(sh, ID_OVL_HITS + i);
            emit_destroy(sh, ID_OVL_QUERY);
            emit_destroy(sh, ID_OVL_BG);
        }
        sh->ovl_built = 0;
        sh->ovl_open = 0;
        sh->ovl_hit_count = 0;
        sh->ovl_sel = 0;
        sh->ovl_query[0] = '\0';
        /* Destroy the toast (lazily created, may not exist) */
        if (sh->toast_id != 0) {
            emit_destroy(sh, ID_TOAST_BODY);
            emit_destroy(sh, ID_TOAST_TITLE);
            emit_destroy(sh, ID_TOAST);
            sh->toast_id = 0;
            sh->toast_life = 0;
            sh->toast_up = 0;
        }
        /* Destroy the volume button (exists after every build) */
        {
            scene_node_vis tbv;
            if (scene_store_node_vis(sh->store, ID_VOL_BTN, &tbv) == 0)
                emit_destroy(sh, ID_VOL_BTN);
        }
        emit_destroy(sh, ID_CLOCK);
        emit_destroy(sh, ID_START_BTN);
        emit_destroy(sh, ID_PANEL);
        emit_destroy(sh, ID_BACKGROUND);
        /* Free old wallpaper */
        if (sh->wp) {
            scene_wallpaper_free(sh->wp);
            sh->wp = NULL;
            if (sh->cp && sh->wp_tex_registered) {
                scene_compositor_release_texture(sh->cp, sh->wp_tex_ref);
                sh->wp_tex_registered = 0;
            }
        }
        sh->built = 0;
        sh->menu_open = 0;
        sh->hovered_id = 0;
        sh->active_task_id = 0;
        sh->moving_titlebar = 0;
        sh->resizing_window = 0;
        return scene_shell_build(sh, sh->width, sh->height);
    }
    return 0;
}

/* ---- output changes -------------------------------------------------- */

int scene_shell_resize(scene_shell *sh, int32_t width, int32_t height)
{
    if (!sh || !sh->built) return -1;
    sh->width  = width;
    sh->height = height;

    uint32_t ph = sh->cfg.panel_height;

    /* Reposition background */
    emit_rect(sh, ID_BACKGROUND, 0, 0, width, height);

    /* Reposition panel */
    emit_rect(sh, ID_PANEL, 0, height - (int32_t)ph, width, (int32_t)ph);

    /* Reposition start button (absolute coords) */
    emit_rect(sh, ID_START_BTN, 2, height - (int32_t)ph + 2,
              (int32_t)ph - 4, (int32_t)ph - 4);

    /* Reposition clock (right-aligned, absolute coords) */
    emit_rect(sh, ID_CLOCK, width - 100, height - (int32_t)ph + 2, 96, (int32_t)ph - 4);

    /* Reposition tray (left of clock, absolute coords) */
    emit_rect(sh, ID_TRAY, width - 100 - 56, height - (int32_t)ph + 2,
              52, (int32_t)ph - 4);

    /* Reposition volume button (left of tray, vertically centered) */
    emit_rect(sh, ID_VOL_BTN, width - 202,
              height - (int32_t)ph + ((int32_t)ph - 22) / 2, 40, 22);

    /* Reposition menu */
    emit_rect(sh, ID_MENU, 0, height - (int32_t)ph - 160, 160, 160);

    /* Reposition toast (top-right, children follow the origin) */
    if (sh->toast_id != 0) {
        int32_t tx = width - 272 - 12;
        emit_rect(sh, ID_TOAST, tx, 12, 272, 56);
        emit_rect(sh, ID_TOAST_TITLE, tx + 8, 4, 256, 16);
        emit_rect(sh, ID_TOAST_BODY, tx + 8, 22, 256, 24);
    }

    /* Reposition the search overlay (if built) */
    if (sh->ovl_built)
        ovl_layout(sh);

    /* Reposition the lock screen (if showing) */
    if (sh->locked && sh->lock_built)
        lock_layout(sh);

    /* Resize wallpaper */
    if (sh->wp) {
        scene_wallpaper_free(sh->wp);
        if (sh->cp && sh->wp_tex_registered)
            scene_compositor_release_texture(sh->cp, sh->wp_tex_ref);
        sh->wp_tex_registered = 0;
        sh->wp_tex_registered = 0;
        sh->wp = scene_wallpaper_new((uint32_t)width, (uint32_t)height);
        if (sh->wp && sh->cp) {
            scene_wp_params p;
            memset(&p, 0, sizeof(p));
            p.speed = sh->cfg.wallpaper_speed;
            switch (sh->cfg.wallpaper_mode) {
            case SCENE_WP_STATIC:
                if (sh->cfg.wallpaper_path[0])
                    scene_wallpaper_set_static(sh->wp, sh->cfg.wallpaper_path);
                break;
            case SCENE_WP_KEN_BURNS:
                if (sh->cfg.wallpaper_path[0])
                    scene_wallpaper_set_ken_burns(sh->wp, sh->cfg.wallpaper_path,
                                                  0, 0);
                break;
            case SCENE_WP_PLASMA:
            case SCENE_WP_AURORA:
            case SCENE_WP_GRADIENT_MESH:
                scene_wallpaper_set_procedural(sh->wp, sh->cfg.wallpaper_mode, &p);
                break;
            default:
                break;
            }
        }
    }

    return 0;
}

/* ---- desktop lock ------------------------------------------------------ */

#if !defined(_WIN32)
/* Default password checker: the first /etc/shadow user entry. Empty
 * hash = no password configured (any password unlocks, so Enter alone
 * opens the desktop); "!"/"*" prefix = locked account (reject);
 * otherwise the hash is verified with crypt(). No shadow file = only
 * the empty password unlocks. */
static int lock_check_shadow(void *ud, const char *password)
{
    char line[512];
    FILE *f;
    char *c1, *hash, *c2;

    (void)ud;
    if (!password) return 0;
    f = fopen("/etc/shadow", "r");
    if (!f) return password[0] == '\0';
    while (fgets(line, (int)sizeof(line), f)) {
        c1 = strchr(line, ':');
        if (!c1) continue;
        hash = c1 + 1;
        c2 = strchr(hash, ':');
        if (!c2) continue;
        *c2 = '\0';
        fclose(f);
        if (hash[0] == '\0') return 1;                /* no password set */
        if (hash[0] == '!' || hash[0] == '*') return 0;   /* locked acct */
        if (hash[0] == '$' && hash[1] != '\0')
            return strcmp(crypt(password, hash), hash) == 0;
        return strcmp(password, hash) == 0;           /* plain legacy     */
    }
    fclose(f);
    return password[0] == '\0';
}
#else
static int lock_check_shadow(void *ud, const char *password)
{
    (void)ud; (void)password;
    return 0;
}
#endif

void scene_shell_set_lock_check(scene_shell *sh, scene_shell_lock_check_fn fn,
                                void *ud)
{
    if (!sh) return;
    sh->lock_check = fn ? fn : lock_check_shadow;
    sh->lock_check_ud = ud;
}

/* Lock screen layout (re-centered, full-screen backdrop). */
static void lock_layout(scene_shell *sh)
{
    int32_t cw = 420, ch = 150;
    int32_t x = (sh->width - cw) / 2, y = (sh->height - ch) / 2;

    emit_rect(sh, ID_LOCK_BG, 0, 0, sh->width, sh->height);
    emit_rect(sh, ID_LOCK_TITLE, x, y + 22, cw, 26);
    emit_rect(sh, ID_LOCK_PWD, x, y + 66, cw, 26);
    emit_rect(sh, ID_LOCK_HINT, x, y + 104, cw, 20);
}

/* Render the password as box-glyph dots (the font's index-96 glyph). */
static void lock_show_pwd(scene_shell *sh)
{
    char dots[LOCK_PWD_MAX + 1];
    uint32_t i;

    for (i = 0; i < sh->lock_pwd_len; i++) dots[i] = (char)0x7F;
    dots[sh->lock_pwd_len] = '\0';
    emit_text(sh, ID_LOCK_PWD, 1, dots, sh->lock_pwd_len);
}

/* The lock screen is a shell-session (layer 0) full-screen node: with
 * the compositor locked, app layers stop painting and all input routes
 * to the shell, so this backdrop covers the whole desktop. */
static int lock_build(scene_shell *sh)
{
    int32_t cw = 420, ch = 150;
    int32_t x = (sh->width - cw) / 2, y = (sh->height - ch) / 2;
    int r;

    r = emit_create(sh, SCENE_NO_PARENT, ID_LOCK_BG, SCENE_ROLE_WINDOW,
                    0, 0, sh->width, sh->height, SCENE_FLAG_VISIBLE);
    if (r != 0) return -1;
    scene_client_set_style(sh->client, ID_LOCK_BG, SHELL_STYLE_LOCK);
    r = emit_create(sh, ID_LOCK_BG, ID_LOCK_TITLE, SCENE_ROLE_LABEL,
                    x, y + 22, cw, 26, SCENE_FLAG_VISIBLE);
    if (r != 0) return -1;
    scene_client_set_style(sh->client, ID_LOCK_TITLE, SHELL_STYLE_LABEL);
    r = emit_create(sh, ID_LOCK_BG, ID_LOCK_PWD, SCENE_ROLE_LABEL,
                    x, y + 66, cw, 26, SCENE_FLAG_VISIBLE);
    if (r != 0) return -1;
    scene_client_set_style(sh->client, ID_LOCK_PWD, SHELL_STYLE_LABEL);
    r = emit_create(sh, ID_LOCK_BG, ID_LOCK_HINT, SCENE_ROLE_LABEL,
                    x, y + 104, cw, 20, SCENE_FLAG_VISIBLE);
    if (r != 0) return -1;
    scene_client_set_style(sh->client, ID_LOCK_HINT, SHELL_STYLE_LABEL);
    emit_text(sh, ID_LOCK_TITLE, 1, "screen locked", 13);
    emit_text(sh, ID_LOCK_HINT, 1, "enter password", 14);
    lock_show_pwd(sh);
    return 0;
}

int scene_shell_lock(scene_shell *sh)
{
    if (!sh || !sh->built || sh->locked) return 0;
    sh->locked = 1;
    sh->lock_pwd_len = 0;
    if (lock_build(sh) != 0)
        sh->lock_built = 0;
    else
        sh->lock_built = 1;
    if (sh->cp) scene_compositor_set_locked(sh->cp, 1);
    sh->last_activity = now_sec();
    return 0;
}

int scene_shell_locked(const scene_shell *sh)
{
    return sh ? sh->locked : 0;
}

static void lock_unlock(scene_shell *sh)
{
    if (!sh->locked) return;
    sh->locked = 0;
    if (sh->lock_built) {
        emit_destroy(sh, ID_LOCK_BG);   /* children die with it */
        sh->lock_built = 0;
    }
    if (sh->cp) scene_compositor_set_locked(sh->cp, 0);
    sh->lock_pwd_len = 0;
}

/* Keys while locked: the lock screen owns the keyboard. Printable keys
 * append to the password; Backspace edits; Enter tries to unlock;
 * everything else is consumed. */
static int lock_key(scene_shell *sh, uint32_t key_code, uint8_t state,
                    uint8_t modifiers)
{
    char c;

    if (!state) return 1;
    sh->last_activity = now_sec();
    if (key_code == SCENE_KEY_ENTER) {
        /* Check only the typed length: the buffer may hold stale bytes
         * from a previous attempt (unlock resets the length, not the
         * memory). A bounded copy is the honest input. */
        char pwd[LOCK_PWD_MAX];
        uint32_t n = sh->lock_pwd_len;
        int ok;

        if (n >= sizeof(pwd)) n = (uint32_t)(sizeof(pwd) - 1);
        memcpy(pwd, sh->lock_pwd, n);
        pwd[n] = '\0';
        ok = sh->lock_check ? sh->lock_check(sh->lock_check_ud, pwd) : 0;
        if (ok) {
            lock_unlock(sh);
        } else {
            emit_text(sh, ID_LOCK_HINT, 1, "wrong password", 14);
            sh->lock_pwd_len = 0;
            lock_show_pwd(sh);
        }
        return 1;
    }
    if (key_code == SCENE_KEY_BACKSPACE) {
        if (sh->lock_pwd_len > 0) sh->lock_pwd_len--;
        if (sh->lock_built) {
            emit_text(sh, ID_LOCK_HINT, 1, "enter password", 14);
            lock_show_pwd(sh);
        }
        return 1;
    }
    if (sh->lock_pwd_len < LOCK_PWD_MAX - 1) {
        c = ovl_printable_char(key_code, modifiers);
        if (c != 0) {
            sh->lock_pwd[sh->lock_pwd_len++] = c;
            sh->lock_pwd[sh->lock_pwd_len] = '\0';
            if (sh->lock_built) {
                emit_text(sh, ID_LOCK_HINT, 1, "enter password", 14);
                lock_show_pwd(sh);
            }
        }
    }
    return 1;
}

/* ---- keyboard handling ----------------------------------------------- */

int scene_shell_handle_key(scene_shell *sh, uint32_t key_code,
                           uint8_t state, uint8_t modifiers)
{
    if (!sh || !sh->built) return 0;
    /* Only handle key-down events. */
    if (!state) return 0;
    sh->last_activity = now_sec();

    /* While locked the lock screen owns every key (password entry,
     * unlock). Apps never see anything. */
    if (sh->locked) return lock_key(sh, key_code, state, modifiers);

    int alt = (modifiers & SCENE_MOD_ALT) != 0;
    int shift = (modifiers & SCENE_MOD_SHIFT) != 0;
    int super = (modifiers & SCENE_MOD_SUPER) != 0;

    /* Super+L: lock the desktop (OS key grab — the chord reaches the
     * shell regardless of keyboard focus). */
    if (super && key_code == SCENE_KEY_L) {
        scene_shell_lock(sh);
        return 1;
    }

    /* Super+S: toggle the cross-app search overlay (OS key grab —
     * the chord reaches the shell regardless of keyboard focus). */
    if (super && key_code == OVL_HOTKEY_CODE) {
        if (sh->ovl_open)
            ovl_close(sh);
        else
            ovl_open_shell(sh);
        return 1;
    }

    /* While the overlay is open it owns the keyboard. */
    if (sh->ovl_open) {
        if (key_code == SCENE_KEY_ESC) { ovl_close(sh); return 1; }
        if (key_code == SCENE_KEY_BACKSPACE) { ovl_backspace(sh); return 1; }
        if (key_code == SCENE_KEY_ENTER) { ovl_enter(sh); return 1; }
        if (key_code == SCENE_KEY_UP) { ovl_sel_move(sh, -1); return 1; }
        if (key_code == SCENE_KEY_DOWN) { ovl_sel_move(sh, 1); return 1; }
        if (ovl_printable(sh, key_code, modifiers)) return 1;
        return 0;
    }

    /* PrtSc: OS screenshot service. While the overlay is open the key
     * stays with it (the block above returns 0 for unmatched keys). */
    if (key_code == SCENE_KEY_SYSRQ && modifiers == 0) {
        if (!sh->cp) return 0;
#if defined(_WIN32)
        const char *shot_path = "build/shot.bmp";
#else
        const char *shot_path = "/home/user/shot.bmp";
#endif
        int rc = scene_compositor_capture_bmp(sh->cp, shot_path);
        scene_shell_notify(sh, "screenshot",
                           rc == 0 ? "saved to shot.bmp" : "capture failed");
        return 1;
    }

    /* Escape: close start menu if open. */
    if (key_code == SCENE_KEY_ESC && sh->menu_open) {
        menu_set_visible(sh, 0);
        return 1;
    }

    /* Alt+Tab / Alt+Shift+Tab: cycle focus through task windows. */
    if (alt && key_code == SCENE_KEY_TAB) {
        if (sh->task_count == 0) return 1;  /* consumed, nothing to do */
        scene_node_id cur = scene_store_focus(sh->store);
        int idx = -1;
        uint32_t i;
        for (i = 0; i < sh->task_count; i++) {
            if (sh->tasks[i].window_id == cur) { idx = (int)i; break; }
        }
        if (shift) {
            idx = (idx <= 0) ? (int)sh->task_count - 1 : idx - 1;
        } else {
            idx = (idx < 0 || idx + 1 >= (int)sh->task_count) ? 0 : idx + 1;
        }
        scene_client_focus(sh->client, sh->tasks[idx].window_id);
        return 1;
    }

    /* Tab / Shift+Tab: cycle focus through all focusable nodes. */
    if (key_code == SCENE_KEY_TAB) {
        scene_client_focus_next(sh->client, shift ? -1 : 1);
        return 1;
    }

    /* Enter: activate the focused node (like a click). */
    if (key_code == SCENE_KEY_ENTER) {
        scene_node_id focused = scene_store_focus(sh->store);
        if (focused != SCENE_NO_PARENT) {
            return scene_shell_handle_activate(sh, focused);
        }
    }

    return 0;
}
