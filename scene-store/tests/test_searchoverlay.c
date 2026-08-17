/* test_searchoverlay.c — cross-app search overlay (Super+S).
 *
 * Super+S is an OS key grab: the compositor routes the chord to the
 * shell session (layer 0) regardless of keyboard focus. The overlay
 * searches committed texts LIVE across every layer (layer ascending,
 * then the engine's document order) and activates a hit through the
 * host WM APIs on that hit's layer store — zero wire bytes into the
 * app session. Enter/click activate, Escape closes, Backspace edits,
 * Up/Down move the selection.
 *
 * Harness (test_shellwm pattern): shell client + app client, each on
 * its own loopback, one shared compositor. The shell client wires the
 * engine INPUT_KEY/INPUT_ACTIVATE records to scene_shell_handle_key /
 * scene_shell_handle_activate and acks every record (the engine's
 * flow-control gate is one un-acked input per store).
 *
 * No scene_shell_tick is ever run: the clock/tray/task reconciliation
 * would add shell-layer texts (the app task button copies the window
 * title "Alpha Beta" into the shell store), which the search would
 * also find. Determinism: only the texts the test itself creates.
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
    scene_style_ref    hover_style;

    /* shell client wire callbacks */
    int sh_key_calls, sh_act_calls, sh_ptr_calls, sh_focus_calls;
    scene_node_id sh_act_id;

    /* app client inbound wire callbacks (must stay 0: the host WM
     * speaks to app stores directly, never over the wire) */
    int ap_focus_calls, ap_ptr_calls, ap_act_calls, ap_key_calls;
};

/* ---- shell client callbacks: engine INPUT_* -> shell + ack ----------- */

static void cb_sh_welcome(void *ud, uint32_t sid, uint16_t ver,
                          const scene_limits *lim)
{
    (void)ud; (void)sid; (void)ver; (void)lim;
}

static void cb_sh_pointer(void *ud, uint64_t seq, uint8_t device,
                          int32_t x, int32_t y, uint8_t buttons)
{
    struct harness *h = (struct harness *)ud;
    (void)device; (void)x; (void)y; (void)buttons;
    h->sh_ptr_calls++;
    scene_client_ack(h->cl, seq);
}

static void cb_sh_activate(void *ud, uint64_t seq, scene_node_id id)
{
    struct harness *h = (struct harness *)ud;
    h->sh_act_calls++;
    h->sh_act_id = id;
    scene_shell_handle_activate(h->sh, id);
    scene_client_ack(h->cl, seq);
}

static void cb_sh_key(void *ud, uint64_t seq, uint32_t key_code,
                      uint8_t state, uint8_t modifiers)
{
    struct harness *h = (struct harness *)ud;
    h->sh_key_calls++;
    scene_shell_handle_key(h->sh, key_code, state, modifiers);
    scene_client_ack(h->cl, seq);
}

static void cb_sh_focus(void *ud, uint64_t seq, scene_node_id id,
                        uint8_t st)
{
    struct harness *h = (struct harness *)ud;
    (void)seq; (void)id; (void)st;
    h->sh_focus_calls++;
}

static const scene_client_cbs g_sh_cbs = {
    cb_sh_welcome, NULL, NULL, NULL, NULL, NULL,
    cb_sh_pointer, cb_sh_activate, cb_sh_focus, cb_sh_key,
    NULL, NULL, NULL, NULL, NULL
};

/* ---- app client callbacks: counters only ------------------------------ */

