/* test_shellwm.c — the shell host WM across all layers + network tray.
 *
 * The shell owns layer 0 (its own session); app sessions live on layers
 * 1..n (scene_compositor_add_session). This suite drives a real two-
 * session desktop over two loopbacks and asserts the host WM contract:
 *   - a WINDOW on an app layer gets a task button (id ID_APP_TASK_BASE)
 *   - clicking that button host-focuses the window on ITS layer's store
 *     (scene_store_host_focus, direct call — zero wire bytes into the
 *     app session; the app client's input_focus callback must stay 0)
 *   - clicking again minimizes (host_set_visible off); a third click
 *     restores and focuses
 *   - layer-0 windows keep the old behavior (task button + client focus)
 *   - the active-focus style follows the focused layer's button
 *   - the network tray label follows the probe stub
 *   - identical configs produce identical trees (determinism)
 *
 * Harness: shell client + app client, each on its own loopback, one
 * shared compositor; tickf pumps both links then runs one compositor
 * frame (pattern of test_sessions/test_launcher).
 */
#include "scene_shell.h"
#include "scene_compositor.h"
#include "scene_transport.h"
#include "scene_client.h"
#include "scene_server.h"
#include "scene_fmt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ---- loopback harness: shell (layer 0) + app session (layer 1) -------- */

struct harness {
    scene_compositor  *cp;
    scene_loopback    *lb0, *lb1;
    scene_transport   *server_ts0, *server_ts1;
    scene_client      *cl;            /* shell client (layer 0)            */
    scene_server      *sv1;           /* app session (owned by compositor) */
    scene_client      *ac;            /* app client (layer 1)              */
    scene_shell       *sh;
    scene_style_ref    active_style;

    /* app client inbound wire callbacks (must all stay 0: the host WM
     * speaks to app stores directly, never over the wire) */
    int ap_focus_calls, ap_ptr_calls, ap_act_calls, ap_key_calls;
};

static void cb_welcome(void *ud, uint32_t sid, uint16_t ver,
                       const scene_limits *lim)
{
    (void)ud; (void)sid; (void)ver; (void)lim;
}

static void cb_ap_focus(void *ud, uint64_t seq, scene_node_id id, uint8_t st)
{
    struct harness *h = (struct harness *)ud;
    (void)seq; (void)id; (void)st;
    h->ap_focus_calls++;
}

static void cb_ap_pointer(void *ud, uint64_t seq, uint8_t device,
                          int32_t x, int32_t y, uint8_t buttons)
{
    struct harness *h = (struct harness *)ud;
    (void)seq; (void)device; (void)x; (void)y; (void)buttons;
    h->ap_ptr_calls++;
}

static void cb_ap_activate(void *ud, uint64_t seq, scene_node_id id)
{
    struct harness *h = (struct harness *)ud;
    (void)seq; (void)id;
    h->ap_act_calls++;
}

static void cb_ap_key(void *ud, uint64_t seq, uint32_t code, uint8_t state,
                      uint8_t mods)
{
    struct harness *h = (struct harness *)ud;
    (void)seq; (void)code; (void)state; (void)mods;
    h->ap_key_calls++;
}

static const scene_client_cbs g_app_cbs = {
    cb_welcome, NULL, NULL, NULL, NULL, NULL,
    cb_ap_pointer, cb_ap_activate, cb_ap_focus, cb_ap_key, NULL, NULL, NULL, NULL
};

/* Pump both sessions' links (4 rounds), then one compositor frame.
 * Does NOT call scene_shell_tick — the tests drive that explicitly so
 * the clock/tray/task reconciliation order stays visible. */
static void tickf(struct harness *h)
{
    int round;
    for (round = 0; round < 4; round++) {
        scene_client_flush(h->cl);
        scene_client_flush(h->ac);
        uint8_t buf[8192];
        uint32_t got;
        while (scene_transport_recv(h->server_ts0, buf, sizeof(buf), &got)
               == 0 && got)
            if (scene_server_feed(scene_compositor_server(h->cp), buf, got)
                != 0) break;
        const uint8_t *f;
        uint32_t flen;
        while (scene_server_out_next_frame(scene_compositor_server(h->cp),
                                           &f, &flen) == 1)
            scene_transport_send(h->server_ts0, f, flen);
        scene_client_pump(h->cl);

        while (scene_transport_recv(h->server_ts1, buf, sizeof(buf), &got)
               == 0 && got)
            if (scene_server_feed(h->sv1, buf, got) != 0) break;
        while (scene_server_out_next_frame(h->sv1, &f, &flen) == 1)
            scene_transport_send(h->server_ts1, f, flen);
        scene_client_pump(h->ac);
    }
    scene_compositor_frame(h->cp);
}

