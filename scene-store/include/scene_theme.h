/*
 * scene_theme.h — theme save/load for the desktop shell.
 *
 * Themes are Option=Value text files (same format as shell.conf) with
 * an optional [Theme] section header. This allows users to save, share,
 * and switch between visual configurations.
 *
 * Usage:
 *   scene_shell_config cfg;
 *   scene_shell_config_defaults(&cfg);
 *   scene_theme_load("my-theme.conf", &cfg);
 *   scene_theme_save(&cfg, "my-theme.conf");
 */
#ifndef SCENE_THEME_H
#define SCENE_THEME_H

#include "scene_shell.h"

/* ---- lifecycle -------------------------------------------------------- */

/* Save a shell config to a theme file. Returns 0 on success.             */
int scene_theme_save(const scene_shell_config *cfg, const char *path);

/* Load a theme file into a config struct. Returns 0 on success.
 * Existing values not present in the file are preserved (update in place). */
int scene_theme_load(const char *path, scene_shell_config *cfg);

/* ---- built-in themes -------------------------------------------------- */

/* Apply a built-in theme by name: "dark", "light", "midnight", "solarized".
 * Returns 0 on success, -1 if unknown name.                              */
int scene_theme_apply_builtin(scene_shell_config *cfg, const char *name);

/* Get the number of built-in themes available.                            */
int scene_theme_builtin_count(void);

/* Get the name of a built-in theme by index (0-based).                   */
const char *scene_theme_builtin_name(int index);

#endif /* SCENE_THEME_H */