static void cb_ap_welcome(void *ud, uint32_t sid, uint16_t ver,
                          const scene_limits *lim)
{
    (void)ud; (void)sid; (void)ver; (void)lim;
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

static void cb_ap_focus(void *ud, uint64_t seq, scene_node_id id, uint8_t st)
{
    struct harness *h = (struct harness *)ud;
    (void)seq; (void)id; (void)st;
    h->ap_focus_calls++;
}

static void cb_ap_key(void *ud, uint64_t seq, uint32_t code, uint8_t state,
                      uint8_t mods)
{
    struct harness *h = (struct harness *)ud;
    (void)seq; (void)code; (void)state; (void)mods;
    h->ap_key_calls++;
}

static const scene_client_cbs g_app_cbs = {
    cb_ap_welcome, NULL, NULL, NULL, NULL, NULL,
    cb_ap_pointer, cb_ap_activate, cb_ap_focus, cb_ap_key,
    NULL, NULL, NULL, NULL, NULL
};

/* Pump both sessions' links (4 rounds), then one compositor frame.
 * Does NOT call scene_shell_tick: task buttons / clock / tray texts
 * would pollute the cross-app search determinism. */
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

/* Deliver any queued client ops to the stores (app window build). */
static void settle(struct harness *h)
{
    tickf(h);
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
                                  "shell", &g_sh_cbs, h), 0);

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
    /* hover style slot 1: the overlay's selected-row highlight */
    h->hover_style = scene_compositor_setup_hover_style(h->cp,
        0xFF2A2A4E, 0xFFFFFFFF);
    CHECK_EQ(h->hover_style, 1u);
    scene_shell_set_hover_style(h->sh, h->hover_style);
    tickf(h);
}

/* App session (layer 1): a WINDOW titled "Alpha Beta" with a button
 * "gamma". */
static void app_build(struct harness *h)
{
    uint8_t vis = SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE;
    CHECK_EQ(scene_client_create_node(h->ac, SCENE_NO_PARENT, 41000,
                                      SCENE_ROLE_WINDOW,
                                      &(scene_rect){100, 50, 240, 160},
                                      vis), 0);
    CHECK_EQ(scene_client_set_text(h->ac, 41000, 1, "Alpha Beta", 10), 0);
    CHECK_EQ(scene_client_create_node(h->ac, 41000, 41001,
                                      SCENE_ROLE_BUTTON,
                                      &(scene_rect){120, 182, 80, 24},
                                      vis), 0);
    CHECK_EQ(scene_client_set_text(h->ac, 41001, 1, "gamma", 5), 0);
    settle(h);
}

/* Inject a key-down through the compositor and pump until delivered. */
static void inject_key(struct harness *h, uint32_t code, uint8_t mods)
{
    CHECK_EQ(scene_compositor_input_key(h->cp, code, 1, mods), 0);
    tickf(h);
}

/* Read one text slot of a shell-store node into buf (cap). Returns the
 * length, or -1 when the node has no texts. */
static int shell_text(scene_store *ss, scene_node_id id,
                      char *buf, size_t cap)
{
    scene_node_text_vis tv[8];
    int n = scene_store_node_texts(ss, id, tv, 8);
    if (n > 0 && tv[0].len > 0 && tv[0].len < cap) {
        memcpy(buf, tv[0].data, tv[0].len);
        buf[tv[0].len] = '\0';
        return (int)tv[0].len;
    }
    return -1;
}

/* ---- tests ------------------------------------------------------------ */

/* Super+S opens the overlay (lazily built, query empty, keyboard focus
 * moves to the shell); Super+S again closes it; reopening clears the
 * query. */
static void test_ovl_open_close(void)
{
    struct harness h;
    harness_init(&h);
    harness_shell(&h);
    app_build(&h);

    scene_store *ss = scene_compositor_store(h.cp);
    scene_node_vis v;
    char buf[80];

    /* lazily built: nodes do not exist before the first open */
    CHECK(scene_store_node_vis(ss, 50000, &v) != 0);

    /* Super+S opens */
    inject_key(&h, 31, SCENE_MOD_SUPER);
    CHECK_EQ(scene_store_node_vis(ss, 50000, &v), 0);
    CHECK(v.flags & SCENE_FLAG_VISIBLE);
    CHECK_EQ(v.role, SCENE_ROLE_MENU);
    CHECK_EQ(scene_store_node_vis(ss, 50001, &v), 0);
    CHECK(v.flags & SCENE_FLAG_VISIBLE);
    CHECK_EQ(scene_store_focus(ss), 10000u);      /* background focused */
    CHECK_EQ(scene_compositor_focus_is_shell(h.cp), 1);
    CHECK_EQ(shell_text(ss, 50001, buf, sizeof(buf)), 2);
    CHECK(memcmp(buf, "> ", 2) == 0);

    /* Super+S closes */
    inject_key(&h, 31, SCENE_MOD_SUPER);
    CHECK_EQ(scene_store_node_vis(ss, 50000, &v), 0);
    CHECK(!(v.flags & SCENE_FLAG_VISIBLE));
    CHECK_EQ(scene_store_node_vis(ss, 50001, &v), 0);
    CHECK(!(v.flags & SCENE_FLAG_VISIBLE));
    CHECK_EQ(scene_store_node_vis(ss, 50010, &v), 0);
    CHECK(!(v.flags & SCENE_FLAG_VISIBLE));

    /* reopen: query cleared again */
    inject_key(&h, 31, SCENE_MOD_SUPER);
    CHECK_EQ(shell_text(ss, 50001, buf, sizeof(buf)), 2);
    CHECK(memcmp(buf, "> ", 2) == 0);

    scene_shell_free(h.sh);
    harness_free(&h);
    printf("test_ovl_open_close: ok\n");
}

