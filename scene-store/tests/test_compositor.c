/*
 * test_compositor.c — compositor core tests: the store's first consumer.
 *
 * A scene_client drives a compositor-hosted session over the loopback
 * transport; after each ingest batch the compositor runs one frame and
 * the tests assert exact framebuffer pixels, damage rects, rendered
 * seq, style re-theming, texture painting, visibility, destroy/ghost/
 * replay behavior, and input forwarding with flow control.
 */
#include "scene_compositor.h"
#include "scene_client.h"
#include "scene_transport.h"

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

/* ---- captured client events ------------------------------------------- */

#define MAXE 16

struct harness {
    scene_loopback *lb;
    scene_transport *server_ts;
    scene_client *cl;
    scene_compositor *cp;

    int wel_called; uint32_t wel_sid;
    int err_called; uint16_t err_code;
    int ptr_calls; uint64_t ptr_seq[MAXE];
    int32_t ptr_xy[MAXE][2]; uint8_t ptr_btn[MAXE];
    int act_calls; uint64_t act_seq[MAXE]; uint32_t act_id[MAXE];
    int pd_calls; uint64_t pd_seq[MAXE], pd_token[MAXE];
    int key_calls; uint64_t key_seq;
    uint32_t key_code; uint8_t key_state; uint8_t key_mod;
    int txt_calls; uint64_t txt_seq; uint32_t txt_len;
    char txt_buf[256];
    int closed_calls;
};

static void cb_welcome(void *ud, uint32_t sid, uint16_t ver,
                       const scene_limits *lim)
{
    struct harness *h = (struct harness *)ud;
    (void)ver; (void)lim;
    h->wel_called++;
    h->wel_sid = sid;
}

static void cb_error(void *ud, uint16_t code, const char *msg, uint32_t len)
{
    struct harness *h = (struct harness *)ud;
    (void)msg; (void)len;
    h->err_called++;
    h->err_code = code;
}

static void cb_input_pointer(void *ud, uint64_t seq, uint8_t device,
                             int32_t x, int32_t y, uint8_t buttons)
{
    struct harness *h = (struct harness *)ud;
    (void)device;
    if (h->ptr_calls < MAXE) {
        h->ptr_seq[h->ptr_calls] = seq;
        h->ptr_xy[h->ptr_calls][0] = x;
        h->ptr_xy[h->ptr_calls][1] = y;
        h->ptr_btn[h->ptr_calls] = buttons;
    }
    h->ptr_calls++;
}

static void cb_input_activate(void *ud, uint64_t seq, scene_node_id id)
{
    struct harness *h = (struct harness *)ud;
    if (h->act_calls < MAXE) {
        h->act_seq[h->act_calls] = seq;
        h->act_id[h->act_calls] = id;
    }
    h->act_calls++;
}

static void cb_present_done(void *ud, uint64_t seq, uint64_t token,
                            uint64_t latency_us)
{
    struct harness *h = (struct harness *)ud;
    (void)latency_us;
    if (h->pd_calls < MAXE) {
        h->pd_seq[h->pd_calls] = seq;
        h->pd_token[h->pd_calls] = token;
    }
    h->pd_calls++;
}

static void cb_closed(void *ud)
{
    struct harness *h = (struct harness *)ud;
    h->closed_calls++;
}

static void cb_input_key(void *ud, uint64_t seq, uint32_t key_code,
                          uint8_t state, uint8_t modifiers)
{
    struct harness *h = (struct harness *)ud;
    h->key_seq = seq;
    h->key_code = key_code;
    h->key_state = state;
    h->key_mod = modifiers;
    h->key_calls++;
}

static void cb_input_text(void *ud, uint64_t seq, const char *text,
                          uint32_t len)
{
    struct harness *h = (struct harness *)ud;
    h->txt_seq = seq;
    h->txt_len = len;
    if (len < sizeof(h->txt_buf)) {
        memcpy(h->txt_buf, text, len);
        h->txt_buf[len] = 0;
    }
    h->txt_calls++;
}

static const scene_client_cbs g_cbs = {
    cb_welcome, cb_error, NULL, NULL, NULL, NULL,
    cb_input_pointer, cb_input_activate, NULL, cb_input_key, cb_input_text,
    cb_present_done, NULL, NULL, cb_closed
};

/* ---- harness plumbing --------------------------------------------------- */

static void harness_init(struct harness *h)
{
    memset(h, 0, sizeof(*h));
    h->lb = scene_loopback_new();
    h->server_ts = scene_loopback_server_end(h->lb);
    h->cp = scene_compositor_new(NULL, 800, 600);
    h->cl = scene_client_new();
    scene_compositor_store(h->cp);
    CHECK(h->cp != NULL);
    scene_server_attach(scene_compositor_server(h->cp));
    CHECK_EQ(scene_client_connect(h->cl, scene_loopback_client_end(h->lb),
                                  "loopback", &g_cbs, h), 0);
}

static void harness_free(struct harness *h)
{
    scene_client_free(h->cl);
    scene_compositor_free(h->cp);
    scene_transport_close(h->server_ts);
    scene_loopback_free(h->lb);
}

/* Drive both ends, then run exactly one compositor frame. */
static void tickf(struct harness *h)
{
    int round;
    for (round = 0; round < 4; round++) {
        scene_client_flush(h->cl);
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
        scene_client_pump(h->cl);
    }
    CHECK_EQ(scene_compositor_frame(h->cp), 0);
}

static void op_ok(struct harness *h, int r, const char *what)
{
    (void)h;
    if (r != 0) {
        failures++;
        printf("FAIL %s: op %s returned %d\n", __FILE__, what, r);
    }
    checks++;
}

/* ---- multi-session harness (notification tests) ------------------------ */
/* Layer 0 = shell session client, layer 1 = an app session: its own
 * scene_server attached via scene_compositor_add_session, driven by a
 * raw scene_client over its own loopback. Same per-frame cadence as the
 * single-session harness (flush -> feed -> out-next -> pump, x4, then
 * exactly one compositor frame).                                        */
struct nh {
    struct harness sh_ev;    /* shell client event records                */
    struct harness ap_ev;    /* app client event records                  */
    scene_compositor *cp;
    scene_server     *sv1;   /* app session (owned by the compositor)     */
    scene_loopback   *lb0, *lb1;
    scene_transport  *ts0, *ts1;
    scene_client     *cl0, *cl1;

    int nf_count;            /* notification callback fires               */
    int nf_layer[16];
    scene_node_id nf_id[16];
    uint32_t nf_len[16];
    char nf_text[16][64];
};

static void cb_nf(void *ud, int layer, scene_node_id id, const char *text,
                  uint32_t len)
{
    struct nh *h = (struct nh *)ud;
    if (h->nf_count < 16) {
        h->nf_layer[h->nf_count] = layer;
        h->nf_id[h->nf_count] = id;
        h->nf_len[h->nf_count] = len;
        if (len < sizeof(h->nf_text[0])) {
            memcpy(h->nf_text[h->nf_count], text ? text : "", len);
            h->nf_text[h->nf_count][len] = 0;
        }
    }
    h->nf_count++;
}

/* Drive both sessions' links, then exactly one compositor frame. */
static void ntick(struct nh *h)
{
    int round;
    for (round = 0; round < 4; round++) {
        scene_client_flush(h->cl0);
        scene_client_flush(h->cl1);
        uint8_t buf[8192];
        uint32_t got;
        while (scene_transport_recv(h->ts0, buf, sizeof(buf), &got) == 0
               && got)
            if (scene_server_feed(scene_compositor_server(h->cp), buf, got)
                != 0) break;
        while (scene_transport_recv(h->ts1, buf, sizeof(buf), &got) == 0
               && got)
            if (scene_server_feed(h->sv1, buf, got) != 0) break;
        const uint8_t *f;
        uint32_t flen;
        while (scene_server_out_next_frame(scene_compositor_server(h->cp),
                                           &f, &flen) == 1)
            scene_transport_send(h->ts0, f, flen);
        while (scene_server_out_next_frame(h->sv1, &f, &flen) == 1)
            scene_transport_send(h->ts1, f, flen);
        scene_client_pump(h->cl0);
        scene_client_pump(h->cl1);
    }
    CHECK_EQ(scene_compositor_frame(h->cp), 0);
}

static void nh_init(struct nh *h)
{
    memset(h, 0, sizeof(*h));
    h->cp = scene_compositor_new(NULL, 800, 600);
    CHECK(h->cp != NULL);

    /* shell session (layer 0) on loopback 0 */
    h->lb0 = scene_loopback_new();
    h->ts0 = scene_loopback_server_end(h->lb0);
    h->cl0 = scene_client_new();
    scene_server_attach(scene_compositor_server(h->cp));
    CHECK_EQ(scene_client_connect(h->cl0, scene_loopback_client_end(h->lb0),
                                  "shell", &g_cbs, &h->sh_ev), 0);

    /* app session (layer 1): its own server, compositor-attached */
    h->lb1 = scene_loopback_new();
    h->ts1 = scene_loopback_server_end(h->lb1);
    h->sv1 = scene_server_new(NULL);
    CHECK(h->sv1 != NULL);
    CHECK_EQ(scene_compositor_add_session(h->cp, h->sv1), 1);
    scene_server_attach(h->sv1);
    h->cl1 = scene_client_new();
    CHECK_EQ(scene_client_connect(h->cl1, scene_loopback_client_end(h->lb1),
                                  "app", &g_cbs, &h->ap_ev), 0);
    ntick(h);   /* handshake: both WELCOMEs delivered, emit guards open */
}

