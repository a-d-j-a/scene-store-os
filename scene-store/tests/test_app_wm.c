/*
 * test_app_wm.c — black-box tests for scene_app's opt-in WM mode:
 * titlebar drag-to-move and edge/corner drag-to-resize, driven purely
 * by the INPUT_POINTER record stream (no timers, no wall clock).
 */
#include "scene_app.h"
#include "scene_compositor.h"
#include "scene_transport.h"
#include "scene_client.h"
#include "scene_server.h"
#include "scene_store.h"
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
    scene_compositor  *cp;
    scene_app         *app;
    scene_node_id      win, tb, label, close, content;
};

static void tickf(struct harness *h)
{
    int round;
    for (round = 0; round < 4; round++) {
        scene_app_flush(h->app);
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
        scene_app_pump(h->app);
    }
    scene_compositor_frame(h->cp);
}

static void harness_init(struct harness *h, int wm)
{
    memset(h, 0, sizeof(*h));
    h->lb = scene_loopback_new();
    h->server_ts = scene_loopback_server_end(h->lb);
    h->cp = scene_compositor_new(NULL, 800, 600);
    scene_server_attach(scene_compositor_server(h->cp));
    h->app = scene_app_new(scene_loopback_client_end(h->lb), NULL, NULL);
    tickf(h);
    h->content = scene_app_create_window(h->app, 100, 50, 240, 160, "WM");
    if (wm) CHECK_EQ(scene_app_set_wm(h->app, 1), 0);
    tickf(h);
    scene_node_vis cv;
    CHECK(scene_store_node_vis(scene_compositor_store(h->cp),
                               h->content, &cv) == 0);
    h->win = cv.parent;
    h->tb = h->win + 1;
    h->label = h->win + 2;
    h->close = h->win + 3;
}

static void harness_destroy(struct harness *h)
{
    scene_app_free(h->app);
    scene_compositor_free(h->cp);
    scene_loopback_free(h->lb);
}

static void check_rect(struct harness *h, scene_node_id id,
                       int32_t x, int32_t y, int32_t w, int32_t ht)
{
    scene_node_vis v;
    CHECK(scene_store_node_vis(scene_compositor_store(h->cp), id, &v) == 0);
    CHECK_EQ((int32_t)v.rect[0], x);
    CHECK_EQ((int32_t)v.rect[1], y);
    CHECK_EQ((int32_t)v.rect[2], w);
    CHECK_EQ((int32_t)v.rect[3], ht);
}

/* Relatives: children keep their placement relative to the window.     */
static void check_rels(struct harness *h)
{
    int32_t wx = 0, wy = 0, rw = 0, rh = 0;
    scene_node_vis v;
    CHECK(scene_store_node_vis(scene_compositor_store(h->cp), h->win, &v) == 0);
    wx = v.rect[0];
    wy = v.rect[1];
    rw = v.rect[2];
    rh = v.rect[3];
    check_rect(h, h->win,    wx, wy, rw, rh);
    check_rect(h, h->tb,     wx, wy, rw, 32);
    check_rect(h, h->label,  wx + 4, wy + 4, rw - 40, 24);
    check_rect(h, h->close,  wx + rw - 28, wy + 4, 24, 24);
    check_rect(h, h->content, wx, wy + 32, rw, rh - 32);
}

/* ---- Tests ----------------------------------------------------------- */

static void test_wm_move(void)
{
    struct harness h;
    harness_init(&h, 1);

    CHECK_EQ(scene_app_wm_on(h.app), 1);
    CHECK_EQ(scene_app_set_wm(h.app, 1), 0);

    /* Window role sanity + pre-drag relatives. */
    scene_node_vis v;
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp), h.tb, &v) == 0);
    CHECK_EQ((uint32_t)v.role, SCENE_ROLE_TITLEBAR);
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp),
                               h.label, &v) == 0);
    CHECK_EQ((uint32_t)v.role, SCENE_ROLE_LABEL);
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp),
                               h.close, &v) == 0);
    CHECK_EQ((uint32_t)v.role, SCENE_ROLE_BUTTON);
    check_rect(&h, h.win,    100, 50, 240, 160);
    check_rect(&h, h.tb,     100, 50, 240, 32);
    check_rect(&h, h.label,  104, 54, 200, 24);
    check_rect(&h, h.close,  312, 54, 24, 24);
    check_rect(&h, h.content, 100, 82, 240, 128);

    /* Press titlebar at (220,66), move by (+30,+12), release.          */
    scene_compositor_input_pointer(h.cp, 0, 220, 66, 0x01);
    tickf(&h);
    scene_compositor_input_pointer(h.cp, 0, 250, 78, 0x01);
    tickf(&h);
    scene_compositor_input_pointer(h.cp, 0, 250, 78, 0x00);
    tickf(&h);

    /* Window moved by exactly (+30,+12); children keep byte-identical
     * relative placement. */
    check_rect(&h, h.win, 130, 62, 240, 160);
    check_rels(&h);

    harness_destroy(&h);
}

