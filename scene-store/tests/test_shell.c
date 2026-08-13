/*
 * test_shell.c — black-box tests for the desktop shell layer.
 *
 * Uses the same loopback harness pattern as test_automation.c.
 */
#include "scene_shell.h"
#include "scene_compositor.h"
#include "scene_transport.h"
#include "scene_client.h"
#include "scene_server.h"
#include "scene_a11y.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

/* ---- harness --------------------------------------------------------- */

struct harness {
    scene_loopback    *lb;
    scene_transport   *server_ts;
    scene_client      *cl;
    scene_compositor  *cp;
};

static void tickf(struct harness *h)
{
    int round;
    for (round = 0; round < 4; round++) {
        scene_client_flush(h->cl);
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
    scene_compositor_frame(h->cp);
}

static void harness_init(struct harness *h)
{
    memset(h, 0, sizeof(*h));
    h->lb = scene_loopback_new();
    h->server_ts = scene_loopback_server_end(h->lb);
    h->cl = scene_client_new();
    h->cp = scene_compositor_new(NULL, 800, 600);
    scene_server_attach(scene_compositor_server(h->cp));
    scene_client_connect(h->cl, scene_loopback_client_end(h->lb),
                         "shell", NULL, NULL);
    tickf(h);
}

static void harness_destroy(struct harness *h)
{
    scene_client_free(h->cl);
    scene_compositor_free(h->cp);
    scene_loopback_free(h->lb);
}

/* ---- Tests ----------------------------------------------------------- */

static void test_shell_build(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    CHECK(sh != NULL);

    int r = scene_shell_build(sh, 800, 600);
    CHECK_EQ(r, 0);
    tickf(&h);

    /* Verify nodes exist via a11y walk */
    scene_store *s = scene_compositor_store(h.cp);
    scene_a11y_node an;

    /* Background */
    CHECK_EQ(scene_store_a11y_node(s, 10000, &an), 0);
    CHECK_EQ(an.role, SCENE_ROLE_WINDOW);

    /* Panel */
    CHECK_EQ(scene_store_a11y_node(s, 10001, &an), 0);
    CHECK_EQ(an.role, SCENE_ROLE_PANEL);

    /* Start button */
    CHECK_EQ(scene_store_a11y_node(s, 10002, &an), 0);
    CHECK_EQ(an.role, SCENE_ROLE_BUTTON);

    /* Clock */
    CHECK_EQ(scene_store_a11y_node(s, 10003, &an), 0);
    CHECK_EQ(an.role, SCENE_ROLE_LABEL);

    /* Menu (hidden) */
    CHECK_EQ(scene_store_a11y_node(s, 10004, &an), 0);
    CHECK_EQ(an.role, SCENE_ROLE_MENU);

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_build: ok\n");
}

static void test_shell_background_rect(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    scene_shell_build(sh, 800, 600);
    tickf(&h);

    scene_store *s = scene_compositor_store(h.cp);
    scene_node_vis v;
    CHECK_EQ(scene_store_node_vis(s, 10000, &v), 0);
    CHECK_EQ(v.rect[0], 0);
    CHECK_EQ(v.rect[1], 0);
    CHECK_EQ(v.rect[2], 800);
    CHECK_EQ(v.rect[3], 600);

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_background_rect: ok\n");
}

static void test_shell_panel_rect(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    cfg.panel_height = 40;
    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    scene_shell_build(sh, 800, 600);
    tickf(&h);

    scene_store *s = scene_compositor_store(h.cp);
    scene_node_vis v;
    CHECK_EQ(scene_store_node_vis(s, 10001, &v), 0);
    CHECK_EQ(v.rect[0], 0);
    CHECK_EQ(v.rect[1], 560);    /* y = 600 - 40 */
    CHECK_EQ(v.rect[2], 800);
    CHECK_EQ(v.rect[3], 40);

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_panel_rect: ok\n");
}

static void test_shell_start_click(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    strcpy(cfg.launcher_apps[0], "terminal");
    strcpy(cfg.launcher_apps[1], "editor");
    cfg.launcher_app_count = 2;
    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    scene_shell_build(sh, 800, 600);
    tickf(&h);

    scene_store *s = scene_compositor_store(h.cp);
    scene_node_vis v;

    /* Menu should be invisible initially */
    CHECK_EQ(scene_store_node_vis(s, 10004, &v), 0);
    CHECK(!(v.flags & SCENE_FLAG_VISIBLE));

    /* Click start button */
    int consumed = scene_shell_handle_activate(sh, 10002);
    CHECK_EQ(consumed, 1);
    tickf(&h);

    /* Menu should now be visible */
    CHECK_EQ(scene_store_node_vis(s, 10004, &v), 0);
    CHECK(v.flags & SCENE_FLAG_VISIBLE);

    /* Menu items should be visible */
    CHECK_EQ(scene_store_node_vis(s, 20000, &v), 0);
    CHECK(v.flags & SCENE_FLAG_VISIBLE);

    /* Click start button again to close */
    consumed = scene_shell_handle_activate(sh, 10002);
    CHECK_EQ(consumed, 1);
    tickf(&h);

    CHECK_EQ(scene_store_node_vis(s, 10004, &v), 0);
    CHECK(!(v.flags & SCENE_FLAG_VISIBLE));

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_start_click: ok\n");
}

static void test_shell_task_list(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    scene_shell_build(sh, 800, 600);
    tickf(&h);

    /* Create a window through the client (simulates an app) */
    scene_rect r_win = {50, 50, 400, 300};
    scene_client_create_node(h.cl, SCENE_NO_PARENT, 500,
                             SCENE_ROLE_WINDOW, &r_win,
                             SCENE_FLAG_VISIBLE);
    scene_client_set_text(h.cl, 500, 1, "My App", 6);
    tickf(&h);

    /* Tick the shell to reconcile */
    scene_shell_tick(sh);
    tickf(&h);

    /* There should now be a task button in the panel (ID >= 30000) */
    scene_store *s = scene_compositor_store(h.cp);
    scene_node_vis v;
    int found = 0;
    uint32_t i;
    for (i = 30000; i < 30100; i++) {
        if (scene_store_node_vis(s, i, &v) == 0) {
            found = 1;
            break;
        }
    }
    CHECK(found);

    /* Destroy the window — flush so store sees it */
    scene_client_destroy_node(h.cl, 500);
    tickf(&h);

    /* Now reconcile: walk won't find window 500 → marks task inactive → destroys button */
    scene_shell_tick(sh);
    tickf(&h);

    /* Task button should be gone */
    int still_there = 0;
    for (i = 30000; i < 30100; i++) {
        if (scene_store_node_vis(s, i, &v) == 0)
            still_there = 1;
    }
    CHECK(!still_there);

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_task_list: ok\n");
}

static void test_shell_clock_update(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    scene_shell_build(sh, 800, 600);
    tickf(&h);

    /* Force clock text by ticking BEFORE the first tickf (so any minute
     * triggers the update; avoids midnight edge case where cur_min == 0). */
    scene_shell_tick(sh);
    tickf(&h);

    scene_store *s = scene_compositor_store(h.cp);
    scene_node_text_vis tv[16];
    int n = scene_store_node_texts(s, 10003, tv, 16);
    CHECK(n > 0);
    if (n > 0) {
        CHECK(tv[0].len == 5);  /* "HH:MM" */
    }

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_clock_update: ok\n");
}

static void test_shell_resize(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    scene_shell_build(sh, 800, 600);
    tickf(&h);

    /* Resize to 1024x768 */
    scene_shell_resize(sh, 1024, 768);
    tickf(&h);

    scene_store *s = scene_compositor_store(h.cp);
    scene_node_vis v;

    /* Background should match new size */
    CHECK_EQ(scene_store_node_vis(s, 10000, &v), 0);
    CHECK_EQ(v.rect[2], 1024);
    CHECK_EQ(v.rect[3], 768);

    /* Panel should be at new bottom */
    CHECK_EQ(scene_store_node_vis(s, 10001, &v), 0);
    CHECK_EQ(v.rect[1], 736);  /* 768 - 32 */
    CHECK_EQ(v.rect[2], 1024);

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_resize: ok\n");
}

static void test_shell_config_reload(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    scene_shell_build(sh, 800, 600);
    tickf(&h);

    /* Write temp config file with different panel_height */
    const char *path = "test_shell_tmp.conf";
    FILE *f = fopen(path, "w");
    fprintf(f, "panel_height=48\n");
    fclose(f);

    /* Use scene_shell_config_load directly (public API) */
    scene_shell_config new_cfg;
    scene_shell_config_defaults(&new_cfg);
    int r = scene_shell_config_load(&new_cfg, path);
    CHECK_EQ(r, 0);
    CHECK_EQ(new_cfg.panel_height, 48u);

    /* Also test scene_shell_load_config (convenience wrapper) */
    r = scene_shell_load_config(sh, path);
    CHECK_EQ(r, 0);

    remove(path);
    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_config_reload: ok\n");
}

static void test_shell_non_shell_click(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    scene_shell_build(sh, 800, 600);
    tickf(&h);

    /* Click on a non-shell node */
    int consumed = scene_shell_handle_activate(sh, 99999);
    CHECK_EQ(consumed, 0);

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_non_shell_click: ok\n");
}

static void test_shell_determinism(void)
{
    struct harness ha, hb;
    harness_init(&ha);
    harness_init(&hb);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    cfg.panel_height = 40;

    scene_shell *sha = scene_shell_new(ha.cl,
        scene_compositor_store(ha.cp), ha.cp, &cfg);
    scene_shell *shb = scene_shell_new(hb.cl,
        scene_compositor_store(hb.cp), hb.cp, &cfg);

    scene_shell_build(sha, 800, 600);
    tickf(&ha);
    scene_shell_build(shb, 800, 600);
    tickf(&hb);

    /* Tick both shells to set clock text */
    scene_shell_tick(sha);
    tickf(&ha);
    scene_shell_tick(shb);
    tickf(&hb);

    scene_store *sa = scene_compositor_store(ha.cp);
    scene_store *sb = scene_compositor_store(hb.cp);
    scene_node_vis va, vb;

    CHECK_EQ(scene_store_node_vis(sa, 10000, &va), 0);
    CHECK_EQ(scene_store_node_vis(sb, 10000, &vb), 0);
    CHECK_EQ(va.rect[2], vb.rect[2]);
    CHECK_EQ(va.rect[3], vb.rect[3]);

    CHECK_EQ(scene_store_node_vis(sa, 10001, &va), 0);
    CHECK_EQ(scene_store_node_vis(sb, 10001, &vb), 0);
    CHECK_EQ(va.rect[1], vb.rect[1]);
    CHECK_EQ(va.rect[3], vb.rect[3]);

    scene_node_text_vis ta, tb;
    int na = scene_store_node_texts(sa, 10003, &ta, 1);
    int nb = scene_store_node_texts(sb, 10003, &tb, 1);
    CHECK(na > 0 && nb > 0);
    if (na > 0 && nb > 0)
        CHECK_EQ(ta.len, tb.len);

    scene_shell_free(sha);
    scene_shell_free(shb);
    harness_destroy(&ha);
    harness_destroy(&hb);
    printf("test_shell_determinism: ok\n");
}

static void test_shell_hover(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    scene_shell_build(sh, 800, 600);
    tickf(&h);

    /* Set up hover style on compositor */
    scene_style_ref href = scene_compositor_setup_hover_style(h.cp,
        0xFF3A3A5E, 0xFFFFFFFF);
    CHECK_EQ(href, 1u);
    scene_shell_set_hover_style(sh, href);

    /* Pointer away from all buttons — no hover */
    scene_node_id r = scene_shell_handle_pointer(sh, 400, 300, 0);
    CHECK_EQ(r, 0);

    /* Pointer over start button (ID 10002, at panel coords 2,2 size 28x28
     * with panel_height=32, panel at y=568) — panel starts at y=568 */
    /* Start button is at (2, 570, 28, 28) in absolute coords */
    r = scene_shell_handle_pointer(sh, 10, 575, 0);
    CHECK_EQ(r, 10002);

    /* Pointer moves off start button */
    r = scene_shell_handle_pointer(sh, 400, 575, 0);
    CHECK_EQ(r, 0);

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_hover: ok\n");
}

static void test_shell_12h_clock(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    cfg.clock_12h = 1;
    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    scene_shell_build(sh, 800, 600);
    tickf(&h);

    /* Force clock text */
    scene_shell_tick(sh);
    tickf(&h);

    scene_store *s = scene_compositor_store(h.cp);
    scene_node_text_vis tv[16];
    int n = scene_store_node_texts(s, 10003, tv, 16);
    CHECK(n > 0);
    if (n > 0) {
        /* 12h format: " 1:30p" or "12:00a" — max 6 chars */
        CHECK(tv[0].len >= 4 && tv[0].len <= 6);
    }

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_12h_clock: ok\n");
}

static void test_shell_active_highlight(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    scene_shell_build(sh, 800, 600);
    tickf(&h);

    /* Set up active style */
    scene_style_ref aref = scene_compositor_setup_active_style(h.cp,
        0xFF4A4A6E, 0xFFFFFFFF);
    CHECK_EQ(aref, 2u);
    scene_shell_set_active_style(sh, aref);

    /* Create a window */
    scene_rect r_win = {50, 50, 400, 300};
    scene_client_create_node(h.cl, SCENE_NO_PARENT, 500,
                             SCENE_ROLE_WINDOW, &r_win,
                             SCENE_FLAG_VISIBLE);
    scene_client_set_text(h.cl, 500, 1, "Test App", 8);
    tickf(&h);

    /* Tick to create task button */
    scene_shell_tick(sh);
    tickf(&h);

    /* Focus the window */
    scene_client_focus(h.cl, 500);
    tickf(&h);

    /* Tick to apply active style */
    scene_shell_tick(sh);
    tickf(&h);

    /* Verify the task button has style 2 (active) */
    scene_store *s = scene_compositor_store(h.cp);
    scene_node_vis v;
    int found_active = 0;
    uint32_t i;
    for (i = 30000; i < 30100; i++) {
        if (scene_store_node_vis(s, i, &v) == 0) {
            if (v.style == 2u)
                found_active = 1;
        }
    }
    CHECK(found_active);

    /* Unfocus (focus background node — not a tracked task) */
    scene_client_focus(h.cl, 10000);
    tickf(&h);
    scene_shell_tick(sh);
    tickf(&h);

    found_active = 0;
    for (i = 30000; i < 30100; i++) {
        if (scene_store_node_vis(s, i, &v) == 0) {
            if (v.style == 2u)
                found_active = 1;
        }
    }
    CHECK(!found_active);

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_active_highlight: ok\n");
}

static void test_shell_task_text_refresh(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    scene_shell_build(sh, 800, 600);
    tickf(&h);

    /* Create a window with title "v1" */
    scene_rect r_win = {50, 50, 400, 300};
    scene_client_create_node(h.cl, SCENE_NO_PARENT, 600,
                             SCENE_ROLE_WINDOW, &r_win,
                             SCENE_FLAG_VISIBLE);
    scene_client_set_text(h.cl, 600, 1, "v1", 2);
    tickf(&h);

    scene_shell_tick(sh);
    tickf(&h);

    /* Verify task button has text "v1" */
    scene_store *s = scene_compositor_store(h.cp);
    scene_node_vis v;
    scene_node_id btn_id = 0;
    uint32_t i;
    for (i = 30000; i < 30100; i++) {
        if (scene_store_node_vis(s, i, &v) == 0) {
            btn_id = i;
            break;
        }
    }
    CHECK(btn_id != 0);
    scene_node_text_vis tv[16];
    int nt = scene_store_node_texts(s, btn_id, tv, 16);
    CHECK(nt > 0);
    if (nt > 0) {
        CHECK(tv[0].len == 2);
        CHECK(tv[0].data[0] == 'v');
        CHECK(tv[0].data[1] == '1');
    }

    /* Change window title to "v2" */
    scene_client_set_text(h.cl, 600, 1, "v2", 2);
    tickf(&h);

    /* Tick should refresh the task button text */
    scene_shell_tick(sh);
    tickf(&h);

    nt = scene_store_node_texts(s, btn_id, tv, 16);
    CHECK(nt > 0);
    if (nt > 0) {
        CHECK(tv[0].len == 2);
        CHECK(tv[0].data[0] == 'v');
        CHECK(tv[0].data[1] == '2');
    }

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_task_text_refresh: ok\n");
}

static void test_shell_key_tab_focus(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    scene_shell_build(sh, 800, 600);
    tickf(&h);

    /* Tab should cycle focus through focusable nodes. */
    int r = scene_shell_handle_key(sh, SCENE_KEY_TAB, 1, 0);
    CHECK_EQ(r, 1);

    /* Shift+Tab should cycle backwards. */
    r = scene_shell_handle_key(sh, SCENE_KEY_TAB, 1, SCENE_MOD_SHIFT);
    CHECK_EQ(r, 1);

    /* Escape with no menu open should not be consumed. */
    r = scene_shell_handle_key(sh, SCENE_KEY_ESC, 1, 0);
    CHECK_EQ(r, 0);

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_key_tab_focus: ok\n");
}

static void test_shell_key_escape_menu(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    scene_shell_build(sh, 800, 600);
    tickf(&h);

    /* Open the start menu by clicking the start button. */
    scene_shell_handle_activate(sh, 10002);
    tickf(&h);

    /* Menu should be open now. Escape should close it. */
    int r = scene_shell_handle_key(sh, SCENE_KEY_ESC, 1, 0);
    CHECK_EQ(r, 1);

    /* Second escape (menu now closed) should not be consumed. */
    r = scene_shell_handle_key(sh, SCENE_KEY_ESC, 1, 0);
    CHECK_EQ(r, 0);

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_key_escape_menu: ok\n");
}

/* ---- Wallpaper integration tests -------------------------------------- */

static void test_shell_wallpaper_plasma(void)
{
    printf("test_shell_wallpaper_plasma:\n");
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    cfg.wallpaper_mode = SCENE_WP_PLASMA;

    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    CHECK(sh != NULL);
    int r = scene_shell_build(sh, 800, 600);
    CHECK_EQ(r, 0);
    tickf(&h);

    /* Tick should advance wallpaper — tick the shell */
    scene_shell_tick(sh);
    tickf(&h);

    /* Second tick — wallpaper should produce pixels (plasma always changes) */
    scene_shell_tick(sh);
    tickf(&h);

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_wallpaper_plasma: ok\n");
}

static void test_shell_wallpaper_config(void)
{
    printf("test_shell_wallpaper_config:\n");
    /* Write a temp config with wallpaper options */
    const char *tmp = "test_wp_shell.conf";
    FILE *f = fopen(tmp, "w");
    fprintf(f, "background_color=0xFF000000\n");
    fprintf(f, "wallpaper_mode=aurora\n");
    fprintf(f, "wallpaper_speed=2.0\n");
    fprintf(f, "wallpaper_path=/tmp/wall.bmp\n");
    fclose(f);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    CHECK_EQ(scene_shell_config_load(&cfg, tmp), 0);
    CHECK_EQ(cfg.wallpaper_mode, (uint8_t)SCENE_WP_AURORA);
    /* speed is float, compare via memcpy to avoid strict-aliasing UB */
    { float expected = 2.0f; CHECK(memcmp(&cfg.wallpaper_speed, &expected, sizeof(float)) == 0); }
    CHECK(strcmp(cfg.wallpaper_path, "/tmp/wall.bmp") == 0);

    remove(tmp);
    printf("test_shell_wallpaper_config: ok\n");
}

static void test_shell_wallpaper_resize(void)
{
    printf("test_shell_wallpaper_resize:\n");
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    cfg.wallpaper_mode = SCENE_WP_PLASMA;

    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    scene_shell_build(sh, 800, 600);
    tickf(&h);
    scene_shell_tick(sh);
    tickf(&h);

    /* Resize — wallpaper should survive */
    int r = scene_shell_resize(sh, 1024, 768);
    CHECK_EQ(r, 0);
    scene_shell_tick(sh);
    tickf(&h);

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_wallpaper_resize: ok\n");
}

static void test_shell_no_wallpaper_no_compositor(void)
{
    printf("test_shell_no_wallpaper_no_compositor:\n");
    /* Shell with cp=NULL should work fine, just no wallpaper */
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    cfg.wallpaper_mode = SCENE_WP_PLASMA;

    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), NULL, &cfg);
    CHECK(sh != NULL);
    int r = scene_shell_build(sh, 800, 600);
    CHECK_EQ(r, 0);
    tickf(&h);
    scene_shell_tick(sh);
    tickf(&h);

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_no_wallpaper_no_compositor: ok\n");
}