static void nh_free(struct nh *h)
{
    scene_client_free(h->cl1);  /* closes the app client-end transport */
    scene_transport_close(h->ts1);
    scene_loopback_free(h->lb1);
    scene_client_free(h->cl0);
    scene_transport_close(h->ts0);
    scene_loopback_free(h->lb0);
    scene_compositor_free(h->cp);   /* frees sv1 (take-ownership) */
}

/* Deterministic app scene through the wire (12 ops -> scene_seq 12).
 *  100 WINDOW (10,10,300,200) VISIBLE
 *  101 PANEL  (10,40,300,170) VISIBLE  parent 100
 *  200 BUTTON (30,60,100,30)  VISIBLE|FOCUSABLE  parent 101  "Open"
 *  201 BUTTON (140,60,100,30) VISIBLE|FOCUSABLE  parent 101  "Save"
 *  202 LABEL  (30,100,160,8)  VISIBLE  parent 101           "Hello"
 *  203 BUTTON (30,130,100,30) hidden (no flags)  parent 101
 *  204 IMAGE  (200,120,8,8)   VISIBLE  parent 101 */
static void build_app(struct harness *h)
{
    static const scene_rect r_win = {10, 10, 300, 200};
    static const scene_rect r_pan = {10, 40, 300, 170};
    static const scene_rect r_b1  = {30, 60, 100, 30};
    static const scene_rect r_b2  = {140, 60, 100, 30};
    static const scene_rect r_lab = {30, 100, 160, 8};
    static const scene_rect r_b3  = {30, 130, 100, 30};
    static const scene_rect r_img = {200, 120, 8, 8};
    op_ok(h, scene_client_create_node(h->cl, SCENE_NO_PARENT, 100,
                                      SCENE_ROLE_WINDOW, &r_win,
                                      SCENE_FLAG_VISIBLE), "create 100");
    op_ok(h, scene_client_create_node(h->cl, 100, 101, SCENE_ROLE_PANEL,
                                      &r_pan, SCENE_FLAG_VISIBLE), "create 101");
    op_ok(h, scene_client_create_node(h->cl, 101, 200, SCENE_ROLE_BUTTON,
                                      &r_b1, SCENE_FLAG_VISIBLE |
                                      SCENE_FLAG_FOCUSABLE), "create 200");
    op_ok(h, scene_client_set_text(h->cl, 200, 1, "Open", 4), "text 200");
    op_ok(h, scene_client_create_node(h->cl, 101, 201, SCENE_ROLE_BUTTON,
                                      &r_b2, SCENE_FLAG_VISIBLE |
                                      SCENE_FLAG_FOCUSABLE), "create 201");
    op_ok(h, scene_client_set_text(h->cl, 201, 1, "Save", 4), "text 201");
    op_ok(h, scene_client_create_node(h->cl, 101, 202, SCENE_ROLE_LABEL,
                                      &r_lab, SCENE_FLAG_VISIBLE),
          "create 202");
    op_ok(h, scene_client_set_text(h->cl, 202, 1, "Hello", 5), "text 202");
    op_ok(h, scene_client_create_node(h->cl, 101, 203, SCENE_ROLE_BUTTON,
                                      &r_b3, 0), "create 203");
    op_ok(h, scene_client_set_text(h->cl, 203, 1, "Invisible", 9),
          "text 203");
    op_ok(h, scene_client_create_node(h->cl, 101, 204, SCENE_ROLE_IMAGE,
                                      &r_img, SCENE_FLAG_VISIBLE),
          "create 204");
}

/* ==================================================================== */
/* Tests                                                                  */
/* ==================================================================== */

static void test_comp_empty_and_force(void)
{
    struct harness h;
    harness_init(&h);
    tickf(&h);                     /* nothing ingested yet */
    CHECK_EQ(h.wel_called, 1);
    CHECK_EQ(scene_compositor_rendered_seq(h.cp), 0);
    scene_rect d[8];
    CHECK_EQ(scene_compositor_damage(h.cp, d, 8), 0);
    CHECK_EQ(PX(h.cp, 100, 100), 0);                 /* zeroed fb */
    /* a forced frame clears the fb and reports full damage */
    scene_compositor_force_repaint(h.cp);
    tickf(&h);
    CHECK_EQ(scene_compositor_damage(h.cp, d, 8), 1);
    CHECK_EQ(d[0].w, 800);
    CHECK_EQ(d[0].h, 600);
    CHECK_EQ(PX(h.cp, 100, 100), 0xFF101010u);
    /* resize repaints everything */
    scene_compositor_resize(h.cp, 400, 300);
    tickf(&h);
    CHECK_EQ(scene_compositor_damage(h.cp, d, 8), 1);
    CHECK_EQ(d[0].w, 400);
    CHECK_EQ(d[0].h, 300);
    CHECK_EQ(PX(h.cp, 799, 599), 0);            /* old buffer freed */
    /* clear color change repaints */
    scene_compositor_set_clear(h.cp, 0xFF202028u);
    tickf(&h);
    CHECK_EQ(PX(h.cp, 399, 299), 0xFF202028u);
    harness_free(&h);
    printf("test_comp_empty_and_force: ok\n");
}

static void test_comp_build_paint(void)
{
    struct harness h;
    harness_init(&h);
    tickf(&h);
    build_app(&h);
    scene_client_present(h.cl, 7);
    tickf(&h);
    CHECK_EQ(h.pd_calls, 1);
    CHECK_EQ(h.pd_token[0], 7);
    /* one frame after the build: six visible nodes damaged */
    scene_rect d[16];
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 6);
    CHECK_EQ(scene_compositor_rendered_seq(h.cp), 11);
    /* clear color outside the window */
    CHECK_EQ(PX(h.cp, 5, 5), 0xFF101010u);
    /* window fill */
    CHECK_EQ(PX(h.cp, 200, 20), 0xFF202020u);
    /* panel over window */
    CHECK_EQ(PX(h.cp, 200, 150), 0xFF2A2A2Au);
    /* button 200: fill, border, corner, text "Open" at its origin */
    CHECK_EQ(PX(h.cp, 50, 65), 0xFF3C3C3Cu);
    CHECK_EQ(PX(h.cp, 30, 65), 0xFF555555u);    /* left border */
    CHECK_EQ(PX(h.cp, 80, 60), 0xFF555555u);    /* top border */
    /* corner notch (Pass 7): the fillet arc cuts the top-left corner,
     * so the corner pixel shows the panel, not the border; the arc's
     * top row starts at x+radius (see test_comp_rounded_corner) */
    CHECK_EQ(PX(h.cp, 30, 60), 0xFF2A2A2Au);
    CHECK_EQ(PX(h.cp, 32, 60), 0xFFFFFFFFu);    /* 'O' row 0 */
    CHECK_EQ(PX(h.cp, 32, 61), 0xFFFFFFFFu);    /* 'O' row 1 */
    /* button 201 */
    CHECK_EQ(PX(h.cp, 190, 65), 0xFF3C3C3Cu);
    CHECK_EQ(PX(h.cp, 140, 65), 0xFF555555u);
    /* label "Hello": H pixels, gap, and a lowercase 'o' (rows 2..6 of
     * the glyph; the top rows of lowercase are empty) */
    CHECK_EQ(PX(h.cp, 32, 100), 0xFFFFFFFFu);
    CHECK_EQ(PX(h.cp, 33, 100), 0xFF2A2A2Au);   /* gap inside 'H' */
    CHECK_EQ(PX(h.cp, 36, 100), 0xFFFFFFFFu);   /* 'H' right stem */
    CHECK_EQ(PX(h.cp, 64, 102), 0xFFFFFFFFu);   /* 'o' row 2 */
    CHECK_EQ(PX(h.cp, 63, 102), 0xFF2A2A2Au);   /* 'o' gap */
    /* hidden button 203 is not painted */
    CHECK_EQ(PX(h.cp, 50, 145), 0xFF2A2A2Au);
    /* image node shows its role fill before any texture */
    CHECK_EQ(PX(h.cp, 204, 124), 0xFF1E1E1Eu);
    /* a second frame with no changes: no damage, no repaint */
    tickf(&h);
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 0);
    CHECK_EQ(scene_compositor_rendered_seq(h.cp), 11);
    CHECK_EQ(PX(h.cp, 50, 65), 0xFF3C3C3Cu);
    harness_free(&h);
    printf("test_comp_build_paint: ok\n");
}

static void test_comp_move_text_visibility(void)
{
    struct harness h;
    scene_rect d[16];
    harness_init(&h);
    tickf(&h);
    build_app(&h);
    tickf(&h);
    /* move button 200: old + new rect damaged */
    static const scene_rect r_mv = {250, 150, 100, 30};
    op_ok(&h, scene_client_set_rect(h.cl, 200, &r_mv), "move 200");
    tickf(&h);
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 2);
    CHECK_EQ(d[0].x, 30); CHECK_EQ(d[0].y, 60);
    CHECK_EQ(d[0].w, 100); CHECK_EQ(d[0].h, 30);
    CHECK_EQ(d[1].x, 250); CHECK_EQ(d[1].y, 150);
    CHECK_EQ(PX(h.cp, 50, 65), 0xFF2A2A2Au);    /* old spot: panel */
    CHECK_EQ(PX(h.cp, 270, 155), 0xFF3C3C3Cu);  /* new spot: button */
    /* text change only (same rect): one damage rect */
    op_ok(&h, scene_client_set_text(h.cl, 200, 1, "New", 3), "retitle 200");
    tickf(&h);
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 1);
    CHECK_EQ(d[0].x, 250); CHECK_EQ(d[0].y, 150);
    CHECK_EQ(PX(h.cp, 252, 150), 0xFFFFFFFFu);  /* 'N' stem, row 0 */
    CHECK_EQ(PX(h.cp, 254, 151), 0xFF3C3C3Cu);  /* gap inside 'N', row 1 */
    /* hidden node becomes visible */
    op_ok(&h, scene_client_set_flags(h.cl, 203,
                                     SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE),
          "show 203");
    tickf(&h);
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 1);
    CHECK_EQ(PX(h.cp, 50, 145), 0xFF3C3C3Cu);
    /* and back to hidden: damage again */
    op_ok(&h, scene_client_set_flags(h.cl, 203, 0), "hide 203");
    tickf(&h);
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 1);
    CHECK_EQ(PX(h.cp, 50, 145), 0xFF2A2A2Au);
    harness_free(&h);
    printf("test_comp_move_text_visibility: ok\n");
}

