/*
 * scene_settings.c — tabbed settings GUI for the desktop shell.
 *
 * UI layout (600×400):
 *   [Titlebar with close button]
 *   [Tab bar: Colors | Layout | Effects | Apps | Theme]
 *   [Content area — changes per tab]
 *   [Apply | Reset buttons]
 */
#include "scene_settings.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *wp_mode_names[] = {
    "Static", "Ken Burns", "Slideshow", "Plasma", "Aurora", "Gradient Mesh"
};
#define WP_MODE_COUNT (sizeof(wp_mode_names) / sizeof(wp_mode_names[0]))

/* ---- internal state --------------------------------------------------- */

#define STG_MAX_FIELDS 32

typedef struct field_entry {
    scene_node_id label_id;
    scene_node_id field_id;
    char          label_text[64];
    char          value_text[64];
    uint32_t      value_color;
    uint8_t       is_color;
    uint8_t       field_type;  /* 0=normal, 1=color, 2=mode, 3=speed */
    uint8_t       dirty;
} field_entry;

struct scene_settings {
    scene_client       *client;
    scene_store        *store;
    scene_settings_cbs  cbs;
    scene_shell_config  cfg;
    int                 is_open;
    int32_t             x, y, w, h;
    int                 active_tab;
    int                 content_dirty;

    scene_node_id       titlebar_id;
    scene_node_id       title_label_id;
    scene_node_id       close_btn_id;

    scene_node_id       tab_ids[SCENE_SETTINGS_TAB_COUNT];

    scene_node_id       content_id;
    scene_node_id       apply_btn_id;
    scene_node_id       reset_btn_id;

    field_entry         fields[STG_MAX_FIELDS];
    int                 field_count;
};

/* ---- helpers ---------------------------------------------------------- */

static int emit_create(scene_settings *st, scene_node_id parent,
                       scene_node_id id, uint16_t role,
                       int32_t x, int32_t y, int32_t w, int32_t h,
                       uint8_t flags)
{
    scene_rect r = { x, y, w, h };
    return scene_client_create_node(st->client, parent, id, role, &r, flags);
}

static int emit_text(scene_settings *st, scene_node_id id,
                     scene_text_id slot, const char *text, uint32_t len)
{
    return scene_client_set_text(st->client, id, slot, text, len);
}

static int emit_destroy(scene_settings *st, scene_node_id id)
{
    return scene_client_destroy_node(st->client, id);
}

/* ---- color string helpers --------------------------------------------- */

static void color_to_hex(uint32_t c, char *buf, int buflen)
{
    if (buflen >= 9)
        snprintf(buf, (size_t)buflen, "%08X", c);
}

/* ---- content rebuild -------------------------------------------------- */

static void clear_content(scene_settings *st)
{
    int i;
    for (i = 0; i < st->field_count; i++) {
        emit_destroy(st, st->fields[i].label_id);
        emit_destroy(st, st->fields[i].field_id);
    }
    st->field_count = 0;
    if (st->apply_btn_id) { emit_destroy(st, st->apply_btn_id); st->apply_btn_id = 0; }
    if (st->reset_btn_id) { emit_destroy(st, st->reset_btn_id); st->reset_btn_id = 0; }
}

