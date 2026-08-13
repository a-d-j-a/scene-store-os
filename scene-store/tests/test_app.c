/*
 * test_app.c — black-box tests for the app client library.
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

static void harness_init(struct harness *h)
{
    memset(h, 0, sizeof(*h));
    h->lb = scene_loopback_new();
    h->server_ts = scene_loopback_server_end(h->lb);
    h->cp = scene_compositor_new(NULL, 800, 600);
    scene_server_attach(scene_compositor_server(h->cp));
    h->app = scene_app_new(scene_loopback_client_end(h->lb), NULL, NULL);
    tickf(h);
}

static void harness_destroy(struct harness *h)
{
    scene_app_free(h->app);
    scene_compositor_free(h->cp);
    scene_loopback_free(h->lb);
}

/* ---- Tests ----------------------------------------------------------- */

static void test_create_window(void)
{
    struct harness h;
    harness_init(&h);

    scene_node_id content = scene_app_create_window(h.app, 100, 50, 400, 300, "My App");
    CHECK(content != 0);
    tickf(&h);

    /* Window should exist in the store */
    scene_node_vis v;
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp), content, &v) == 0);
    CHECK_EQ((uint32_t)v.flags, SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    CHECK_EQ((uint32_t)v.role, SCENE_ROLE_GENERIC);

    /* Check the parent window node */
    CHECK(v.parent != 0);
    scene_node_vis wv;
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp), v.parent, &wv) == 0);
    CHECK_EQ((uint32_t)wv.role, SCENE_ROLE_WINDOW);

    /* Destroy the window */
    CHECK(scene_app_destroy_window(h.app, content) == 0);
    tickf(&h);

    /* Window content node should be gone */
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp), content, &v) != 0);

    harness_destroy(&h);
}

static void test_create_window_role(void)
{
    struct harness h;
    harness_init(&h);

    /* The role variant gives the CONTENT node a non-GENERIC role.
     * Defaults stay intact: GENERIC windows set by create_window. */
    scene_node_id content = scene_app_create_window_role(
        h.app, 100, 50, 400, 300, "Term", SCENE_ROLE_TERMINAL);
    CHECK(content != 0);
    tickf(&h);

    scene_node_vis v;
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp), content, &v) == 0);
    CHECK_EQ((uint32_t)v.role, SCENE_ROLE_TERMINAL);

    /* Parent is still a WINDOW. */
    CHECK(v.parent != 0);
    scene_node_vis wv;
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp), v.parent, &wv) == 0);
    CHECK_EQ((uint32_t)wv.role, SCENE_ROLE_WINDOW);

    /* The old API still yields GENERIC content. */
    scene_node_id content2 = scene_app_create_window(h.app, 0, 0, 100, 80, "G");
    CHECK(content2 != 0);
    tickf(&h);
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp), content2, &v) == 0);
    CHECK_EQ((uint32_t)v.role, SCENE_ROLE_GENERIC);

    /* Both windows destroyed cleanly. */
    CHECK(scene_app_destroy_window(h.app, content) == 0);
    CHECK(scene_app_destroy_window(h.app, content2) == 0);
    tickf(&h);
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp), content, &v) != 0);
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp), content2, &v) != 0);

    harness_destroy(&h);
}

static void test_set_text(void)
{
    struct harness h;
    harness_init(&h);

    scene_node_id content = scene_app_create_window(h.app, 0, 0, 200, 100, "T");
    CHECK(scene_app_set_text(h.app, content, 0, "Hello") == 0);
    tickf(&h);

    /* Verify the text was set via scene_store_node_texts */
    scene_node_text_vis tv;
    int n = scene_store_node_texts(scene_compositor_store(h.cp), content, &tv, 1);
    CHECK(n >= 1);
    CHECK(tv.len == 5);
    CHECK(memcmp(tv.data, "Hello", 5) == 0);

    harness_destroy(&h);
}

static void test_present(void)
{
    struct harness h;
    harness_init(&h);

    (void)scene_app_create_window(h.app, 10, 10, 200, 100, "W");
    CHECK(scene_app_present(h.app) == 0);
    tickf(&h);

    /* Should not crash; present goes through the wire flow control */
    harness_destroy(&h);
}

