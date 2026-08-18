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

#define PX(cp, x, y) scene_fb_get(scene_compositor_fb(cp), (x), (y))

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

/* System menu: the power hook records the action instead of powering
 * off the machine. */
static uint32_t g_power_calls;
static int      g_power_action;

static void cb_power(void *ud, int action)
{
    (void)ud;
    g_power_calls++;
    g_power_action = action;
}

static void test_shell_power_menu(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    cfg.launcher_app_count = 1;
    snprintf(cfg.launcher_apps[0], 64, "demo-app");

    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    scene_shell_build(sh, 800, 600);
    g_power_calls = 0;
    g_power_action = -1;
    scene_shell_set_power_cb(sh, cb_power, NULL);
    tickf(&h);

    /* Menu rects: 1 launcher + 2 system items = height 8+3*28 = 92,
     * above the 32px panel: y = 600-32-92 = 476. Items: app at 480,
     * Restart at 508, Power Off at 536 (24px tall each). */
    scene_store *s = scene_compositor_store(h.cp);
    scene_node_vis v;
    CHECK_EQ(scene_store_node_vis(s, 10004, &v), 0);   /* menu */
    CHECK_EQ(v.rect[1], 476);
    CHECK_EQ(v.rect[3], 92);
    CHECK_EQ(scene_store_node_vis(s, 20032, &v), 0);   /* restart */
    CHECK_EQ(v.rect[1], 508);
    CHECK_EQ(v.rect[3], 24);
    CHECK_EQ(scene_store_node_vis(s, 20033, &v), 0);   /* power off */
    CHECK_EQ(v.rect[1], 536);
    CHECK_EQ(v.rect[3], 24);

    /* System items hidden while the menu is closed */
    CHECK_EQ(scene_store_node_vis(s, 20032, &v), 0);
    CHECK_EQ(v.flags & SCENE_FLAG_VISIBLE, 0u);
    CHECK_EQ(scene_store_node_vis(s, 20033, &v), 0);
    CHECK_EQ(v.flags & SCENE_FLAG_VISIBLE, 0u);

    /* Open the menu: system items become visible */
    CHECK_EQ(scene_shell_handle_activate(sh, 10002), 1);
    tickf(&h);
    CHECK_EQ(scene_store_node_vis(s, 20032, &v), 0);
    CHECK(v.flags & SCENE_FLAG_VISIBLE);
    CHECK_EQ(scene_store_node_vis(s, 20033, &v), 0);
    CHECK(v.flags & SCENE_FLAG_VISIBLE);

    /* Restart item: hook fires with ACTION_RESTART, menu closes */
    CHECK_EQ(scene_shell_handle_activate(sh, 20032), 1);
    tickf(&h);
    CHECK_EQ(g_power_calls, 1);
    CHECK_EQ(g_power_action, SCENE_SHELL_ACTION_RESTART);
    CHECK_EQ(scene_store_node_vis(s, 20032, &v), 0);
    CHECK_EQ(v.flags & SCENE_FLAG_VISIBLE, 0u);

    /* Power Off item */
    CHECK_EQ(scene_shell_handle_activate(sh, 10002), 1);
    tickf(&h);
    CHECK_EQ(scene_shell_handle_activate(sh, 20033), 1);
    tickf(&h);
    CHECK_EQ(g_power_calls, 2);
    CHECK_EQ(g_power_action, SCENE_SHELL_ACTION_POWEROFF);

    /* Launcher items still work (item 0 = app; with the launch cb set) */
    CHECK_EQ(scene_shell_handle_activate(sh, 10002), 1);
    tickf(&h);
    g_launch_calls = 0;
    scene_shell_set_launch_cb(sh, cb_launch, NULL);
    CHECK_EQ(scene_shell_handle_activate(sh, 20000), 1);
    tickf(&h);
    CHECK_EQ(g_launch_calls, 1);
    CHECK_EQ(g_power_calls, 2);   /* untouched */

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_power_menu: ok\n");
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

static void test_shell_notify(void)
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
    scene_node_text_vis tv[16];
    int n;
    uint32_t i;

    /* Nothing before the first notify (lazy creation) */
    CHECK(scene_store_node_vis(s, 60000, &v) != 0);

    /* Raise: toast + title + body visible, texts set */
    CHECK_EQ(scene_shell_notify(sh, "t", "hello world"), 0);
    tickf(&h);

    CHECK_EQ(scene_store_node_vis(s, 60000, &v), 0);
    CHECK_EQ(v.role, SCENE_ROLE_NOTIFICATION);
    CHECK(v.flags & SCENE_FLAG_VISIBLE);
    CHECK(!(v.flags & SCENE_FLAG_FOCUSABLE));
    CHECK_EQ(v.rect[0], 800 - 272 - 12);   /* top-right */
    CHECK_EQ(v.rect[1], 12);
    CHECK_EQ(v.rect[2], 272);
    CHECK_EQ(v.rect[3], 56);

    CHECK_EQ(scene_store_node_vis(s, 60001, &v), 0);
    CHECK(v.flags & SCENE_FLAG_VISIBLE);
    CHECK_EQ(scene_store_node_vis(s, 60002, &v), 0);
    CHECK(v.flags & SCENE_FLAG_VISIBLE);

    n = scene_store_node_texts(s, 60001, tv, 16);
    CHECK(n > 0);
    if (n > 0) {
        CHECK_EQ(tv[0].len, 1);
        CHECK(tv[0].data[0] == 't');
    }
    n = scene_store_node_texts(s, 60002, tv, 16);
    CHECK(n > 0);
    if (n > 0) {
        CHECK_EQ(tv[0].len, 11);
        CHECK(memcmp(tv[0].data, "hello world", 11) == 0);
    }

    /* TOAST_TICKS-1 ticks: still visible (life decremented only) */
    for (i = 0; i < SCENE_SHELL_TOAST_TICKS - 1; i++) {
        scene_shell_tick(sh);
        tickf(&h);
    }
    CHECK_EQ(scene_store_node_vis(s, 60000, &v), 0);
    CHECK(v.flags & SCENE_FLAG_VISIBLE);

    /* One more tick: hidden */
    scene_shell_tick(sh);
    tickf(&h);
    CHECK_EQ(scene_store_node_vis(s, 60000, &v), 0);
    CHECK(!(v.flags & SCENE_FLAG_VISIBLE));
    CHECK_EQ(scene_store_node_vis(s, 60001, &v), 0);
    CHECK(!(v.flags & SCENE_FLAG_VISIBLE));
    CHECK_EQ(scene_store_node_vis(s, 60002, &v), 0);
    CHECK(!(v.flags & SCENE_FLAG_VISIBLE));

    /* Re-raise: same nodes (no recreate), replaced body text */
    CHECK_EQ(scene_shell_notify(sh, "t2", "v2"), 0);
    tickf(&h);
    CHECK_EQ(scene_store_node_vis(s, 60000, &v), 0);
    CHECK(v.flags & SCENE_FLAG_VISIBLE);
    n = scene_store_node_texts(s, 60002, tv, 16);
    CHECK(n > 0);
    if (n > 0) {
        CHECK_EQ(tv[0].len, 2);
        CHECK(tv[0].data[0] == 'v');
        CHECK(tv[0].data[1] == '2');
    }

    /* Hide is idempotent: another full lifetime emits nothing, no crash */
    for (i = 0; i < SCENE_SHELL_TOAST_TICKS; i++) {
        scene_shell_tick(sh);
        tickf(&h);
    }
    CHECK_EQ(scene_store_node_vis(s, 60000, &v), 0);
    CHECK(!(v.flags & SCENE_FLAG_VISIBLE));

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_notify: ok\n");
}

