/*
 * test_launcher.c — the app factory: spawn real app processes, wire
 * them into the compositor as session layers.
 *
 * The host creates a compositor + scene_launcher; a real child process
 * (test_launcher_app) is spawned, connects back over TCP using the
 * SCENE_STORE_PORT env contract, builds a window via the full scene_app
 * stack, and is composited as layer 1. Tests assert: spawn/join/paint,
 * input routing to the app process (logged by the child), keyboard
 * focus routing, multi-spawn layer order, kill → reap → desktop
 * restored, and a spawn whose app never connects (timeout drop).
 *
 * Cross-process timing is bounded with retry loops; the suite must pass
 * deterministically on any machine.
 */
#include "scene_launcher.h"
#include "scene_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
static void msleep(unsigned m) { Sleep(m); }
#else
#include <unistd.h>
#include <sys/types.h>
static void msleep(unsigned m) { usleep(m * 1000); }
#endif

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do {                                                    \
    checks++;                                                               \
    if (!(cond)) {                                                          \
        failures++;                                                         \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
    }                                                                       \
} while (0)

#define CHECK_EQ(a, b) do {                                                 \
    checks++;                                                               \
    uint64_t va = (uint64_t)(a), vb = (uint64_t)(b);                        \
    if (va != vb) {                                                         \
        failures++;                                                         \
        printf("FAIL %s:%d: %s (%llu) != %s (%llu)\n", __FILE__, __LINE__,  \
               #a, (unsigned long long)va, #b, (unsigned long long)vb);     \
    }                                                                       \
} while (0)

#define PX(cp, x, y) scene_fb_get(scene_compositor_fb(cp), (x), (y))

static char *g_argv0;

/* ---- harness ---------------------------------------------------------- */

struct harness {
    scene_compositor *cp;
    scene_launcher   *sl;
    scene_loopback   *lb;       /* layer-0 shell link (owns both ends) */
    scene_transport  *server_ts;
    scene_client     *sh_cl;    /* layer-0 shell client: the desktop   */
    int  added_calls;   int added_layer;
    int  exited_calls;  int exited_layer;
    uint32_t added_pid, exited_pid;
};

static void cb_added(void *ud, int layer, uint32_t pid)
{
    struct harness *h = (struct harness *)ud;
    h->added_calls++;
    h->added_layer = layer;
    h->added_pid = pid;
}

static void cb_exited(void *ud, int layer, uint32_t pid)
{
    struct harness *h = (struct harness *)ud;
    h->exited_calls++;
    h->exited_layer = layer;
    h->exited_pid = pid;
}

/* One driver loop iteration: pump the launcher + the layer-0 shell
 * link, composite one frame. */
static void tickf(struct harness *h)
{
    scene_launcher_pump(h->sl);
    scene_client_flush(h->sh_cl);
    uint8_t buf[8192];
    uint32_t got;
    while (scene_transport_recv(h->server_ts, buf, sizeof(buf), &got) == 0
           && got) {
        if (scene_server_feed(scene_compositor_server(h->cp), buf, got)
            != 0) break;
    }
    const uint8_t *f;
    uint32_t flen;
    while (scene_server_out_next_frame(scene_compositor_server(h->cp),
                                       &f, &flen) == 1)
        scene_transport_send(h->server_ts, f, flen);
    scene_client_pump(h->sh_cl);
    scene_compositor_frame(h->cp);
}

