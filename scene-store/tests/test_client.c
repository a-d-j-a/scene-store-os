/*
 * test_client.c — integration tests for the reference wire client and
 * the server adapter (the compositor seam), run end-to-end through the
 * loopback transport, plus one threaded TCP round-trip.
 *
 * These tests exercise the locked v0 protocol over real framed bytes:
 * welcome, typed ops with seq management, present/ack flow control,
 * snapshot/search/capture replies, focus and text-index events, fatal
 * error paths, ghost reconnect (same session) and fresh-session rebuild,
 * replay over the wire, and macros over the wire.
 */
#include "scene_client.h"
#include "scene_server.h"
#include "scene_transport.h"
#include "scene_fb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
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

/* ---- captured events ---------------------------------------------------- */

#define MAXE 16

struct harness {
    scene_loopback *lb;
    scene_transport *server_ts;
    scene_client *cl;
    scene_server *sv;

    int wel_called; uint32_t wel_sid; uint16_t wel_ver; scene_limits wel_lim;
    int err_called; uint16_t err_code; char err_msg[256];
    int snap_called; uint32_t snap_req;
    uint8_t *snap_payload; uint32_t snap_plen;
    int cap_called; uint32_t cap_req; uint64_t cap_seq;
    uint8_t *cap_payload; uint32_t cap_plen;
    int sr_calls; uint32_t sr_req; uint32_t sr_count; scene_search_hit sr_hits[MAXE];
    int pong_called; uint64_t pong_nonce;
    int ptr_calls; uint64_t ptr_seq[MAXE]; uint8_t ptr_dev[MAXE];
    int32_t ptr_xy[MAXE][2]; uint8_t ptr_btn[MAXE];
    int act_calls; uint64_t act_seq[MAXE]; uint32_t act_id[MAXE];
    int foc_calls; uint64_t foc_seq[MAXE]; uint32_t foc_id[MAXE]; uint8_t foc_state[MAXE];
    int pd_calls; uint64_t pd_seq[MAXE], pd_token[MAXE], pd_lat[MAXE];
    int ti_calls; int ti_total; scene_text_hit ti[64];
    int imp_calls; scene_texture_ref imp_ref; uint8_t imp_ok;
    int imp_cb_calls; scene_texture_ref imp_cb_ref; char imp_cb_path[256];
    int closed_calls;
};

static void cb_welcome(void *ud, uint32_t sid, uint16_t ver,
                       const scene_limits *lim)
{
    struct harness *h = (struct harness *)ud;
    h->wel_called++;
    h->wel_sid = sid;
    h->wel_ver = ver;
    h->wel_lim = *lim;
}

static void cb_error(void *ud, uint16_t code, const char *msg, uint32_t len)
{
    struct harness *h = (struct harness *)ud;
    h->err_called++;
    h->err_code = code;
    if (len > sizeof(h->err_msg) - 1) len = (uint32_t)sizeof(h->err_msg) - 1;
    memcpy(h->err_msg, msg, len);
    h->err_msg[len] = '\0';
}

static void cb_snapshot(void *ud, uint32_t req, const uint8_t *p, uint32_t plen)
{
    struct harness *h = (struct harness *)ud;
    h->snap_called++;
    h->snap_req = req;
    free(h->snap_payload);
    h->snap_payload = (uint8_t *)malloc(plen ? plen : 1);
    if (h->snap_payload) {
        if (plen) memcpy(h->snap_payload, p, plen);
        h->snap_plen = plen;
    } else {
        h->snap_plen = 0;
    }
}

static void cb_capture(void *ud, uint32_t req, uint64_t seq,
                       const uint8_t *p, uint32_t plen)
{
    struct harness *h = (struct harness *)ud;
    h->cap_called++;
    h->cap_req = req;
    h->cap_seq = seq;
    free(h->cap_payload);
    h->cap_payload = (uint8_t *)malloc(plen ? plen : 1);
    if (h->cap_payload) {
        if (plen) memcpy(h->cap_payload, p, plen);
        h->cap_plen = plen;
    } else {
        h->cap_plen = 0;
    }
}

static void cb_search_result(void *ud, uint32_t req, uint32_t count,
                             const scene_search_hit *hits)
{
    struct harness *h = (struct harness *)ud;
    h->sr_calls++;
    h->sr_req = req;
    h->sr_count = count;
    if (count > MAXE) count = MAXE;
    uint32_t i;
    for (i = 0; i < count; i++) h->sr_hits[i] = hits[i];
}

static void cb_pong(void *ud, uint64_t nonce)
{
    struct harness *h = (struct harness *)ud;
    h->pong_called++;
    h->pong_nonce = nonce;
}