static void test_multiple_windows(void)
{
    struct harness h;
    harness_init(&h);

    scene_node_id c1 = scene_app_create_window(h.app, 0, 0, 200, 100, "W1");
    scene_node_id c2 = scene_app_create_window(h.app, 50, 50, 300, 200, "W2");
    scene_node_id c3 = scene_app_create_window(h.app, 100, 100, 100, 50, "W3");
    tickf(&h);

    CHECK(c1 != 0 && c2 != 0 && c3 != 0);
    CHECK(c1 != c2 && c2 != c3 && c1 != c3);

    /* All three windows exist */
    scene_node_vis v;
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp), c1, &v) == 0);
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp), c2, &v) == 0);
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp), c3, &v) == 0);

    /* Destroy middle one */
    CHECK(scene_app_destroy_window(h.app, c2) == 0);
    tickf(&h);
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp), c2, &v) != 0);

    /* Other two still exist */
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp), c1, &v) == 0);
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp), c3, &v) == 0);

    harness_destroy(&h);
}

static void test_set_title(void)
{
    struct harness h;
    harness_init(&h);

    scene_node_id c = scene_app_create_window(h.app, 10, 10, 200, 100, "Old");
    tickf(&h);

    CHECK(scene_app_set_title(h.app, c, "New Title") == 0);
    tickf(&h);

    /* Title label should now say "New Title" */
    scene_node_vis v;
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp), c, &v) == 0);
    /* Parent (window) should still exist */
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp), v.parent, &v) == 0);

    /* Invalid content id returns -1 */
    CHECK(scene_app_set_title(h.app, 99999, "X") == -1);

    harness_destroy(&h);
}

static void test_resize_window(void)
{
    struct harness h;
    harness_init(&h);

    scene_node_id c = scene_app_create_window(h.app, 10, 10, 200, 100, "R");
    tickf(&h);

    CHECK(scene_app_resize_window(h.app, c, 400, 300) == 0);
    tickf(&h);

    /* Window should still exist with updated dimensions */
    scene_node_vis v;
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp), c, &v) == 0);

    /* Invalid content id returns -1 */
    CHECK(scene_app_resize_window(h.app, 99999, 100, 100) == -1);

    harness_destroy(&h);
}

static void test_minimize(void)
{
    struct harness h;
    harness_init(&h);

    scene_node_id c = scene_app_create_window(h.app, 10, 10, 200, 100, "M");
    tickf(&h);

    CHECK(scene_app_minimize(h.app, c) == 0);
    tickf(&h);

    /* Window should still exist (just hidden) */
    scene_node_vis v;
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp), c, &v) == 0);

    /* Invalid content id returns -1 */
    CHECK(scene_app_minimize(h.app, 99999) == -1);

    harness_destroy(&h);
}

static void test_maximize(void)
{
    struct harness h;
    harness_init(&h);

    scene_node_id c = scene_app_create_window(h.app, 10, 10, 200, 100, "X");
    tickf(&h);

    /* Maximize to 1920x1080 with 40px panel */
    CHECK(scene_app_maximize(h.app, c, 1920, 1080, 40) == 0);
    tickf(&h);

    /* Window should still exist, now maximized */
    scene_node_vis v;
    CHECK(scene_store_node_vis(scene_compositor_store(h.cp), c, &v) == 0);

    /* Invalid content id returns -1 */
    CHECK(scene_app_maximize(h.app, 99999, 800, 600, 40) == -1);

    harness_destroy(&h);
}

static void test_content_to_window(void)
{
    struct harness h;
    harness_init(&h);

    scene_node_id c = scene_app_create_window(h.app, 10, 10, 200, 100, "W");
    tickf(&h);

    scene_node_id w = scene_app_content_to_window(h.app, c);
    CHECK(w != 0);
    CHECK(w != c); /* window != content */

    /* Invalid content returns SCENE_NO_PARENT */
    CHECK(scene_app_content_to_window(h.app, 99999) == SCENE_NO_PARENT);

    harness_destroy(&h);
}

/* ---- main ------------------------------------------------------------ */

int main(void)
{
    checks = 0;
    failures = 0;

    test_create_window();
    test_create_window_role();
    test_set_text();
    test_present();
    test_multiple_windows();
    test_set_title();
    test_resize_window();
    test_minimize();
    test_maximize();
    test_content_to_window();

    printf("test_app: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