static void harness_init(struct harness *h, uint32_t w, uint32_t hh)
{
    memset(h, 0, sizeof(*h));
    h->cp = scene_compositor_new(NULL, w, hh);
    CHECK(h->cp != NULL);
    scene_compositor_set_clear(h->cp, 0xFF101010);
    scene_launcher_cbs cbs = { cb_added, cb_exited };
    h->sl = scene_launcher_new(h->cp, NULL, &cbs, h);
    CHECK(h->sl != NULL);

    /* Layer-0 shell session: a desktop background node, exactly like the
     * real system (the shell owns the desktop; app layers damage over
     * it). Without it the compositor never establishes the background. */
    h->lb = scene_loopback_new();
    h->server_ts = scene_loopback_server_end(h->lb);
    h->sh_cl = scene_client_new();
    scene_server_attach(scene_compositor_server(h->cp));
    scene_client_connect(h->sh_cl, scene_loopback_client_end(h->lb),
                         "shell", NULL, NULL);
    tickf(h);                                  /* WELCOME */
    static const scene_rect bg = {0, 0, 800, 600};
    CHECK(scene_client_create_node(h->sh_cl, SCENE_NO_PARENT, 10000,
            SCENE_ROLE_CANVAS, &bg, SCENE_FLAG_VISIBLE) == 0);
    CHECK(scene_client_present(h->sh_cl, 0) == 0);
    tickf(h);
}

static void harness_destroy(struct harness *h)
{
    scene_launcher_free(h->sl);
    scene_client_free(h->sh_cl);
    scene_compositor_free(h->cp);
    scene_loopback_free(h->lb);
}

static void pump_n(struct harness *h, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        tickf(h);
        msleep(5);
    }
}

/* Child executable path: argv[0]-relative, then CWD-relative. */
static const char *child_exe_path(void)
{
    static char path[1024];
    const char *a0 = g_argv0 ? g_argv0 : "";
    const char *slash = strrchr(a0, '/');
    const char *bs = strrchr(a0, '\\');
    const char *sep = NULL;
    if (slash && bs) sep = slash > bs ? slash : bs;
    else if (slash) sep = slash;
    else sep = bs;
    if (sep) {
        size_t n = (size_t)(sep - a0) + 1;
        if (n < sizeof(path)) {
            snprintf(path, sizeof(path), "%.*stest_launcher_app.exe",
                     (int)n, a0);
            FILE *f = fopen(path, "rb");
            if (f) { fclose(f); return path; }
        }
    }
    snprintf(path, sizeof(path), "test_launcher_app.exe");
    return path;
}

/* Pump until the child's window is painted (window fill at 150,150). */
static void pump_until_window(struct harness *h, int max_iters)
{
    int i;
    for (i = 0; i < max_iters; i++) {
        tickf(h);
        if (PX(h->cp, 150, 150) == 0xFF202020u) break;
        msleep(5);
    }
}

/* ---- tests ------------------------------------------------------------ */

static void test_spawn_attach(void)
{
    struct harness h;
    harness_init(&h, 800, 600);

    uint32_t pid = 0;
    CHECK(scene_launcher_spawn(h.sl, child_exe_path(), NULL, &pid) == 0);
    CHECK(pid != 0);

    pump_until_window(&h, 200);
    CHECK_EQ(h.added_calls, 1);              /* joined exactly once */
    CHECK(h.added_layer >= 1);
    CHECK_EQ(h.added_pid, pid);
    CHECK_EQ(scene_launcher_session_count(h.sl), 1);

    /* The child's window is painted: fill inside, desktop outside */
    CHECK(PX(h.cp, 150, 150) == 0xFF202020u);   /* window content */
    CHECK(PX(h.cp, 50, 50) == 0xFF101010u);     /* desktop */
    CHECK(PX(h.cp, 170, 198) == 0xFF3C3C3Cu);   /* button */
    CHECK(PX(h.cp, 150, 55) == 0xFF1A1A1Au);    /* titlebar band */

    /* Only one frame's worth of damage; the app did not disturb the
     * shell-less desktop outside its window */
    CHECK_EQ(h.exited_calls, 0);

    scene_launcher_kill(h.sl, pid);
    pump_n(&h, 50);
    CHECK_EQ(h.exited_calls, 1);
    CHECK_EQ(h.exited_layer, h.added_layer);
    CHECK_EQ(h.exited_pid, pid);
    CHECK_EQ(scene_launcher_session_count(h.sl), 0);

    harness_destroy(&h);
}