/* scene_shell_tick then pump both links + one frame. */
static void sh_tick(struct harness *h)
{
    scene_shell_tick(h->sh);
    tickf(h);
}

/* Deliver any queued client ops, then run one shell reconciliation:
 * the app window must already be IN the app store when the tick walks
 * the layers, so ops queued in the client outbound are pumped first. */
static void settle(struct harness *h)
{
    tickf(h);              /* deliver queued ops to the stores */
    scene_shell_tick(h->sh);
    tickf(h);              /* deliver the tick's emits */
}

static void harness_init(struct harness *h)
{
    memset(h, 0, sizeof(*h));
    h->cp = scene_compositor_new(NULL, 800, 600);
    CHECK(h->cp != NULL);

    /* shell session (layer 0) */
    h->lb0 = scene_loopback_new();
    h->server_ts0 = scene_loopback_server_end(h->lb0);
    h->cl = scene_client_new();
    scene_server_attach(scene_compositor_server(h->cp));
    CHECK_EQ(scene_client_connect(h->cl, scene_loopback_client_end(h->lb0),
                                  "shell", NULL, NULL), 0);

    /* app session (layer 1): raw scene_client (no launcher needed) */
    h->lb1 = scene_loopback_new();
    h->server_ts1 = scene_loopback_server_end(h->lb1);
    h->sv1 = scene_server_new(NULL);
    CHECK(h->sv1 != NULL);
    CHECK_EQ(scene_compositor_add_session(h->cp, h->sv1), 1);
    scene_server_attach(h->sv1);
    h->ac = scene_client_new();
    CHECK_EQ(scene_client_connect(h->ac, scene_loopback_client_end(h->lb1),
                                  "app", &g_app_cbs, h), 0);

    /* handshake: pump both links so both WELCOMEs dispatch and the emit
     * guards open before any node is created */
    tickf(h);
}

static void harness_free(struct harness *h)
{
    scene_client_free(h->ac);
    scene_transport_close(h->server_ts1);
    scene_loopback_free(h->lb1);
    scene_client_free(h->cl);
    scene_transport_close(h->server_ts0);
    scene_loopback_free(h->lb0);
    scene_compositor_free(h->cp);   /* frees sv1 (take-ownership) */
}

/* Build the shell tree and attach a fresh shell to the harness. */
static void harness_shell(struct harness *h)
{
    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    h->sh = scene_shell_new(h->cl, scene_compositor_store(h->cp),
                            h->cp, &cfg);
    CHECK(h->sh != NULL);
    CHECK_EQ(scene_shell_build(h->sh, 800, 600), 0);
    scene_style_ref aref = scene_compositor_setup_active_style(h->cp,
        0xFF4A4A6E, 0xFFFFFFFF);
    CHECK_EQ(aref, 2u);
    scene_shell_set_active_style(h->sh, aref);
    h->active_style = aref;
    tickf(h);
}

/* App session: one WINDOW "AppWin" with a child button. */
static void app_build_window(struct harness *h)
{
    uint8_t vis = SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE;
    CHECK_EQ(scene_client_create_node(h->ac, SCENE_NO_PARENT, 41000,
                                      SCENE_ROLE_WINDOW,
                                      &(scene_rect){100, 50, 240, 160},
                                      vis), 0);
    CHECK_EQ(scene_client_set_text(h->ac, 41000, 1, "AppWin", 6), 0);
    CHECK_EQ(scene_client_create_node(h->ac, 41000, 41001,
                                      SCENE_ROLE_BUTTON,
                                      &(scene_rect){120, 182, 80, 24},
                                      vis), 0);
}

/* ---- tests ------------------------------------------------------------ */

/* The app window on layer 1 gains an app task button (ID_APP_TASK_BASE +
 * slot) with the window's title; a layer-0 window still gains the classic
 * task button (ID_TASK_BASE + seq). Both live in the SHELL store. */