static void test_comp_retheme(void)
{
    struct harness h;
    harness_init(&h);
    tickf(&h);
    build_app(&h);
    tickf(&h);
    /* server-owned style table: node 201 switches to style 1, then the
     * compositor re-themes style 1 live (re-theme of a running app) */
    scene_compositor_set_style_count(h.cp, 2);   /* refs 1..1 settable */
    scene_style st1 = {0xFF4488AAu, 0xFF224466u, 0xFFFFFFFFu, 1, 0, 0, 0};
    CHECK_EQ(scene_compositor_set_style(h.cp, 1, &st1), 0);
    op_ok(&h, scene_client_set_style(h.cl, 201, 1), "style 201");
    tickf(&h);
    scene_rect d[16];
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 1);
    CHECK_EQ(PX(h.cp, 190, 65), 0xFF4488AAu);
    CHECK_EQ(PX(h.cp, 190, 60), 0xFF224466u);   /* new border */
    /* re-theme: change style 1 -> every referencing node dirties */
    scene_style st2 = {0xFF88CC44u, 0xFF668822u, 0xFFFFFFFFu, 1, 0, 0, 0};
    CHECK_EQ(scene_compositor_set_style(h.cp, 1, &st2), 0);
    tickf(&h);
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 1);
    CHECK_EQ(PX(h.cp, 190, 65), 0xFF88CC44u);
    /* other buttons keep role defaults */
    CHECK_EQ(PX(h.cp, 50, 65), 0xFF3C3C3Cu);
    /* unchanged style set is a no-op */
    CHECK_EQ(scene_compositor_set_style(h.cp, 1, &st2), 0);
    harness_free(&h);
    printf("test_comp_retheme: ok\n");
}

static void test_comp_texture(void)
{
    struct harness h;
    harness_init(&h);
    tickf(&h);
    build_app(&h);
    tickf(&h);
    /* opaque XRGB texture over the image node */
    uint32_t tex[8 * 8];
    uint32_t i;
    for (i = 0; i < 8 * 8; i++) tex[i] = 0xFF800040u;
    CHECK_EQ(scene_compositor_register_texture(h.cp, 77, 8, 8,
                                               SCENE_TEX_FMT_XRGB, 1, tex), 0);
    static const scene_rect tsrc = {0, 0, 8, 8};
    op_ok(&h, scene_client_set_texture(h.cl, 204, 77, &tsrc, 0, 255),
          "texture 204");
    tickf(&h);
    scene_rect d[16];
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 1);
    CHECK_EQ(PX(h.cp, 204, 124), 0xFF800040u);  /* opaque replace */
    /* per-node opacity blending: 0x80 over the image fill 0xFF1E1E1E:
     * out = (src*op + dst*(255-op))/255 per channel */
    uint32_t tex2[8 * 8];
    for (i = 0; i < 8 * 8; i++) tex2[i] = 0xFF008040u;
    CHECK_EQ(scene_compositor_register_texture(h.cp, 78, 8, 8,
                                               SCENE_TEX_FMT_XRGB, 1, tex2), 0);
    op_ok(&h, scene_client_set_texture(h.cl, 204, 78, &tsrc, 0, 128),
          "texture 204 opacity");
    tickf(&h);
    CHECK_EQ(PX(h.cp, 204, 124), 0xFF0E4F2Fu);
    /* re-registering pixels of a referenced texture dirties it (node still
     * at opacity 128, so blue 0x0000FF over fill 0x1E1E1E blends to
     * 0xFF0E0E8E) */
    for (i = 0; i < 8 * 8; i++) tex2[i] = 0xFF0000FFu;
    CHECK_EQ(scene_compositor_register_texture(h.cp, 78, 8, 8,
                                               SCENE_TEX_FMT_XRGB, 1, tex2), 0);
    tickf(&h);
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 1);
    CHECK_EQ(PX(h.cp, 204, 124), 0xFF0E0E8Eu);
    /* releasing the texture unpaints it (back to the role fill) */
    CHECK_EQ(scene_compositor_release_texture(h.cp, 78), 0);
    tickf(&h);
    CHECK_EQ(PX(h.cp, 204, 124), 0xFF1E1E1Eu);
    harness_free(&h);
    printf("test_comp_texture: ok\n");
}

static void test_comp_destroy_ghost_replay(void)
{
    struct harness h;
    harness_init(&h);
    tickf(&h);
    build_app(&h);
    tickf(&h);
    /* destroy the label: its rect is damaged, pixels revert */
    op_ok(&h, scene_client_destroy_node(h.cl, 202), "destroy 202");
    tickf(&h);
    scene_rect d[16];
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 1);
    CHECK_EQ(d[0].x, 30); CHECK_EQ(d[0].y, 100);
    CHECK_EQ(PX(h.cp, 40, 100), 0xFF2A2A2Au);
    /* ghost crash: client dies, reconnects, re-issues; content identical
     * -> the diff finds nothing to repaint, rendered seq advances */
    uint32_t sid = h.wel_sid;
    scene_server_detach(scene_compositor_server(h.cp));
    scene_server_attach(scene_compositor_server(h.cp));
    CHECK_EQ(scene_client_reconnect(h.cl, scene_loopback_client_end(h.lb)), 0);
    tickf(&h);
    CHECK_EQ(h.wel_called, 2);
    CHECK_EQ(h.wel_sid, sid);
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 0);
    CHECK(scene_compositor_rendered_seq(h.cp) > 12);
    CHECK_EQ(PX(h.cp, 40, 100), 0xFF2A2A2Au);   /* label still gone */
    CHECK_EQ(PX(h.cp, 50, 65), 0xFF3C3C3Cu);    /* rest intact */
    /* replay: rewind to seq 4 (window, panel, button 200 with "Open").
     * Goods gone vs rendered: 201 and 204 (203 is hidden); 200 unchanged. */
    op_ok(&h, scene_client_set_input_mode(h.cl, SCENE_MODE_REPLAY),
          "replay mode");
    op_ok(&h, scene_client_seek(h.cl, 4), "seek 4");
    tickf(&h);
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 2);
    CHECK_EQ(PX(h.cp, 50, 65), 0xFF3C3C3Cu);    /* 200 still there */
    CHECK_EQ(PX(h.cp, 190, 65), 0xFF2A2A2Au);   /* 201 reverted */
    CHECK_EQ(PX(h.cp, 204, 124), 0xFF2A2A2Au);  /* 204 reverted */
    /* forward to seq 24 (the re-issued end, 202 still destroyed): the
     * two reverted nodes come back */
    op_ok(&h, scene_client_seek(h.cl, 24), "seek 24");
    tickf(&h);
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 2);
    CHECK_EQ(PX(h.cp, 190, 65), 0xFF3C3C3Cu);
    CHECK_EQ(PX(h.cp, 204, 124), 0xFF1E1E1Eu);
    CHECK_EQ(PX(h.cp, 32, 100), 0xFF2A2A2Au);   /* 202 stays destroyed */
    /* back to live: scene still the re-issued end state, no repaint */
    op_ok(&h, scene_client_set_input_mode(h.cl, SCENE_MODE_LIVE),
          "live mode");
    tickf(&h);
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 0);
    harness_free(&h);
    printf("test_comp_destroy_ghost_replay: ok\n");
}

static void test_comp_input_flow(void)
{
    struct harness h;
    harness_init(&h);
    tickf(&h);
    build_app(&h);
    tickf(&h);
    /* press over button 200 through the compositor */
    CHECK_EQ(scene_compositor_input_pointer(h.cp, 0, 50, 65, 0x01), 0);
    tickf(&h);
    CHECK_EQ(h.ptr_calls, 1);
    CHECK_EQ(h.ptr_seq[0], 11);
    CHECK_EQ(h.ptr_xy[0][0], 50);
    CHECK_EQ(h.ptr_xy[0][1], 65);
    CHECK_EQ(h.ptr_btn[0], 0x01);
    CHECK_EQ(h.act_calls, 1);
    CHECK_EQ(h.act_id[0], 200);
    /* second injection before an ack is dropped */
    CHECK_EQ(scene_compositor_input_pointer(h.cp, 0, 700, 500, 0x01), 0);
    tickf(&h);
    CHECK_EQ(h.ptr_calls, 1);
    CHECK_EQ(h.act_calls, 1);
    /* ack + a fresh press is delivered */
    scene_client_ack(h.cl, 11);
    tickf(&h);
    CHECK_EQ(scene_compositor_input_pointer(h.cp, 0, 200, 150, 0x01), 0);
    tickf(&h);
    CHECK_EQ(h.ptr_calls, 2);
    CHECK_EQ(h.ptr_xy[1][0], 200);
    CHECK_EQ(h.ptr_xy[1][1], 150);
    CHECK_EQ(h.act_calls, 2);
    CHECK_EQ(h.act_id[1], 101);            /* panel background */
    harness_free(&h);
    printf("test_comp_input_flow: ok\n");
}

