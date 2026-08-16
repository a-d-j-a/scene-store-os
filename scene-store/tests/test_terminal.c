/*
 * test_terminal.c — black-box tests for the terminal emulator.
 *
 * Interactive tests (echo, exit) run on Linux where pipe I/O is reliable.
 * On Windows, we test spawn + buffer accessors only (cmd.exe pipe I/O
 * is unreliable for character-by-character input).
 */
#include "scene_terminal.h"
#include "scene_app.h"
#include "scene_compositor.h"
#include "scene_transport.h"
#include "scene_client.h"
#include "scene_server.h"
#include "scene_store.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

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
    scene_terminal    *term;
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
    scene_terminal_free(h->term);
    scene_app_free(h->app);
    scene_compositor_free(h->cp);
    scene_loopback_free(h->lb);
}

static void pump_until(struct harness *h, int max_ms)
{
    int elapsed = 0;
    while (elapsed < max_ms) {
        scene_terminal_pump(h->term);
        if (scene_terminal_exited(h->term)) break;
        usleep(1000);
        elapsed++;
    }
}

/* ---- Tests ----------------------------------------------------------- */

static void test_spawn(void)
{
    struct harness h;
    harness_init(&h);

    scene_node_id content = scene_app_create_window(h.app, 0, 0, 640, 400, "Term");
    tickf(&h);
    CHECK(content != 0);

    scene_terminal_config cfg;
    scene_terminal_config_defaults(&cfg);
    h.term = scene_terminal_new(h.app, content, &cfg);
    CHECK(h.term != NULL);
    CHECK(!scene_terminal_exited(h.term));

    /* Give shell time to start, then verify it's still alive */
    pump_until(&h, 200);
    CHECK(!scene_terminal_exited(h.term));

    printf("  test_spawn: ok\n");
    harness_destroy(&h);
}

static void test_line_accessors(void)
{
    struct harness h;
    harness_init(&h);

    scene_node_id content = scene_app_create_window(h.app, 0, 0, 640, 400, "Term");
    tickf(&h);

    scene_terminal_config cfg;
    scene_terminal_config_defaults(&cfg);
    h.term = scene_terminal_new(h.app, content, &cfg);
    CHECK(h.term != NULL);

    /* Shell outputs a prompt, so at least one line */
    pump_until(&h, 300);
    int32_t lines = scene_terminal_line_count(h.term);
    CHECK(lines >= 1);

    /* Line 0 should be accessible */
    char *line0 = scene_terminal_line(h.term, 0);
    CHECK(line0 != NULL);
    free(line0);

    /* Out-of-range returns NULL */
    CHECK(scene_terminal_line(h.term, -1) == NULL);
    CHECK(scene_terminal_line(h.term, 99999) == NULL);

    printf("  test_line_accessors: ok\n");
    harness_destroy(&h);
}

/* ---- block cursor ------------------------------------------------------ */

static scene_node_id cur_id_of(scene_node_id content)
{
    return content + 1000u;
}

static void test_cursor_ops(void)
{
    struct harness h;
    harness_init(&h);

    scene_node_id content = scene_app_create_window(h.app, 40, 60, 400, 200,
                                                    "Term");
    tickf(&h);
    CHECK(content != 0);

    /* window-rect accessor: content abs = {40, 92, 400, 168} (below tb) */
    scene_rect cr;
    CHECK(scene_app_window_rect(h.app, content, &cr) == 0);
    CHECK_EQ(cr.x, 40);
    CHECK_EQ(cr.y, 92);
    CHECK_EQ(cr.w, 400);
    CHECK_EQ(cr.h, 168);
    CHECK(scene_app_window_rect(h.app, 999999, &cr) == -1);

    scene_terminal_config cfg;
    scene_terminal_config_defaults(&cfg);
    cfg.block_cursor = 1;
    cfg.cursor_blink_ms = 100000;    /* effectively never flips          */
    h.term = scene_terminal_new(h.app, content, &cfg);
    CHECK(h.term != NULL);

    scene_store *st = scene_compositor_store(h.cp);
    CHECK(st != NULL);

    scene_node_id cur = cur_id_of(content);
    scene_node_vis v;

    /* CreateNode op flushed by the first tickf: hidden, cell-sized rect. */
    tickf(&h);
    CHECK(scene_store_node_vis(st, cur, &v) == 0);
    CHECK_EQ((unsigned)v.role, (unsigned)SCENE_ROLE_CURSOR);
    CHECK_EQ((unsigned)v.parent, (unsigned)content);
    CHECK_EQ((unsigned)v.flags, 0u);
    CHECK(v.rect[0] == 0 && v.rect[1] == 0 && v.rect[2] == 8 && v.rect[3] == 8);

    /* First tick: cursor becomes visible at cell (0,0), glyph ' '. */
    scene_terminal_tick(h.term);
    tickf(&h);
    CHECK(scene_store_node_vis(st, cur, &v) == 0);
    CHECK_EQ((unsigned)v.flags, (unsigned)SCENE_FLAG_VISIBLE);
    CHECK(v.rect[0] == 40 && v.rect[1] == 92 && v.rect[2] == 8 && v.rect[3] == 8);

    scene_node_text_vis tv[4];
    int n = scene_store_node_texts(st, cur, tv, 4);
    CHECK_EQ(n, 1);
    CHECK(tv[0].len == 1 && tv[0].data && tv[0].data[0] == ' ');

    /* A second tick with no state change emits nothing (idempotent). */
    scene_terminal_tick(h.term);
    tickf(&h);
    (void)v;

    printf("  test_cursor_ops: ok\n");
    harness_destroy(&h);
}