static void test_wm_task_buttons(void)
{
    struct harness h;
    harness_init(&h);
    harness_shell(&h);
    app_build_window(&h);
    settle(&h);

    scene_store *ss = scene_compositor_store(h.cp);
    scene_node_vis v;

    /* app task button exists, placed on the panel with the title text */
    CHECK_EQ(scene_store_node_vis(ss, 40000, &v), 0);
    CHECK_EQ(v.role, SCENE_ROLE_BUTTON);
    CHECK(v.flags & SCENE_FLAG_VISIBLE);
    CHECK_EQ(v.parent, 10001u);           /* panel */
    CHECK_EQ(v.rect[1], 570);             /* panel_y + 2, ph=32 */
    scene_node_text_vis tv[8];
    int n = scene_store_node_texts(ss, 40000, tv, 8);
    CHECK(n > 0);
    if (n > 0) {
        CHECK_EQ(tv[0].len, 6u);
        CHECK(memcmp(tv[0].data, "AppWin", 6) == 0);
    }

    /* layer-0 window keeps the old path: ID_TASK_BASE button */
    CHECK_EQ(scene_client_create_node(h.cl, SCENE_NO_PARENT, 1,
                                      SCENE_ROLE_WINDOW,
                                      &(scene_rect){200, 100, 120, 80},
                                      SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE),
             0);
    CHECK_EQ(scene_client_set_text(h.cl, 1, 1, "L0Win", 5), 0);
    settle(&h);
    CHECK_EQ(scene_store_node_vis(ss, 30000, &v), 0);
    CHECK_EQ(v.role, SCENE_ROLE_BUTTON);
    CHECK_EQ(v.parent, 10001u);
    n = scene_store_node_texts(ss, 30000, tv, 8);
    CHECK(n > 0);
    if (n > 0) {
        CHECK_EQ(tv[0].len, 5u);
        CHECK(memcmp(tv[0].data, "L0Win", 5) == 0);
    }
    /* app button survives; the two buttons do not collide */
    CHECK_EQ(scene_store_node_vis(ss, 40000, &v), 0);

    /* no wire bytes reached the app client at any point */
    CHECK_EQ(h.ap_focus_calls, 0);
    CHECK_EQ(h.ap_ptr_calls, 0);
    CHECK_EQ(h.ap_act_calls, 0);

    scene_shell_free(h.sh);
    harness_free(&h);
    printf("test_wm_task_buttons: ok\n");
}

/* Click cycle on the app task button: focus → minimize → restore+focus.
 * The focus lands on the APP layer's store (direct host call), and the
 * app client sees zero inbound records. */
static void test_wm_app_click_cycle(void)
{
    struct harness h;
    harness_init(&h);
    harness_shell(&h);
    app_build_window(&h);
    settle(&h);

    scene_store *appst = scene_compositor_layer_store(h.cp, 1);
    CHECK(appst != NULL);
    scene_store *ss = scene_compositor_store(h.cp);
    scene_node_vis v;

    CHECK_EQ(scene_store_focus(appst), SCENE_NO_PARENT);

    /* 1st click: focus the app window on its own layer's store */
    CHECK_EQ(scene_shell_handle_activate(h.sh, 40000), 1);
    CHECK_EQ(scene_store_focus(appst), 41000u);
    sh_tick(&h);
    /* focus was painted into the shell store? no: focus is engine state
     * on the app store; the shell store's focus is untouched */
    CHECK_EQ(scene_store_focus(ss), SCENE_NO_PARENT);

    /* 2nd click: minimize — window invisible, button still present */
    CHECK_EQ(scene_shell_handle_activate(h.sh, 40000), 1);
    CHECK_EQ(scene_store_node_vis(appst, 41000, &v), 0);
    CHECK(!(v.flags & SCENE_FLAG_VISIBLE));
    sh_tick(&h);
    CHECK_EQ(scene_store_node_vis(ss, 40000, &v), 0);
    CHECK(v.flags & SCENE_FLAG_VISIBLE);   /* task button stays */

    /* 3rd click: restore + focus */
    CHECK_EQ(scene_shell_handle_activate(h.sh, 40000), 1);
    CHECK_EQ(scene_store_node_vis(appst, 41000, &v), 0);
    CHECK(v.flags & SCENE_FLAG_VISIBLE);
    CHECK_EQ(scene_store_focus(appst), 41000u);

    /* zero wire bytes into the app session, ever */
    CHECK_EQ(h.ap_focus_calls, 0);
    CHECK_EQ(h.ap_ptr_calls, 0);
    CHECK_EQ(h.ap_act_calls, 0);
    CHECK_EQ(h.ap_key_calls, 0);

    scene_shell_free(h.sh);
    harness_free(&h);
    printf("test_wm_app_click_cycle: ok\n");
}