static void test_comp_node_vis_ground_truth(void)
{
    struct harness h;
    harness_init(&h);
    tickf(&h);
    build_app(&h);
    tickf(&h);
    scene_store *st = scene_compositor_store(h.cp);
    CHECK_EQ(scene_store_node_count(st), 7);
    CHECK_EQ(scene_store_committed_seq(st), 11);
    scene_node_vis v;
    CHECK_EQ(scene_store_node_vis(st, 200, &v), 0);
    CHECK_EQ(v.id, 200);
    CHECK_EQ(v.parent, 101);
    CHECK_EQ(v.role, SCENE_ROLE_BUTTON);
    CHECK_EQ(v.flags, SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    CHECK_EQ(v.rect[0], 30);
    CHECK_EQ(v.rect[1], 60);
    CHECK_EQ(v.rect[2], 100);
    CHECK_EQ(v.rect[3], 30);
    CHECK_EQ(v.style, 0);
    CHECK_EQ(v.tex, SCENE_NO_TEXTURE);
    CHECK_EQ(v.text_count, 1);
    scene_node_text_vis t[4];
    CHECK_EQ(scene_store_node_texts(st, 200, t, 4), 1);
    CHECK_EQ(t[0].text_id, 1);
    CHECK_EQ(t[0].len, 4);
    CHECK(memcmp(t[0].data, "Open", 4) == 0);
    CHECK_EQ(scene_store_node_vis(st, 9999, &v), -1);  /* unknown */
    CHECK_EQ(scene_store_node_texts(st, 9999, t, 4), -1);
    /* region resolution agrees with painting geometry */
    CHECK_EQ(scene_store_region_at(st, 50, 65), 200);
    CHECK_EQ(scene_store_region_at(st, 5, 5), SCENE_NO_PARENT);
    harness_free(&h);
    printf("test_comp_node_vis_ground_truth: ok\n");
}

/* ==================================================================== */
/* Pass 7: compositor effects (enter/exit fades, rounded corners)        */
/* ==================================================================== */

/* FILLET: cut(dy) = r - floor(sqrt(2*r*dy - dy^2)). Hand-table for the
 * button (r=4, border 1): rows cut 4,2,1,1 then 0; the inner fill is
 * the concentric circle of radius r-1 (stroke follows the corner).     */
static void test_comp_rounded_corner(void)
{
    struct harness h;
    static const scene_rect r_btn = {100, 100, 40, 40};
    static const scene_rect r_win = {200, 100, 40, 40};
    static const scene_rect r_pan = {300, 100, 40, 40};

    harness_init(&h);
    tickf(&h);
    /* BUTTON (100,100,40,40): r=4, border 1. Role defaults: fill
     * 0xFF3C3C3C, border 0xFF555555. No text: pure geometry. */
    op_ok(&h, scene_client_create_node(h.cl, SCENE_NO_PARENT, 100,
                                       SCENE_ROLE_BUTTON, &r_btn,
                                       SCENE_FLAG_VISIBLE), "corner btn");
    /* WINDOW (200,100,40,40): r=8, no border. Fill 0xFF202020. */
    op_ok(&h, scene_client_create_node(h.cl, SCENE_NO_PARENT, 101,
                                       SCENE_ROLE_WINDOW, &r_win,
                                       SCENE_FLAG_VISIBLE), "corner win");
    /* PANEL (300,100,40,40): fill-only, but the role default radius
     * is 4 — same fillet geometry as the button, no border. */
    op_ok(&h, scene_client_create_node(h.cl, SCENE_NO_PARENT, 102,
                                       SCENE_ROLE_PANEL, &r_pan,
                                       SCENE_FLAG_VISIBLE), "corner pan");
    /* SCROLLBAR (400,100,40,40): radius 0 control, square corner. */
    static const scene_rect r_sb = {400, 100, 40, 40};
    op_ok(&h, scene_client_create_node(h.cl, SCENE_NO_PARENT, 103,
                                       SCENE_ROLE_SCROLLBAR, &r_sb,
                                       SCENE_FLAG_VISIBLE), "corner sb");
    tickf(&h);
    scene_rect d[16];
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 4);

    /* ---- button top-left arc (r=4, bw=1) ---- */
    CHECK_EQ(PX(h.cp, 100, 100), 0xFF101010u);   /* notch: outside circle */
    CHECK_EQ(PX(h.cp, 103, 100), 0xFF101010u);
    CHECK_EQ(PX(h.cp, 104, 100), 0xFF555555u);   /* arc top row starts x+r */
    CHECK_EQ(PX(h.cp, 105, 100), 0xFF555555u);
    CHECK_EQ(PX(h.cp, 102, 101), 0xFF555555u);   /* cut 2 at dy=1 */
    CHECK_EQ(PX(h.cp, 103, 101), 0xFF555555u);
    CHECK_EQ(PX(h.cp, 104, 101), 0xFF3C3C3Cu);   /* inner fill, radius r-1 */
    CHECK_EQ(PX(h.cp, 101, 102), 0xFF555555u);   /* cut 1 at dy=2 */
    CHECK_EQ(PX(h.cp, 102, 102), 0xFF3C3C3Cu);
    CHECK_EQ(PX(h.cp, 101, 103), 0xFF555555u);
    CHECK_EQ(PX(h.cp, 102, 103), 0xFF3C3C3Cu);
    CHECK_EQ(PX(h.cp, 100, 104), 0xFF555555u);   /* cut 0: left edge */
    CHECK_EQ(PX(h.cp, 101, 104), 0xFF3C3C3Cu);
    /* ---- button bottom-left arc (mirror of the top) ---- */
    CHECK_EQ(PX(h.cp, 100, 139), 0xFF101010u);
    CHECK_EQ(PX(h.cp, 103, 139), 0xFF101010u);
    CHECK_EQ(PX(h.cp, 104, 139), 0xFF555555u);
    CHECK_EQ(PX(h.cp, 100, 136), 0xFF101010u);   /* cut 1 at d=3 */
    CHECK_EQ(PX(h.cp, 101, 136), 0xFF555555u);
    CHECK_EQ(PX(h.cp, 102, 136), 0xFF3C3C3Cu);
    CHECK_EQ(PX(h.cp, 101, 138), 0xFF101010u);   /* cut 2 at d=1 */
    CHECK_EQ(PX(h.cp, 102, 138), 0xFF555555u);
    CHECK_EQ(PX(h.cp, 103, 138), 0xFF555555u);
    CHECK_EQ(PX(h.cp, 104, 138), 0xFF3C3C3Cu);
    /* ---- window top-left arc (r=8, no border) ---- */
    CHECK_EQ(PX(h.cp, 200, 100), 0xFF101010u);
    CHECK_EQ(PX(h.cp, 207, 100), 0xFF101010u);
    CHECK_EQ(PX(h.cp, 208, 100), 0xFF202020u);   /* arc top row starts x+r */
    CHECK_EQ(PX(h.cp, 209, 100), 0xFF202020u);
    CHECK_EQ(PX(h.cp, 204, 101), 0xFF101010u);   /* cut 5 at dy=1 */
    CHECK_EQ(PX(h.cp, 205, 101), 0xFF202020u);
    CHECK_EQ(PX(h.cp, 202, 102), 0xFF101010u);   /* cut 3 at dy=2 */
    CHECK_EQ(PX(h.cp, 203, 102), 0xFF202020u);
    CHECK_EQ(PX(h.cp, 201, 103), 0xFF101010u);   /* cut 2 at dy=3 */
    CHECK_EQ(PX(h.cp, 202, 103), 0xFF202020u);
    CHECK_EQ(PX(h.cp, 200, 105), 0xFF101010u);   /* cut 1 at dy=5 */
    CHECK_EQ(PX(h.cp, 201, 105), 0xFF202020u);
    CHECK_EQ(PX(h.cp, 200, 108), 0xFF202020u);   /* cut 0 at dy=8 */
    /* ---- window bottom-left arc (mirror) ---- */
    CHECK_EQ(PX(h.cp, 200, 139), 0xFF101010u);
    CHECK_EQ(PX(h.cp, 207, 139), 0xFF101010u);
    CHECK_EQ(PX(h.cp, 208, 139), 0xFF202020u);
    CHECK_EQ(PX(h.cp, 200, 132), 0xFF101010u);   /* cut 1 at d=7 */
    CHECK_EQ(PX(h.cp, 201, 132), 0xFF202020u);
    CHECK_EQ(PX(h.cp, 201, 135), 0xFF101010u);   /* cut 2 at d=4 */
    CHECK_EQ(PX(h.cp, 202, 135), 0xFF202020u);
    /* ---- panel top-left arc (r=4 by role default, fill only) ---- */
    CHECK_EQ(PX(h.cp, 300, 100), 0xFF101010u);   /* notch */
    CHECK_EQ(PX(h.cp, 304, 100), 0xFF2A2A2Au);   /* cut 4: chord x+r */
    CHECK_EQ(PX(h.cp, 302, 101), 0xFF2A2A2Au);   /* cut 2 at dy=1 */
    CHECK_EQ(PX(h.cp, 301, 102), 0xFF2A2A2Au);   /* cut 1 at dy=2 */
    CHECK_EQ(PX(h.cp, 301, 103), 0xFF2A2A2Au);   /* cut 1 at dy=3 */
    CHECK_EQ(PX(h.cp, 300, 104), 0xFF2A2A2Au);   /* cut 0: left edge */
    /* ---- radius 0 control: square corner, no notch ---- */
    CHECK_EQ(PX(h.cp, 400, 100), 0xFF3F3F3Fu);   /* border row runs to corner */
    CHECK_EQ(PX(h.cp, 400, 139), 0xFF3F3F3Fu);
    CHECK_EQ(PX(h.cp, 405, 105), 0xFF2E2E2Eu);   /* interior: square fill */
    CHECK_EQ(PX(h.cp, 405, 134), 0xFF2E2E2Eu);
    harness_free(&h);
    printf("test_comp_rounded_corner: ok\n");
}