static void test_input_routing(void)
{
    struct harness h;
    harness_init(&h, 800, 600);

    uint32_t pid = 0;
    CHECK(scene_launcher_spawn(h.sl, child_exe_path(), "launcher_input.log",
                               &pid) == 0);
    pump_until_window(&h, 200);
    CHECK_EQ(h.added_calls, 1);

    /* Click the button: the child must receive pointer + activate and
     * ack them (the log proves it consumed them). */
    scene_compositor_input_pointer(h.cp, 0, 170, 198, 1);
    pump_n(&h, 30);

    /* Keyboard goes to the clicked app layer (click-to-focus). */
    scene_compositor_input_key(h.cp, 30, 1, 0);   /* 'A' down */
    pump_n(&h, 30);

    CHECK(scene_compositor_focus_is_shell(h.cp) == 0);

    /* Read the child's event log */
    FILE *f = fopen("launcher_input.log", "r");
    CHECK(f != NULL);
    if (f) {
        char line[128];
        int got_p = 0, got_a = 0, got_k = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "P 170 198 1", 11) == 0) got_p = 1;
            if (line[0] == 'A' && line[1] == ' ') got_a = 1;
            if (strncmp(line, "K 30 1 0", 8) == 0) got_k = 1;
        }
        fclose(f);
        CHECK_EQ(got_p, 1);
        CHECK_EQ(got_a, 1);
        CHECK_EQ(got_k, 1);
    }
    remove("launcher_input.log");

    /* Desktop click moves keyboard focus back to the shell */
    scene_compositor_input_pointer(h.cp, 0, 700, 500, 1);
    pump_n(&h, 30);
    CHECK_EQ(scene_compositor_focus_is_shell(h.cp), 1);

    scene_launcher_kill(h.sl, pid);
    pump_n(&h, 50);
    harness_destroy(&h);
}

static void test_multi_spawn(void)
{
    struct harness h;
    harness_init(&h, 800, 600);

    uint32_t p1 = 0, p2 = 0;
    CHECK(scene_launcher_spawn(h.sl, child_exe_path(), "launcher_a.log",
                               &p1) == 0);
    CHECK(scene_launcher_spawn(h.sl, child_exe_path(), "launcher_b.log",
                               &p2) == 0);
    CHECK(p1 != p2);

    /* Both children must join: wait for both (pump_until_window breaks
     * on the first window and the second child is still connecting). */
    int i;
    for (i = 0; i < 400 && h.added_calls < 2; i++) {
        tickf(&h);
        msleep(5);
    }
    CHECK_EQ(h.added_calls, 2);
    CHECK_EQ(scene_launcher_session_count(h.sl), 2);

    /* Both children must have BUILT their windows (join fires on accept,
     * before the child finishes the welcome/build handshake). */
    for (i = 0; i < 400 && PX(h.cp, 150, 150) != 0xFF202020u; i++) {
        tickf(&h);
        msleep(5);
    }
    CHECK(PX(h.cp, 150, 150) == 0xFF202020u);

    /* Two windows, one per session layer. The slot order is the spawn
     * order, but the LAYER order is the connect order (children race to
     * connect; the first to join gets layer 1). So the layers must be
     * distinct, not slot-sorted. */
    int l1 = scene_launcher_layer_at(h.sl, 0);
    int l2 = scene_launcher_layer_at(h.sl, 1);
    CHECK(l1 >= 1 && l2 >= 1);
    CHECK(l1 != l2);
    CHECK(PX(h.cp, 150, 150) == 0xFF202020u);

    /* Click on the shared window area: routes to the topmost app */
    scene_compositor_input_pointer(h.cp, 0, 170, 198, 1);
    pump_n(&h, 30);
    CHECK_EQ(scene_compositor_focus_is_shell(h.cp), 0);

    scene_launcher_kill(h.sl, p1);
    pump_n(&h, 50);
    CHECK_EQ(h.exited_calls, 1);
    CHECK_EQ(scene_launcher_session_count(h.sl), 1);

    scene_launcher_kill(h.sl, p2);
    pump_n(&h, 50);
    CHECK_EQ(h.exited_calls, 2);
    CHECK_EQ(scene_launcher_session_count(h.sl), 0);
    CHECK(PX(h.cp, 150, 150) == 0xFF101010u);   /* desktop restored */

    remove("launcher_a.log");
    remove("launcher_b.log");
    harness_destroy(&h);
}

