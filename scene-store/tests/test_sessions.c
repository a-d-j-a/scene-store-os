/*
 * test_sessions.c — multi-session composition: the app-hosting layer.
 *
 * The compositor merges the shell session (layer 0) with app sessions
 * (scene_compositor_add_session). These tests drive a real two-session
 * desktop over two loopbacks: the shell is a raw scene_client, the app
 * is a scene_app client building its own window with per-session node
 * ids. Asserts layer order in the framebuffer, input routing by
 * hit-test, keyboard focus, per-session flow control, app-session
 * death (desktop survives), remove/re-add, and full determinism.
 */
#include "scene_compositor.h"
#include "scene_client.h"
#include "scene_transport.h"
#include "scene_app.h"

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

#define PX(cp, x, y) scene_fb_get(scene_compositor_fb(cp), (x), (y))

/* ---- harness: shell client + app client on separate sessions ---------- */

struct harness {
    scene_compositor *cp;
    scene_server     *sv1;          /* app session (owned by compositor)  */
    scene_loopback   *lb0, *lb1;
    scene_transport  *server_ts0, *server_ts1;
    scene_client     *cl;           /* shell client                       */
    scene_app        *app;          /* app client (scene_app path)        */
    int              app_active;    /* tickf pumps the app link only when  */
                                    /* a session is attached (removed or   */
                                    /* killed sessions must not be fed)    */

    /* shell captured events */
    int sh_ptr_calls; uint32_t sh_ptr_x, sh_ptr_y, sh_ptr_btn;
    int sh_act_calls; uint64_t sh_act_seq; uint32_t sh_act_id;
    int sh_key_calls; uint32_t sh_key_code;

    /* app captured events */
    int ap_ptr_calls; uint64_t ap_ptr_seq[8];
    int ap_act_calls; uint64_t ap_act_seq[8]; uint32_t ap_act_id[8];
    int ap_key_calls; uint32_t ap_key_code; uint32_t ap_key_mod;
};

static void cb_welcome(void *ud, uint32_t sid, uint16_t ver,
                       const scene_limits *lim)
{
    (void)ud; (void)sid; (void)ver; (void)lim;
}

static void cb_sh_pointer(void *ud, uint64_t seq, uint8_t device,
                          int32_t x, int32_t y, uint8_t buttons)
{
    struct harness *h = (struct harness *)ud;
    (void)seq; (void)device;
    h->sh_ptr_calls++;
    h->sh_ptr_x = (uint32_t)x;
    h->sh_ptr_y = (uint32_t)y;
    h->sh_ptr_btn = buttons;
}

static void cb_sh_activate(void *ud, uint64_t seq, scene_node_id id)
{
    struct harness *h = (struct harness *)ud;
    h->sh_act_calls++;
    h->sh_act_seq = seq;
    h->sh_act_id = (uint32_t)id;
}

static void cb_sh_key(void *ud, uint64_t seq, uint32_t code, uint8_t state,
                      uint8_t mods)
{
    struct harness *h = (struct harness *)ud;
    (void)seq; (void)state; (void)mods;
    h->sh_key_calls++;
    h->sh_key_code = code;
}

static const scene_client_cbs g_sh_cbs = {
    cb_welcome, NULL, NULL, NULL, NULL, NULL,
    cb_sh_pointer, cb_sh_activate, NULL, cb_sh_key, NULL, NULL, NULL, NULL,
    NULL
};

static void cb_ap_pointer(void *ud, uint64_t seq, int32_t x, int32_t y,
                          uint8_t buttons)
{
    struct harness *h = (struct harness *)ud;
    (void)x; (void)y; (void)buttons;
    if (h->ap_ptr_calls < 8) h->ap_ptr_seq[h->ap_ptr_calls] = seq;
    h->ap_ptr_calls++;
}

static void cb_ap_activate(void *ud, uint64_t seq, scene_node_id id)
{
    struct harness *h = (struct harness *)ud;
    if (h->ap_act_calls < 8) {
        h->ap_act_seq[h->ap_act_calls] = seq;
        h->ap_act_id[h->ap_act_calls] = (uint32_t)id;
    }
    h->ap_act_calls++;
}