/* TERMINAL role default: radius 0, no border, fill 0xFF0C0C0C (the
 * terminal emulator's bg_color). A window whose CONTENT node is created
 * with SCENE_ROLE_TERMINAL must paint that fill over its body area.    */
static void test_comp_terminal_role_fill(void)
{
    struct harness h;
    static const scene_rect r_term = {60, 40, 648, 168};
    const int32_t cx = 60;       /* content body (below titlebar) */
    const int32_t cy = 72;
    const int32_t cw = 648;
    const int32_t ch = 136;

    harness_init(&h);
    tickf(&h);
    /* Full window: WINDOW + TITLEBAR + CONTENT. The CONTENT node gets
     * role TERMINAL, so its body paints 0xFF0C0C0C over the WINDOW fill. */
    op_ok(&h, scene_client_create_node(h.cl, SCENE_NO_PARENT, 60,
                                       SCENE_ROLE_WINDOW, &r_term,
                                       SCENE_FLAG_VISIBLE), "term win");
    static const scene_rect r_tb = {60, 40, 648, 32};
    op_ok(&h, scene_client_create_node(h.cl, 60, 61, SCENE_ROLE_TITLEBAR,
                                       &r_tb, SCENE_FLAG_VISIBLE), "ter tb");
    static const scene_rect r_ct = {cx, cy, cw, ch};
    op_ok(&h, scene_client_create_node(h.cl, 60, 62, SCENE_ROLE_TERMINAL,
                                       &r_ct, SCENE_FLAG_VISIBLE), "ter ct");
    tickf(&h);

    /* Titlebar keeps the WINDOW role default derive (TITLEBAR 0xFF1A1A1A). */
    CHECK_EQ(PX(h.cp, 200, 50), 0xFF1A1A1Au);
    /* Body: TERMINAL role fill, not the WINDOW fill underneath. */
    CHECK_EQ(PX(h.cp, 100, 80), 0xFF0C0C0Cu);
    CHECK_EQ(PX(h.cp, 100, 200), 0xFF0C0C0Cu);
    CHECK_EQ(PX(h.cp, 600, 150), 0xFF0C0C0Cu);
    /* Right at the body edge: fill, no border (radius/bw 0). */
    CHECK_EQ(PX(h.cp, 60, 72), 0xFF0C0C0Cu);
    CHECK_EQ(PX(h.cp, 60, 207), 0xFF0C0C0Cu);
    /* Desktop (outside the window) is the clear color. */
    CHECK_EQ(PX(h.cp, 5, 5), 0xFF101010u);

    harness_free(&h);
    printf("test_comp_terminal_role_fill: ok\n");
}

/* ENTER: a new visible node fades in over 8 ticks (eff = t*255/8) while
 * sliding up from 6px below (off = (8-t)*6/8). off reaches 0 at t=7, so
 * frames 1..6 damage base+sweep (2 rects); t=7 and t=8 damage base only. */
static void test_comp_effects_enter(void)
{
    struct harness h;
    static const scene_rect r_btn = {30, 60, 100, 30};
    scene_rect d[16];
    static const uint32_t ramp[8] = {0xFF151515u, 0xFF1A1A1Au, 0xFF202020u,
                                     0xFF252525u, 0xFF2B2B2Bu, 0xFF303030u,
                                     0xFF363636u, 0xFF3C3C3Cu};
    static const int32_t slide_y[8] = {65, 64, 63, 63, 62, 61, 60, 60};
    int i;

    harness_init(&h);
    scene_compositor_set_effects(h.cp, 1);
    tickf(&h);                                 /* frame 0: empty */
    op_ok(&h, scene_client_create_node(h.cl, SCENE_NO_PARENT, 100,
                                       SCENE_ROLE_BUTTON, &r_btn,
                                       SCENE_FLAG_VISIBLE), "fx enter");
    for (i = 0; i < 8; i++) {
        tickf(&h);                             /* frames 1..8 */
        CHECK_EQ(scene_compositor_anim_count(h.cp), i < 7 ? 1 : 0);
        CHECK_EQ(scene_compositor_damage(h.cp, d, 16), i < 6 ? 2 : 1);
        CHECK_EQ(d[0].x, 30); CHECK_EQ(d[0].y, 60);   /* base rect */
        CHECK_EQ(d[0].w, 100);
        CHECK_EQ(d[0].h, i < 7 ? 30 : 36);   /* band h = base.h + SLIDE */
        if (i < 6) {
            CHECK_EQ(d[1].x, 30); CHECK_EQ(d[1].y, slide_y[i]); /* sweep */
        }
        CHECK_EQ(PX(h.cp, 50, 75), ramp[i]);     /* fill fade ramp */
        CHECK_EQ(PX(h.cp, 30, 60), 0xFF101010u); /* corner stays clear
                                                  * while the node slides */
    }
    /* fully settled: identical to the identity paint (fill + border) */
    CHECK_EQ(PX(h.cp, 50, 75), 0xFF3C3C3Cu);
    CHECK_EQ(PX(h.cp, 34, 60), 0xFF555555u);
    tickf(&h);                                 /* frame 9: no changes */
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 0);
    CHECK_EQ(scene_compositor_anim_count(h.cp), 0);
    harness_free(&h);
    printf("test_comp_effects_enter: ok\n");
}

/* EXIT: destroying a settled node keeps a fading phantom (alpha =
 * (8-t)*255/8) drawn over the live scene; 8 ticks later it is freed and
 * the next frame has zero damage. The phantom carries the border too.  */
static void test_comp_effects_exit(void)
{
    struct harness h;
    static const scene_rect r_btn = {30, 60, 100, 30};
    scene_rect d[16];
    static const uint32_t ramp[7] = {0xFF363636u, 0xFF303030u, 0xFF2B2B2Bu,
                                     0xFF252525u, 0xFF202020u, 0xFF1A1A1Au,
                                     0xFF151515u};
    static const uint32_t bramp[7] = {0xFF4C4C4Cu, 0xFF434343u, 0xFF3B3B3Bu,
                                      0xFF323232u, 0xFF292929u, 0xFF212121u,
                                      0xFF181818u};
    int i;

    harness_init(&h);
    scene_compositor_set_effects(h.cp, 1);
    tickf(&h);
    op_ok(&h, scene_client_create_node(h.cl, SCENE_NO_PARENT, 100,
                                       SCENE_ROLE_BUTTON, &r_btn,
                                       SCENE_FLAG_VISIBLE), "fx exit");
    for (i = 0; i < 8; i++) tickf(&h);         /* settle the enter fade */
    CHECK_EQ(scene_compositor_anim_count(h.cp), 0);
    CHECK_EQ(PX(h.cp, 50, 75), 0xFF3C3C3Cu);
    op_ok(&h, scene_client_destroy_node(h.cl, 100), "fx destroy");
    for (i = 0; i < 7; i++) {                  /* frames: phantom 223..31 */
        tickf(&h);
        CHECK_EQ(scene_compositor_anim_count(h.cp), 1);
        CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 1);
        CHECK_EQ(d[0].x, 30); CHECK_EQ(d[0].y, 60);
        CHECK_EQ(d[0].w, 100); CHECK_EQ(d[0].h, 30);
        CHECK_EQ(PX(h.cp, 50, 75), ramp[i]);   /* fill fade ramp */
        CHECK_EQ(PX(h.cp, 34, 60), bramp[i]);  /* border fades too */
    }
    tickf(&h);                                 /* t=8: freed */
    CHECK_EQ(scene_compositor_anim_count(h.cp), 0);
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 1);
    CHECK_EQ(PX(h.cp, 50, 75), 0xFF101010u);   /* gone: clear */
    tickf(&h);                                 /* next frame: idle */
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 0);
    CHECK_EQ(scene_compositor_anim_count(h.cp), 0);
    harness_free(&h);
    printf("test_comp_effects_exit: ok\n");
}

/* IDENTITY PROOF: with effects on, once every transition settles the
 * framebuffer is byte-identical to the effects-off paint.              */
static void test_comp_effects_identity(void)
{
    struct harness hA, hB;
    const scene_fb *fa, *fb;
    int i;

    harness_init(&hA);
    harness_init(&hB);
    scene_compositor_set_effects(hB.cp, 1);
    tickf(&hA);
    tickf(&hB);
    build_app(&hA);
    build_app(&hB);
    tickf(&hA);                                /* off: painted once */
    for (i = 0; i < 8; i++) tickf(&hB);        /* on: settles the enters */
    CHECK_EQ(scene_compositor_anim_count(hB.cp), 0);
    fa = scene_compositor_fb(hA.cp);
    fb = scene_compositor_fb(hB.cp);
    CHECK_EQ(fa->w, fb->w);
    CHECK_EQ(fa->h, fb->h);
    CHECK(memcmp(fa->px, fb->px, (size_t)fa->w * fa->h * sizeof(uint32_t))
          == 0);
    CHECK_EQ(PX(hB.cp, 50, 65), 0xFF3C3C3Cu);  /* sanity: content present */
    harness_free(&hB);
    harness_free(&hA);
    printf("test_comp_effects_identity: ok\n");
}