static void test_kill_reap(void)
{
    struct harness h;
    harness_init(&h, 800, 600);

    uint32_t pid = 0;
    CHECK(scene_launcher_spawn(h.sl, child_exe_path(), NULL, &pid) == 0);
    pump_until_window(&h, 200);
    CHECK_EQ(h.added_calls, 1);

    /* Kill the child: the connection closes, the pump reaps the
     * session, and the desktop repaints over the dead window. */
    CHECK(scene_launcher_kill(h.sl, pid) == 0);
    pump_n(&h, 50);
    CHECK_EQ(h.exited_calls, 1);
    CHECK_EQ(scene_launcher_session_count(h.sl), 0);
    pump_n(&h, 10);
    CHECK(PX(h.cp, 150, 150) == 0xFF101010u);

    /* Kill of an unknown pid fails */
    CHECK(scene_launcher_kill(h.sl, 0xDEADu) != 0);

    harness_destroy(&h);
}

static void test_spawn_timeout(void)
{
    struct harness h;
    harness_init(&h, 800, 600);
    scene_launcher_spawn_timeout(h.sl, 1);

    /* A spawn whose app exits before connecting: the pending slot must
     * be dropped after the timeout, with no session ever added and no
     * exit notification (the app never joined). */
    uint32_t pid = 0;
    CHECK(scene_launcher_spawn(h.sl, child_exe_path(), "no-connect",
                               &pid) == 0);
    CHECK(pid != 0);
    pump_n(&h, 150);              /* > 1s at 5ms + pump */
    CHECK_EQ(h.added_calls, 0);
    CHECK_EQ(h.exited_calls, 0);
    CHECK_EQ(scene_launcher_session_count(h.sl), 0);

    /* A spawn that fails to launch reports failure, adds nothing */
    CHECK(scene_launcher_spawn(h.sl, "no_such_app_never_exists.exe",
                               NULL, &pid) != 0);
    CHECK_EQ(scene_launcher_session_count(h.sl), 0);

    harness_destroy(&h);
}

static void test_shell_survives_app_death(void)
{
    struct harness h;
    harness_init(&h, 800, 600);

    uint32_t pid = 0;
    CHECK(scene_launcher_spawn(h.sl, child_exe_path(), NULL, &pid) == 0);
    pump_until_window(&h, 200);
    CHECK_EQ(h.added_calls, 1);

    /* App death must not disturb the layer-0 session: the compositor
     * still renders frames. (No shell client exists in this harness, so
     * the layer-0 committed seq stays 0 by design.) */
    scene_launcher_kill(h.sl, pid);
    pump_n(&h, 50);
    CHECK_EQ(h.exited_calls, 1);
    CHECK(scene_compositor_frame(h.cp) == 0);
    scene_launcher_spawn(h.sl, child_exe_path(), NULL, &pid);
    pump_until_window(&h, 200);
    CHECK_EQ(h.added_calls, 2);    /* a new app session still joins */
    scene_launcher_kill(h.sl, pid);
    pump_n(&h, 50);

    harness_destroy(&h);
}

int main(int argc, char **argv)
{
    (void)argc;
    g_argv0 = argv[0];

    test_spawn_attach();
    test_input_routing();
    test_multi_spawn();
    test_kill_reap();
    test_spawn_timeout();
    test_shell_survives_app_death();

    printf("test_launcher: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
