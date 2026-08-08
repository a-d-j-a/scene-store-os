/*
 * test_settings.c — black-box tests for the settings GUI.
 *
 * Uses the same harness pattern as test_shell.c: loopback transport +
 * compositor (with server) + scene_client + scene_shell + scene_settings.
 */
#include "scene_settings.h"
#include "scene_shell.h"
#include "scene_compositor.h"
#include "scene_transport.h"
#include "scene_client.h"
#include "scene_server.h"
#include "scene_store.h"
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

/* ---- harness ---------------------------------------------------------- */

struct harness {
    scene_loopback    *lb;
    scene_transport   *server_ts;
    scene_client      *cl;
    scene_compositor  *cp;
    scene_shell       *sh;
    scene_shell_config sh_cfg;
    scene_settings    *settings;
    int                apply_called;
    int                close_called;
};

static void tickf(struct harness *h)
{
    int round;
    for (round = 0; round < 4; round++) {
        uint8_t buf[8192];
        uint32_t got;
        while (scene_transport_recv(h->server_ts, buf, sizeof(buf), &got) == 0
               && got) {
            scene_server_feed(scene_compositor_server(h->cp), buf, got);
        }
        const uint8_t *f;
        uint32_t flen;
        while (scene_server_out_next_frame(scene_compositor_server(h->cp),
                                           &f, &flen) == 1)
            scene_transport_send(h->server_ts, f, flen);
        scene_client_pump(h->cl);
    }
    scene_settings_tick(h->settings);
    scene_compositor_frame(h->cp);
}

static void on_apply(const scene_shell_config *cfg, void *ud)
{
    struct harness *h = (struct harness *)ud;
    (void)cfg;
    h->apply_called++;
}

static void on_close_s(void *ud)
{
    struct harness *h = (struct harness *)ud;
    h->close_called++;
}

static void on_reset_s(void *ud) { (void)ud; }

static void harness_init(struct harness *h)
{
    memset(h, 0, sizeof(*h));
    h->lb = scene_loopback_new();
    h->server_ts = scene_loopback_server_end(h->lb);
    h->cl = scene_client_new();
    h->cp = scene_compositor_new(NULL, 800, 600);
    scene_server_attach(scene_compositor_server(h->cp));
    scene_client_connect(h->cl, scene_loopback_client_end(h->lb),
                         "settings", NULL, NULL);
    tickf(h);

    /* Create shell */
    scene_shell_config_defaults(&h->sh_cfg);
    h->sh = scene_shell_new(h->cl, scene_compositor_store(h->cp), h->cp, &h->sh_cfg);
    scene_shell_build(h->sh, 800, 600);
    tickf(h);

    /* Create settings app */
    scene_settings_cbs cbs = {
        .on_apply  = on_apply,
        .on_reset  = on_reset_s,
        .on_close  = on_close_s,
        .userdata  = h
    };
    h->settings = scene_settings_new(h->cl, scene_compositor_store(h->cp), &cbs);
}

static void harness_destroy(struct harness *h)
{
    if (h->settings) scene_settings_free(h->settings);
    if (h->sh) scene_shell_free(h->sh);
    scene_client_free(h->cl);
    scene_compositor_free(h->cp);
    scene_loopback_free(h->lb);
}

/* ---- Tests ------------------------------------------------------------ */

static void test_open_close(void)
{
    printf("test_open_close:\n");
    struct harness h;
    harness_init(&h);

    CHECK(!scene_settings_is_open(h.settings));

    int r = scene_settings_open(h.settings, 100, 100, 600, 400);
    CHECK_EQ(r, 0);
    CHECK(scene_settings_is_open(h.settings));
    tickf(&h);

    r = scene_settings_close(h.settings);
    CHECK_EQ(r, 0);
    CHECK(!scene_settings_is_open(h.settings));

    r = scene_settings_close(h.settings);
    CHECK_EQ(r, -1);

    harness_destroy(&h);
}

static void test_tab_switch(void)
{
    printf("test_tab_switch:\n");
    struct harness h;
    harness_init(&h);
    scene_settings_open(h.settings, 100, 100, 600, 400);
    tickf(&h);

    scene_node_id id;
    id = 50011;
    CHECK_EQ(scene_settings_handle_pointer(h.settings, id), 1);
    tickf(&h);

    id = 50012;
    CHECK_EQ(scene_settings_handle_pointer(h.settings, id), 1);
    tickf(&h);

    id = 50010;
    CHECK_EQ(scene_settings_handle_pointer(h.settings, id), 1);
    tickf(&h);

    id = 99999;
    CHECK_EQ(scene_settings_handle_pointer(h.settings, id), 0);

    scene_settings_close(h.settings);
    harness_destroy(&h);
}