/* DETERMINISM: two compositors driven through the same op stream with
 * effects on must produce byte-identical framebuffers and damage after
 * every frame, including the enter and exit fades.                     */
static void test_comp_effects_determinism(void)
{
    struct harness hA, hB;
    static const scene_rect r_btn = {30, 60, 100, 30};
    scene_rect dA[16], dB[16];
    const scene_fb *fa, *fb;
    int i;

    harness_init(&hA);
    harness_init(&hB);
    scene_compositor_set_effects(hA.cp, 1);
    scene_compositor_set_effects(hB.cp, 1);
    tickf(&hA);
    tickf(&hB);
    op_ok(&hA, scene_client_create_node(hA.cl, SCENE_NO_PARENT, 100,
                                        SCENE_ROLE_BUTTON, &r_btn,
                                        SCENE_FLAG_VISIBLE), "det create");
    op_ok(&hA, scene_client_set_text(hA.cl, 100, 1, "Open", 4), "det text");
    op_ok(&hB, scene_client_create_node(hB.cl, SCENE_NO_PARENT, 100,
                                        SCENE_ROLE_BUTTON, &r_btn,
                                        SCENE_FLAG_VISIBLE), "det create");
    op_ok(&hB, scene_client_set_text(hB.cl, 100, 1, "Open", 4), "det text");
    for (i = 0; i < 8; i++) {                  /* enter fade frames */
        tickf(&hA);
        tickf(&hB);
        fa = scene_compositor_fb(hA.cp);
        fb = scene_compositor_fb(hB.cp);
        CHECK(memcmp(fa->px, fb->px, (size_t)fa->w * fa->h * sizeof(uint32_t))
              == 0);
        CHECK_EQ(scene_compositor_damage(hA.cp, dA, 16),
                 scene_compositor_damage(hB.cp, dB, 16));
        CHECK_EQ(scene_compositor_anim_count(hA.cp),
                 scene_compositor_anim_count(hB.cp));
    }
    op_ok(&hA, scene_client_destroy_node(hA.cl, 100), "det destroy");
    op_ok(&hB, scene_client_destroy_node(hB.cl, 100), "det destroy");
    for (i = 0; i < 9; i++) {                  /* exit fade frames */
        tickf(&hA);
        tickf(&hB);
        fa = scene_compositor_fb(hA.cp);
        fb = scene_compositor_fb(hB.cp);
        CHECK(memcmp(fa->px, fb->px, (size_t)fa->w * fa->h * sizeof(uint32_t))
              == 0);
        CHECK_EQ(scene_compositor_damage(hA.cp, dA, 16),
                 scene_compositor_damage(hB.cp, dB, 16));
        CHECK_EQ(scene_compositor_anim_count(hA.cp),
                 scene_compositor_anim_count(hB.cp));
    }
    CHECK_EQ(scene_compositor_anim_count(hA.cp), 0);
    CHECK_EQ(PX(hA.cp, 50, 75), 0xFF101010u);
    harness_free(&hB);
    harness_free(&hA);
    printf("test_comp_effects_determinism: ok\n");
}

/* REPLAY/GHOST: transitions never fire while seeking (replay) or
 * re-issuing (ghost reconnect): anim_count stays 0 and nodes appear and
 * disappear instantly, with no fade phantom.                            */
static void test_comp_effects_noanim_replay_ghost(void)
{
    struct harness h;
    scene_rect d[16];
    uint32_t sid;
    int i;

    harness_init(&h);
    scene_compositor_set_effects(h.cp, 1);
    tickf(&h);
    build_app(&h);
    for (i = 0; i < 8; i++) tickf(&h);         /* settle all enters */
    CHECK_EQ(scene_compositor_anim_count(h.cp), 0);
    /* destroy the label while effects are on: an exit fade starts */
    op_ok(&h, scene_client_destroy_node(h.cl, 202), "fx ghost del");
    tickf(&h);
    CHECK_EQ(scene_compositor_anim_count(h.cp), 1);
    CHECK_EQ(PX(h.cp, 40, 100), 0xFF2A2A2Au);  /* phantom fades label */
    /* replay: seeking must clear the fade and paint instantly */
    op_ok(&h, scene_client_set_input_mode(h.cl, SCENE_MODE_REPLAY),
          "fx replay mode");
    op_ok(&h, scene_client_seek(h.cl, 4), "fx seek 4");
    tickf(&h);
    CHECK_EQ(scene_compositor_anim_count(h.cp), 0);   /* cleared */
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 3); /* 201 + 204 gone + phantom cleared */
    CHECK_EQ(PX(h.cp, 190, 65), 0xFF2A2A2Au);
    CHECK_EQ(PX(h.cp, 204, 124), 0xFF2A2A2Au);
    /* forward seek repaints nodes in again, still no fade */
    op_ok(&h, scene_client_seek(h.cl, 11), "fx seek 11");
    tickf(&h);
    CHECK_EQ(scene_compositor_anim_count(h.cp), 0);
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 3); /* 201 + 202 + 204 new */
    CHECK_EQ(PX(h.cp, 190, 65), 0xFF3C3C3Cu);
    CHECK_EQ(PX(h.cp, 32, 100), 0xFFFFFFFFu);  /* label back, instantly */
    /* seek past the destroy: label disappears, still no fade */
    op_ok(&h, scene_client_seek(h.cl, 12), "fx seek 12");
    tickf(&h);
    CHECK_EQ(scene_compositor_anim_count(h.cp), 0);
    CHECK_EQ(PX(h.cp, 40, 100), 0xFF2A2A2Au);
    /* back to live, then ghost reconnect: the re-issued ops must not
     * animate either (zero diff: content identical) */
    op_ok(&h, scene_client_set_input_mode(h.cl, SCENE_MODE_LIVE),
          "fx live mode");
    tickf(&h);
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 0);
    sid = h.wel_sid;
    scene_server_detach(scene_compositor_server(h.cp));
    scene_server_attach(scene_compositor_server(h.cp));
    CHECK_EQ(scene_client_reconnect(h.cl, scene_loopback_client_end(h.lb)), 0);
    tickf(&h);
    CHECK_EQ(h.wel_sid, sid);
    CHECK_EQ(scene_compositor_anim_count(h.cp), 0);
    CHECK_EQ(scene_compositor_damage(h.cp, d, 16), 0);
    CHECK_EQ(PX(h.cp, 50, 65), 0xFF3C3C3Cu);   /* content intact */
    CHECK_EQ(PX(h.cp, 40, 100), 0xFF2A2A2Au);  /* label still destroyed */
    harness_free(&h);
    printf("test_comp_effects_noanim_replay_ghost: ok\n");
}

static void test_comp_key_input(void)
{
    struct harness h;
    static const scene_rect r = {10, 10, 100, 30};

    harness_init(&h);
    tickf(&h);

    /* Create a focusable button. */
    op_ok(&h, scene_client_create_node(h.cl, SCENE_NO_PARENT, 200,
                                       SCENE_ROLE_BUTTON, &r,
                                       SCENE_FLAG_VISIBLE |
                                       SCENE_FLAG_FOCUSABLE), "btn");
    tickf(&h);

    /* Inject a key event: Enter (key code 28), pressed, no modifiers. */
    h.key_calls = 0;
    CHECK_EQ(scene_compositor_input_key(h.cp, 28, 1, 0), 0);
    tickf(&h);

    /* Client should have received a key event. */
    CHECK_EQ(h.key_calls, 1);
    CHECK_EQ(h.key_code, 28u);
    CHECK_EQ(h.key_state, 1u);
    CHECK_EQ(h.key_mod, 0u);

    /* Ack the press, then release. */
    scene_client_ack(h.cl, h.key_seq);
    tickf(&h);

    CHECK_EQ(scene_compositor_input_key(h.cp, 28, 0, 0), 0);
    tickf(&h);
    CHECK_EQ(h.key_calls, 2);
    CHECK_EQ(h.key_state, 0u);

    /* Ack the release, then send Shift+A. */
    scene_client_ack(h.cl, h.key_seq);
    tickf(&h);

    CHECK_EQ(scene_compositor_input_key(h.cp, 30, 1,
                                         SCENE_MOD_SHIFT), 0);
    tickf(&h);
    CHECK_EQ(h.key_calls, 3);
    CHECK_EQ(h.key_code, 30u);
    CHECK_EQ(h.key_state, 1u);
    CHECK_EQ(h.key_mod & SCENE_MOD_SHIFT, SCENE_MOD_SHIFT);

    /* Ack and send another key (Alt+1). */
    scene_client_ack(h.cl, h.key_seq);
    tickf(&h);

    CHECK_EQ(scene_compositor_input_key(h.cp, 2, 1, SCENE_MOD_ALT), 0);
    tickf(&h);
    CHECK_EQ(h.key_calls, 4);

    harness_free(&h);
    printf("test_comp_key_input: ok\n");
}