/* The active-task style follows the focused layer: app window focused →
 * its button gets the active style; a focused layer-0 window reverts the
 * app button to the shell button theme. */
static void test_wm_active_highlight(void)
{
    struct harness h;
    harness_init(&h);
    harness_shell(&h);
    app_build_window(&h);
    settle(&h);

    scene_store *ss = scene_compositor_store(h.cp);
    scene_node_vis v;

    CHECK_EQ(scene_store_node_vis(ss, 40000, &v), 0);
    CHECK_EQ(v.style, 5u);   /* SHELL_STYLE_BUTTON initially */

    /* focus the app window through the task button → active style */
    CHECK_EQ(scene_shell_handle_activate(h.sh, 40000), 1);
    settle(&h);
    CHECK_EQ(scene_store_node_vis(ss, 40000, &v), 0);
    CHECK_EQ(v.style, h.active_style);

    /* layer-0 window: focus it via client → its button is highlighted,
     * the app button reverts to the button theme */
    CHECK_EQ(scene_client_create_node(h.cl, SCENE_NO_PARENT, 1,
                                      SCENE_ROLE_WINDOW,
                                      &(scene_rect){200, 100, 120, 80},
                                      SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE),
             0);
    settle(&h);
    CHECK_EQ(scene_client_focus(h.cl, 1), 0);
    settle(&h);
    CHECK_EQ(scene_store_node_vis(ss, 30000, &v), 0);
    CHECK_EQ(v.style, h.active_style);
    CHECK_EQ(scene_store_node_vis(ss, 40000, &v), 0);
    CHECK_EQ(v.style, 5u);

    scene_shell_free(h.sh);
    harness_free(&h);
    printf("test_wm_active_highlight: ok\n");
}

/* ---- network tray probe stub ------------------------------------------ */

static const char *g_stub_tray;

static const char *stub_tray_probe(void)
{
    return g_stub_tray ? g_stub_tray : "NA";
}

/* The tray label (ID 10005, left of the clock) shows the probe output
 * on the first tick; a different probe text lands in a fresh shell. */
static void test_wm_tray_probe(void)
{
    const char *(*saved)(void) = scene_shell_tray_probe;
    scene_shell_tray_probe = stub_tray_probe;

    g_stub_tray = "net";
    {
        struct harness h;
        harness_init(&h);
        harness_shell(&h);
        sh_tick(&h);

        scene_store *ss = scene_compositor_store(h.cp);
        scene_node_vis v;
        CHECK_EQ(scene_store_node_vis(ss, 10005, &v), 0);
        CHECK_EQ(v.role, SCENE_ROLE_LABEL);
        CHECK_EQ(v.rect[0], 644);   /* width - 100 - 56 */
        scene_node_text_vis tv[8];
        int n = scene_store_node_texts(ss, 10005, tv, 8);
        CHECK(n > 0);
        if (n > 0) {
            CHECK_EQ(tv[0].len, 3u);
            CHECK(memcmp(tv[0].data, "net", 3) == 0);
        }
        scene_shell_free(h.sh);
        harness_free(&h);
    }

    g_stub_tray = "no net";
    {
        struct harness h;
        harness_init(&h);
        harness_shell(&h);
        sh_tick(&h);
        scene_store *ss = scene_compositor_store(h.cp);
        scene_node_text_vis tv[8];
        int n = scene_store_node_texts(ss, 10005, tv, 8);
        CHECK(n > 0);
        if (n > 0) {
            CHECK_EQ(tv[0].len, 6u);
            CHECK(memcmp(tv[0].data, "no net", 6) == 0);
        }
        scene_shell_free(h.sh);
        harness_free(&h);
    }

    scene_shell_tray_probe = saved;
    printf("test_wm_tray_probe: ok\n");
}

/* Layer-0 window behavior is unchanged: task button + client focus path,
 * and clicking an app task button never touches the shell store's focus. */