static void test_wm_resize(void)
{
    struct harness h;
    harness_init(&h, 1);

    /* Press bottom-right corner (w-2,h-2), drag (+40,+20), release. */
    scene_compositor_input_pointer(h.cp, 0, 338, 208, 0x01);
    tickf(&h);
    scene_compositor_input_pointer(h.cp, 0, 378, 228, 0x01);
    tickf(&h);
    scene_compositor_input_pointer(h.cp, 0, 378, 228, 0x00);
    tickf(&h);

    check_rect(&h, h.win,    100, 50, 280, 180);
    check_rect(&h, h.tb,     100, 50, 280, 32);
    check_rect(&h, h.label,  104, 54, 240, 24);
    check_rect(&h, h.close,  352, 54, 24, 24);
    check_rect(&h, h.content, 100, 82, 280, 148);

    harness_destroy(&h);
}

static void test_wm_edge_only(void)
{
    struct harness h;
    harness_init(&h, 1);

    /* Right edge only: press (w-2, mid-body), drag (+30,+50).          */
    scene_compositor_input_pointer(h.cp, 0, 338, 150, 0x01);
    tickf(&h);
    scene_compositor_input_pointer(h.cp, 0, 368, 200, 0x01);
    tickf(&h);
    scene_compositor_input_pointer(h.cp, 0, 368, 200, 0x00);
    tickf(&h);

    check_rect(&h, h.win, 100, 50, 270, 160);
    check_rect(&h, h.close, 342, 54, 24, 24);
    check_rect(&h, h.content, 100, 82, 270, 128);

    /* Bottom edge only: press (mid-body, h-2), drag (+40,+30).         */
    scene_compositor_input_pointer(h.cp, 0, 200, 206, 0x01);
    tickf(&h);
    scene_compositor_input_pointer(h.cp, 0, 240, 236, 0x01);
    tickf(&h);
    scene_compositor_input_pointer(h.cp, 0, 240, 236, 0x00);
    tickf(&h);

    check_rect(&h, h.win, 100, 50, 270, 190);
    check_rect(&h, h.tb, 100, 50, 270, 32);
    check_rect(&h, h.content, 100, 82, 270, 158);

    harness_destroy(&h);
}

static void test_wm_min_clamp(void)
{
    struct harness h;
    harness_init(&h, 1);

    /* Right-edge drag far left: w clamps at 96, origin and h unchanged. */
    scene_compositor_input_pointer(h.cp, 0, 338, 150, 0x01);
    tickf(&h);
    scene_compositor_input_pointer(h.cp, 0, -162, 150, 0x01);
    tickf(&h);
    scene_compositor_input_pointer(h.cp, 0, -162, 150, 0x00);
    tickf(&h);

    check_rect(&h, h.win, 100, 50, 96, 160);

    /* Bottom-edge drag far up: h clamps at 64. Press (150,206), the
     * bottom edge of the clamped 96-wide window (x must stay inside). */
    scene_compositor_input_pointer(h.cp, 0, 150, 206, 0x01);
    tickf(&h);
    scene_compositor_input_pointer(h.cp, 0, 150, -294, 0x01);
    tickf(&h);
    scene_compositor_input_pointer(h.cp, 0, 150, -294, 0x00);
    tickf(&h);

    check_rect(&h, h.win, 100, 50, 96, 64);

    /* Corner drag past minimum: still 96x64, never negative.            */
    scene_compositor_input_pointer(h.cp, 0, 194, 112, 0x01);
    tickf(&h);
    scene_compositor_input_pointer(h.cp, 0, 94, 12, 0x01);
    tickf(&h);
    scene_compositor_input_pointer(h.cp, 0, 94, 12, 0x00);
    tickf(&h);

    scene_node_vis v;
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp), h.win, &v) == 0);
    CHECK_EQ((int32_t)v.rect[0], 100);
    CHECK_EQ((int32_t)v.rect[1], 50);
    CHECK_EQ((int32_t)v.rect[2], 96);
    CHECK_EQ((int32_t)v.rect[3], 64);
    CHECK(v.rect[2] >= 96 && v.rect[3] >= 64);

    harness_destroy(&h);
}