static void test_shell_volume_btn(void)
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
    scene_node_text_vis tv[16];
    int n;

    remove("build/scene-volume");

    /* Built: button role, visible, "vol" text */
    CHECK_EQ(scene_store_node_vis(s, 10006, &v), 0);
    CHECK_EQ(v.role, SCENE_ROLE_BUTTON);
    CHECK(v.flags & SCENE_FLAG_VISIBLE);
    n = scene_store_node_texts(s, 10006, tv, 16);
    CHECK(n > 0);
    if (n > 0) {
        CHECK_EQ(tv[0].len, 3);
        CHECK(memcmp(tv[0].data, "vol", 3) == 0);
    }

    /* Toggle to muted: text flips, volume file written "0" */
    CHECK_EQ(scene_shell_handle_activate(sh, 10006), 1);
    tickf(&h);
    n = scene_store_node_texts(s, 10006, tv, 16);
    CHECK(n > 0);
    if (n > 0) {
        CHECK_EQ(tv[0].len, 5);
        CHECK(memcmp(tv[0].data, "muted", 5) == 0);
    }
    {
        FILE *vf = fopen("build/scene-volume", "rb");
        CHECK(vf != NULL);
        if (vf) {
            CHECK_EQ(fgetc(vf), '0');
            fclose(vf);
        }
    }

    /* Toggle back: "vol" text, volume file "100" */
    CHECK_EQ(scene_shell_handle_activate(sh, 10006), 1);
    tickf(&h);
    n = scene_store_node_texts(s, 10006, tv, 16);
    CHECK(n > 0);
    if (n > 0) {
        CHECK_EQ(tv[0].len, 3);
        CHECK(memcmp(tv[0].data, "vol", 3) == 0);
    }
    {
        FILE *vf = fopen("build/scene-volume", "rb");
        CHECK(vf != NULL);
        if (vf) {
            CHECK_EQ(fgetc(vf), '1');
            CHECK_EQ(fgetc(vf), '0');
            CHECK_EQ(fgetc(vf), '0');
            fclose(vf);
        }
    }
    remove("build/scene-volume");

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_volume_btn: ok\n");
}