static void test_color_edit(void)
{
    printf("test_color_edit:\n");
    struct harness h;
    harness_init(&h);
    scene_settings_open(h.settings, 100, 100, 600, 400);
    tickf(&h);

    scene_node_id id = 50040;
    CHECK_EQ(scene_settings_handle_pointer(h.settings, id), 1);

    const scene_shell_config *cfg = scene_settings_get_config(h.settings);
    CHECK(cfg->bg_color != 0xFF1A1A2E);

    CHECK_EQ(scene_settings_handle_pointer(h.settings, id), 1);
    cfg = scene_settings_get_config(h.settings);
    CHECK(cfg->bg_color != 0xFF0000FF);

    scene_settings_close(h.settings);
    harness_destroy(&h);
}

static void test_apply(void)
{
    printf("test_apply:\n");
    struct harness h;
    harness_init(&h);
    scene_settings_open(h.settings, 100, 100, 600, 400);
    tickf(&h);

    h.apply_called = 0;
    scene_node_id id = STG_ID_APPLY_BTN;
    CHECK_EQ(scene_settings_handle_pointer(h.settings, id), 1);
    CHECK_EQ(h.apply_called, 1);

    scene_settings_close(h.settings);
    harness_destroy(&h);
}

static void test_reset(void)
{
    printf("test_reset:\n");
    struct harness h;
    harness_init(&h);
    scene_settings_open(h.settings, 100, 100, 600, 400);
    tickf(&h);

    scene_node_id id = 50040;
    scene_settings_handle_pointer(h.settings, id);
    const scene_shell_config *cfg = scene_settings_get_config(h.settings);
    CHECK(cfg->bg_color != 0xFF1A1A2E);

    id = STG_ID_RESET_BTN;
    CHECK_EQ(scene_settings_handle_pointer(h.settings, id), 1);
    tickf(&h);

    cfg = scene_settings_get_config(h.settings);
    CHECK_EQ(cfg->bg_color, 0xFF1A1A2E);

    scene_settings_close(h.settings);
    harness_destroy(&h);
}

static void test_set_config(void)
{
    printf("test_set_config:\n");
    struct harness h;
    harness_init(&h);
    scene_settings_open(h.settings, 100, 100, 600, 400);
    tickf(&h);

    scene_shell_config custom;
    scene_shell_config_defaults(&custom);
    custom.bg_color = 0xFF222222;
    custom.panel_color = 0xFF333333;
    scene_settings_set_config(h.settings, &custom);
    tickf(&h);

    const scene_shell_config *cfg = scene_settings_get_config(h.settings);
    CHECK_EQ(cfg->bg_color, 0xFF222222);
    CHECK_EQ(cfg->panel_color, 0xFF333333);

    scene_settings_close(h.settings);
    harness_destroy(&h);
}

static void test_determinism(void)
{
    printf("test_determinism:\n");
    struct harness h1, h2;
    harness_init(&h1);
    harness_init(&h2);

    scene_settings_open(h1.settings, 100, 100, 600, 400);
    scene_settings_open(h2.settings, 100, 100, 600, 400);
    tickf(&h1);
    tickf(&h2);

    scene_node_id id1 = 50040;
    scene_node_id id2 = 50040;
    scene_settings_handle_pointer(h1.settings, id1);
    scene_settings_handle_pointer(h2.settings, id2);
    tickf(&h1);
    tickf(&h2);

    const scene_shell_config *c1 = scene_settings_get_config(h1.settings);
    const scene_shell_config *c2 = scene_settings_get_config(h2.settings);
    CHECK_EQ(c1->bg_color, c2->bg_color);
    CHECK_EQ(c1->panel_color, c2->panel_color);

    scene_settings_close(h1.settings);
    scene_settings_close(h2.settings);
    harness_destroy(&h1);
    harness_destroy(&h2);
}