/* Typing searches live: "alp" matches exactly the app window title,
 * shown as "[L1] Alpha Beta"; "gamma" is not matched; the shell's own
 * texts (start button "Menu", no clock/tray without tick) never match. */
static void test_ovl_search(void)
{
    struct harness h;
    harness_init(&h);
    harness_shell(&h);
    app_build(&h);

    scene_store *ss = scene_compositor_store(h.cp);
    scene_node_vis v;
    char buf[80];

    inject_key(&h, 31, SCENE_MOD_SUPER);   /* open */
    inject_key(&h, 30, 0);                 /* 'a' */
    inject_key(&h, 38, 0);                 /* 'l' */
    inject_key(&h, 25, 0);                 /* 'p' */

    CHECK_EQ(shell_text(ss, 50001, buf, sizeof(buf)), 5);
    CHECK(memcmp(buf, "> alp", 5) == 0);

    /* exactly one hit: the app window title, with the layer prefix */
    CHECK_EQ(shell_text(ss, 50010, buf, sizeof(buf)), 15);
    CHECK(memcmp(buf, "[L1] Alpha Beta", 15) == 0);
    CHECK_EQ(scene_store_node_vis(ss, 50011, &v), 0);
    CHECK(!(v.flags & SCENE_FLAG_VISIBLE));   /* no second hit */

    scene_shell_free(h.sh);
    harness_free(&h);
    printf("test_ovl_search: ok\n");
}

/* Enter activates hit 0: the app window becomes the engine focus of its
 * own layer's store (host WM) and the overlay closes. */
static void test_ovl_activate(void)
{
    struct harness h;
    harness_init(&h);
    harness_shell(&h);
    app_build(&h);

    scene_store *ss = scene_compositor_store(h.cp);
    scene_store *appst = scene_compositor_layer_store(h.cp, 1);
    scene_node_vis v;

    inject_key(&h, 31, SCENE_MOD_SUPER);   /* open */
    inject_key(&h, 30, 0);                 /* 'a' */
    inject_key(&h, 38, 0);                 /* 'l' */
    inject_key(&h, 25, 0);                 /* 'p' */
    inject_key(&h, 28, 0);                 /* Enter */

    CHECK_EQ(scene_store_focus(appst), 41000u);
    CHECK_EQ(scene_store_node_vis(ss, 50000, &v), 0);
    CHECK(!(v.flags & SCENE_FLAG_VISIBLE));
    CHECK_EQ(scene_store_node_vis(ss, 50010, &v), 0);
    CHECK(!(v.flags & SCENE_FLAG_VISIBLE));   /* rows hidden with it */
    /* zero wire bytes reached the app client */
    CHECK_EQ(h.ap_focus_calls, 0);
    CHECK_EQ(h.ap_act_calls, 0);

    scene_shell_free(h.sh);
    harness_free(&h);
    printf("test_ovl_activate: ok\n");
}

/* A click on a hit row activates the hit through the shell's normal
 * INPUT_ACTIVATE path: the "gamma" button gains the engine focus. The
 * click lands outside the app window rect, so the compositor routes it
 * to the shell session. */