static void cb_ap_key(void *ud, uint64_t seq, uint32_t code, uint8_t state,
                      uint8_t mods)
{
    struct harness *h = (struct harness *)ud;
    (void)seq; (void)state;
    h->ap_key_calls++;
    h->ap_key_code = code;
    h->ap_key_mod = mods;
}

static const scene_app_cbs g_ap_cbs = {
    cb_ap_pointer, cb_ap_activate, cb_ap_key, NULL, NULL, NULL
};

static void tickf(struct harness *h);

static void harness_init(struct harness *h)
{
    memset(h, 0, sizeof(*h));
    h->cp = scene_compositor_new(NULL, 800, 600);
    CHECK(h->cp != NULL);

    /* shell session (layer 0): client on loopback 0 */
    h->lb0 = scene_loopback_new();
    h->server_ts0 = scene_loopback_server_end(h->lb0);
    h->cl = scene_client_new();
    scene_server_attach(scene_compositor_server(h->cp));
    CHECK_EQ(scene_client_connect(h->cl, scene_loopback_client_end(h->lb0),
                                  "shell", &g_sh_cbs, h), 0);

    /* app session (layer 1): scene_app client on loopback 1 */
    h->lb1 = scene_loopback_new();
    h->server_ts1 = scene_loopback_server_end(h->lb1);
    h->sv1 = scene_server_new(NULL);
    CHECK(h->sv1 != NULL);
    CHECK_EQ(scene_compositor_add_session(h->cp, h->sv1), 1);
    scene_server_attach(h->sv1);
    h->app = scene_app_new(scene_loopback_client_end(h->lb1),
                           &g_ap_cbs, h);
    CHECK(h->app != NULL);
    h->app_active = 1;
    tickf(h);   /* handshake: both WELCOMEs delivered, emit guards open */
}

static void harness_free(struct harness *h)
{
    if (h->app) scene_app_free(h->app); /* closes app_ts via its client  */
    if (h->server_ts1) scene_transport_close(h->server_ts1);
    if (h->lb1) scene_loopback_free(h->lb1);
    scene_client_free(h->cl); /* closes the shell client-end transport */
    scene_transport_close(h->server_ts0);
    scene_loopback_free(h->lb0);
    scene_compositor_free(h->cp);   /* frees sv1 (take-ownership) */
}

/* Drive both sessions' links, then exactly one compositor frame. */
static void tickf(struct harness *h)
{
    int round;
    for (round = 0; round < 4; round++) {
        scene_client_flush(h->cl);
        scene_app_flush(h->app);
        uint8_t buf[8192];
        uint32_t got;
        scene_server *sv0 = scene_compositor_server(h->cp);
        while (scene_transport_recv(h->server_ts0, buf, sizeof(buf), &got)
               == 0 && got)
            if (scene_server_feed(sv0, buf, got) != 0) break;
        const uint8_t *f;
        uint32_t flen;
        while (scene_server_out_next_frame(sv0, &f, &flen) == 1)
            scene_transport_send(h->server_ts0, f, flen);
        scene_client_pump(h->cl);
        if (h->app_active) {
            while (scene_transport_recv(h->server_ts1, buf, sizeof(buf),
                                        &got) == 0 && got)
                if (scene_server_feed(h->sv1, buf, got) != 0) break;
            while (scene_server_out_next_frame(h->sv1, &f, &flen) == 1)
                scene_transport_send(h->server_ts1, f, flen);
            scene_app_pump(h->app);
        }
    }
    scene_compositor_frame(h->cp);
}

/* ---- scene builders ----------------------------------------------------- */

/* Shell session: a full-desktop background WINDOW node (role default
 * fill 0xFF202020 over the compositor clear 0xFF101010).              */
