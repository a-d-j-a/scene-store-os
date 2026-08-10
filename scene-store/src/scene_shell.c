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

/* ---- internal IDs ---------------------------------------------------- */
#define ID_BACKGROUND  10000u
#define ID_PANEL       10001u
#define ID_START_BTN   10002u
#define ID_CLOCK       10003u
#define ID_MENU        10004u
#define ID_MENU_BASE   20000u   /* menu items: ID_MENU_BASE + i */
#define ID_TASK_BASE   30000u   /* task buttons: ID_TASK_BASE + seq */

/* Compositor style slots owned by the shell theme. Slots 1 (hover) and
 * 2 (active) belong to iso_drm (scene_compositor_setup_hover_style /
 * setup_active_style); the shell uses 3..7 and grows the table to 8.
 * Style 0 = role default, so themed nodes must never be reset to 0. */
#define SHELL_STYLE_BG      3u
#define SHELL_STYLE_PANEL   4u
#define SHELL_STYLE_BUTTON  5u
#define SHELL_STYLE_LABEL   6u
#define SHELL_STYLE_MENU    7u
#define SHELL_STYLE_SLOTS   8u

/* ---- internal state -------------------------------------------------- */
#define MAX_TASKS 256

typedef struct task_entry {
    scene_node_id window_id;    /* the WINDOW node in the store            */
    scene_node_id button_id;    /* the BUTTON node in the panel            */
    uint8_t       active;       /* 1 = present in current reconciliation   */
} task_entry;

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

    /* launcher state */
    uint8_t              menu_open;
    uint32_t             last_clock_min;  /* for clock update debounce */

    /* hover tracking */
    scene_node_id        hovered_id;      /* current node under cursor   */
    scene_style_ref      hover_style;     /* compositor style slot for hover */
    scene_style_ref      active_style;    /* compositor style slot for focused */
    scene_node_id        active_task_id;  /* task btn with active style  */

    /* window move (drag title bar) */
    scene_node_id        moving_titlebar; /* titlebar being dragged      */
    int32_t              move_off_x, move_off_y; /* offset from pointer to window origin */

    /* wallpaper */
    scene_wallpaper     *wp;
    uint32_t             wp_tex_ref;      /* texture ref for bg wallpaper */
    int                  wp_tex_registered;
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

/* ---- tree construction ----------------------------------------------- */

/* The style a node reverts to when its hover/active style is removed.
 * Menu and menu items use the menu theme, everything else the button
 * theme. Never 0 (role default) — that would lose the shell theme. */
static scene_style_ref base_style_for(scene_node_id id)
{
    if (id == ID_MENU || (id >= ID_MENU_BASE &&
                          id < ID_MENU_BASE + SCENE_SHELL_MAX_APPS))
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

    if (!sh->client) return;
    scene_client_set_style(sh->client, ID_BACKGROUND, SHELL_STYLE_BG);
    scene_client_set_style(sh->client, ID_PANEL, SHELL_STYLE_PANEL);
    scene_client_set_style(sh->client, ID_START_BTN, SHELL_STYLE_BUTTON);
    scene_client_set_style(sh->client, ID_CLOCK, SHELL_STYLE_LABEL);
    scene_client_set_style(sh->client, ID_MENU, SHELL_STYLE_MENU);
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

    /* Launcher menu — initially hidden, positioned above start button */
    int32_t menu_y = height - (int32_t)ph - 160;
    r = emit_create(sh, SCENE_NO_PARENT, ID_MENU,
                    SCENE_ROLE_MENU, 0, menu_y,
                    160, 160,
                    0);  /* not visible initially */
    if (r != 0) return -1;

    /* Pre-create launcher item slots (hidden until menu is opened) */
    uint32_t i;
    for (i = 0; i < sh->cfg.launcher_app_count && i < SCENE_SHELL_MAX_APPS; i++) {
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

int scene_shell_tick(scene_shell *sh)
{
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

/* ---- input handling -------------------------------------------------- */

int scene_shell_handle_activate(scene_shell *sh, scene_node_id activated_id)
{
    if (!sh || !sh->built) return 0;

    /* Start button — toggle launcher menu */
    if (activated_id == ID_START_BTN) {
        sh->menu_open = !sh->menu_open;
        uint8_t flags = sh->menu_open ?
            (SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE) : 0;
        emit_flags(sh, ID_MENU, flags);
        /* Show/hide menu items */
        uint32_t i;
        for (i = 0; i < sh->cfg.launcher_app_count; i++) {
            uint8_t item_flags = sh->menu_open ?
                (SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE) : 0;
            emit_flags(sh, ID_MENU_BASE + i, item_flags);
        }
        return 1;
    }

    /* Menu item — launch app and close menu */
    if (activated_id >= ID_MENU_BASE &&
        activated_id < ID_MENU_BASE + SCENE_SHELL_MAX_APPS) {
        uint32_t idx = activated_id - ID_MENU_BASE;
        if (idx < sh->cfg.launcher_app_count) {
            /* Close menu first */
            sh->menu_open = 0;
            emit_flags(sh, ID_MENU, 0);
            uint32_t j;
            for (j = 0; j < sh->cfg.launcher_app_count; j++)
                emit_flags(sh, ID_MENU_BASE + j, 0);

            /* Launch the app (best-effort, non-blocking) */
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "%s &",
                     sh->cfg.launcher_apps[idx]);
            (void)system(cmd);
        }
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

scene_node_id scene_shell_handle_pointer(scene_shell *sh, int32_t x, int32_t y,
                                uint8_t buttons)
{
    if (!sh || !sh->built) return 0;

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
        ID_START_BTN, ID_CLOCK,
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

    /* Start window move: if button pressed on a titlebar, begin drag. */
    if ((buttons & 0x01) && hit != 0) {
        scene_node_vis v;
        if (scene_store_node_vis(sh->store, hit, &v) == 0 &&
            v.role == SCENE_ROLE_TITLEBAR &&
            v.parent != SCENE_NO_PARENT) {
            sh->moving_titlebar = hit;
            /* Compute offset from pointer to window origin. */
            scene_node_vis wv;
            if (scene_store_node_vis(sh->store, v.parent, &wv) == 0) {
                int32_t abs_x[4] = {0};
                get_abs_rect(sh->store, v.parent, abs_x);
                sh->move_off_x = x - abs_x[0];
                sh->move_off_y = y - abs_x[1];
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

    /* Reposition menu */
    emit_rect(sh, ID_MENU, 0, height - (int32_t)ph - 160, 160, 160);

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

/* ---- keyboard handling ----------------------------------------------- */

int scene_shell_handle_key(scene_shell *sh, uint32_t key_code,
                           uint8_t state, uint8_t modifiers)
{
    if (!sh || !sh->built) return 0;
    /* Only handle key-down events. */
    if (!state) return 0;

    int alt = (modifiers & SCENE_MOD_ALT) != 0;
    int shift = (modifiers & SCENE_MOD_SHIFT) != 0;

    /* Escape: close start menu if open. */
    if (key_code == SCENE_KEY_ESC && sh->menu_open) {
        sh->menu_open = 0;
        emit_flags(sh, ID_MENU, 0);
        uint32_t j;
        for (j = 0; j < sh->cfg.launcher_app_count; j++)
            emit_flags(sh, ID_MENU_BASE + j, 0);
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