static void test_ovl_click_hit(void)
{
    struct harness h;
    harness_init(&h);
    harness_shell(&h);
    app_build(&h);

    scene_store *ss = scene_compositor_store(h.cp);
    scene_store *appst = scene_compositor_layer_store(h.cp, 1);
    scene_node_vis v;
    char buf[80];

    inject_key(&h, 31, SCENE_MOD_SUPER);   /* open */
    inject_key(&h, 34, 0);                 /* 'g' */
    inject_key(&h, 30, 0);                 /* 'a' */

    CHECK_EQ(shell_text(ss, 50010, buf, sizeof(buf)), 10);
    CHECK(memcmp(buf, "[L1] gamma", 10) == 0);

    /* click the row's center (row 0: x in [176,624), app window is
     * x in [100,340), so the compositor routes this to layer 0) */
    CHECK_EQ(scene_store_node_vis(ss, 50010, &v), 0);
    {
        int32_t cx = v.rect[0] + v.rect[2] / 2;
        int32_t cy = v.rect[1] + v.rect[3] / 2;
        h.sh_act_calls = 0;
        CHECK_EQ(scene_compositor_input_pointer(h.cp, 0, cx, cy, 1), 0);
        tickf(&h);
    }
    CHECK_EQ(h.sh_act_calls, 1);
    CHECK_EQ(h.sh_act_id, 50010u);
    CHECK_EQ(scene_store_focus(appst), 41001u);   /* "gamma" button */
    CHECK_EQ(scene_store_node_vis(ss, 50000, &v), 0);
    CHECK(!(v.flags & SCENE_FLAG_VISIBLE));       /* overlay closed */

    scene_shell_free(h.sh);
    harness_free(&h);
    printf("test_ovl_click_hit: ok\n");
}

/* Backspace edits the query; Escape closes; reopening resets it. */
static void test_ovl_backspace_esc(void)
{
    struct harness h;
    harness_init(&h);
    harness_shell(&h);
    app_build(&h);

    scene_store *ss = scene_compositor_store(h.cp);
    scene_node_vis v;
    char buf[80];

    inject_key(&h, 31, SCENE_MOD_SUPER);   /* open */
    inject_key(&h, 30, 0);                 /* 'a' */
    inject_key(&h, 48, 0);                 /* 'b' */
    inject_key(&h, 48, 0);                 /* 'b' */
    CHECK_EQ(shell_text(ss, 50001, buf, sizeof(buf)), 5);
    CHECK(memcmp(buf, "> abb", 5) == 0);

    inject_key(&h, 14, 0);                 /* Backspace */
    CHECK_EQ(shell_text(ss, 50001, buf, sizeof(buf)), 4);
    CHECK(memcmp(buf, "> ab", 4) == 0);

    inject_key(&h, 1, 0);                  /* Escape */
    CHECK_EQ(scene_store_node_vis(ss, 50000, &v), 0);
    CHECK(!(v.flags & SCENE_FLAG_VISIBLE));

    inject_key(&h, 31, SCENE_MOD_SUPER);   /* reopen */
    CHECK_EQ(shell_text(ss, 50001, buf, sizeof(buf)), 2);
    CHECK(memcmp(buf, "> ", 2) == 0);

    scene_shell_free(h.sh);
    harness_free(&h);
    printf("test_ovl_backspace_esc: ok\n");
}

/* The key grab wins over focus: clicking into the app window moves the
 * keyboard focus to layer 1, yet Super+S still opens the overlay and
 * the typed letters reach the shell, never the app. */
static void test_ovl_key_grab_while_app_focused(void)
{
    struct harness h;
    harness_init(&h);
    harness_shell(&h);
    app_build(&h);

    scene_store *ss = scene_compositor_store(h.cp);
    scene_node_vis v;
    char buf[80];

    /* click into the app window: keyboard focus moves to layer 1 */
    h.ap_act_calls = 0;
    CHECK_EQ(scene_compositor_input_pointer(h.cp, 0, 200, 100, 1), 0);
    tickf(&h);
    CHECK_EQ(scene_compositor_focus_is_shell(h.cp), 0);
    CHECK_EQ(h.ap_act_calls, 1);

    /* Super+S: the grab routes to the shell regardless of focus */
    h.ap_key_calls = 0;
    inject_key(&h, 31, SCENE_MOD_SUPER);
    CHECK_EQ(scene_compositor_focus_is_shell(h.cp), 1);
    CHECK_EQ(scene_store_node_vis(ss, 50000, &v), 0);
    CHECK(v.flags & SCENE_FLAG_VISIBLE);

    /* subsequent letters reach the shell (focus moved with the overlay) */
    inject_key(&h, 30, 0);                 /* 'a' */
    inject_key(&h, 38, 0);                 /* 'l' */
    CHECK_EQ(shell_text(ss, 50001, buf, sizeof(buf)), 4);
    CHECK(memcmp(buf, "> al", 4) == 0);
    CHECK_EQ(h.ap_key_calls, 0);           /* none ever reached the app */

    scene_shell_free(h.sh);
    harness_free(&h);
    printf("test_ovl_key_grab_while_app_focused: ok\n");
}