/* Launch callback: the host hook fires instead of system(), with the
 * menu item's index and configured app name. */
static uint32_t g_launch_calls, g_launch_idx;
static char     g_launch_name[64];

static void cb_launch(void *ud, uint32_t idx, const char *name)
{
    (void)ud;
    g_launch_calls++;
    g_launch_idx = idx;
    snprintf(g_launch_name, sizeof(g_launch_name), "%s", name);
}

static void test_shell_launch_cb(void)
{
    printf("test_shell_launch_cb:\n");
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    cfg.launcher_app_count = 1;
    snprintf(cfg.launcher_apps[0], 64, "demo-app");

    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    scene_shell_build(sh, 800, 600);
    g_launch_calls = g_launch_idx = 0;
    g_launch_name[0] = '\0';
    scene_shell_set_launch_cb(sh, cb_launch, NULL);

    /* Open the menu, then activate the item: the cb must fire with
     * the item's index + name, and the menu must close. */
    CHECK_EQ(scene_shell_handle_activate(sh, 10002), 1);   /* start btn */
    tickf(&h);
    CHECK_EQ(scene_shell_handle_activate(sh, 20000), 1);   /* item 0 */
    tickf(&h);

    CHECK_EQ(g_launch_calls, 1);
    CHECK_EQ(g_launch_idx, 0);
    CHECK(strcmp(g_launch_name, "demo-app") == 0);

    /* Menu closed after launch (items hidden again) */
    scene_store *s = scene_compositor_store(h.cp);
    scene_node_vis v;
    CHECK_EQ(scene_store_node_vis(s, 20000, &v), 0);
    CHECK_EQ(v.flags & SCENE_FLAG_VISIBLE, 0u);

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_launch_cb: ok\n");
}