static void cb_input_pointer(void *ud, uint64_t seq, uint8_t device,
                             int32_t x, int32_t y, uint8_t buttons)
{
    struct harness *h = (struct harness *)ud;
    if (h->ptr_calls < MAXE) {
        h->ptr_seq[h->ptr_calls] = seq;
        h->ptr_dev[h->ptr_calls] = device;
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

static void cb_input_focus(void *ud, uint64_t seq, scene_node_id id,
                           uint8_t state)
{
    struct harness *h = (struct harness *)ud;
    if (h->foc_calls < MAXE) {
        h->foc_seq[h->foc_calls] = seq;
        h->foc_id[h->foc_calls] = id;
        h->foc_state[h->foc_calls] = state;
    }
    h->foc_calls++;
}

static void cb_present_done(void *ud, uint64_t seq, uint64_t token,
                            uint64_t latency_us)
{
    struct harness *h = (struct harness *)ud;
    if (h->pd_calls < MAXE) {
        h->pd_seq[h->pd_calls] = seq;
        h->pd_token[h->pd_calls] = token;
        h->pd_lat[h->pd_calls] = latency_us;
    }
    h->pd_calls++;
}

static void cb_text_index(void *ud, const scene_text_hit *entries,
                          uint32_t count)
{
    struct harness *h = (struct harness *)ud;
    h->ti_calls++;
    uint32_t i;
    for (i = 0; i < count; i++) {
        if (h->ti_total < 64) h->ti[h->ti_total++] = entries[i];
    }
}

static void cb_closed(void *ud)
{
    struct harness *h = (struct harness *)ud;
    h->closed_calls++;
}

static void cb_import_result(void *ud, scene_texture_ref ref, uint8_t ok)
{
    struct harness *h = (struct harness *)ud;
    h->imp_ref = ref;
    h->imp_ok = ok;
    h->imp_calls++;
}

static void cb_input_key(void *ud, uint64_t seq, uint32_t key_code,
                          uint8_t state, uint8_t modifiers)
{
    (void)ud; (void)seq; (void)key_code; (void)state; (void)modifiers;
}

static const scene_client_cbs g_cbs = {
    cb_welcome, cb_error, cb_snapshot, cb_search_result, cb_capture,
    cb_pong, cb_input_pointer, cb_input_activate, cb_input_focus,
    cb_input_key, cb_present_done, cb_text_index, cb_import_result, cb_closed
};

/* ---- harness plumbing --------------------------------------------------- */

static void harness_init(struct harness *h)
{
    memset(h, 0, sizeof(*h));
    h->lb = scene_loopback_new();
    h->server_ts = scene_loopback_server_end(h->lb);
    h->cl = scene_client_new();
    h->sv = scene_server_new(NULL);
    scene_server_attach(h->sv);
    CHECK_EQ(scene_client_connect(h->cl, scene_loopback_client_end(h->lb),
                                  "loopback", &g_cbs, h), 0);
}

static void harness_free(struct harness *h)
{
    scene_client_free(h->cl);
    scene_server_free(h->sv);
    scene_transport_close(h->server_ts);
    scene_loopback_free(h->lb);
    free(h->snap_payload);
    free(h->cap_payload);
}

/* Drive both ends of the loopback link for a few rounds: client flush ->
 * server feed -> server outbound drain -> client pump. Enough rounds for
 * op -> reply -> ack -> nothing. */
static void tick2(struct harness *h, scene_transport *ts, scene_server *sv)
{
    int round;
    for (round = 0; round < 4; round++) {
        scene_client_flush(h->cl);
        uint8_t buf[8192];
        uint32_t got;
        while (scene_transport_recv(ts, buf, sizeof(buf), &got) == 0 && got) {
            if (scene_server_feed(sv, buf, got) != 0) break;
        }
        const uint8_t *f;
        uint32_t flen;
        while (scene_server_out_next_frame(sv, &f, &flen) == 1)
            scene_transport_send(ts, f, flen);
        scene_client_pump(h->cl);
    }
}

static void tick(struct harness *h)
{
    tick2(h, h->server_ts, h->sv);
}

/* ---- scene builders over the wire ---------------------------------------- */

static void op_ok(struct harness *h, int r, const char *what)
{
    (void)h;
    if (r != 0) {
        failures++;
        printf("FAIL %s: op %s returned %d\n", __FILE__, what, r);
    }
    checks++;
}

/* Deterministic app scene through the client (11 scene ops, scene_seq 11). */
static void build_app_client(struct harness *h)
{
    static const scene_rect r_win = {0, 0, 800, 600};
    static const scene_rect r_tb  = {0, 0, 800, 30};
    static const scene_rect r_pan = {0, 30, 800, 570};
    static const scene_rect r_b1  = {10, 40, 100, 30};
    static const scene_rect r_b2  = {120, 40, 100, 30};
    static const scene_rect r_b3  = {230, 40, 100, 30};
    op_ok(h, scene_client_create_node(h->cl, SCENE_NO_PARENT, 100,
                                      SCENE_ROLE_WINDOW, &r_win,
                                      SCENE_FLAG_VISIBLE), "create 100");
    op_ok(h, scene_client_create_node(h->cl, 100, 101, SCENE_ROLE_TITLEBAR,
                                      &r_tb, SCENE_FLAG_VISIBLE), "create 101");
    op_ok(h, scene_client_set_text(h->cl, 101, 1, "My App", 6), "text 101");
    op_ok(h, scene_client_create_node(h->cl, 100, 102, SCENE_ROLE_PANEL,
                                      &r_pan, SCENE_FLAG_VISIBLE), "create 102");
    op_ok(h, scene_client_create_node(h->cl, 102, 200, SCENE_ROLE_BUTTON,
                                      &r_b1, SCENE_FLAG_VISIBLE |
                                      SCENE_FLAG_FOCUSABLE), "create 200");
    op_ok(h, scene_client_set_text(h->cl, 200, 1, "Open", 4), "text 200");
    op_ok(h, scene_client_create_node(h->cl, 102, 201, SCENE_ROLE_BUTTON,
                                      &r_b2, SCENE_FLAG_VISIBLE |
                                      SCENE_FLAG_FOCUSABLE), "create 201");
    op_ok(h, scene_client_set_text(h->cl, 201, 1, "Save", 4), "text 201");
    op_ok(h, scene_client_create_node(h->cl, 102, 202, SCENE_ROLE_BUTTON,
                                      &r_b3, SCENE_FLAG_VISIBLE |
                                      SCENE_FLAG_FOCUSABLE), "create 202");
    op_ok(h, scene_client_set_text(h->cl, 202, 1, "Quit", 4), "text 202");
    op_ok(h, scene_client_set_rect(h->cl, 201, &r_b2), "rect 201");
}

/* ---- raw frame builder (for protocol-error injection) -------------------- */

static uint8_t *mk_frame(uint16_t opcode, const uint8_t *payload,
                         uint32_t plen, uint32_t *out_len)
{
    uint32_t total = SCENE_HEADER_SIZE + plen;
    uint8_t *f = (uint8_t *)malloc(total);
    scene_put_u32(f + 0, SCENE_MAGIC);
    scene_put_u16(f + 4, SCENE_PROTOCOL_V0);
    scene_put_u16(f + 6, opcode);
    scene_put_u32(f + 8, plen);
    memset(f + 12, 0, 4);
    if (plen) memcpy(f + SCENE_HEADER_SIZE, payload, plen);
    scene_put_u32(f + 12, scene_fnv1a32(f, SCENE_HEADER_SIZE + plen));
    *out_len = total;
    return f;
}

/* ==================================================================== */
/* Tests                                                                  */
/* ==================================================================== */

static void test_wire_welcome_and_ping(void)
{
    struct harness h;
    harness_init(&h);
    tick(&h);
    CHECK_EQ(h.wel_called, 1);
    CHECK(h.wel_sid != 0);
    CHECK_EQ(h.wel_ver, SCENE_PROTOCOL_V0);
    CHECK_EQ(h.wel_lim.max_nodes_per_session, SCENE_DEFAULT_NODES);
    CHECK_EQ(h.wel_lim.max_text_bytes_per_slot, SCENE_DEFAULT_TEXT_BYTES);
    CHECK_EQ(h.wel_lim.max_text_slots_per_node, SCENE_DEFAULT_TEXT_SLOTS);
    CHECK_EQ(h.wel_lim.max_record_length, SCENE_DEFAULT_RECORD_LENGTH);
    CHECK_EQ(h.wel_lim.input_latency_budget_us, SCENE_DEFAULT_LATENCY_US);
    CHECK_EQ(scene_client_scene_id(h.cl), h.wel_sid);
    CHECK_EQ(scene_client_next_seq(h.cl), 1);
    /* ping round trip */
    scene_client_ping(h.cl, UINT64_C(0x1122334455667788));
    tick(&h);
    CHECK_EQ(h.pong_called, 1);
    CHECK_EQ(h.pong_nonce, UINT64_C(0x1122334455667788));
    harness_free(&h);
    printf("test_wire_welcome_and_ping: ok\n");
}

static void test_wire_build_present_snapshot(void)
{
    struct harness h;
    harness_init(&h);
    tick(&h);
    build_app_client(&h);
    scene_client_present(h.cl, 7);
    tick(&h);
    CHECK_EQ(h.pd_calls, 1);
    CHECK_EQ(h.pd_seq[0], 11);       /* scene_seq at commit time */
    CHECK_EQ(h.pd_token[0], 7);
    /* snapshot round trip */
    scene_client_snapshot(h.cl, 9);
    tick(&h);
    CHECK_EQ(h.snap_called, 1);
    CHECK_EQ(h.snap_req, 9);
    uint64_t seq; uint32_t texc, texts_used;
    scene_snapshot_node nodes[16];
    scene_snapshot_text tbuf[32];
    scene_texture_ref texes[4];
    int n = scene_snapshot_parse(h.snap_payload, h.snap_plen, &seq, &texc,
                                 &texts_used, nodes, 16, tbuf, 32, texes, 4);
    CHECK_EQ(n, 6);
    CHECK_EQ(seq, 11);
    CHECK_EQ(texc, 0);
    CHECK_EQ(texts_used, 4);
    /* document order: 100, 101, 102, 200, 201, 202 */
    CHECK_EQ(nodes[0].id, 100);
    CHECK_EQ(nodes[0].parent, SCENE_NO_PARENT);   /* root */
    CHECK_EQ(nodes[1].id, 101);
    CHECK_EQ(nodes[1].parent, 100);
    CHECK_EQ(nodes[1].role, SCENE_ROLE_TITLEBAR);
    CHECK_EQ(nodes[2].id, 102);
    CHECK_EQ(nodes[2].parent, 100);
    CHECK_EQ(nodes[3].id, 200);
    CHECK_EQ(nodes[3].parent, 102);
    CHECK_EQ(nodes[3].role, SCENE_ROLE_BUTTON);
    CHECK_EQ(nodes[4].id, 201);
    CHECK_EQ(nodes[4].parent, 102);               /* dump_node parent fix */
    CHECK_EQ(nodes[4].rect.x, 120);
    CHECK_EQ(nodes[4].rect.y, 40);
    CHECK_EQ(nodes[4].rect.w, 100);
    CHECK_EQ(nodes[4].rect.h, 30);
    CHECK_EQ(nodes[5].id, 202);
    CHECK_EQ(nodes[5].parent, 102);
    /* text of node 201 (its single slot, index 1 in the flat buffer) */
    CHECK_EQ(nodes[4].text_count, 1);
    CHECK_EQ(nodes[4].texts[0].len, 4);
    CHECK(memcmp(nodes[4].texts[0].data, "Save", 4) == 0);
    CHECK_EQ(nodes[1].texts[0].len, 6);
    CHECK(memcmp(nodes[1].texts[0].data, "My App", 6) == 0);
    /* server-side ground truth */
    CHECK_EQ(scene_store_node_count(scene_server_store(h.sv)), 6);
    CHECK_EQ(scene_store_committed_seq(scene_server_store(h.sv)), 11);
    harness_free(&h);
    printf("test_wire_build_present_snapshot: ok\n");
}

static void test_wire_search_capture_textindex(void)
{
    struct harness h;
    harness_init(&h);
    tick(&h);
    build_app_client(&h);
    tick(&h);
    /* four SetText ops -> four TEXT_INDEX records during the build */
    CHECK_EQ(h.ti_calls, 4);
    CHECK_EQ(h.ti_total, 4);
    /* text_index entry contents (order of SetText calls) */
    CHECK_EQ(h.ti[0].node_id, 101);
    CHECK_EQ(h.ti[0].len, 6);
    CHECK(memcmp(h.ti[0].data, "My App", 6) == 0);
    CHECK_EQ(h.ti[1].node_id, 200);
    CHECK(memcmp(h.ti[1].data, "Open", 4) == 0);
    /* search round trip */
    scene_client_search(h.cl, 3, "save", 4);
    tick(&h);
    CHECK_EQ(h.sr_calls, 1);
    CHECK_EQ(h.sr_req, 3);
    CHECK_EQ(h.sr_count, 1);
    CHECK_EQ(h.sr_hits[0].id, 201);
    CHECK_EQ(h.sr_hits[0].role, SCENE_ROLE_BUTTON);
    CHECK_EQ(h.sr_hits[0].rect.x, 120);
    CHECK_EQ(h.sr_hits[0].rect.h, 30);
    CHECK_EQ(h.sr_hits[0].text_id, 1);
    /* capture round trip */
    scene_client_capture(h.cl, 5);
    tick(&h);
    CHECK_EQ(h.cap_called, 1);
    CHECK_EQ(h.cap_req, 5);
    CHECK_EQ(h.cap_seq, 11);
    uint64_t seq; uint32_t texc, texts_used;
    scene_snapshot_node nodes[16];
    scene_snapshot_text tbuf[32];
    scene_texture_ref texes[4];
    int n = scene_snapshot_parse(h.cap_payload, h.cap_plen, &seq, &texc,
                                 &texts_used, nodes, 16, tbuf, 32, texes, 4);
    CHECK_EQ(n, 6);
    CHECK_EQ(seq, 11);
    harness_free(&h);
    printf("test_wire_search_capture_textindex: ok\n");
}

static void test_wire_input_flow_and_activation(void)
{
    struct harness h;
    harness_init(&h);
    tick(&h);
    build_app_client(&h);
    scene_client_present(h.cl, 1);
    tick(&h);
    CHECK_EQ(h.pd_calls, 1);
    /* compositor injects a press over button 201 */
    CHECK_EQ(scene_server_input_pointer(h.sv, 0, 150, 55, 0x01), 0);
    tick(&h);
    CHECK_EQ(h.ptr_calls, 1);
    CHECK_EQ(h.ptr_seq[0], 11);          /* scene_seq at delivery */
    CHECK_EQ(h.ptr_dev[0], 0);
    CHECK_EQ(h.ptr_xy[0][0], 150);
    CHECK_EQ(h.ptr_xy[0][1], 55);
    CHECK_EQ(h.ptr_btn[0], 0x01);
    CHECK_EQ(h.act_calls, 1);
    CHECK_EQ(h.act_id[0], 201);          /* engine-resolved target */
    CHECK_EQ(h.act_seq[0], 11);
    /* flow control: second injection without an ack is dropped */
    CHECK_EQ(scene_server_input_pointer(h.sv, 0, 700, 500, 0x01), 0);
    tick(&h);
    CHECK_EQ(h.ptr_calls, 1);            /* still one */
    CHECK_EQ(h.act_calls, 1);
    /* ack the consumed input seq (token = last presented); the ack must
     * reach the server before the gate reopens, and dropped presses are
     * never queued (flow control drops, it does not buffer) */
    scene_client_ack(h.cl, 11);
    tick(&h);                              /* ack ingested: gate reopens */
    CHECK_EQ(scene_server_input_pointer(h.sv, 0, 700, 500, 0x01), 0);
    tick(&h);                              /* fresh press is delivered */
    CHECK_EQ(h.ptr_calls, 2);
    CHECK_EQ(h.ptr_seq[1], 11);            /* scene unchanged */
    CHECK_EQ(h.ptr_xy[1][0], 700);
    CHECK_EQ(h.ptr_xy[1][1], 500);
    CHECK_EQ(h.act_calls, 2);
    CHECK_EQ(h.act_id[1], 102);          /* panel background */
    /* focus events from the Focus op */
    scene_client_focus(h.cl, 201);
    tick(&h);
    CHECK_EQ(h.foc_calls, 1);            /* only gained: focus was empty */
    CHECK_EQ(h.foc_id[0], 201);
    CHECK_EQ(h.foc_state[0], 1);
    scene_client_focus(h.cl, 202);
    tick(&h);
    CHECK_EQ(h.foc_calls, 3);            /* lost(201,0), gained(202,1) */
    CHECK_EQ(h.foc_id[1], 201);
    CHECK_EQ(h.foc_state[1], 0);
    CHECK_EQ(h.foc_id[2], 202);
    CHECK_EQ(h.foc_state[2], 1);
    CHECK_EQ(scene_store_focus(scene_server_store(h.sv)), 202);
    harness_free(&h);
    printf("test_wire_input_flow_and_activation: ok\n");
}

static void test_wire_error_fatal(void)
{
    struct harness h;
    harness_init(&h);
    tick(&h);
    /* a valid frame with a non-monotonic seq: engine rejects it fatally */
    uint8_t pl[35];
    scene_put_u64(pl + 0, 99);           /* seq 99 != expected 1 */
    scene_put_u32(pl + 8, SCENE_NO_PARENT);
    scene_put_u32(pl + 12, 1);
    scene_put_u16(pl + 16, SCENE_ROLE_WINDOW);
    memset(pl + 18, 0, 16);
    pl[34] = SCENE_FLAG_VISIBLE;
    uint32_t flen;
    uint8_t *f = mk_frame(SCENE_OP_CREATE_NODE, pl, sizeof(pl), &flen);
    uint8_t *snd = (uint8_t *)malloc(flen);
    memcpy(snd, f, flen);
    free(f);
    int r = scene_server_feed(h.sv, snd, flen);
    free(snd);
    CHECK(r < 0);                        /* session closed */
    /* the ERROR record reaches the client */
    const uint8_t *fr; uint32_t frlen;
    while (scene_server_out_next_frame(h.sv, &fr, &frlen) == 1)
        scene_transport_send(h.server_ts, fr, frlen);
    scene_client_pump(h.cl);
    CHECK_EQ(h.err_called, 1);
    CHECK_EQ(h.err_code, SCENE_ERR_SEQ);
    CHECK(scene_client_dead(h.cl));
    CHECK(strstr(h.err_msg, "seq") != NULL);
    /* further ops are refused client-side */
    static const scene_rect r0 = {0, 0, 10, 10};
    CHECK(scene_client_create_node(h.cl, SCENE_NO_PARENT, 2,
                                   SCENE_ROLE_WINDOW, &r0, 0) < 0);
    /* reconnect after a server ERROR is refused */
    scene_transport *new_ts = scene_loopback_client_end(h.lb);
    CHECK(scene_client_reconnect(h.cl, new_ts) < 0);
    scene_transport_close(new_ts);
    harness_free(&h);

    /* checksum violation is detected at the frame level */
    struct harness h2;
    harness_init(&h2);
    tick(&h2);
    memset(pl, 0, sizeof(pl));
    scene_put_u64(pl + 0, 1);
    scene_put_u32(pl + 8, SCENE_NO_PARENT);
    scene_put_u32(pl + 12, 1);
    scene_put_u16(pl + 16, SCENE_ROLE_WINDOW);
    pl[34] = SCENE_FLAG_VISIBLE;
    f = mk_frame(SCENE_OP_CREATE_NODE, pl, sizeof(pl), &flen);
    f[20] ^= 0xAA;                       /* corrupt payload, stale cksum */
    r = scene_server_feed(h2.sv, f, flen);
    free(f);
    CHECK(r < 0);
    while (scene_server_out_next_frame(h2.sv, &fr, &frlen) == 1)
        scene_transport_send(h2.server_ts, fr, frlen);
    scene_client_pump(h2.cl);
    CHECK_EQ(h2.err_called, 1);
    CHECK_EQ(h2.err_code, SCENE_ERR_CKSUM);
    CHECK(scene_client_dead(h2.cl));
    harness_free(&h2);
    printf("test_wire_error_fatal: ok\n");
}

static void test_wire_ghost_reconnect(void)
{
    struct harness h;
    harness_init(&h);
    tick(&h);
    build_app_client(&h);
    scene_client_present(h.cl, 2);
    tick(&h);
    uint32_t sid = h.wel_sid;
    CHECK_EQ(h.ti_calls, 4);
    CHECK_EQ(scene_client_next_seq(h.cl), 13);   /* 11 ops + present */
    /* the client dies; the server ghost-marks the retained scene */
    scene_server_detach(h.sv);
    scene_server_attach(h.sv);   /* rejoin: WELCOME again, stream rebase */
    /* reconnect on the same link with a fresh transport */
    CHECK_EQ(scene_client_reconnect(h.cl, scene_loopback_client_end(h.lb)), 0);
    tick(&h);                    /* WELCOME -> client re-issues its 11 ops */
    CHECK_EQ(h.wel_called, 2);
    CHECK_EQ(h.wel_sid, sid);                    /* same retained session */
    CHECK_EQ(scene_client_next_seq(h.cl), 24);   /* 13 + 11 re-issued */
    /* retained state survives: snapshot shows the full scene */
    scene_client_snapshot(h.cl, 1);
    tick(&h);                    /* re-issue rebased+applied; replies arrive */
    /* the re-issued SetText ops re-emit text indexes (4 more) */
    CHECK_EQ(h.ti_calls, 8);
    CHECK_EQ(h.snap_called, 1);
    uint64_t seq; uint32_t texc, texts_used;
    scene_snapshot_node nodes[16];
    scene_snapshot_text tbuf[32];
    scene_texture_ref texes[4];
    int n = scene_snapshot_parse(h.snap_payload, h.snap_plen, &seq, &texc,
                                 &texts_used, nodes, 16, tbuf, 32, texes, 4);
    CHECK_EQ(n, 6);
    CHECK_EQ(seq, 23);                   /* last re-issued mutation seq */
    CHECK_EQ(texts_used, 4);
    CHECK_EQ(nodes[3].id, 200);          /* "Open" text survived */
    CHECK_EQ(nodes[3].texts[0].len, 4);
    CHECK(memcmp(nodes[3].texts[0].data, "Open", 4) == 0);
    /* server ground truth */
    scene_store *st = scene_server_store(h.sv);
    CHECK_EQ(scene_store_node_count(st), 6);
    CHECK_EQ(scene_store_committed_seq(st), 23);
    harness_free(&h);
    printf("test_wire_ghost_reconnect: ok\n");
}

static void test_wire_fresh_session_rebuild(void)
{
    struct harness h;
    harness_init(&h);
    tick(&h);
    build_app_client(&h);
    scene_client_present(h.cl, 3);
    tick(&h);
    uint32_t sid1 = h.wel_sid;
    scene_server_detach(h.sv);
    /* server restarts: new link, new store -> new scene_id */
    scene_loopback *lb2 = scene_loopback_new();
    scene_server *sv2 = scene_server_new(NULL);
    scene_transport *ts2 = scene_loopback_server_end(lb2);
    scene_server_attach(sv2);
    CHECK_EQ(scene_client_reconnect(h.cl, scene_loopback_client_end(lb2)), 0);
    tick2(&h, ts2, sv2);
    CHECK_EQ(h.wel_called, 2);
    CHECK(h.wel_sid != sid1);            /* fresh session */
    CHECK_EQ(scene_client_next_seq(h.cl), 12);   /* restart at 1 + 11 ops */
    /* the op log rebuilt the scene on the fresh session */
    scene_client_snapshot(h.cl, 1);
    tick2(&h, ts2, sv2);
    CHECK_EQ(h.snap_called, 1);
    uint64_t seq; uint32_t texc, texts_used;
    scene_snapshot_node nodes[16];
    scene_snapshot_text tbuf[32];
    scene_texture_ref texes[4];
    int n = scene_snapshot_parse(h.snap_payload, h.snap_plen, &seq, &texc,
                                 &texts_used, nodes, 16, tbuf, 32, texes, 4);
    CHECK_EQ(n, 6);
    CHECK_EQ(seq, 11);
    CHECK_EQ(texts_used, 4);
    CHECK_EQ(scene_store_node_count(scene_server_store(sv2)), 6);
    CHECK_EQ(scene_store_committed_seq(scene_server_store(sv2)), 11);
    /* teardown: client owns its transport on lb2 now; free link 1 first */
    scene_client_free(h.cl);
    h.cl = NULL;
    scene_server_free(h.sv);
    h.sv = NULL;
    scene_transport_close(h.server_ts);
    h.server_ts = NULL;
    scene_loopback_free(h.lb);
    h.lb = NULL;
    scene_transport_close(ts2);
    scene_server_free(sv2);
    scene_loopback_free(lb2);
    free(h.snap_payload);
    free(h.cap_payload);
    printf("test_wire_fresh_session_rebuild: ok\n");
}

static void test_wire_replay(void)
{
    struct harness h;
    harness_init(&h);
    tick(&h);
    build_app_client(&h);
    scene_client_present(h.cl, 3);
    tick(&h);
    /* rewind to seq 4: only the window, titlebar and panel exist */
    scene_client_set_input_mode(h.cl, SCENE_MODE_REPLAY);
    scene_client_seek(h.cl, 4);
    scene_client_snapshot(h.cl, 2);
    tick(&h);
    CHECK_EQ(h.snap_called, 1);
    uint64_t seq; uint32_t texc, texts_used;
    scene_snapshot_node nodes[16];
    scene_snapshot_text tbuf[32];
    scene_texture_ref texes[4];
    int n = scene_snapshot_parse(h.snap_payload, h.snap_plen, &seq, &texc,
                                 &texts_used, nodes, 16, tbuf, 32, texes, 4);
    CHECK_EQ(n, 3);
    CHECK_EQ(seq, 4);
    /* forward to 11: full scene */
    scene_client_seek(h.cl, 11);
    scene_client_snapshot(h.cl, 3);
    tick(&h);
    CHECK_EQ(h.snap_called, 2);
    n = scene_snapshot_parse(h.snap_payload, h.snap_plen, &seq, &texc,
                             &texts_used, nodes, 16, tbuf, 32, texes, 4);
    CHECK_EQ(n, 6);
    CHECK_EQ(seq, 11);
    /* back to live: live scene still complete */
    scene_client_set_input_mode(h.cl, SCENE_MODE_LIVE);
    scene_client_snapshot(h.cl, 4);
    tick(&h);
    CHECK_EQ(h.snap_called, 3);
    n = scene_snapshot_parse(h.snap_payload, h.snap_plen, &seq, &texc,
                             &texts_used, nodes, 16, tbuf, 32, texes, 4);
    CHECK_EQ(n, 6);
    CHECK_EQ(seq, 11);
    harness_free(&h);
    printf("test_wire_replay: ok\n");
}

static void test_wire_macro(void)
{
    struct harness h;
    harness_init(&h);
    tick(&h);
    static const scene_rect r_win = {0, 0, 400, 300};
    static const scene_rect r_pan = {0, 30, 400, 270};
    static const scene_rect r_b   = {10, 40, 100, 30};
    static const scene_rect r_bm  = {120, 40, 100, 30};
    op_ok(&h, scene_client_create_node(h.cl, SCENE_NO_PARENT, 100,
                                       SCENE_ROLE_WINDOW, &r_win,
                                       SCENE_FLAG_VISIBLE), "create 100");
    op_ok(&h, scene_client_create_node(h.cl, 100, 101, SCENE_ROLE_PANEL,
                                       &r_pan, SCENE_FLAG_VISIBLE), "create 101");
    op_ok(&h, scene_client_create_node(h.cl, 101, 200, SCENE_ROLE_BUTTON,
                                       &r_b, SCENE_FLAG_VISIBLE |
                                       SCENE_FLAG_FOCUSABLE), "create 200");
    op_ok(&h, scene_client_set_text(h.cl, 200, 1, "Open", 4), "text 200");
    /* record macro 42: move the button, add a second text */
    op_ok(&h, scene_client_macro_begin(h.cl, 42), "macro begin");
    op_ok(&h, scene_client_set_rect(h.cl, 200, &r_bm), "macro set_rect");
    op_ok(&h, scene_client_set_text(h.cl, 200, 2, "Save As", 7), "macro text");
    op_ok(&h, scene_client_macro_end(h.cl, 42), "macro end");
    tick(&h);
    /* replace the button with a new one at the recorded rect */
    op_ok(&h, scene_client_destroy_node(h.cl, 200), "destroy 200");
    op_ok(&h, scene_client_create_node(h.cl, 101, 201, SCENE_ROLE_BUTTON,
                                       &r_bm, SCENE_FLAG_VISIBLE |
                                       SCENE_FLAG_FOCUSABLE), "create 201");
    op_ok(&h, scene_client_set_text(h.cl, 201, 1, "New", 3), "text 201");
    /* exec: region-resolves (120,40) -> 201, re-applies the recorded ops */
    op_ok(&h, scene_client_exec_macro(h.cl, 42), "exec macro");
    tick(&h);
    scene_client_search(h.cl, 7, "save", 4);
    tick(&h);
    CHECK_EQ(h.sr_calls, 1);
    CHECK_EQ(h.sr_count, 1);
    CHECK_EQ(h.sr_hits[0].id, 201);      /* macro hit the new button */
    /* snapshot shows the second text slot */
    scene_client_snapshot(h.cl, 8);
    tick(&h);
    uint64_t seq; uint32_t texc, texts_used;
    scene_snapshot_node nodes[8];
    scene_snapshot_text tbuf[16];
    scene_texture_ref texes[4];
    int n = scene_snapshot_parse(h.snap_payload, h.snap_plen, &seq, &texc,
                                 &texts_used, nodes, 8, tbuf, 16, texes, 4);
    CHECK_EQ(n, 3);
    CHECK_EQ(texts_used, 2);             /* New, Save As (Open died with 200) */
    /* node 201 carries both slots */
    scene_snapshot_node *b201 = NULL;
    int i;
    for (i = 0; i < n; i++)
        if (nodes[i].id == 201) b201 = &nodes[i];
    CHECK(b201 != NULL);
    if (b201) {
        CHECK_EQ(b201->text_count, 2);
        CHECK_EQ(b201->texts[1].len, 7);
        CHECK(memcmp(b201->texts[1].data, "Save As", 7) == 0);
    }
    harness_free(&h);
    printf("test_wire_macro: ok\n");
}

/* Host-side importer (the OS seam): decodes the file, registers the ref
 * into the session store, then reports success. missing path = decode
 * failure: returns nonzero, server replies IMPORT_RESULT ok=0. */
static int imp_host_cb(void *ud, scene_server *sv, scene_texture_ref ref,
                       const char *path)
{
    struct harness *h = (struct harness *)ud;
    h->imp_cb_calls++;
    h->imp_cb_ref = ref;
    snprintf(h->imp_cb_path, sizeof(h->imp_cb_path), "%s", path);
    if (strcmp(path, "/data/missing.bmp") == 0) return -1;
    if (scene_store_register_texture(scene_server_store(sv), ref, 4, 2,
                                     SCENE_TEX_FMT_XRGB, 1) != 0)
        return -1;
    return scene_server_import_result(sv, ref, 1);
}

static void test_wire_import_texture(void)
{
    struct harness h;
    harness_init(&h);
    scene_server_set_import_cb(h.sv, imp_host_cb, &h);
    tick(&h);
    static const scene_rect r_img = {10, 10, 40, 24};
    op_ok(&h, scene_client_create_node(h.cl, SCENE_NO_PARENT, 100,
                                       SCENE_ROLE_IMAGE, &r_img,
                                       SCENE_FLAG_VISIBLE), "create 100");
    op_ok(&h, scene_client_import_texture(h.cl, 77u, "/data/pic.bmp"),
          "import 77");
    tick(&h);
    CHECK_EQ(h.imp_cb_calls, 1);       /* host saw the request */
    CHECK_EQ(h.imp_cb_ref, 77u);
    CHECK(strcmp(h.imp_cb_path, "/data/pic.bmp") == 0);
    CHECK_EQ(h.imp_calls, 1);          /* ok=1 result echoed */
    CHECK_EQ(h.imp_ref, 77u);
    CHECK_EQ(h.imp_ok, 1);
    /* the registered ref now validates SET_TEXTURE end-to-end */
    static const scene_rect r_src = {0, 0, 4, 2};
    op_ok(&h, scene_client_set_texture(h.cl, 100, 77u, &r_src, 0, 255),
          "set texture 77");
    tick(&h);
    CHECK_EQ(h.err_called, 0);         /* accepted, no ERROR */
    /* decode failure: ok=0 flows back, session stays alive */
    op_ok(&h, scene_client_import_texture(h.cl, 78u, "/data/missing.bmp"),
          "import 78");
    tick(&h);
    CHECK_EQ(h.imp_calls, 2);
    CHECK_EQ(h.imp_ref, 78u);
    CHECK_EQ(h.imp_ok, 0);
    CHECK_EQ(h.err_called, 0);         /* not fatal */
    /* a later SET_TEXTURE of the unregistered ref is rejected by the
     * engine: ERROR + dead session (engine policy: op errors are fatal) */
    op_ok(&h, scene_client_set_texture(h.cl, 100, 78u, &r_src, 0, 255),
          "set texture 78");
    tick(&h);
    CHECK_EQ(h.err_called, 1);         /* engine rejected the unknown ref */
    CHECK_EQ(h.err_code, SCENE_ERR_PROTOCOL);
    CHECK_EQ(scene_server_dead(h.sv), 1);
    harness_free(&h);
    printf("test_wire_import_texture: ok\n");
}

#if defined(_WIN32)
/* ---- TCP round trip (threaded listener) ---------------------------------- */

static scene_server *g_sv;
static volatile uint16_t g_port;

static int tcp_accept_cb(void *ud, scene_transport *peer)
{
    (void)ud;
    uint8_t buf[8192];
    const uint8_t *f;
    uint32_t flen;
    /* pre-drain: the WELCOME (and anything else already queued) must
     * reach the client before we block on recv, or we deadlock */
    while (scene_server_out_next_frame(g_sv, &f, &flen) == 1)
        scene_transport_send(peer, f, flen);
    for (;;) {
        uint32_t got = 0;
        int r = scene_transport_recv(peer, buf, sizeof(buf), &got);
        if (r != 0) break;
        if (got == 0) break;
        scene_server_feed(g_sv, buf, got);
        while (scene_server_out_next_frame(g_sv, &f, &flen) == 1)
            scene_transport_send(peer, f, flen);
    }
    scene_server_detach(g_sv);
    scene_transport_close(peer);
    return 0;
}

static DWORD WINAPI tcp_srv_thread(LPVOID ud)
{
    (void)ud;
    scene_tcp_listen(0, (uint16_t *)&g_port, tcp_accept_cb, NULL);
    return 0;
}

static void test_tcp_roundtrip(void)
{
    struct harness h;
    memset(&h, 0, sizeof(h));
    h.cl = scene_client_new();
    h.sv = scene_server_new(NULL);
    scene_server_attach(h.sv);
    g_sv = h.sv;
    g_port = 0;
    HANDLE th = CreateThread(NULL, 0, tcp_srv_thread, NULL, 0, NULL);
    CHECK(th != NULL);
    int tries;
    for (tries = 0; tries < 5000 && g_port == 0; tries++) Sleep(1);
    CHECK(g_port != 0);
    char target[64];
    snprintf(target, sizeof(target), "127.0.0.1:%u", (unsigned)g_port);
    CHECK_EQ(scene_client_connect(h.cl, scene_tcp_client(target), target,
                                  &g_cbs, &h), 0);
    for (tries = 0; tries < 5000 && h.wel_called == 0; tries++) {
        scene_client_pump(h.cl);
        Sleep(1);
    }
    CHECK_EQ(h.wel_called, 1);
    build_app_client(&h);
    scene_client_present(h.cl, 5);
    scene_client_flush(h.cl);
    for (tries = 0; tries < 5000 && h.pd_calls == 0; tries++) {
        scene_client_pump(h.cl);
        Sleep(1);
    }
    CHECK_EQ(h.pd_calls, 1);
    CHECK_EQ(h.pd_token[0], 5);
    scene_client_snapshot(h.cl, 7);
    scene_client_flush(h.cl);
    for (tries = 0; tries < 5000 && h.snap_called == 0; tries++) {
        scene_client_pump(h.cl);
        Sleep(1);
    }
    CHECK_EQ(h.snap_called, 1);
    uint64_t seq; uint32_t texc, texts_used;
    scene_snapshot_node nodes[16];
    scene_snapshot_text tbuf[32];
    scene_texture_ref texes[4];
    int n = scene_snapshot_parse(h.snap_payload, h.snap_plen, &seq, &texc,
                                 &texts_used, nodes, 16, tbuf, 32, texes, 4);
    CHECK_EQ(n, 6);
    CHECK_EQ(seq, 11);
    /* close the client; the accept loop ends when the listener closes */
    scene_client_free(h.cl);
    h.cl = NULL;
    Sleep(50);
    scene_tcp_listen_close();
    CHECK_EQ(WaitForSingleObject(th, 5000), WAIT_OBJECT_0);
    CloseHandle(th);
    /* server-side ground truth after the connection is gone */
    scene_store *st = scene_server_store(h.sv);
    CHECK_EQ(scene_store_node_count(st), 6);
    CHECK_EQ(scene_store_committed_seq(st), 11);
    scene_server_free(h.sv);
    free(h.snap_payload);
    free(h.cap_payload);
    printf("test_tcp_roundtrip: ok\n");
}

/* ---- launcher-child contract: silent server must not deadlock ---------
 * A launcher child must never block in pump: the scene server only
 * replies after the client speaks, so a blocking recv before any flush
 * deadlocks with the buffered ops stuck in the out buffer (the ISO
 * terminal bug: window built, l1=0, zero bytes fed). The child sets
 * non-blocking; pump returns would-block immediately and the loop keeps
 * flushing each tick even though the server stays silent. This test
 * proves: (a) recv on the non-blocking socket returns would-block (1)
 * immediately when empty, (b) buffered window ops still reach the wire
 * with a server that never replies.                                       */

static volatile long g_silent_rx;   /* bytes the silent server received   */
static volatile int  g_silent_done;

static int silent_accept_cb(void *ud, scene_transport *peer)
{
    (void)ud;
    /* real WELCOME so the client becomes welcomed, then silence */
    const uint8_t *f;
    uint32_t flen;
    while (scene_server_out_next_frame(g_sv, &f, &flen) == 1)
        scene_transport_send(peer, f, flen);
    scene_tcp_set_nonblock(peer, 1);
    uint8_t buf[8192];
    g_silent_rx = 0;
    for (;;) {
        uint32_t got = 0;
        int r = scene_transport_recv(peer, buf, sizeof(buf), &got);
        if (r == -1) break;                 /* peer closed                  */
        if (r == 1 || got == 0) {           /* would-block / nothing        */
            Sleep(1);
            if (g_silent_done) break;
            continue;
        }
        g_silent_rx += got;                 /* count, never reply           */
    }
    scene_transport_close(peer);
    return 0;
}

static DWORD WINAPI silent_srv_thread(LPVOID ud)
{
    (void)ud;
    scene_tcp_listen(0, (uint16_t *)&g_port, silent_accept_cb, NULL);
    return 0;
}

static void test_tcp_silent_server_flush(void)
{
    struct harness h;
    memset(&h, 0, sizeof(h));
    h.cl = scene_client_new();
    h.sv = scene_server_new(NULL);
    scene_server_attach(h.sv);
    g_sv = h.sv;
    g_port = 0;
    g_silent_rx = 0;
    g_silent_done = 0;
    HANDLE th = CreateThread(NULL, 0, silent_srv_thread, NULL, 0, NULL);
    CHECK(th != NULL);
    int tries;
    for (tries = 0; tries < 5000 && g_port == 0; tries++) Sleep(1);
    CHECK(g_port != 0);
    char target[64];
    snprintf(target, sizeof(target), "127.0.0.1:%u", (unsigned)g_port);
    scene_transport *t = scene_tcp_client(target);
    CHECK(t != NULL);
    CHECK_EQ(scene_client_connect(h.cl, t, target, &g_cbs, &h), 0);
    /* launcher-children rule: the render loop must never block on recv */
    CHECK_EQ(scene_tcp_set_nonblock(t, 1), 0);
    for (tries = 0; tries < 5000 && h.wel_called == 0; tries++) {
        scene_client_pump(h.cl);
        scene_client_flush(h.cl);
        Sleep(1);
    }
    CHECK_EQ(h.wel_called, 1);
    /* non-blocking contract: recv on the now-empty socket returns
     * would-block (1) immediately instead of stalling the loop          */
    {
        uint8_t tmp[16];
        uint32_t got = 0;
        int r = scene_transport_recv(t, tmp, sizeof(tmp), &got);
        CHECK(r == 1 || (r == 0 && got == 0));
    }
    build_app_client(&h);
    scene_client_present(h.cl, 9);
    int round;
    for (round = 0; round < 200 && h.closed_calls == 0; round++) {
        scene_client_pump(h.cl);
        scene_client_flush(h.cl);
        Sleep(1);
    }
    CHECK(h.closed_calls == 0);             /* connection stayed open      */
    CHECK(g_silent_rx > 0);                 /* window ops reached the wire */
    scene_client_free(h.cl);
    h.cl = NULL;
    g_silent_done = 1;
    Sleep(50);
    scene_tcp_listen_close();
    CHECK_EQ(WaitForSingleObject(th, 5000), WAIT_OBJECT_0);
    CloseHandle(th);
    scene_server_free(h.sv);
    free(h.snap_payload);
    free(h.cap_payload);
    printf("test_tcp_silent_server_flush: ok (server rx=%ld bytes)\n",
           g_silent_rx);
}
#endif /* _WIN32 */

int main(void)
{
    test_wire_welcome_and_ping();
    test_wire_build_present_snapshot();
    test_wire_search_capture_textindex();
    test_wire_input_flow_and_activation();
    test_wire_error_fatal();
    test_wire_ghost_reconnect();
    test_wire_fresh_session_rebuild();
    test_wire_replay();
    test_wire_macro();
    test_wire_import_texture();
#if defined(_WIN32)
    test_tcp_roundtrip();
    test_tcp_silent_server_flush();
#endif
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
