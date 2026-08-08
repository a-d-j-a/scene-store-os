/*
 * test_theme.c — tests for the theme save/load system.
 */
#include "scene_theme.h"
#include "scene_shell.h"
#include <stdio.h>
#include <string.h>

static int checks, failures;

#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { failures++; printf("FAIL %s:%d: %s\n", \
        __FILE__, __LINE__, #expr); } \
} while(0)
#define CHECK_EQ(a, b) do { \
    checks++; \
    if ((a) != (b)) { failures++; printf("FAIL %s:%d: %s (%lu) != %s (%lu)\n", \
        __FILE__, __LINE__, #a, (unsigned long)(a), #b, (unsigned long)(b)); } \
} while(0)

static const char *TMP = "test_theme_out.conf";

/* ---- Tests ------------------------------------------------------------ */

static void test_save_load_roundtrip(void)
{
    printf("test_save_load_roundtrip:\n");
    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    cfg.bg_color = 0xFF123456;
    cfg.panel_height = 48;
    cfg.clock_12h = 1;

    CHECK(scene_theme_save(&cfg, TMP) == 0);

    scene_shell_config loaded;
    scene_shell_config_defaults(&loaded);
    CHECK(scene_theme_load(TMP, &loaded) == 0);
    CHECK_EQ(loaded.bg_color, 0xFF123456u);
    CHECK_EQ(loaded.panel_height, 48u);
    CHECK_EQ(loaded.clock_12h, 1u);

    remove(TMP);
}

static void test_builtin_count(void)
{
    printf("test_builtin_count:\n");
    int n = scene_theme_builtin_count();
    CHECK(n >= 4);
    CHECK(scene_theme_builtin_name(0) != NULL);
    CHECK(scene_theme_builtin_name(n) == NULL);
    CHECK(scene_theme_builtin_name(-1) == NULL);
}

static void test_apply_builtin(void)
{
    printf("test_apply_builtin:\n");
    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);

    CHECK(scene_theme_apply_builtin(&cfg, "dark") == 0);
    CHECK_EQ(cfg.bg_color, 0xFF1A1A2E);

    CHECK(scene_theme_apply_builtin(&cfg, "light") == 0);
    CHECK_EQ(cfg.bg_color, 0xFFF0F0F0);

    CHECK(scene_theme_apply_builtin(&cfg, "midnight") == 0);
    CHECK_EQ(cfg.bg_color, 0xFF0D1117);

    CHECK(scene_theme_apply_builtin(&cfg, "solarized") == 0);
    CHECK_EQ(cfg.bg_color, 0xFF002B36);

    /* Unknown theme returns -1 */
    CHECK(scene_theme_apply_builtin(&cfg, "nonexistent") == -1);
}

static void test_null_safety(void)
{
    printf("test_null_safety:\n");
    CHECK(scene_theme_save(NULL, "x") == -1);
    CHECK(scene_theme_save(&((scene_shell_config){0}), NULL) == -1);
    CHECK(scene_theme_load(NULL, &((scene_shell_config){0})) == -1);
    CHECK(scene_theme_load("x", NULL) == -1);
    CHECK(scene_theme_apply_builtin(NULL, "dark") == -1);
    CHECK(scene_theme_apply_builtin(&((scene_shell_config){0}), NULL) == -1);
}

static void test_load_invalid_file(void)
{
    printf("test_load_invalid_file:\n");
    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    CHECK(scene_theme_load("nonexistent_file.conf", &cfg) != 0);
}

static void test_save_launcher_apps(void)
{
    printf("test_save_launcher_apps:\n");
    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    snprintf(cfg.launcher_apps[0], 64, "Term");
    snprintf(cfg.launcher_apps[1], 64, "Edit");
    cfg.launcher_app_count = 2;

    CHECK(scene_theme_save(&cfg, TMP) == 0);

    scene_shell_config loaded;
    scene_shell_config_defaults(&loaded);
    CHECK(scene_theme_load(TMP, &loaded) == 0);
    CHECK_EQ(loaded.launcher_app_count, 2u);
    CHECK(strcmp(loaded.launcher_apps[0], "Term") == 0);
    CHECK(strcmp(loaded.launcher_apps[1], "Edit") == 0);

    remove(TMP);
}

/* ---- main ------------------------------------------------------------ */

int main(void)
{
    test_save_load_roundtrip();
    test_builtin_count();
    test_apply_builtin();
    test_null_safety();
    test_load_invalid_file();
    test_save_launcher_apps();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