/* ---- window resize (pointer drag on window edges) ---------------------- */

/* Fake app window replicating scene_app_create_window_role's layout:
 * WINDOW at (0,0,260,34) — deliberately small height so the 32px
 * TITLEBAR reaches the window's bottom edge band (6px), which is where
 * the corner/bottom resize grips live (a tall window hides its bottom
 * edge under the content area, away from the titlebar). Rects are
 * absolute session space (spec §3), children anchored at the window
 * origin: TITLEBAR (0,0,w,32), LABEL (4,4,w-40,24), CLOSE (w-28,4,24,24)
 * under the titlebar, CONTENT (0,32,w,h-32). */
static void build_resize_app(struct harness *h)
{
    scene_client_create_node(h->cl, SCENE_NO_PARENT, 500, SCENE_ROLE_WINDOW,
        &(scene_rect){0, 0, 260, 34}, SCENE_FLAG_VISIBLE);
    scene_client_create_node(h->cl, 500, 501, SCENE_ROLE_TITLEBAR,
        &(scene_rect){0, 0, 260, 32},
        SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    scene_client_create_node(h->cl, 501, 502, SCENE_ROLE_LABEL,
        &(scene_rect){4, 4, 220, 24}, SCENE_FLAG_VISIBLE);
    scene_client_set_text(h->cl, 502, 1, "T", 1);
    scene_client_create_node(h->cl, 501, 503, SCENE_ROLE_BUTTON,
        &(scene_rect){232, 4, 24, 24},
        SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    scene_client_set_text(h->cl, 503, 1, "X", 1);
    scene_client_create_node(h->cl, 500, 504, SCENE_ROLE_GENERIC,
        &(scene_rect){0, 32, 260, 2}, SCENE_FLAG_VISIBLE);
    tickf(h);
}

static void test_shell_resize_corner(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    scene_shell_build(sh, 800, 600);
    tickf(&h);

    build_resize_app(&h);
    scene_store *s = scene_compositor_store(h.cp);
    scene_node_vis v;

    /* Press the bottom-right corner: (258,30) is inside the 6px bands
     * of both the right edge (x >= 254) and the bottom edge (y >= 28),
     * and still inside the titlebar (y < 32). */
    scene_node_id r = scene_shell_handle_pointer(sh, 258, 30, 0x01);
    CHECK_EQ(r, 501);              /* pressed the titlebar */

    /* Drag outward +30,+30 → 260+30 x 34+30 = 290x64. */
    r = scene_shell_handle_pointer(sh, 288, 60, 0x01);
    CHECK_EQ(r, 500);              /* resize drag reports the window */
    tickf(&h);

    CHECK_EQ(scene_store_node_vis(s, 500, &v), 0);
    CHECK_EQ(v.rect[0], 0); CHECK_EQ(v.rect[1], 0);
    CHECK_EQ(v.rect[2], 290); CHECK_EQ(v.rect[3], 64);
    CHECK_EQ(scene_store_node_vis(s, 501, &v), 0);
    CHECK_EQ(v.rect[0], 0); CHECK_EQ(v.rect[1], 0);
    CHECK_EQ(v.rect[2], 290); CHECK_EQ(v.rect[3], 32);  /* titlebar w = window w */
    CHECK_EQ(scene_store_node_vis(s, 502, &v), 0);
    CHECK_EQ(v.rect[0], 4); CHECK_EQ(v.rect[1], 4);
    CHECK_EQ(v.rect[2], 250); CHECK_EQ(v.rect[3], 24);  /* w-40 */
    CHECK_EQ(scene_store_node_vis(s, 503, &v), 0);
    CHECK_EQ(v.rect[0], 262); CHECK_EQ(v.rect[1], 4);   /* w-28 */
    CHECK_EQ(v.rect[2], 24); CHECK_EQ(v.rect[3], 24);
    CHECK_EQ(scene_store_node_vis(s, 504, &v), 0);
    CHECK_EQ(v.rect[0], 0); CHECK_EQ(v.rect[1], 32);
    CHECK_EQ(v.rect[2], 290); CHECK_EQ(v.rect[3], 32);  /* h-32 */

    /* Drag again without releasing — deltas are measured from the
     * ORIGINAL press origin (258,30), not incrementally: +60,+40 →
     * 320x74. */
    r = scene_shell_handle_pointer(sh, 318, 70, 0x01);
    CHECK_EQ(r, 500);
    tickf(&h);
    CHECK_EQ(scene_store_node_vis(s, 500, &v), 0);
    CHECK_EQ(v.rect[2], 320); CHECK_EQ(v.rect[3], 74);
    CHECK_EQ(scene_store_node_vis(s, 501, &v), 0);
    CHECK_EQ(v.rect[2], 320);
    CHECK_EQ(scene_store_node_vis(s, 504, &v), 0);
    CHECK_EQ(v.rect[2], 320); CHECK_EQ(v.rect[3], 42);

    /* Release: resize ends, state cleared. */
    r = scene_shell_handle_pointer(sh, 318, 70, 0x00);
    CHECK_EQ(r, 0);

    /* Moving again later still works: press the titlebar away from
     * every edge band (right band starts x=314, bottom band y=28),
     * drag → window position follows the move gesture, size preserved;
     * move does not re-derive children (existing behavior). */
    r = scene_shell_handle_pointer(sh, 130, 20, 0x01);
    CHECK_EQ(r, 501);
    r = scene_shell_handle_pointer(sh, 200, 50, 0x01);
    CHECK_EQ(r, 501);
    tickf(&h);
    CHECK_EQ(scene_store_node_vis(s, 500, &v), 0);
    CHECK_EQ(v.rect[0], 70);   /* 200 - 130 */
    CHECK_EQ(v.rect[1], 30);   /* 50 - 20 */
    CHECK_EQ(v.rect[2], 320); CHECK_EQ(v.rect[3], 74);
    r = scene_shell_handle_pointer(sh, 200, 50, 0x00);
    CHECK_EQ(r, 0);

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_resize_corner: ok\n");
}

static void test_shell_resize_edge(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    scene_shell_build(sh, 800, 600);
    tickf(&h);

    build_resize_app(&h);
    scene_store *s = scene_compositor_store(h.cp);
    scene_node_vis v;

    /* Right edge only: press (258,20) — inside the right band
     * (x >= 254), left of the bottom band (y=20 < 28). Drag +60,+20:
     * width grows, height stays 34. */
    scene_node_id r = scene_shell_handle_pointer(sh, 258, 20, 0x01);
    CHECK_EQ(r, 501);
    r = scene_shell_handle_pointer(sh, 318, 40, 0x01);
    CHECK_EQ(r, 500);
    tickf(&h);
    CHECK_EQ(scene_store_node_vis(s, 500, &v), 0);
    CHECK_EQ(v.rect[2], 320); CHECK_EQ(v.rect[3], 34);   /* h unchanged */
    CHECK_EQ(scene_store_node_vis(s, 501, &v), 0);
    CHECK_EQ(v.rect[2], 320); CHECK_EQ(v.rect[3], 32);
    CHECK_EQ(scene_store_node_vis(s, 504, &v), 0);
    CHECK_EQ(v.rect[2], 320); CHECK_EQ(v.rect[3], 2);    /* content h unchanged */
    CHECK_EQ(scene_store_node_vis(s, 503, &v), 0);
    CHECK_EQ(v.rect[0], 292);                           /* w-28 */
    r = scene_shell_handle_pointer(sh, 318, 40, 0x00);
    CHECK_EQ(r, 0);

    /* Bottom edge only: press (70,30) — inside the bottom band
     * (y >= 28), left of the right band (x=70 < 314 now). Drag +30,+34:
     * height grows, width stays. */
    r = scene_shell_handle_pointer(sh, 70, 30, 0x01);
    CHECK_EQ(r, 501);
    r = scene_shell_handle_pointer(sh, 100, 64, 0x01);
    CHECK_EQ(r, 500);
    tickf(&h);
    CHECK_EQ(scene_store_node_vis(s, 500, &v), 0);
    CHECK_EQ(v.rect[2], 320); CHECK_EQ(v.rect[3], 68);   /* w unchanged */
    CHECK_EQ(scene_store_node_vis(s, 501, &v), 0);
    CHECK_EQ(v.rect[2], 320); CHECK_EQ(v.rect[3], 32);
    CHECK_EQ(scene_store_node_vis(s, 504, &v), 0);
    CHECK_EQ(v.rect[2], 320); CHECK_EQ(v.rect[3], 36);   /* 68-32 */
    r = scene_shell_handle_pointer(sh, 100, 64, 0x00);
    CHECK_EQ(r, 0);

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_resize_edge: ok\n");
}

static void test_shell_resize_clamp(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    scene_shell_build(sh, 800, 600);
    tickf(&h);

    build_resize_app(&h);
    scene_store *s = scene_compositor_store(h.cp);
    scene_node_vis v;

    /* Corner press, drag far up-left: deltas (-358,-230) → raw sizes
     * (-98,-196) → both clamp to the 96x64 minimum. */
    scene_node_id r = scene_shell_handle_pointer(sh, 258, 30, 0x01);
    CHECK_EQ(r, 501);
    r = scene_shell_handle_pointer(sh, -100, -200, 0x01);
    CHECK_EQ(r, 500);
    tickf(&h);
    CHECK_EQ(scene_store_node_vis(s, 500, &v), 0);
    CHECK_EQ(v.rect[2], 96); CHECK_EQ(v.rect[3], 64);   /* never below minimum */
    CHECK_EQ(scene_store_node_vis(s, 501, &v), 0);
    CHECK_EQ(v.rect[2], 96); CHECK_EQ(v.rect[3], 32);
    CHECK_EQ(scene_store_node_vis(s, 504, &v), 0);
    CHECK_EQ(v.rect[0], 0); CHECK_EQ(v.rect[1], 32);
    CHECK_EQ(v.rect[2], 96); CHECK_EQ(v.rect[3], 32);
    CHECK_EQ(scene_store_node_vis(s, 502, &v), 0);
    CHECK_EQ(v.rect[2], 56);                            /* 96-40 */
    CHECK_EQ(scene_store_node_vis(s, 503, &v), 0);
    CHECK_EQ(v.rect[0], 68);                            /* 96-28 */
    r = scene_shell_handle_pointer(sh, -100, -200, 0x00);
    CHECK_EQ(r, 0);

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_resize_clamp: ok\n");
}

static void test_shell_resize_not_on_edges(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    scene_shell_build(sh, 800, 600);
    tickf(&h);

    build_resize_app(&h);
    scene_store *s = scene_compositor_store(h.cp);
    scene_node_vis v;

    /* Press inside the window body (the content area, not the titlebar)
     * → neither resize nor move starts; dragging changes nothing. */
    scene_node_id r = scene_shell_handle_pointer(sh, 150, 33, 0x01);
    CHECK_EQ(r, 0);
    r = scene_shell_handle_pointer(sh, 500, 400, 0x01);
    CHECK_EQ(r, 0);
    tickf(&h);
    CHECK_EQ(scene_store_node_vis(s, 500, &v), 0);
    CHECK_EQ(v.rect[0], 0); CHECK_EQ(v.rect[1], 0);
    CHECK_EQ(v.rect[2], 260); CHECK_EQ(v.rect[3], 34);  /* unchanged */
    r = scene_shell_handle_pointer(sh, 500, 400, 0x00);
    CHECK_EQ(r, 0);

    /* A titlebar press away from the edge bands still moves (not
     * resize): position follows the pointer, size preserved. */
    r = scene_shell_handle_pointer(sh, 130, 16, 0x01);
    CHECK_EQ(r, 501);
    r = scene_shell_handle_pointer(sh, 300, 100, 0x01);
    CHECK_EQ(r, 501);
    tickf(&h);
    CHECK_EQ(scene_store_node_vis(s, 500, &v), 0);
    CHECK_EQ(v.rect[0], 170); CHECK_EQ(v.rect[1], 84);
    CHECK_EQ(v.rect[2], 260); CHECK_EQ(v.rect[3], 34);
    r = scene_shell_handle_pointer(sh, 300, 100, 0x00);
    CHECK_EQ(r, 0);

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_resize_not_on_edges: ok\n");
}

int main(void)
{
    test_shell_build();
    test_shell_background_rect();
    test_shell_panel_rect();
    test_shell_start_click();
    test_shell_task_list();
    test_shell_clock_update();
    test_shell_resize();
    test_shell_config_reload();
    test_shell_non_shell_click();
    test_shell_determinism();
    test_shell_hover();
    test_shell_12h_clock();
    test_shell_active_highlight();
    test_shell_task_text_refresh();
    test_shell_key_tab_focus();
    test_shell_key_escape_menu();
    test_shell_wallpaper_plasma();
    test_shell_wallpaper_config();
    test_shell_wallpaper_resize();
    test_shell_no_wallpaper_no_compositor();
    test_shell_launch_cb();
    test_shell_resize_corner();
    test_shell_resize_edge();
    test_shell_resize_clamp();
    test_shell_resize_not_on_edges();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