static void test_comp_key_flow_control(void)
{
    struct harness h;
    harness_init(&h);
    tickf(&h);

    /* First key goes through. */
    CHECK_EQ(scene_compositor_input_key(h.cp, 28, 1, 0), 0);
    tickf(&h);
    CHECK_EQ(h.key_calls, 1);

    /* Second key is dropped (flow control: un-acked). */
    CHECK_EQ(scene_compositor_input_key(h.cp, 29, 1, 0), 0);
    tickf(&h);
    CHECK_EQ(h.key_calls, 1);   /* still 1, second was dropped */

    /* Ack the first key. */
    scene_client_ack(h.cl, h.key_seq);
    tickf(&h);

    /* Now the next key goes through. */
    CHECK_EQ(scene_compositor_input_key(h.cp, 30, 1, 0), 0);
    tickf(&h);
    CHECK_EQ(h.key_calls, 2);

    harness_free(&h);
    printf("test_comp_key_flow_control: ok\n");
}

static void test_comp_input_text_and_wheel(void)
{
    struct harness h;
    harness_init(&h);
    tickf(&h);   /* pump WELCOME: cli_emit requires welcomed=1 before ops */

    /* a focused text carrier + a textless node in the shell session */
    static const scene_rect r_lab = {100, 100, 200, 20};
    static const scene_rect r_btn = {100, 140, 100, 30};
    op_ok(&h, scene_client_create_node(h.cl, SCENE_NO_PARENT, 301,
                                       SCENE_ROLE_LABEL, &r_lab,
                                       SCENE_FLAG_VISIBLE |
                                       SCENE_FLAG_FOCUSABLE), "create 301");
    op_ok(&h, scene_client_set_text(h.cl, 301, 1, "clip this", 9), "text 301");
    op_ok(&h, scene_client_create_node(h.cl, SCENE_NO_PARENT, 302,
                                       SCENE_ROLE_BUTTON, &r_btn,
                                       SCENE_FLAG_VISIBLE |
                                       SCENE_FLAG_FOCUSABLE), "create 302");
    tickf(&h);
    CHECK_EQ(h.key_calls, 0);
    CHECK_EQ(h.txt_calls, 0);

    /* wheel-only record: delivered with the transient bit, no activate */
    CHECK_EQ(scene_compositor_input_pointer(h.cp, 0, 120, 110,
                                            SCENE_BTN_WHEEL_UP), 0);
    tickf(&h);
    CHECK_EQ(h.ptr_calls, 1);
    CHECK_EQ(h.ptr_btn[0], SCENE_BTN_WHEEL_UP);
    CHECK_EQ(h.act_calls, 0);
    /* flow control: the next wheel drops until the ack */
    CHECK_EQ(scene_compositor_input_pointer(h.cp, 0, 120, 110,
                                            SCENE_BTN_WHEEL_DOWN), 0);
    tickf(&h);
    CHECK_EQ(h.ptr_calls, 1);
    scene_client_ack(h.cl, h.ptr_seq[0]);
    tickf(&h);   /* ack reaches the server: gate reopens */
    CHECK_EQ(scene_compositor_input_pointer(h.cp, 0, 120, 110,
                                            SCENE_BTN_WHEEL_DOWN), 0);
    tickf(&h);
    CHECK_EQ(h.ptr_calls, 2);
    CHECK_EQ(h.ptr_btn[1], SCENE_BTN_WHEEL_DOWN);
    CHECK_EQ(h.act_calls, 0);
    scene_client_ack(h.cl, h.ptr_seq[1]);

    /* Super+C on the focused text node feeds the OS clipboard */
    CHECK_EQ(scene_store_host_focus(scene_compositor_store(h.cp), 301), 0);
    tickf(&h);
    CHECK_EQ(scene_compositor_input_key(h.cp, SCENE_KEY_C, 1,
                                        SCENE_MOD_SUPER), 0);
    tickf(&h);
    CHECK_EQ(h.key_calls, 0);   /* the chord never reaches the app */
    CHECK(scene_compositor_clipboard(h.cp) != NULL);
    CHECK_EQ(scene_compositor_clipboard_len(h.cp), 9);
    CHECK(strcmp(scene_compositor_clipboard(h.cp), "clip this") == 0);

    /* Super+V -> INPUT_TEXT into the focused session */
    CHECK_EQ(scene_compositor_input_key(h.cp, SCENE_KEY_V, 1,
                                        SCENE_MOD_SUPER), 0);
    tickf(&h);
    CHECK_EQ(h.txt_calls, 1);
    CHECK_EQ(h.txt_len, 9);
    CHECK(strcmp(h.txt_buf, "clip this") == 0);
    /* the text record holds the gate: the next paste is dropped */
    CHECK_EQ(scene_compositor_input_key(h.cp, SCENE_KEY_V, 1,
                                        SCENE_MOD_SUPER), 0);
    tickf(&h);
    CHECK_EQ(h.txt_calls, 1);
    scene_client_ack(h.cl, h.txt_seq);
    tickf(&h);   /* ack reaches the server: gate reopens */
    CHECK_EQ(scene_compositor_input_key(h.cp, SCENE_KEY_V, 1,
                                        SCENE_MOD_SUPER), 0);
    tickf(&h);
    CHECK_EQ(h.txt_calls, 2);

    /* textless focus: copy captures nothing, clipboard keeps its value */
    CHECK_EQ(scene_store_host_focus(scene_compositor_store(h.cp), 302), 0);
    tickf(&h);
    CHECK_EQ(scene_compositor_input_key(h.cp, SCENE_KEY_C, 1,
                                        SCENE_MOD_SUPER), 0);
    CHECK_EQ(scene_compositor_clipboard_len(h.cp), 9);

    /* programmatic clipboard + paste */
    scene_compositor_clipboard_set(h.cp, "prog", 4);
    CHECK_EQ(scene_compositor_clipboard_len(h.cp), 4);
    scene_client_ack(h.cl, h.txt_seq);
    tickf(&h);   /* ack reaches the server: gate reopens */
    CHECK_EQ(scene_compositor_input_key(h.cp, SCENE_KEY_V, 1,
                                        SCENE_MOD_SUPER), 0);
    tickf(&h);
    CHECK_EQ(h.txt_calls, 3);
    CHECK(strcmp(h.txt_buf, "prog") == 0);

    harness_free(&h);
    printf("test_comp_input_text_and_wheel: ok\n");
}

/* OS toast source: NOTIFICATION-role nodes on app layers (>= 1) raise
 * the cb once per NEW signature — create with text, later text change,
 * empty text on create, destroy+recreate. Layer 0 (the shell session)
 * never fires; other roles never fire; no callback = inert.           */
static void test_comp_notify(void)
{
    struct nh h;
    static const scene_rect r_toast = {10, 10, 200, 50};
    static const scene_rect r_btn = {10, 70, 100, 30};

    nh_init(&h);
    scene_compositor_set_notify_cb(h.cp, cb_nf, (void *)&h);
    CHECK_EQ(h.nf_count, 0);        /* handshake frame: nothing yet */

    /* (a) notification WITH text in the app layer: exactly one fire */
    op_ok(&h.ap_ev, scene_client_create_node(h.cl1, SCENE_NO_PARENT, 500,
                                             SCENE_ROLE_NOTIFICATION,
                                             &r_toast, SCENE_FLAG_VISIBLE),
          "nf create 500");
    op_ok(&h.ap_ev, scene_client_set_text(h.cl1, 500, 1, "New message", 12),
          "nf text 500");
    ntick(&h);
    CHECK_EQ(h.nf_count, 1);
    CHECK_EQ(h.nf_layer[0], 1);
    CHECK_EQ(h.nf_id[0], 500);
    CHECK_EQ(h.nf_len[0], 12);
    CHECK(memcmp(h.nf_text[0], "New message", 12) == 0);

    /* (b) unchanged frame: no re-fire */
    ntick(&h);
    CHECK_EQ(h.nf_count, 1);

    /* (c) text change: exactly one new fire with the new text */
    op_ok(&h.ap_ev, scene_client_set_text(h.cl1, 500, 1, "Message 2", 9),
          "nf retitle 500");
    ntick(&h);
    CHECK_EQ(h.nf_count, 2);
    CHECK_EQ(h.nf_layer[1], 1);
    CHECK_EQ(h.nf_id[1], 500);
    CHECK_EQ(h.nf_len[1], 9);
    CHECK(memcmp(h.nf_text[1], "Message 2", 9) == 0);

    /* (d) a BUTTON with text in the app layer: never fires */
    op_ok(&h.ap_ev, scene_client_create_node(h.cl1, SCENE_NO_PARENT, 501,
                                             SCENE_ROLE_BUTTON, &r_btn,
                                             SCENE_FLAG_VISIBLE),
          "nf btn 501");
    op_ok(&h.ap_ev, scene_client_set_text(h.cl1, 501, 1, "Press", 5),
          "nf btn text");
    ntick(&h);
    CHECK_EQ(h.nf_count, 2);

    /* (e) a NOTIFICATION in layer 0 (the shell session): never fires,
     * not on create, not on text change (the shell's own toast nodes
     * must not loop) */
    op_ok(&h.sh_ev, scene_client_create_node(h.cl0, SCENE_NO_PARENT, 900,
                                             SCENE_ROLE_NOTIFICATION,
                                             &r_toast, SCENE_FLAG_VISIBLE),
          "nf shell 900");
    op_ok(&h.sh_ev, scene_client_set_text(h.cl0, 900, 1, "shell toast", 11),
          "nf shell text");
    ntick(&h);
    CHECK_EQ(h.nf_count, 2);
    op_ok(&h.sh_ev, scene_client_set_text(h.cl0, 900, 1, "changed", 7),
          "nf shell retitle");
    ntick(&h);
    CHECK_EQ(h.nf_count, 2);

    /* (f) notification created with EMPTY text fires once, len 0; text
     * set in a later batch fires again with the new text */
    op_ok(&h.ap_ev, scene_client_create_node(h.cl1, SCENE_NO_PARENT, 502,
                                             SCENE_ROLE_NOTIFICATION,
                                             &r_toast, SCENE_FLAG_VISIBLE),
          "nf empty 502");
    ntick(&h);
    CHECK_EQ(h.nf_count, 3);
    CHECK_EQ(h.nf_id[2], 502);
    CHECK_EQ(h.nf_len[2], 0);
    op_ok(&h.ap_ev, scene_client_set_text(h.cl1, 502, 1, "Late", 4),
          "nf late text");
    ntick(&h);
    CHECK_EQ(h.nf_count, 4);
    CHECK_EQ(h.nf_id[3], 502);
    CHECK_EQ(h.nf_len[3], 4);
    CHECK(memcmp(h.nf_text[3], "Late", 4) == 0);

    /* (g) destroy + recreate: the fresh model entry is a new signature */
    op_ok(&h.ap_ev, scene_client_destroy_node(h.cl1, 500), "nf destroy 500");
    ntick(&h);
    CHECK_EQ(h.nf_count, 4);
    op_ok(&h.ap_ev, scene_client_create_node(h.cl1, SCENE_NO_PARENT, 503,
                                             SCENE_ROLE_NOTIFICATION,
                                             &r_toast, SCENE_FLAG_VISIBLE),
          "nf recreate 503");
    op_ok(&h.ap_ev, scene_client_set_text(h.cl1, 503, 1, "Again", 5),
          "nf recreate text");
    ntick(&h);
    CHECK_EQ(h.nf_count, 5);
    CHECK_EQ(h.nf_layer[4], 1);
    CHECK_EQ(h.nf_id[4], 503);
    CHECK(memcmp(h.nf_text[4], "Again", 5) == 0);

    /* no callback registered: the feature is inert */
    scene_compositor_set_notify_cb(h.cp, NULL, NULL);
    op_ok(&h.ap_ev, scene_client_set_text(h.cl1, 503, 1, "Silent", 6),
          "nf silent");
    ntick(&h);
    CHECK_EQ(h.nf_count, 5);

    nh_free(&h);
    printf("test_comp_notify: ok\n");
}