static void add_field_t(scene_settings *st, const char *label,
                        const char *value, uint32_t color, int is_color,
                        uint8_t field_type)
{
    if (st->field_count >= STG_MAX_FIELDS) return;
    int i = st->field_count;
    int32_t fy = 8 + i * 28;

    st->fields[i].label_id = STG_ID_LABEL_BASE + (scene_node_id)i;
    st->fields[i].field_id = STG_ID_FIELD_BASE + (scene_node_id)i;
    st->fields[i].is_color = (uint8_t)is_color;
    st->fields[i].field_type = field_type;
    st->fields[i].value_color = color;
    snprintf(st->fields[i].label_text, sizeof(st->fields[i].label_text), "%s", label);
    snprintf(st->fields[i].value_text, sizeof(st->fields[i].value_text), "%s", value);

    emit_create(st, st->content_id, st->fields[i].label_id,
                SCENE_ROLE_LABEL, 8, fy, 140, 24, SCENE_FLAG_VISIBLE);
    emit_text(st, st->fields[i].label_id, 1,
              st->fields[i].label_text, (uint32_t)strlen(st->fields[i].label_text));

    emit_create(st, st->content_id, st->fields[i].field_id,
                SCENE_ROLE_BUTTON, 160, fy, 200, 24,
                SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    emit_text(st, st->fields[i].field_id, 1,
              st->fields[i].value_text, (uint32_t)strlen(st->fields[i].value_text));

    st->field_count++;
}

static void add_field(scene_settings *st, const char *label,
                      const char *value, uint32_t color, int is_color)
{
    add_field_t(st, label, value, color, is_color, is_color ? 1 : 0);
}

static void add_button(scene_settings *st, scene_node_id id,
                        const char *label, int32_t x, int32_t y)
{
    emit_create(st, st->content_id, id, SCENE_ROLE_BUTTON,
                x, y, 80, 28, SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    emit_text(st, id, 1, label, (uint32_t)strlen(label));
    if (id == STG_ID_APPLY_BTN) st->apply_btn_id = id;
    if (id == STG_ID_RESET_BTN) st->reset_btn_id = id;
}

static void rebuild_colors_tab(scene_settings *st)
{
    char hex[16];
    st->field_count = 0;

    color_to_hex(st->cfg.bg_color, hex, sizeof(hex));
    add_field(st, "Background", hex, st->cfg.bg_color, 1);
    color_to_hex(st->cfg.panel_color, hex, sizeof(hex));
    add_field(st, "Panel", hex, st->cfg.panel_color, 1);
    color_to_hex(st->cfg.button_color, hex, sizeof(hex));
    add_field(st, "Button", hex, st->cfg.button_color, 1);
    color_to_hex(st->cfg.button_text, hex, sizeof(hex));
    add_field(st, "Button Text", hex, st->cfg.button_text, 1);
    color_to_hex(st->cfg.hover_color, hex, sizeof(hex));
    add_field(st, "Hover", hex, st->cfg.hover_color, 1);
    color_to_hex(st->cfg.label_text, hex, sizeof(hex));
    add_field(st, "Label Text", hex, st->cfg.label_text, 1);
    color_to_hex(st->cfg.menu_color, hex, sizeof(hex));
    add_field(st, "Menu", hex, st->cfg.menu_color, 1);
    color_to_hex(st->cfg.menu_item_color, hex, sizeof(hex));
    add_field(st, "Menu Item", hex, st->cfg.menu_item_color, 1);
    color_to_hex(st->cfg.menu_item_text, hex, sizeof(hex));
    add_field(st, "Menu Text", hex, st->cfg.menu_item_text, 1);

    add_button(st, STG_ID_APPLY_BTN, "Apply", 400, 300);
    add_button(st, STG_ID_RESET_BTN, "Reset", 490, 300);
}

static void rebuild_layout_tab(scene_settings *st)
{
    char buf[32];
    st->field_count = 0;

    snprintf(buf, sizeof(buf), "%u", st->cfg.panel_height);
    add_field(st, "Panel Height", buf, 0, 0);
    snprintf(buf, sizeof(buf), "%u", (uint32_t)st->cfg.panel_radius);
    add_field(st, "Panel Radius", buf, 0, 0);
    snprintf(buf, sizeof(buf), "%u", (uint32_t)st->cfg.panel_border_w);
    add_field(st, "Border Width", buf, 0, 0);

    add_button(st, STG_ID_APPLY_BTN, "Apply", 400, 300);
    add_button(st, STG_ID_RESET_BTN, "Reset", 490, 300);
}

static void rebuild_effects_tab(scene_settings *st)
{
    st->field_count = 0;
    add_field(st, "12h Clock", st->cfg.clock_12h ? "Yes" : "No", 0, 0);
    add_button(st, STG_ID_APPLY_BTN, "Apply", 400, 300);
    add_button(st, STG_ID_RESET_BTN, "Reset", 490, 300);
}

static void rebuild_apps_tab(scene_settings *st)
{
    st->field_count = 0;
    uint32_t i;
    for (i = 0; i < st->cfg.launcher_app_count && i < SCENE_SHELL_MAX_APPS && i < 8; i++) {
        scene_node_id lid = STG_ID_LIST_BASE + i * 2;
        scene_node_id fid = STG_ID_LIST_BASE + i * 2 + 1;
        if (st->field_count >= STG_MAX_FIELDS) break;
        st->fields[st->field_count].label_id = lid;
        st->fields[st->field_count].field_id = fid;
        st->fields[st->field_count].is_color = 0;
        st->fields[st->field_count].value_color = 0;
        snprintf(st->fields[st->field_count].label_text, 64, "App %u", i + 1);
        snprintf(st->fields[st->field_count].value_text, 64, "%s",
                 st->cfg.launcher_apps[i]);
        int32_t fy = 8 + st->field_count * 28;
        emit_create(st, st->content_id, lid, SCENE_ROLE_LABEL,
                    8, fy, 60, 24, SCENE_FLAG_VISIBLE);
        emit_text(st, lid, 1, st->fields[st->field_count].label_text,
                  (uint32_t)strlen(st->fields[st->field_count].label_text));
        emit_create(st, st->content_id, fid, SCENE_ROLE_BUTTON,
                    80, fy, 320, 24,
                    SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
        emit_text(st, fid, 1, st->fields[st->field_count].value_text,
                  (uint32_t)strlen(st->fields[st->field_count].value_text));
        st->field_count++;
    }
    add_button(st, STG_ID_APPLY_BTN, "Apply", 400, 300);
    add_button(st, STG_ID_RESET_BTN, "Reset", 490, 300);
}

static void rebuild_theme_tab(scene_settings *st)
{
    st->field_count = 0;
    add_field(st, "Theme", "(coming soon)", 0, 0);
    add_button(st, STG_ID_APPLY_BTN, "Apply", 400, 300);
    add_button(st, STG_ID_RESET_BTN, "Reset", 490, 300);
}

static void rebuild_wallpaper_tab(scene_settings *st)
{
    st->field_count = 0;

    /* Mode selector (click to cycle) */
    const char *mode_name = (st->cfg.wallpaper_mode < WP_MODE_COUNT) ?
                            wp_mode_names[st->cfg.wallpaper_mode] : "?";
    add_field_t(st, "Mode", mode_name, 0, 0, 2);

    /* Image path */
    add_field(st, "Image Path",
              st->cfg.wallpaper_path[0] ? st->cfg.wallpaper_path : "(none)",
              0, 0);

    /* Speed */
    char spd[32];
    snprintf(spd, sizeof(spd), "%.1f", st->cfg.wallpaper_speed);
    add_field_t(st, "Speed", spd, 0, 0, 3);

    /* Slideshow directory */
    add_field(st, "Slide Dir",
              st->cfg.wallpaper_slideshow_dir[0] ?
                  st->cfg.wallpaper_slideshow_dir : "(none)",
              0, 0);

    /* Slideshow interval */
    char sec[32];
    snprintf(sec, sizeof(sec), "%.0fs", st->cfg.wallpaper_slideshow_sec);
    add_field(st, "Slide Time", sec, 0, 0);

    /* Slideshow fade */
    char fade[32];
    snprintf(fade, sizeof(fade), "%.1fs", st->cfg.wallpaper_slideshow_fade);
    add_field(st, "Fade Time", fade, 0, 0);

    add_button(st, STG_ID_APPLY_BTN, "Apply", 400, 300);
    add_button(st, STG_ID_RESET_BTN, "Reset", 490, 300);
}

static void rebuild_content(scene_settings *st)
{
    clear_content(st);
    switch (st->active_tab) {
    case SCENE_SETTINGS_TAB_COLORS:    rebuild_colors_tab(st);    break;
    case SCENE_SETTINGS_TAB_LAYOUT:    rebuild_layout_tab(st);    break;
    case SCENE_SETTINGS_TAB_EFFECTS:   rebuild_effects_tab(st);   break;
    case SCENE_SETTINGS_TAB_APPS:      rebuild_apps_tab(st);      break;
    case SCENE_SETTINGS_TAB_THEME:     rebuild_theme_tab(st);     break;
    case SCENE_SETTINGS_TAB_WALLPAPER: rebuild_wallpaper_tab(st); break;
    }
    st->content_dirty = 0;
}

static void create_tab_bar(scene_settings *st)
{
    const char *tab_names[] = { "Colors", "Layout", "Effects", "Apps",
                                "Theme", "Wallpaper" };
    int i;
    for (i = 0; i < SCENE_SETTINGS_TAB_COUNT; i++) {
        st->tab_ids[i] = STG_ID_TAB_BASE + (scene_node_id)i;
        int32_t tx = 8 + i * 100;
        emit_create(st, STG_ID_WINDOW, st->tab_ids[i],
                    SCENE_ROLE_BUTTON, tx, 36, 92, 28,
                    SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
        emit_text(st, st->tab_ids[i], 1,
                  tab_names[i], (uint32_t)strlen(tab_names[i]));
    }
}

/* ---- public API ------------------------------------------------------- */

scene_settings *scene_settings_new(scene_client *client, scene_store *store,
                                   const scene_settings_cbs *cbs)
{
    if (!client || !store) return NULL;
    scene_settings *st = (scene_settings *)calloc(1, sizeof(scene_settings));
    if (!st) return NULL;
    st->client = client;
    st->store  = store;
    if (cbs) st->cbs = *cbs;
    scene_shell_config_defaults(&st->cfg);
    st->active_tab = SCENE_SETTINGS_TAB_COLORS;
    st->content_dirty = 1;
    return st;
}

void scene_settings_free(scene_settings *st)
{
    if (!st) return;
    if (st->is_open) scene_settings_close(st);
    free(st);
}

int scene_settings_open(scene_settings *st, int32_t x, int32_t y,
                        int32_t width, int32_t height)
{
    if (!st || st->is_open) return -1;
    st->x = x;
    st->y = y;
    st->w = width;
    st->h = height;

    emit_create(st, SCENE_NO_PARENT, STG_ID_WINDOW,
                SCENE_ROLE_WINDOW, x, y, width, height,
                SCENE_FLAG_VISIBLE);

    st->titlebar_id = STG_ID_TITLEBAR;
    emit_create(st, STG_ID_WINDOW, STG_ID_TITLEBAR,
                SCENE_ROLE_PANEL, 0, 0, width, 32, SCENE_FLAG_VISIBLE);
    st->title_label_id = STG_ID_TITLE_LABEL;
    emit_create(st, STG_ID_TITLEBAR, STG_ID_TITLE_LABEL,
                SCENE_ROLE_LABEL, 8, 4, 200, 24, SCENE_FLAG_VISIBLE);
    emit_text(st, STG_ID_TITLE_LABEL, 1, "Settings", 8);

    st->close_btn_id = STG_ID_CLOSE_BTN;
    emit_create(st, STG_ID_TITLEBAR, STG_ID_CLOSE_BTN,
                SCENE_ROLE_BUTTON, width - 28, 4, 24, 24,
                SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    emit_text(st, STG_ID_CLOSE_BTN, 1, "X", 1);

    create_tab_bar(st);

    st->content_id = STG_ID_CONTENT;
    emit_create(st, STG_ID_WINDOW, STG_ID_CONTENT,
                SCENE_ROLE_PANEL, 0, 68, width, height - 68,
                SCENE_FLAG_VISIBLE);

    st->is_open = 1;
    st->content_dirty = 1;
    return 0;
}

int scene_settings_close(scene_settings *st)
{
    if (!st || !st->is_open) return -1;
    clear_content(st);
    emit_destroy(st, STG_ID_CLOSE_BTN);
    emit_destroy(st, STG_ID_TITLE_LABEL);
    emit_destroy(st, STG_ID_TITLEBAR);
    int i;
    for (i = 0; i < SCENE_SETTINGS_TAB_COUNT; i++)
        emit_destroy(st, st->tab_ids[i]);
    emit_destroy(st, STG_ID_CONTENT);
    emit_destroy(st, STG_ID_WINDOW);
    st->is_open = 0;
    return 0;
}

int scene_settings_is_open(const scene_settings *st)
{
    return st ? st->is_open : 0;
}

int scene_settings_tick(scene_settings *st)
{
    if (!st || !st->is_open) return 0;
    if (st->content_dirty) rebuild_content(st);
    return 0;
}

int scene_settings_handle_pointer(scene_settings *st, scene_node_id id)
{
    if (!st || !st->is_open) return 0;

    if (id == STG_ID_CLOSE_BTN) {
        if (st->cbs.on_close) st->cbs.on_close(st->cbs.userdata);
        scene_settings_close(st);
        return 1;
    }

    int i;
    for (i = 0; i < SCENE_SETTINGS_TAB_COUNT; i++) {
        if (id == st->tab_ids[i]) {
            if (st->active_tab != i) {
                st->active_tab = i;
                st->content_dirty = 1;
            }
            return 1;
        }
    }

    if (id == STG_ID_APPLY_BTN && st->apply_btn_id) {
        if (st->cbs.on_apply) st->cbs.on_apply(&st->cfg, st->cbs.userdata);
        return 1;
    }

    if (id == STG_ID_RESET_BTN && st->reset_btn_id) {
        scene_shell_config_defaults(&st->cfg);
        st->content_dirty = 1;
        if (st->cbs.on_reset) st->cbs.on_reset(st->cbs.userdata);
        return 1;
    }

    for (i = 0; i < st->field_count; i++) {
        if (id == st->fields[i].field_id && st->fields[i].is_color) {
            uint32_t c = st->fields[i].value_color;
            uint32_t next;
            if (c == 0xFF0000FF)      next = 0xFF00FF00;
            else if (c == 0xFF00FF00) next = 0xFFFF0000;
            else if (c == 0xFFFF0000) next = 0xFFFFFFFF;
            else if (c == 0xFFFFFFFF) next = 0xFF0000FF;
            else                      next = 0xFF0000FF;

            st->fields[i].value_color = next;
            char hex[16];
            color_to_hex(next, hex, sizeof(hex));
            snprintf(st->fields[i].value_text, sizeof(st->fields[i].value_text),
                     "%s", hex);
            emit_text(st, st->fields[i].field_id, 1,
                      st->fields[i].value_text,
                      (uint32_t)strlen(st->fields[i].value_text));

            if (st->active_tab == SCENE_SETTINGS_TAB_COLORS) {
                switch (i) {
                case 0: st->cfg.bg_color = next; break;
                case 1: st->cfg.panel_color = next; break;
                case 2: st->cfg.button_color = next; break;
                case 3: st->cfg.button_text = next; break;
                case 4: st->cfg.hover_color = next; break;
                case 5: st->cfg.label_text = next; break;
                case 6: st->cfg.menu_color = next; break;
                case 7: st->cfg.menu_item_color = next; break;
                case 8: st->cfg.menu_item_text = next; break;
                }
            }
            return 1;
        }

        /* Mode field: click to cycle wallpaper mode */
        if (id == st->fields[i].field_id &&
            st->fields[i].field_type == 2 &&
            st->active_tab == SCENE_SETTINGS_TAB_WALLPAPER) {
            st->cfg.wallpaper_mode = (st->cfg.wallpaper_mode + 1) % WP_MODE_COUNT;
            const char *name = wp_mode_names[st->cfg.wallpaper_mode];
            snprintf(st->fields[i].value_text, sizeof(st->fields[i].value_text),
                     "%s", name);
            emit_text(st, st->fields[i].field_id, 1,
                      st->fields[i].value_text,
                      (uint32_t)strlen(st->fields[i].value_text));
            return 1;
        }

        /* Speed field: click to cycle through preset speeds */
        if (id == st->fields[i].field_id &&
            st->fields[i].field_type == 3 &&
            st->active_tab == SCENE_SETTINGS_TAB_WALLPAPER) {
            static const float speeds[] = {0.5f, 1.0f, 1.5f, 2.0f, 3.0f};
            static const int nspeeds = 5;
            int si = 0;
            int s;
            for (s = 0; s < nspeeds; s++) {
                if (memcmp(&st->cfg.wallpaper_speed, &speeds[s], sizeof(float)) == 0)
                { si = s; break; }
            }
            si = (si + 1) % nspeeds;
            st->cfg.wallpaper_speed = speeds[si];
            char spd[32];
            snprintf(spd, sizeof(spd), "%.1f", speeds[si]);
            snprintf(st->fields[i].value_text, sizeof(st->fields[i].value_text),
                     "%s", spd);
            emit_text(st, st->fields[i].field_id, 1,
                      st->fields[i].value_text,
                      (uint32_t)strlen(st->fields[i].value_text));
            return 1;
        }
    }

    return 0;
}

int scene_settings_handle_key(scene_settings *st, uint32_t key_code,
                              uint8_t state, uint8_t modifiers)
{
    (void)st; (void)key_code; (void)state; (void)modifiers;
    return 0;
}

const scene_shell_config *scene_settings_get_config(const scene_settings *st)
{
    return st ? &st->cfg : NULL;
}

void scene_settings_set_config(scene_settings *st, const scene_shell_config *cfg)
{
    if (st && cfg) {
        st->cfg = *cfg;
        st->content_dirty = 1;
    }
}