static void test_wm_layer0_unchanged(void)
{
    struct harness h;
    harness_init(&h);
    harness_shell(&h);
    app_build_window(&h);
    settle(&h);

    scene_store *ss = scene_compositor_store(h.cp);

    /* a layer-0 WINDOW gains the classic task button */
    CHECK_EQ(scene_client_create_node(h.cl, SCENE_NO_PARENT, 1,
                                      SCENE_ROLE_WINDOW,
                                      &(scene_rect){200, 100, 120, 80},
                                      SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE),
             0);
    settle(&h);
    scene_node_vis v;
    CHECK_EQ(scene_store_node_vis(ss, 30000, &v), 0);

    /* clicking the layer-0 task button focuses via the client */
    CHECK_EQ(scene_shell_handle_activate(h.sh, 30000), 1);
    settle(&h);
    CHECK_EQ(scene_store_focus(ss), 1u);

    /* clicking the APP task button focuses the app store, not the shell */
    CHECK_EQ(scene_shell_handle_activate(h.sh, 40000), 1);
    settle(&h);
    CHECK_EQ(scene_store_focus(ss), 1u);   /* shell focus untouched */
    CHECK_EQ(scene_store_focus(scene_compositor_layer_store(h.cp, 1)),
             41000u);

    scene_shell_free(h.sh);
    harness_free(&h);
    printf("test_wm_layer0_unchanged: ok\n");
}

/* Identical configs + identical app scenes + identical probe text →
 * identical shell trees (node count, geometry, task buttons, tray). */
static void test_wm_determinism(void)
{
    const char *(*saved)(void) = scene_shell_tray_probe;
    scene_shell_tray_probe = stub_tray_probe;
    g_stub_tray = "net";

    struct harness ha, hb;
    harness_init(&ha);
    harness_init(&hb);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    cfg.panel_height = 40;
    ha.sh = scene_shell_new(ha.cl, scene_compositor_store(ha.cp),
                            ha.cp, &cfg);
    hb.sh = scene_shell_new(hb.cl, scene_compositor_store(hb.cp),
                            hb.cp, &cfg);
    CHECK_EQ(scene_shell_build(ha.sh, 800, 600), 0);
    CHECK_EQ(scene_shell_build(hb.sh, 800, 600), 0);
    app_build_window(&ha);
    app_build_window(&hb);
    settle(&ha);
    settle(&hb);

    scene_store *sa = scene_compositor_store(ha.cp);
    scene_store *sb = scene_compositor_store(hb.cp);
    CHECK_EQ(scene_store_node_count(sa), scene_store_node_count(sb));

    scene_node_vis nva, nvb;
    static const uint32_t probe_ids[] = { 10000, 10001, 10002, 10003,
                                          10004, 10005, 30000, 40000 };
    int r;
    for (r = 0; r < (int)(sizeof(probe_ids) / sizeof(probe_ids[0])); r++) {
        uint32_t id = probe_ids[r];
        int ra = scene_store_node_vis(sa, id, &nva);
        int rb = scene_store_node_vis(sb, id, &nvb);
        CHECK_EQ(ra, rb);
        if (ra == 0 && rb == 0) {
            CHECK_EQ(nva.role, nvb.role);
            CHECK_EQ(nva.rect[0], nvb.rect[0]);
            CHECK_EQ(nva.rect[1], nvb.rect[1]);
            CHECK_EQ(nva.rect[2], nvb.rect[2]);
            CHECK_EQ(nva.rect[3], nvb.rect[3]);
            CHECK_EQ(nva.flags, nvb.flags);
        }
    }

    /* tray text identical (stub probe), clock emitted once (len 5) */
    scene_node_text_vis ta[8], tb[8];
    int na = scene_store_node_texts(sa, 10005, ta, 8);
    int nb = scene_store_node_texts(sb, 10005, tb, 8);
    CHECK(na > 0 && nb > 0);
    if (na > 0 && nb > 0) {
        CHECK_EQ(ta[0].len, tb[0].len);
        CHECK(memcmp(ta[0].data, tb[0].data, ta[0].len) == 0);
    }
    na = scene_store_node_texts(sa, 10003, ta, 8);
    nb = scene_store_node_texts(sb, 10003, tb, 8);
    CHECK(na > 0 && nb > 0);
    if (na > 0 && nb > 0)
        CHECK_EQ(ta[0].len, tb[0].len);

    scene_shell_free(ha.sh);
    scene_shell_free(hb.sh);
    harness_free(&ha);
    harness_free(&hb);

    scene_shell_tray_probe = saved;
    printf("test_wm_determinism: ok\n");
}

int main(void)
{
    test_wm_task_buttons();
    test_wm_app_click_cycle();
    test_wm_active_highlight();
    test_wm_tray_probe();
    test_wm_layer0_unchanged();
    test_wm_determinism();
    printf("test_shellwm: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}