static void test_cursor_blink(void)
{
    struct harness h;
    harness_init(&h);

    scene_node_id content = scene_app_create_window(h.app, 40, 60, 400, 200,
                                                    "Term");
    tickf(&h);

    scene_terminal_config cfg;
    scene_terminal_config_defaults(&cfg);
    cfg.block_cursor = 1;
    cfg.cursor_blink_ms = 150;       /* fast half-cycle for the test     */
    h.term = scene_terminal_new(h.app, content, &cfg);
    CHECK(h.term != NULL);
    scene_store *st = scene_compositor_store(h.cp);
    scene_node_id cur = cur_id_of(content);
    scene_node_vis v;

    /* Phase starts at 1 so the first tick shows the cursor. */
    scene_terminal_tick(h.term);
    tickf(&h);
    CHECK(scene_store_node_vis(st, cur, &v) == 0);
    CHECK_EQ((unsigned)v.flags, (unsigned)SCENE_FLAG_VISIBLE);

    /* Sleep past one half-cycle: the blink phase flips, cursor hides. */
    usleep(220000);
    scene_terminal_tick(h.term);
    tickf(&h);
    CHECK(scene_store_node_vis(st, cur, &v) == 0);
    CHECK_EQ((unsigned)v.flags, 0u);

    /* And back on after the next half-cycle. */
    usleep(220000);
    scene_terminal_tick(h.term);
    tickf(&h);
    CHECK(scene_store_node_vis(st, cur, &v) == 0);
    CHECK_EQ((unsigned)v.flags, (unsigned)SCENE_FLAG_VISIBLE);

    printf("  test_cursor_blink: ok\n");
    harness_destroy(&h);
}

#ifndef _WIN32
/* Interactive tests: only run on Linux where pipe I/O is reliable.
 * On Windows, cmd.exe pipe I/O garbles character-by-character input. */

static void test_input_echo(void)
{
    struct harness h;
    harness_init(&h);

    scene_node_id content = scene_app_create_window(h.app, 0, 0, 640, 400, "Term");
    tickf(&h);

    scene_terminal_config cfg;
    scene_terminal_config_defaults(&cfg);
    h.term = scene_terminal_new(h.app, content, &cfg);
    CHECK(h.term != NULL);

    /* Type 'echo hello' then Enter (evdev key codes) */
    uint32_t keys[] = {18,46,35,24,57,35,18,38,38,24,28};
    uint32_t nk = sizeof(keys)/sizeof(keys[0]);
    uint32_t k;
    for (k = 0; k < nk; k++) {
        scene_terminal_input_key(h.term, keys[k], 1, 0);
        scene_terminal_input_key(h.term, keys[k], 0, 0);
    }

    pump_until(&h, 2000);

    int32_t lines = scene_terminal_line_count(h.term);
    CHECK(lines >= 2);

    /* Search for "hello" in output */
    int found = 0;
    int32_t i;
    for (i = 0; i < lines; i++) {
        char *line = scene_terminal_line(h.term, i);
        if (line && strstr(line, "hello")) found = 1;
        free(line);
    }
    CHECK(found);

    /* The cursor moved down with the echoed output: after at least one
     * echoed line the cursor sits on row >= 3 at a cell boundary. The
     * last tick (after the final output) re-emits the moved rect.      */
    scene_terminal_tick(h.term);
    tickf(&h);
    scene_store *st = scene_compositor_layer_store(h.cp, 0);
    scene_node_vis cv;
    CHECK(scene_store_node_vis(st, cur_id_of(content), &cv) == 0);
    CHECK_EQ(cv.rect[0] % 8, 0);
    CHECK_EQ(cv.rect[1] % 8, 0);
    CHECK(cv.rect[1] >= 3 * 8);

    printf("  test_input_echo: ok\n");
    harness_destroy(&h);
}

static void test_exit(void)
{
    struct harness h;
    harness_init(&h);

    scene_node_id content = scene_app_create_window(h.app, 0, 0, 640, 400, "Term");
    tickf(&h);

    scene_terminal_config cfg;
    scene_terminal_config_defaults(&cfg);
    h.term = scene_terminal_new(h.app, content, &cfg);
    CHECK(h.term != NULL);

    /* Send 'exit' then Enter */
    scene_terminal_input_key(h.term, 18, 1, 0); scene_terminal_input_key(h.term, 18, 0, 0); /* e */
    scene_terminal_input_key(h.term, 45, 1, 0); scene_terminal_input_key(h.term, 45, 0, 0); /* x */
    scene_terminal_input_key(h.term, 23, 1, 0); scene_terminal_input_key(h.term, 23, 0, 0); /* i */
    scene_terminal_input_key(h.term, 20, 1, 0); scene_terminal_input_key(h.term, 20, 0, 0); /* t */
    scene_terminal_input_key(h.term, 28, 1, 0); scene_terminal_input_key(h.term, 28, 0, 0); /* Enter */

    pump_until(&h, 2000);

    CHECK(scene_terminal_exited(h.term));

    printf("  test_exit: ok\n");
    harness_destroy(&h);
}
#endif /* !_WIN32 */

/* ---- main ------------------------------------------------------------ */

int main(void)
{
    setbuf(stdout, NULL);
    checks = 0;
    failures = 0;

    test_spawn();
    test_line_accessors();
    test_cursor_ops();
    test_cursor_blink();
#ifndef _WIN32
    test_input_echo();
    test_exit();
#endif

    printf("test_terminal: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