static void build_shell(struct harness *h)
{
    scene_client_create_node(h->cl, SCENE_NO_PARENT, 1, SCENE_ROLE_WINDOW,
        &(scene_rect){0, 0, 800, 600}, SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
}

/* App session: a DIALOG window (role default fill 0xFF262626, border
 * 0xFF444444) with a TITLEBAR (0xFF1A1A1A), a label, a close button
 * (0xFF3C3C3C) and a transparent CONTENT area. Per-session ids 41000+
 * (scene_app's allocator owns 40000..40999).                          */
static void build_app_window(struct harness *h)
{
    scene_client *ac = scene_app_client(h->app);
    uint8_t vis = SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE;

    scene_client_create_node(ac, SCENE_NO_PARENT, 41000, SCENE_ROLE_DIALOG,
        &(scene_rect){100, 50, 300, 200}, vis);
    scene_client_create_node(ac, 41000, 41001, SCENE_ROLE_TITLEBAR,
        &(scene_rect){100, 50, 300, 24}, vis);
    scene_client_create_node(ac, 41001, 41002, SCENE_ROLE_LABEL,
        &(scene_rect){104, 54, 260, 16}, vis);
    scene_client_set_text(ac, 41002, 0, "App", 3);
    scene_client_create_node(ac, 41001, 41003, SCENE_ROLE_BUTTON,
        &(scene_rect){372, 54, 24, 24}, vis);
    scene_client_create_node(ac, 41000, 41004, SCENE_ROLE_GENERIC,
        &(scene_rect){100, 74, 300, 176}, vis);
}

/* ---- tests -------------------------------------------------------------- */

/* App windows render above the desktop; layers share one framebuffer.  */
static void test_layer_order(void)
{
    struct harness h;

    harness_init(&h);
    build_shell(&h);
    build_app_window(&h);
    tickf(&h);

    /* inside the app dialog (over the desktop) */
    CHECK_EQ(PX(h.cp, 250, 150), 0xFF262626u);
    /* titlebar strip */
    CHECK_EQ(PX(h.cp, 150, 60), 0xFF1A1A1Au);
    /* close button interior */
    CHECK_EQ(PX(h.cp, 384, 66), 0xFF3C3C3Cu);
    /* desktop, outside the app window */
    CHECK_EQ(PX(h.cp, 700, 500), 0xFF202020u);
    /* desktop, where the app window's enter would have to cover */
    CHECK_EQ(PX(h.cp, 120, 60), 0xFF1A1A1Au);
    CHECK(scene_compositor_rendered_seq(h.cp) > 0);
    harness_free(&h);
}

/* Pointer input is routed by hit-test: app window → app session,
 * desktop → shell session; app ids stay in the app's namespace.        */
static void test_input_routing(void)
{
    struct harness h;

    harness_init(&h);
    build_shell(&h);
    build_app_window(&h);
    tickf(&h);

    /* press inside the app window (content area) → app session only */
    scene_compositor_input_pointer(h.cp, 0, 250, 150, 1);
    tickf(&h);
    CHECK_EQ(h.ap_ptr_calls, 1);
    CHECK_EQ(h.ap_act_calls, 1);
    CHECK_EQ(h.ap_act_id[0], 41004u);       /* deepest: CONTENT */
    CHECK_EQ(h.sh_ptr_calls, 0);
    CHECK_EQ(h.sh_act_calls, 0);
    scene_app_ack(h.app, h.ap_ptr_seq[0]);

    /* press on the desktop → shell session only */
    scene_compositor_input_pointer(h.cp, 0, 700, 500, 1);
    tickf(&h);
    CHECK_EQ(h.sh_ptr_calls, 1);
    CHECK_EQ(h.sh_act_calls, 1);
    CHECK_EQ(h.sh_act_id, 1u);              /* background */
    CHECK_EQ(h.ap_ptr_calls, 1);
    scene_client_ack(h.cl, h.sh_act_seq);

    /* motion over the app window → pointer only, no activate */
    scene_compositor_input_pointer(h.cp, 0, 250, 150, 0);
    tickf(&h);
    CHECK_EQ(h.ap_ptr_calls, 2);
    CHECK_EQ(h.ap_act_calls, 1);
    scene_app_ack(h.app, h.ap_ptr_seq[1]);

    /* per-session seq streams stay independent */
    CHECK_EQ(scene_client_next_seq(h.cl), 2u);
    CHECK_EQ(scene_client_next_seq(scene_app_client(h.app)), 7u);
    harness_free(&h);
}

/* Keyboard focus follows the last session that received a pointer
 * event (click-to-focus); the shell is the default.                   */
static void test_focus_keys(void)
{
    struct harness h;

    harness_init(&h);
    build_shell(&h);
    build_app_window(&h);
    tickf(&h);

    /* focus the app window, then type (ack must land first: the gate
     * opens only after the ack is ingested by the server) */
    scene_compositor_input_pointer(h.cp, 0, 250, 150, 1);
    tickf(&h);
    scene_app_ack(h.app, h.ap_ptr_seq[0]);
    tickf(&h);
    scene_compositor_input_key(h.cp, 30, 1, 0);
    tickf(&h);
    CHECK_EQ(h.ap_key_calls, 1);
    CHECK_EQ(h.ap_key_code, 30u);
    CHECK_EQ(h.sh_key_calls, 0);

    /* focus the desktop, then type → shell */
    scene_compositor_input_pointer(h.cp, 0, 700, 500, 1);
    tickf(&h);
    scene_client_ack(h.cl, h.sh_act_seq);
    tickf(&h);
    scene_compositor_input_key(h.cp, 30, 1, 0);
    tickf(&h);
    CHECK_EQ(h.sh_key_calls, 1);
    CHECK_EQ(h.ap_key_calls, 1);
    harness_free(&h);
}

/* Flow control is per-session: an unacked app pointer does not block
 * the shell's deliveries, and the app's next press is dropped until
 * the app acks.                                                        */
static void test_flow_independent(void)
{
    struct harness h;

    harness_init(&h);
    build_shell(&h);
    build_app_window(&h);
    tickf(&h);

    scene_compositor_input_pointer(h.cp, 0, 250, 150, 1);
    tickf(&h);
    CHECK_EQ(h.ap_ptr_calls, 1);
    /* no ack: the app gate is shut */

    scene_compositor_input_pointer(h.cp, 0, 260, 150, 1);
    tickf(&h);
    CHECK_EQ(h.ap_ptr_calls, 1);            /* dropped by the gate */

    scene_compositor_input_pointer(h.cp, 0, 700, 500, 1);
    tickf(&h);
    CHECK_EQ(h.sh_ptr_calls, 1);            /* shell gate independent */
    scene_client_ack(h.cl, h.sh_act_seq);
    tickf(&h);
    scene_app_ack(h.app, h.ap_ptr_seq[0]);
    tickf(&h);                              /* both gates open */
    scene_compositor_input_pointer(h.cp, 0, 250, 150, 1);
    tickf(&h);
    CHECK_EQ(h.ap_ptr_calls, 2);            /* gate reopened */
    scene_app_ack(h.app, h.ap_ptr_seq[1]);
    harness_free(&h);
}

/* An app session's death freezes nothing: the desktop keeps rendering
 * and input keeps flowing; the shell session's death is fatal.         */
static void test_dead_app(void)
{
    struct harness h;
    uint8_t garbage[16];

    memset(garbage, 0xFF, sizeof(garbage));
    harness_init(&h);
    build_shell(&h);
    build_app_window(&h);
    tickf(&h);
    CHECK_EQ(PX(h.cp, 250, 150), 0xFF262626u);

    /* kill the app session with a fatal frame-level violation */
    CHECK(scene_server_feed(h.sv1, garbage, sizeof(garbage)) != 0);
    tickf(&h);
    CHECK_EQ(scene_compositor_frame(h.cp), 0);      /* desktop alive */
    CHECK_EQ(PX(h.cp, 250, 150), 0xFF202020u);      /* window gone */
    CHECK_EQ(h.sh_ptr_calls, 0);

    /* shell input still works, including over the dead window area */
    scene_compositor_input_pointer(h.cp, 0, 700, 500, 1);
    tickf(&h);
    CHECK_EQ(h.sh_ptr_calls, 1);
    CHECK_EQ(h.sh_act_id, 1u);
    scene_client_ack(h.cl, h.sh_act_seq);
    tickf(&h);
    scene_compositor_input_pointer(h.cp, 0, 250, 150, 1);
    tickf(&h);
    CHECK_EQ(h.sh_ptr_calls, 2);
    scene_client_ack(h.cl, h.sh_act_seq);

    /* shell death is fatal to the compositor */
    scene_server_feed(scene_compositor_server(h.cp), garbage,
                      sizeof(garbage));
    CHECK(scene_compositor_frame(h.cp) != 0);
    harness_free(&h);
}

/* remove_session repaints the layer's area as the desktop; a fresh
 * session can take the layer's place.                                  */
static void test_remove_session(void)
{
    struct harness h;

    harness_init(&h);
    build_shell(&h);
    build_app_window(&h);
    tickf(&h);
    CHECK_EQ(PX(h.cp, 250, 150), 0xFF262626u);

    CHECK_EQ(scene_compositor_remove_session(h.cp, h.sv1), 1);
    h.app_active = 0;   /* the session is gone: stop feeding it */
    h.sv1 = NULL;
    tickf(&h);
    CHECK_EQ(PX(h.cp, 250, 150), 0xFF202020u);
    scene_compositor_input_pointer(h.cp, 0, 250, 150, 1);
    tickf(&h);
    CHECK_EQ(h.sh_ptr_calls, 1);            /* input falls to the shell */
    scene_client_ack(h.cl, h.sh_act_seq);
    CHECK_EQ(h.ap_ptr_calls, 0);

    /* a fresh session re-takes layer 1 through the same harness slots */
    h.lb1 = scene_loopback_new();
    h.server_ts1 = scene_loopback_server_end(h.lb1);
    h.sv1 = scene_server_new(NULL);
    CHECK(h.sv1 != NULL);
    CHECK_EQ(scene_compositor_add_session(h.cp, h.sv1), 1);
    scene_server_attach(h.sv1);
    h.app = scene_app_new(scene_loopback_client_end(h.lb1),
                          &g_ap_cbs, &h);
    CHECK(h.app != NULL);
    h.app_active = 1;
    tickf(&h);

    /* same per-session ids, fresh session */
    scene_client *ac = scene_app_client(h.app);
    uint8_t vis = SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE;
    scene_client_create_node(ac, SCENE_NO_PARENT, 41000, SCENE_ROLE_DIALOG,
        &(scene_rect){100, 50, 300, 200}, vis);
    scene_client_create_node(ac, 41000, 41004, SCENE_ROLE_GENERIC,
        &(scene_rect){100, 74, 300, 176}, vis);
    tickf(&h);
    CHECK_EQ(PX(h.cp, 250, 150), 0xFF262626u);

    scene_compositor_input_pointer(h.cp, 0, 250, 150, 1);
    tickf(&h);
    CHECK_EQ(h.ap_ptr_calls, 1);            /* routed to the new app */
    CHECK_EQ(h.ap_act_id[0], 41004u);
    scene_app_ack(h.app, h.ap_ptr_seq[0]);
    harness_free(&h);
}

/* Two identical two-session desktops produce byte-identical
 * framebuffers, mid-enter and settled, with effects on.               */
static void test_determinism(void)
{
    struct harness a, b;
    const scene_fb *fba, *fbb;
    int i;

    harness_init(&a);
    harness_init(&b);
    scene_compositor_set_effects(a.cp, 1);
    scene_compositor_set_effects(b.cp, 1);

    build_shell(&a);
    build_shell(&b);
    build_app_window(&a);
    build_app_window(&b);
    tickf(&a);
    tickf(&b);

    /* mid-enter (frame 1) */
    fba = scene_compositor_fb(a.cp);
    fbb = scene_compositor_fb(b.cp);
    CHECK_EQ(fba->w, fbb->w);
    CHECK_EQ(fba->h, fbb->h);
    CHECK(memcmp(fba->px, fbb->px, (size_t)fba->w * fba->h * 4) == 0);

    /* settled */
    for (i = 0; i < 20; i++) {
        tickf(&a);
        tickf(&b);
    }
    fba = scene_compositor_fb(a.cp);
    fbb = scene_compositor_fb(b.cp);
    CHECK(memcmp(fba->px, fbb->px, (size_t)fba->w * fba->h * 4) == 0);
    CHECK_EQ(scene_compositor_anim_count(a.cp), 0u);
    CHECK_EQ(scene_compositor_anim_count(b.cp), 0u);
    harness_free(&a);
    harness_free(&b);
}

/* Desktop lock: while locked, only the shell session paints and
 * receives input; app windows are hidden and their changes do not
 * repaint. Unlock re-diffs every layer, so changes made while locked
 * appear.                                                              */
static void test_lock_desktop(void)
{
    struct harness h;
    scene_client *ac;

    harness_init(&h);
    build_shell(&h);
    build_app_window(&h);
    tickf(&h);
    CHECK_EQ(PX(h.cp, 250, 150), 0xFF262626u);      /* app window is up */
    CHECK_EQ(scene_compositor_locked(h.cp), 0);

    /* engage the lock, then give the shell a full-screen lock node */
    scene_compositor_set_locked(h.cp, 1);
    CHECK_EQ(scene_compositor_locked(h.cp), 1);
    scene_client_create_node(h.cl, SCENE_NO_PARENT, 2, SCENE_ROLE_WINDOW,
        &(scene_rect){0, 0, 800, 600},
        SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    tickf(&h);

    /* the app window is hidden under the lock screen */
    CHECK_EQ(PX(h.cp, 250, 150), 0xFF202020u);
    CHECK_EQ(PX(h.cp, 150, 60), 0xFF202020u);

    /* clicks inside the app window's area go to the shell only */
    scene_compositor_input_pointer(h.cp, 0, 250, 150, 1);
    tickf(&h);
    CHECK_EQ(h.sh_ptr_calls, 1);
    CHECK_EQ(h.sh_act_id, 2u);                      /* the lock node */
    CHECK_EQ(h.ap_ptr_calls, 0);
    scene_client_ack(h.cl, h.sh_act_seq);
    tickf(&h);                                  /* ack lands: gate opens */

    /* keys go to the shell only, regardless of pre-lock focus */
    scene_compositor_input_key(h.cp, 30, 1, 0);
    tickf(&h);
    CHECK_EQ(h.sh_key_calls, 1);
    CHECK_EQ(h.ap_key_calls, 0);

    /* the app moves its window while locked: nothing repaints */
    ac = scene_app_client(h.app);
    scene_client_set_rect(ac, 41000, &(scene_rect){200, 100, 300, 200});
    tickf(&h);
    CHECK_EQ(PX(h.cp, 250, 150), 0xFF202020u);      /* still the lock */

    /* unlock: the lock node dies and the app layer re-diffs */
    scene_compositor_set_locked(h.cp, 0);
    CHECK_EQ(scene_compositor_locked(h.cp), 0);
    scene_client_destroy_node(h.cl, 2);
    tickf(&h);
    CHECK_EQ(PX(h.cp, 250, 250), 0xFF262626u);      /* moved window */
    scene_compositor_input_pointer(h.cp, 0, 250, 250, 1);
    tickf(&h);
    CHECK_EQ(h.ap_ptr_calls, 1);                    /* app input back */
    CHECK_EQ(h.ap_act_id[0], 41000u);
    scene_app_ack(h.app, h.ap_ptr_seq[0]);
    harness_free(&h);
}

int main(void)
{
    test_layer_order();
    test_input_routing();
    test_focus_keys();
    test_flow_independent();
    test_dead_app();
    test_remove_session();
    test_lock_desktop();
    test_determinism();
    printf("test_sessions: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