static void test_shell_screenshot(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    scene_shell_build(sh, 800, 600);
    tickf(&h);

    /* Settle the enter animations so the framebuffer holds identity
     * colors (the desktop bg 0xFF1A1A2E at full alpha). */
    uint32_t guard = 0;
    while (scene_compositor_anim_count(h.cp) > 0 && guard < 64) {
        scene_compositor_frame(h.cp);
        guard++;
    }
    CHECK_EQ(scene_compositor_anim_count(h.cp), 0u);

    remove("build/shot.bmp");

    /* PrtSc: key-down with no modifiers → capture + notify toast */
    CHECK_EQ(scene_shell_handle_key(sh, SCENE_KEY_SYSRQ, 1, 0), 1);

    {
        FILE *f = fopen("build/shot.bmp", "rb");
        CHECK(f != NULL);
        if (f) {
            uint8_t hdr[54], px[3];
            uint32_t row_bytes = ((800u * 3u + 3u) / 4u) * 4u;
            long off = 54 + (long)(600u - 1u - 5u) * (long)row_bytes + 5L * 3L;

            CHECK_EQ(fread(hdr, 1, 54, f), (size_t)54);
            CHECK_EQ(hdr[0], 'B');
            CHECK_EQ(hdr[1], 'M');
            /* BGR bytes of pixel (5,5) — desktop bg 0xFF1A1A2E, in its
             * bottom-up image row h-1-5 */
            CHECK_EQ(fseek(f, off, SEEK_SET), 0);
            CHECK_EQ(fread(px, 1, 3, f), (size_t)3);
            CHECK_EQ(px[0], 0x2Eu);   /* B */
            CHECK_EQ(px[1], 0x1Au);   /* G */
            CHECK_EQ(px[2], 0x1Au);   /* R */
            fclose(f);
        }
    }

    /* The capture raised a toast notification (visible after flush) */
    tickf(&h);
    scene_store *s = scene_compositor_store(h.cp);
    scene_node_vis v;
    CHECK_EQ(scene_store_node_vis(s, 60000, &v), 0);
    CHECK(v.flags & SCENE_FLAG_VISIBLE);
    remove("build/shot.bmp");

    /* Without a compositor the key is not consumed (no capture) */
    {
        scene_shell *sh0 = scene_shell_new(h.cl,
            scene_compositor_store(h.cp), NULL, &cfg);
        scene_shell_build(sh0, 800, 600);
        CHECK_EQ(scene_shell_handle_key(sh0, SCENE_KEY_SYSRQ, 1, 0), 0);
        scene_shell_free(sh0);
    }

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_screenshot: ok\n");
}

/* ---- desktop lock ------------------------------------------------------ */

/* Test checker: accepts exactly "secret". */
static int lock_check_test(void *ud, const char *password)
{
    (void)ud;
    return password && strcmp(password, "secret") == 0;
}