/* Layer-0 hits carry no prefix: "menu" matches the start button text in
 * the shell's own store. */
static void test_ovl_layer0(void)
{
    struct harness h;
    harness_init(&h);
    harness_shell(&h);
    app_build(&h);

    scene_store *ss = scene_compositor_store(h.cp);
    scene_node_vis v;
    char buf[80];

    inject_key(&h, 31, SCENE_MOD_SUPER);   /* open */
    inject_key(&h, 50, 0);                 /* 'm' */
    inject_key(&h, 18, 0);                 /* 'e' */
    inject_key(&h, 49, 0);                 /* 'n' */
    inject_key(&h, 22, 0);                 /* 'u' */

    CHECK_EQ(shell_text(ss, 50010, buf, sizeof(buf)), 4);
    CHECK(memcmp(buf, "Menu", 4) == 0);    /* no "[L0]" prefix */
    CHECK_EQ(scene_store_node_vis(ss, 50011, &v), 0);
    CHECK(!(v.flags & SCENE_FLAG_VISIBLE));   /* exactly one hit */

    scene_shell_free(h.sh);
    harness_free(&h);
    printf("test_ovl_layer0: ok\n");
}

/* Up/Down move the selection (hover style highlight); Enter activates
 * the selected row, not row 0. */
static void test_ovl_select(void)
{
    struct harness h;
    harness_init(&h);
    harness_shell(&h);
    app_build(&h);

    scene_store *ss = scene_compositor_store(h.cp);
    scene_store *appst = scene_compositor_layer_store(h.cp, 1);
    scene_node_vis v;
    char buf[80];

    inject_key(&h, 31, SCENE_MOD_SUPER);   /* open */
    inject_key(&h, 30, 0);                 /* 'a': 2 hits */

    CHECK_EQ(shell_text(ss, 50010, buf, sizeof(buf)), 15);
    CHECK(memcmp(buf, "[L1] Alpha Beta", 15) == 0);
    CHECK_EQ(shell_text(ss, 50011, buf, sizeof(buf)), 10);
    CHECK(memcmp(buf, "[L1] gamma", 10) == 0);

    /* row 0 selected by default: hover style; row 1 base button theme */
    CHECK_EQ(scene_store_node_vis(ss, 50010, &v), 0);
    CHECK_EQ(v.style, h.hover_style);
    CHECK_EQ(scene_store_node_vis(ss, 50011, &v), 0);
    CHECK_EQ(v.style, 5u);                 /* SHELL_STYLE_BUTTON */

    inject_key(&h, 108, 0);                /* Down: select row 1 */
    CHECK_EQ(scene_store_node_vis(ss, 50011, &v), 0);
    CHECK_EQ(v.style, h.hover_style);
    CHECK_EQ(scene_store_node_vis(ss, 50010, &v), 0);
    CHECK_EQ(v.style, 5u);

    inject_key(&h, 28, 0);                 /* Enter: activates row 1 */
    CHECK_EQ(scene_store_focus(appst), 41001u);   /* "gamma" button */
    CHECK_EQ(scene_store_node_vis(ss, 50000, &v), 0);
    CHECK(!(v.flags & SCENE_FLAG_VISIBLE));       /* overlay closed */

    scene_shell_free(h.sh);
    harness_free(&h);
    printf("test_ovl_select: ok\n");
}

int main(void)
{
    test_ovl_open_close();
    test_ovl_search();
    test_ovl_activate();
    test_ovl_click_hit();
    test_ovl_backspace_esc();
    test_ovl_key_grab_while_app_focused();
    test_ovl_layer0();
    test_ovl_select();
    printf("test_searchoverlay: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