/* Screenshot service: the last committed frame is written as a 24-bit
 * BGR BMP (bottom-up rows, 4-byte stride, zeroed padding). The file is
 * read back and every header field plus the full pixel area is asserted
 * byte-exact; the top-left pixel's channels are probed by hand (clear
 * 0xFF112233 -> bytes 0x33, 0x22, 0x11) so a channel swap can never be
 * self-consistent between writer and verifier.                        */
static void test_comp_capture_bmp(void)
{
    struct harness h;
    static const scene_rect r_win = {10, 10, 40, 40};
    const scene_fb *fb;
    uint8_t *file = NULL;
    uint8_t *want = NULL;
    uint32_t row_bytes, fsize, y, x;
    long sz;
    FILE *f;

    harness_init(&h);
    tickf(&h);
    /* distinct-channel clear + odd width (800*3 % 4 == 0, 801*3 % 4 == 3:
     * the padded-stride path is exercised) */
    scene_compositor_set_clear(h.cp, 0xFF112233u);
    scene_compositor_resize(h.cp, 801, 600);
    tickf(&h);
    CHECK_EQ(PX(h.cp, 0, 0), 0xFF112233u);
    CHECK_EQ(PX(h.cp, 800, 599), 0xFF112233u);
    /* one window so rows differ */
    op_ok(&h, scene_client_create_node(h.cl, SCENE_NO_PARENT, 100,
                                       SCENE_ROLE_WINDOW, &r_win,
                                       SCENE_FLAG_VISIBLE), "cap win");
    tickf(&h);
    CHECK_EQ(PX(h.cp, 20, 20), 0xFF202020u);
    CHECK_EQ(PX(h.cp, 0, 0), 0xFF112233u);

    /* parameter / I/O failure paths */
    CHECK_EQ(scene_compositor_capture_bmp(NULL, "build/shot_comp_test.bmp"),
             -1);
    CHECK_EQ(scene_compositor_capture_bmp(h.cp, NULL), -1);
    CHECK_EQ(scene_compositor_capture_bmp(h.cp, "build/no_dir_zz/s.bmp"),
             -1);

    fb = scene_compositor_fb(h.cp);
    row_bytes = ((fb->w * 3u + 3u) / 4u) * 4u;
    fsize = 54u + row_bytes * fb->h;
    CHECK_EQ(fb->w * 3u % 4u, 3u);   /* the padding path is live */
    CHECK_EQ(scene_compositor_capture_bmp(h.cp, "build/shot_comp_test.bmp"),
             0);

    f = fopen("build/shot_comp_test.bmp", "rb");
    CHECK(f != NULL);
    if (!f) goto done;
    if (fseek(f, 0, SEEK_END) != 0) goto done;
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    CHECK_EQ((uint64_t)sz, (uint64_t)fsize);
    if (sz != (long)fsize) {
        fclose(f);
        goto done;
    }
    file = malloc((size_t)fsize);
    CHECK(file != NULL);
    if (!file) {
        fclose(f);
        goto done;
    }
    CHECK_EQ(fread(file, 1, (size_t)fsize, f), (size_t)fsize);
    fclose(f);

    /* 14-byte BITMAPFILEHEADER */
    CHECK_EQ(file[0], 'B');
    CHECK_EQ(file[1], 'M');
    CHECK_EQ(scene_get_u32(file + 2), fsize);       /* bfSize */
    CHECK_EQ(scene_get_u16(file + 6), 0);           /* bfReserved1 */
    CHECK_EQ(scene_get_u16(file + 8), 0);           /* bfReserved2 */
    CHECK_EQ(scene_get_u32(file + 10), 54);         /* bfOffBits */
    /* 40-byte BITMAPINFOHEADER */
    CHECK_EQ(scene_get_u32(file + 14), 40);         /* biSize */
    CHECK_EQ(scene_get_u32(file + 18), fb->w);      /* biWidth */
    CHECK_EQ(scene_get_u32(file + 22), fb->h);      /* biHeight: bottom-up */
    CHECK_EQ(scene_get_u16(file + 26), 1);          /* biPlanes */
    CHECK_EQ(scene_get_u16(file + 28), 24);         /* biBitCount */
    CHECK_EQ(scene_get_u32(file + 30), 0);          /* biCompression */
    CHECK_EQ(scene_get_u32(file + 34), row_bytes * fb->h); /* biSizeImage */
    CHECK_EQ(scene_get_u32(file + 38), 0);          /* biXPelsPerMeter */
    CHECK_EQ(scene_get_u32(file + 42), 0);          /* biYPelsPerMeter */
    CHECK_EQ(scene_get_u32(file + 46), 0);          /* biClrUsed */
    CHECK_EQ(scene_get_u32(file + 50), 0);          /* biClrImportant */

    /* hand-probed channels: fb (0,0) = 0xFF112233 sits at the BOTTOM
     * image row (biHeight positive = bottom-up) as bytes 0x33,0x22,0x11 */
    {
        uint32_t off = (fb->h - 1u) * row_bytes;
        CHECK_EQ(file[54u + off + 0], 0x33u);
        CHECK_EQ(file[54u + off + 1], 0x22u);
        CHECK_EQ(file[54u + off + 2], 0x11u);
    }
    /* window interior pixel (20,20) = 0xFF202020 in its image row */
    {
        uint32_t off = (fb->h - 1u - 20u) * row_bytes + 20u * 3u;
        CHECK_EQ(file[54u + off + 0], 0x20u);
        CHECK_EQ(file[54u + off + 1], 0x20u);
        CHECK_EQ(file[54u + off + 2], 0x20u);
    }

    /* full pixel-area scan: bottom-up rows, BGR bytes, zeroed padding */
    want = malloc((size_t)row_bytes * fb->h);
    CHECK(want != NULL);
    if (!want) {
        free(file);
        goto done;
    }
    for (y = 0; y < fb->h; y++) {
        const uint32_t *src = fb->px + (fb->h - 1u - y) * fb->pitch;
        uint8_t *p = want + (size_t)y * row_bytes;
        for (x = 0; x < fb->w; x++) {
            uint32_t px = src[x];
            p[0] = (uint8_t)(px & 0xFFu);         /* B */
            p[1] = (uint8_t)((px >> 8) & 0xFFu);  /* G */
            p[2] = (uint8_t)((px >> 16) & 0xFFu); /* R */
            p += 3;
        }
        if (fb->w * 3u < row_bytes)
            memset(p, 0, row_bytes - fb->w * 3u);
    }
    CHECK(memcmp(file + 54, want, (size_t)row_bytes * fb->h) == 0);
    /* one odd-width row's stride padding byte, probed directly */
    CHECK_EQ(want[1u * row_bytes + fb->w * 3u], 0);

    free(want);
    free(file);
done:
    harness_free(&h);
    printf("test_comp_capture_bmp: ok\n");
}

int main(void)
{
    test_comp_empty_and_force();
    test_comp_build_paint();
    test_comp_move_text_visibility();
    test_comp_retheme();
    test_comp_texture();
    test_comp_destroy_ghost_replay();
    test_comp_input_flow();
    test_comp_node_vis_ground_truth();
    test_comp_rounded_corner();
    test_comp_terminal_role_fill();
    test_comp_effects_enter();
    test_comp_effects_exit();
    test_comp_effects_identity();
    test_comp_effects_determinism();
    test_comp_effects_noanim_replay_ghost();
    test_comp_key_input();
    test_comp_key_flow_control();
    test_comp_input_text_and_wheel();
    test_comp_notify();
    test_comp_capture_bmp();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