static void test_all_tabs(void)
{
    printf("test_all_tabs:\n");
    struct harness h;
    harness_init(&h);
    scene_settings_open(h.settings, 100, 100, 600, 400);
    tickf(&h);

    /* Cycle through all 5 tabs */
    int tab;
    for (tab = 0; tab < SCENE_SETTINGS_TAB_COUNT; tab++) {
        scene_node_id tid = STG_ID_TAB_BASE + (scene_node_id)tab;
        CHECK_EQ(scene_settings_handle_pointer(h.settings, tid), 1);
        tickf(&h);
    }

    /* Clicking same tab again is still consumed (no-op) */
    scene_node_id tid = STG_ID_TAB_BASE;
    CHECK_EQ(scene_settings_handle_pointer(h.settings, tid), 1);

    scene_settings_close(h.settings);
    harness_destroy(&h);
}

static void test_close_callback(void)
{
    printf("test_close_callback:\n");
    struct harness h;
    harness_init(&h);
    scene_settings_open(h.settings, 100, 100, 600, 400);
    tickf(&h);

    h.close_called = 0;
    scene_node_id close = STG_ID_CLOSE_BTN;
    CHECK_EQ(scene_settings_handle_pointer(h.settings, close), 1);
    CHECK_EQ(h.close_called, 1);
    CHECK(!scene_settings_is_open(h.settings));

    harness_destroy(&h);
}

static void test_null_safety(void)
{
    printf("test_null_safety:\n");
    int r;
    r = scene_settings_open(NULL, 0, 0, 100, 100);
    CHECK_EQ(r, -1);
    CHECK_EQ(scene_settings_is_open(NULL), 0);
    scene_settings_free(NULL); /* should not crash */
    CHECK(scene_settings_get_config(NULL) == NULL);
}

static void test_wallpaper_mode_cycle(void)
{
    printf("test_wallpaper_mode_cycle:\n");
    struct harness h;
    harness_init(&h);
    scene_settings_open(h.settings, 100, 100, 600, 400);
    tickf(&h);

    /* Switch to wallpaper tab */
    scene_node_id wp_tab = STG_ID_TAB_BASE + SCENE_SETTINGS_TAB_WALLPAPER;
    CHECK_EQ(scene_settings_handle_pointer(h.settings, wp_tab), 1);
    tickf(&h);

    /* Mode field is field 0 -- click to cycle */
    const scene_shell_config *cfg = scene_settings_get_config(h.settings);
    uint8_t initial_mode = cfg->wallpaper_mode;
    scene_node_id mode_field = STG_ID_FIELD_BASE;
    CHECK_EQ(scene_settings_handle_pointer(h.settings, mode_field), 1);

    cfg = scene_settings_get_config(h.settings);
    CHECK_EQ(cfg->wallpaper_mode, (uint8_t)((initial_mode + 1) % 6));

    /* Click 5 more times to cycle through all modes back to start */
    int c;
    for (c = 0; c < 5; c++)
        scene_settings_handle_pointer(h.settings, mode_field);
    cfg = scene_settings_get_config(h.settings);
    CHECK_EQ(cfg->wallpaper_mode, initial_mode);

    scene_settings_close(h.settings);
    harness_destroy(&h);
    printf("test_wallpaper_mode_cycle: ok\n");
}

static void test_wallpaper_speed_cycle(void)
{
    printf("test_wallpaper_speed_cycle:\n");
    struct harness h;
    harness_init(&h);
    scene_settings_open(h.settings, 100, 100, 600, 400);
    tickf(&h);

    /* Switch to wallpaper tab */
    scene_node_id wp_tab = STG_ID_TAB_BASE + SCENE_SETTINGS_TAB_WALLPAPER;
    CHECK_EQ(scene_settings_handle_pointer(h.settings, wp_tab), 1);
    tickf(&h);

    /* Speed field is field 2 -- click to cycle through presets */
    const scene_shell_config *cfg = scene_settings_get_config(h.settings);
    float initial_speed = cfg->wallpaper_speed;
    scene_node_id speed_field = STG_ID_FIELD_BASE + 2;
    CHECK_EQ(scene_settings_handle_pointer(h.settings, speed_field), 1);

    cfg = scene_settings_get_config(h.settings);
    /* Should have changed from initial */
    CHECK(memcmp(&cfg->wallpaper_speed, &initial_speed, sizeof(float)) != 0);

    scene_settings_close(h.settings);
    harness_destroy(&h);
    printf("test_wallpaper_speed_cycle: ok\n");
}

/* ---- main ------------------------------------------------------------ */

int main(void)
{
    test_open_close();
    test_tab_switch();
    test_color_edit();
    test_apply();
    test_reset();
    test_set_config();
    test_determinism();
    test_all_tabs();
    test_close_callback();
    test_null_safety();
    test_wallpaper_mode_cycle();
    test_wallpaper_speed_cycle();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
