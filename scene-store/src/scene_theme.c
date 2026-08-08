/*
 * scene_theme.c — theme save/load for the desktop shell.
 *
 * Theme files are plain-text Option=Value pairs (same as shell.conf).
 * The loader uses the existing scene_shell_config_load() parser.
 * The saver writes each config field as a commented comment + value pair.
 */
#include "scene_theme.h"
#include <stdio.h>
#include <string.h>

/* ---- save/load -------------------------------------------------------- */

int scene_theme_save(const scene_shell_config *cfg, const char *path)
{
    if (!cfg || !path) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "# Scene OS theme file\n");
    fprintf(f, "[Theme]\n");
    fprintf(f, "background_color=0x%08X\n", cfg->bg_color);
    fprintf(f, "panel_height=%u\n", cfg->panel_height);
    fprintf(f, "panel_color=0x%08X\n", cfg->panel_color);
    fprintf(f, "panel_border=0x%08X\n", cfg->panel_border);
    fprintf(f, "panel_border_w=%u\n", cfg->panel_border_w);
    fprintf(f, "panel_radius=%u\n", cfg->panel_radius);
    fprintf(f, "button_color=0x%08X\n", cfg->button_color);
    fprintf(f, "button_border=0x%08X\n", cfg->button_border);
    fprintf(f, "button_text=0x%08X\n", cfg->button_text);
    fprintf(f, "hover_color=0x%08X\n", cfg->hover_color);
    fprintf(f, "label_text=0x%08X\n", cfg->label_text);
    fprintf(f, "menu_color=0x%08X\n", cfg->menu_color);
    fprintf(f, "menu_border=0x%08X\n", cfg->menu_border);
    fprintf(f, "menu_item_color=0x%08X\n", cfg->menu_item_color);
    fprintf(f, "menu_item_text=0x%08X\n", cfg->menu_item_text);
    fprintf(f, "clock_12h=%u\n", cfg->clock_12h);

    if (cfg->launcher_app_count > 0) {
        fprintf(f, "launcher_apps=");
        uint32_t i;
        for (i = 0; i < cfg->launcher_app_count; i++) {
            if (i > 0) fprintf(f, ",");
            fprintf(f, "%s", cfg->launcher_apps[i]);
        }
        fprintf(f, "\n");
    }

    fclose(f);
    return 0;
}

int scene_theme_load(const char *path, scene_shell_config *cfg)
{
    if (!path || !cfg) return -1;
    return scene_shell_config_load(cfg, path);
}

/* ---- built-in themes -------------------------------------------------- */

typedef struct builtin_theme {
    const char     *name;
    scene_shell_config cfg;
} builtin_theme;

static const builtin_theme builtins[] = {
    {
        "dark",
        {
            .bg_color        = 0xFF1A1A2E,
            .panel_height    = 32,
            .panel_color     = 0xFF16213E,
            .panel_border    = 0,
            .panel_border_w  = 0,
            .panel_radius    = 0,
            .button_color    = 0xFF0F3460,
            .button_border   = 0,
            .button_text     = 0xFFE0E0E0,
            .hover_color     = 0xFF1A1A4E,
            .label_text      = 0xFFE0E0E0,
            .menu_color      = 0xFF16213E,
            .menu_border     = 0,
            .menu_item_color = 0xFF0F3460,
            .menu_item_text  = 0xFFE0E0E0,
            .clock_12h       = 0,
            .launcher_app_count = 3,
            .launcher_apps   = { "Terminal", "Editor", "Files" }
        }
    },
    {
        "light",
        {
            .bg_color        = 0xFFF0F0F0,
            .panel_height    = 32,
            .panel_color     = 0xFFE0E0E0,
            .panel_border    = 0xFFCCCCCC,
            .panel_border_w  = 1,
            .panel_radius    = 0,
            .button_color    = 0xFFD0D0D0,
            .button_border   = 0xFFBBBBBB,
            .button_text     = 0xFF222222,
            .hover_color     = 0xFFC0C0C0,
            .label_text      = 0xFF333333,
            .menu_color      = 0xFFE8E8E8,
            .menu_border     = 0xFFCCCCCC,
            .menu_item_color = 0xFFD8D8D8,
            .menu_item_text  = 0xFF222222,
            .clock_12h       = 0,
            .launcher_app_count = 3,
            .launcher_apps   = { "Terminal", "Editor", "Files" }
        }
    },
    {
        "midnight",
        {
            .bg_color        = 0xFF0D1117,
            .panel_height    = 36,
            .panel_color     = 0xFF161B22,
            .panel_border    = 0xFF30363D,
            .panel_border_w  = 1,
            .panel_radius    = 4,
            .button_color    = 0xFF21262D,
            .button_border   = 0xFF30363D,
            .button_text     = 0xFFC9D1D9,
            .hover_color     = 0xFF30363D,
            .label_text      = 0xFFC9D1D9,
            .menu_color      = 0xFF161B22,
            .menu_border     = 0xFF30363D,
            .menu_item_color = 0xFF21262D,
            .menu_item_text  = 0xFFC9D1D9,
            .clock_12h       = 1,
            .launcher_app_count = 3,
            .launcher_apps   = { "Terminal", "Editor", "Files" }
        }
    },
    {
        "solarized",
        {
            .bg_color        = 0xFF002B36,
            .panel_height    = 32,
            .panel_color     = 0xFF073642,
            .panel_border    = 0,
            .panel_border_w  = 0,
            .panel_radius    = 4,
            .button_color    = 0xFF586E75,
            .button_border   = 0,
            .button_text     = 0xFFFDF6E3,
            .hover_color     = 0xFF657B83,
            .label_text      = 0xFF93A1A1,
            .menu_color      = 0xFF073642,
            .menu_border     = 0xFF586E75,
            .menu_item_color = 0xFF586E75,
            .menu_item_text  = 0xFFFDF6E3,
            .clock_12h       = 0,
            .launcher_app_count = 3,
            .launcher_apps   = { "Terminal", "Editor", "Files" }
        }
    }
};

#define BUILTIN_COUNT (sizeof(builtins) / sizeof(builtins[0]))

int scene_theme_builtin_count(void)
{
    return (int)BUILTIN_COUNT;
}

const char *scene_theme_builtin_name(int index)
{
    if (index < 0 || index >= (int)BUILTIN_COUNT) return NULL;
    return builtins[index].name;
}

int scene_theme_apply_builtin(scene_shell_config *cfg, const char *name)
{
    if (!cfg || !name) return -1;
    uint32_t i;
    for (i = 0; i < BUILTIN_COUNT; i++) {
        if (strcmp(builtins[i].name, name) == 0) {
            *cfg = builtins[i].cfg;
            return 0;
        }
    }
    return -1;
}