/* Stub clock for the autolock timeout (deterministic). */
static time_t g_fake_now;
static time_t lock_fake_clock(void) { return g_fake_now; }

static void test_shell_lock_screen(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    scene_shell *sh = scene_shell_new(h.cl, scene_compositor_store(h.cp),
                                      h.cp, &cfg);
    CHECK(sh != NULL);
    scene_shell_set_lock_check(sh, lock_check_test, NULL);
    CHECK_EQ(scene_shell_build(sh, 800, 600), 0);
    tickf(&h);
    scene_store *s = scene_compositor_store(h.cp);

    /* desktop up, unlocked */
    CHECK_EQ(scene_shell_locked(sh), 0);
    CHECK_EQ(scene_compositor_locked(h.cp), 0);
    CHECK_EQ(PX(h.cp, 400, 300), 0xFF1A1A2Eu);

    /* Super+L engages the lock */
    CHECK_EQ(scene_shell_handle_key(sh, SCENE_KEY_L, 1, SCENE_MOD_SUPER), 1);
    tickf(&h);
    CHECK_EQ(scene_shell_locked(sh), 1);
    CHECK_EQ(scene_compositor_locked(h.cp), 1);

    /* lock nodes: full-screen backdrop + title/pwd/hint labels */
    scene_a11y_node an;
    CHECK_EQ(scene_store_a11y_node(s, 61000, &an), 0);
    CHECK_EQ(an.role, SCENE_ROLE_WINDOW);
    CHECK(an.flags & SCENE_FLAG_VISIBLE);
    CHECK_EQ(an.rect[2], 800);
    CHECK_EQ(an.rect[3], 600);
    CHECK_EQ(scene_store_a11y_node(s, 61001, &an), 0);
    CHECK_EQ(an.role, SCENE_ROLE_LABEL);
    CHECK_EQ(scene_store_a11y_node(s, 61002, &an), 0);
    CHECK_EQ(scene_store_a11y_node(s, 61003, &an), 0);

    /* the backdrop covers the desktop */
    CHECK_EQ(PX(h.cp, 400, 300), 0xFF0A0A14u);

    /* every key is consumed while locked (non-printables: they must
     * not leak into the password buffer) */
    CHECK_EQ(scene_shell_handle_key(sh, SCENE_KEY_TAB, 1, 0), 1);
    CHECK_EQ(scene_shell_handle_key(sh, SCENE_KEY_UP, 1, 0), 1);
    CHECK_EQ(scene_shell_handle_key(sh, SCENE_KEY_DOWN, 1, 0), 1);
    CHECK_EQ(scene_shell_handle_key(sh, SCENE_KEY_LEFT, 1, 0), 1);
    CHECK_EQ(scene_shell_handle_key(sh, SCENE_KEY_ESC, 1, 0), 1);
    CHECK_EQ(scene_shell_handle_key(sh, 15, 1, SCENE_MOD_ALT), 1);

    /* typed characters render as box-glyph dots; backspace edits */
    CHECK_EQ(scene_shell_handle_key(sh, 31, 1, 0), 1);   /* 's' */
    CHECK_EQ(scene_shell_handle_key(sh, 18, 1, 0), 1);   /* 'e' */
    CHECK_EQ(scene_shell_handle_key(sh, 46, 1, 0), 1);   /* 'c' */
    tickf(&h);
    CHECK_EQ(scene_store_a11y_node(s, 61002, &an), 0);
    CHECK_EQ(an.primary_text_len, 3u);
    CHECK_EQ(an.primary_text[0], (char)0x7F);
    CHECK_EQ(scene_shell_handle_key(sh, SCENE_KEY_BACKSPACE, 1, 0), 1);
    tickf(&h);
    CHECK_EQ(scene_store_a11y_node(s, 61002, &an), 0);
    CHECK_EQ(an.primary_text_len, 2u);

    /* wrong password: hint flips, still locked */
    CHECK_EQ(scene_shell_handle_key(sh, SCENE_KEY_ENTER, 1, 0), 1);
    tickf(&h);
    CHECK_EQ(scene_shell_locked(sh), 1);
    CHECK_EQ(scene_store_a11y_node(s, 61003, &an), 0);
    CHECK_EQ(an.primary_text_len, 14u);
    CHECK(memcmp(an.primary_text, "wrong password", 14) == 0);

    /* correct password unlocks; nodes die; desktop returns */
    scene_shell_handle_key(sh, 31, 1, 0);   /* s */
    scene_shell_handle_key(sh, 18, 1, 0);   /* e */
    scene_shell_handle_key(sh, 46, 1, 0);   /* c */
    scene_shell_handle_key(sh, 19, 1, 0);   /* r */
    scene_shell_handle_key(sh, 18, 1, 0);   /* e */
    scene_shell_handle_key(sh, 20, 1, 0);   /* t */
    CHECK_EQ(scene_shell_handle_key(sh, SCENE_KEY_ENTER, 1, 0), 1);
    tickf(&h);
    CHECK_EQ(scene_shell_locked(sh), 0);
    CHECK_EQ(scene_compositor_locked(h.cp), 0);
    CHECK(scene_store_a11y_node(s, 61000, &an) != 0);   /* gone */
    CHECK_EQ(PX(h.cp, 400, 300), 0xFF1A1A2Eu);

    /* lock is idempotent; empty password rejected by the checker */
    scene_shell_lock(sh);
    scene_shell_lock(sh);
    CHECK_EQ(scene_shell_locked(sh), 1);
    CHECK_EQ(scene_compositor_locked(h.cp), 1);
    CHECK_EQ(scene_shell_handle_key(sh, SCENE_KEY_ENTER, 1, 0), 1);
    tickf(&h);
    CHECK_EQ(scene_shell_locked(sh), 1);

    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_lock_screen: ok\n");
}