static void test_wm_no_phantom(void)
{
    struct harness h;
    harness_init(&h, 1);

    /* Stray down event on the body (no prior motion, non-chrome zone):
     * a no-op — pointer record only. */
    scene_compositor_input_pointer(h.cp, 0, 300, 100, 0x01);
    tickf(&h);
    check_rect(&h, h.win, 100, 50, 240, 160);

    /* Motion without a press (buttons 0 at a new position): a no-op.    */
    scene_compositor_input_pointer(h.cp, 0, 300, 100, 0x00);
    tickf(&h);
    check_rect(&h, h.win, 100, 50, 240, 160);

    /* Duplicate press mid-drag: same point, zero delta, no change.      */
    scene_compositor_input_pointer(h.cp, 0, 220, 66, 0x01);
    tickf(&h);
    scene_compositor_input_pointer(h.cp, 0, 220, 66, 0x01);
    tickf(&h);
    scene_compositor_input_pointer(h.cp, 0, 220, 66, 0x00);
    tickf(&h);
    check_rect(&h, h.win, 100, 50, 240, 160);

    /* Disabling the WM aborts an in-flight drag and disables reactions. */
    check_rect(&h, h.content, 100, 82, 240, 128);
    harness_destroy(&h);
}

static void test_wm_off(void)
{
    struct harness h;
    harness_init(&h, 0);

    CHECK_EQ(scene_app_wm_on(h.app), 0);
    CHECK_EQ(scene_app_set_wm(h.app, 0), 0);

    /* WM off: titlebar press + drag is ignored entirely.                */
    scene_compositor_input_pointer(h.cp, 0, 220, 66, 0x01);
    tickf(&h);
    scene_compositor_input_pointer(h.cp, 0, 280, 90, 0x01);
    tickf(&h);
    scene_compositor_input_pointer(h.cp, 0, 280, 90, 0x00);
    tickf(&h);
    check_rect(&h, h.win, 100, 50, 240, 160);

    harness_destroy(&h);
}

/* Same op stream in two fresh harnesses → identical final geometry.     */
static void run_drag_seq(struct harness *h)
{
    scene_compositor_input_pointer(h->cp, 0, 220, 66, 0x01);
    tickf(h);
    scene_compositor_input_pointer(h->cp, 0, 250, 78, 0x01);
    tickf(h);
    scene_compositor_input_pointer(h->cp, 0, 235, 60, 0x01);
    tickf(h);
    scene_compositor_input_pointer(h->cp, 0, 235, 60, 0x00);
    tickf(h);
    /* Window is now at (115,44,240,160): its corner is (353,202). */
    scene_compositor_input_pointer(h->cp, 0, 353, 202, 0x01);
    tickf(h);
    scene_compositor_input_pointer(h->cp, 0, 393, 222, 0x01);
    tickf(h);
    scene_compositor_input_pointer(h->cp, 0, 413, 227, 0x01);
    tickf(h);
    scene_compositor_input_pointer(h->cp, 0, 413, 227, 0x00);
    tickf(h);
}

static void test_wm_determinism(void)
{
    struct harness h1, h2;
    harness_init(&h1, 1);
    harness_init(&h2, 1);
    run_drag_seq(&h1);
    run_drag_seq(&h2);

    scene_node_id ids1[5] = {h1.win, h1.tb, h1.label, h1.close, h1.content};
    scene_node_id ids2[5] = {h2.win, h2.tb, h2.label, h2.close, h2.content};
    int i;
    for (i = 0; i < 5; i++) {
        scene_node_vis a, b;
        CHECK(scene_store_node_vis(scene_compositor_store(h1.cp),
                                   ids1[i], &a) == 0);
        CHECK(scene_store_node_vis(scene_compositor_store(h2.cp),
                                   ids2[i], &b) == 0);
        CHECK_EQ((int32_t)a.rect[0], (int32_t)b.rect[0]);
        CHECK_EQ((int32_t)a.rect[1], (int32_t)b.rect[1]);
        CHECK_EQ((int32_t)a.rect[2], (int32_t)b.rect[2]);
        CHECK_EQ((int32_t)a.rect[3], (int32_t)b.rect[3]);
    }
    check_rect(&h1, h1.win, 115, 44, 300, 185);

    harness_destroy(&h1);
    harness_destroy(&h2);
}

/* ---- main ------------------------------------------------------------ */

int main(void)
{
    checks = 0;
    failures = 0;

    test_wm_move();
    test_wm_resize();
    test_wm_edge_only();
    test_wm_min_clamp();
    test_wm_no_phantom();
    test_wm_off();
    test_wm_determinism();

    printf("test_app_wm: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}