static void test_shell_autolock(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    cfg.autolock_sec = 5;

    /* the probe must be installed before scene_shell_new: the shell
     * stamps last_activity from it at construction */
    g_fake_now = 1000;
    scene_shell_clock_probe = lock_fake_clock;
    scene_shell *sh = scene_shell_new(h.cl, scene_compositor_store(h.cp),
                                      h.cp, &cfg);
    CHECK(sh != NULL);
    scene_shell_set_lock_check(sh, lock_check_test, NULL);
    CHECK_EQ(scene_shell_build(sh, 800, 600), 0);
    tickf(&h);
    CHECK_EQ(scene_shell_locked(sh), 0);

    /* idle 4 s: still open */
    g_fake_now = 1004;
    scene_shell_tick(sh);
    tickf(&h);
    CHECK_EQ(scene_shell_locked(sh), 0);

    /* idle 5 s: locked */
    g_fake_now = 1005;
    scene_shell_tick(sh);
    tickf(&h);
    CHECK_EQ(scene_shell_locked(sh), 1);
    CHECK_EQ(scene_compositor_locked(h.cp), 1);
    CHECK_EQ(PX(h.cp, 400, 300), 0xFF0A0A14u);

    /* unlock at fake 1006: typing resets the idle clock */
    g_fake_now = 1006;
    scene_shell_handle_key(sh, 31, 1, 0);   /* s */
    scene_shell_handle_key(sh, 18, 1, 0);   /* e */
    scene_shell_handle_key(sh, 46, 1, 0);   /* c */
    scene_shell_handle_key(sh, 19, 1, 0);   /* r */
    scene_shell_handle_key(sh, 18, 1, 0);   /* e */
    scene_shell_handle_key(sh, 20, 1, 0);   /* t */
    CHECK_EQ(scene_shell_handle_key(sh, SCENE_KEY_ENTER, 1, 0), 1);
    tickf(&h);
    CHECK_EQ(scene_shell_locked(sh), 0);

    /* last key at 1006: idle 4 s fine, 5 s locks again */
    g_fake_now = 1010;
    scene_shell_tick(sh);
    tickf(&h);
    CHECK_EQ(scene_shell_locked(sh), 0);
    g_fake_now = 1011;
    scene_shell_tick(sh);
    tickf(&h);
    CHECK_EQ(scene_shell_locked(sh), 1);

    scene_shell_clock_probe = NULL;
    scene_shell_free(sh);
    harness_destroy(&h);
    printf("test_shell_autolock: ok\n");
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
    test_shell_notify();
    test_shell_volume_btn();
    test_shell_screenshot();
    test_shell_lock_screen();
    test_shell_autolock();
    test_shell_power_menu();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
