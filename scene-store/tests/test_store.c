/* test_store.c — black-box tests for the semantic scene engine core. */
#include "scene_store.h"

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

/* ---- frame builders -------------------------------------------------- */

static uint8_t *mk_frame(uint16_t opcode, const uint8_t *payload,
                         uint32_t plen, uint32_t *out_len)
{
    uint32_t total = SCENE_HEADER_SIZE + plen;
    uint8_t *f = (uint8_t *)malloc(total);
    scene_put_u32(f + 0, SCENE_MAGIC);
    scene_put_u16(f + 4, SCENE_PROTOCOL_V0);
    scene_put_u16(f + 6, opcode);
    scene_put_u32(f + 8, plen);
    memset(f + 12, 0, 4);   /* checksum field is zeroed at compute time  */
    if (plen) memcpy(f + SCENE_HEADER_SIZE, payload, plen);
    scene_put_u32(f + 12, scene_fnv1a32(f, SCENE_HEADER_SIZE + plen));
    *out_len = total;
    return f;
}

static uint8_t pl_buf[1 << 20];

static void put_u64_at(uint32_t off, uint64_t v) { scene_put_u64(pl_buf + off, v); }
static void put_u32_at(uint32_t off, uint32_t v) { scene_put_u32(pl_buf + off, v); }
static void put_u16_at(uint32_t off, uint16_t v) { scene_put_u16(pl_buf + off, v); }
static void put_i32_at(uint32_t off, int32_t v)  { scene_put_i32(pl_buf + off, v); }

static int do_op(scene_store *s, uint16_t opcode, uint32_t plen)
{
    uint32_t flen;
    uint8_t *f = mk_frame(opcode, pl_buf, plen, &flen);
    int r = scene_store_ingest(s, opcode, pl_buf, plen);
    free(f);
    return r;
}

static uint32_t seq_counter;
static void next_seq(void) { seq_counter++; }

static int create(scene_store *s, uint32_t parent, uint32_t id, uint16_t role,
                  int32_t x, int32_t y, int32_t w, int32_t h, uint8_t flags)
{
    next_seq();
    put_u64_at(0, seq_counter);
    put_u32_at(8, parent);
    put_u32_at(12, id);
    put_u16_at(16, role);
    put_i32_at(18, x);
    put_i32_at(22, y);
    put_i32_at(26, w);
    put_i32_at(30, h);
    pl_buf[34] = flags;
    return do_op(s, SCENE_OP_CREATE_NODE, 35);
}

static int destroy(scene_store *s, uint32_t id)
{
    next_seq();
    put_u64_at(0, seq_counter);
    put_u32_at(8, id);
    return do_op(s, SCENE_OP_DESTROY_NODE, 12);
}

static int set_text(scene_store *s, uint32_t id, uint32_t tid,
                    const char *txt)
{
    uint32_t n = (uint32_t)strlen(txt);
    next_seq();
    put_u64_at(0, seq_counter);
    put_u32_at(8, id);
    put_u32_at(12, tid);
    put_u32_at(16, n);
    memcpy(pl_buf + 20, txt, n);
    return do_op(s, SCENE_OP_SET_TEXT, 20 + n);
}

static int set_rect(scene_store *s, uint32_t id,
                    int32_t x, int32_t y, int32_t w, int32_t h)
{
    next_seq();
    put_u64_at(0, seq_counter);
    put_u32_at(8, id);
    put_i32_at(12, x);
    put_i32_at(16, y);
    put_i32_at(20, w);
    put_i32_at(24, h);
    return do_op(s, SCENE_OP_SET_RECT, 28);
}

static int set_flags(scene_store *s, uint32_t id, uint8_t flags)
{
    next_seq();
    put_u64_at(0, seq_counter);
    put_u32_at(8, id);
    pl_buf[12] = flags;
    return do_op(s, SCENE_OP_SET_FLAGS, 13);
}

static int focus(scene_store *s, uint32_t id)
{
    next_seq();
    put_u64_at(0, seq_counter);
    put_u32_at(8, id);
    return do_op(s, SCENE_OP_FOCUS, 12);
}

static int focus_next(scene_store *s, int8_t step)
{
    next_seq();
    put_u64_at(0, seq_counter);
    pl_buf[8] = (uint8_t)step;
    return do_op(s, SCENE_OP_FOCUS_NEXT, 9);
}

static int present(scene_store *s, uint64_t token)
{
    next_seq();
    put_u64_at(0, seq_counter);
    put_u64_at(8, token);
    return do_op(s, SCENE_OP_PRESENT, 16);
}

static int snapshot(scene_store *s, uint32_t req_id)
{
    next_seq();
    put_u64_at(0, seq_counter);
    put_u32_at(8, req_id);
    return do_op(s, SCENE_OP_SNAPSHOT, 12);
}

static int capture(scene_store *s, uint32_t req_id)
{
    next_seq();
    put_u64_at(0, seq_counter);
    put_u32_at(8, req_id);
    return do_op(s, SCENE_OP_CAPTURE, 12);
}

static int search(scene_store *s, uint32_t req_id, const char *term)
{
    uint32_t n = (uint32_t)strlen(term);
    next_seq();
    put_u64_at(0, seq_counter);
    put_u32_at(8, req_id);
    put_u32_at(12, n);
    memcpy(pl_buf + 16, term, n);
    return do_op(s, SCENE_OP_SEARCH, 16 + n);
}

static int ping(scene_store *s, uint64_t nonce)
{
    next_seq();
    put_u64_at(0, seq_counter);
    put_u64_at(8, nonce);
    return do_op(s, SCENE_OP_PING, 16);
}

static int set_input_mode(scene_store *s, uint8_t mode)
{
    next_seq();
    put_u64_at(0, seq_counter);
    pl_buf[8] = mode;
    return do_op(s, SCENE_OP_SET_INPUT_MODE, 9);
}

static int seek(scene_store *s, uint64_t target)
{
    next_seq();
    put_u64_at(0, seq_counter);
    put_u64_at(8, target);
    return do_op(s, SCENE_OP_SEEK, 16);
}

static int ack(scene_store *s, uint64_t consumed, uint64_t token)
{
    put_u64_at(0, consumed);
    put_u64_at(8, token);
    return do_op(s, SCENE_OP_ACK, 16);
}

static int macro_begin(scene_store *s, uint32_t mid)
{
    next_seq();
    put_u64_at(0, seq_counter);
    put_u32_at(8, mid);
    return do_op(s, SCENE_OP_MACRO_BEGIN, 12);
}

static int macro_end(scene_store *s, uint32_t mid)
{
    next_seq();
    put_u64_at(0, seq_counter);
    put_u32_at(8, mid);
    return do_op(s, SCENE_OP_MACRO_END, 12);
}

static int exec_macro(scene_store *s, uint32_t mid)
{
    next_seq();
    put_u64_at(0, seq_counter);
    put_u32_at(8, mid);
    return do_op(s, SCENE_OP_EXEC_MACRO, 12);
}

/* ---- out drain ------------------------------------------------------- */

static uint16_t out_next(scene_store *s, const uint8_t **pay, uint32_t *plen)
{
    uint16_t op;
    if (scene_store_out_next(s, &op, pay, plen) != 1) return 0;
    return op;
}

/* Discard everything currently queued outbound (e.g. TEXT_INDEX records
 * emitted during scene construction) so the next out_next() assertion
 * sees only the record under test.                                       */
static void drain_out(scene_store *s)
{
    uint16_t op;
    const uint8_t *p;
    uint32_t n;
    while (scene_store_out_next(s, &op, &p, &n)) {}
}

/* Build a deterministic app scene: window with a menu bar + 3 buttons.
 * All rects are absolute in session space per spec §3.                  */
static void build_app(scene_store *s)
{
    create(s, SCENE_NO_PARENT, 100, SCENE_ROLE_WINDOW, 0, 0, 800, 600,
           SCENE_FLAG_VISIBLE);
    create(s, 100, 101, SCENE_ROLE_TITLEBAR, 0, 0, 800, 30,
           SCENE_FLAG_VISIBLE);
    set_text(s, 101, 1, "My App");
    create(s, 100, 102, SCENE_ROLE_PANEL, 0, 30, 800, 570,
           SCENE_FLAG_VISIBLE);
    create(s, 102, 200, SCENE_ROLE_BUTTON, 10, 40, 100, 30,
           SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    set_text(s, 200, 1, "Open");
    create(s, 102, 201, SCENE_ROLE_BUTTON, 120, 40, 100, 30,
           SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    set_text(s, 201, 1, "Save");
    create(s, 102, 202, SCENE_ROLE_BUTTON, 230, 40, 100, 30,
           SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    set_text(s, 202, 1, "Quit");
    set_rect(s, 201, 120, 40, 100, 30);
}

/* ==================================================================== */
/* Tests                                                                 */
/* ==================================================================== */

static void test_welcome(void)
{
    scene_store *s = scene_store_new(NULL);
    CHECK(s != NULL);
    scene_store_welcome(s);
    const uint8_t *p; uint32_t n;
    CHECK_EQ(out_next(s, &p, &n), SCENE_SRV_WELCOME);
    CHECK_EQ(n, 30);
    CHECK_EQ(scene_get_u32(p + 4), SCENE_PROTOCOL_V0);
    CHECK_EQ(scene_get_u32(p + 6), SCENE_DEFAULT_NODES);
    scene_store_free(s);
    printf("test_welcome: ok\n");
}

static void test_build_and_query(void)
{
    scene_store *s = scene_store_new(NULL);
    build_app(s);
    CHECK_EQ(scene_store_node_count(s), 6);
    /* records: 6 creates + 4 texts + 1 set_rect = 11 */
    CHECK_EQ(scene_store_committed_seq(s), 11);
    /* region_at on a button */
    CHECK_EQ(scene_store_region_at(s, 150, 55), 201);
    /* set_flags: hide a button; region resolution must skip it */
    set_flags(s, 201, SCENE_FLAG_FOCUSABLE);   /* visible cleared */
    CHECK_EQ(scene_store_region_at(s, 150, 55), 102);
    set_flags(s, 201, SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    CHECK_EQ(scene_store_region_at(s, 150, 55), 201);
    /* background of the panel (no child under it) */
    CHECK_EQ(scene_store_region_at(s, 700, 500), 102);
    /* search */
    scene_node_id nodes[8];
    scene_text_id texts[8];
    size_t tcap = 8;
    size_t hits = scene_store_search(s, "open", 4, nodes, 8, texts, &tcap);
    CHECK_EQ(hits, 1);
    CHECK_EQ(nodes[0], 200);
    hits = scene_store_search(s, "a", 1, nodes, 8, texts, &tcap);
    CHECK_EQ(hits, 2); /* Save, Quit */
    scene_store_free(s);
    printf("test_build_and_query: ok\n");
}

static void test_destroy_subtree(void)
{
    scene_store *s = scene_store_new(NULL);
    build_app(s);
    destroy(s, 200);
    CHECK_EQ(scene_store_node_count(s), 5);
    CHECK_EQ(scene_store_region_at(s, 50, 55), 102);
    /* destroy the panel: children 201, 202 die too */
    destroy(s, 102);
    CHECK_EQ(scene_store_node_count(s), 2);
    CHECK_EQ(scene_store_region_at(s, 150, 55), 100); /* window remains */
    /* destroy the root: the whole scene is gone */
    destroy(s, 100);
    CHECK_EQ(scene_store_node_count(s), 0);
    CHECK_EQ(scene_store_region_at(s, 150, 55), SCENE_NO_PARENT);
    scene_store_free(s);
    printf("test_destroy_subtree: ok\n");
}

static void test_focus_and_focus_next(void)
{
    scene_store *s = scene_store_new(NULL);
    build_app(s);
    CHECK_EQ(scene_store_focus(s), SCENE_NO_PARENT);
    focus(s, 201);
    CHECK_EQ(scene_store_focus(s), 201);
    /* next wraps 201 -> 202 -> 200 */
    focus_next(s, 1);
    CHECK_EQ(scene_store_focus(s), 202);
    focus_next(s, 1);
    CHECK_EQ(scene_store_focus(s), 200);
    focus_next(s, 1);
    CHECK_EQ(scene_store_focus(s), 201);
    focus_next(s, -1);
    CHECK_EQ(scene_store_focus(s), 200);
    /* focus lost on destroy */
    destroy(s, 200);
    CHECK_EQ(scene_store_focus(s), SCENE_NO_PARENT);
    scene_store_free(s);
    printf("test_focus_and_focus_next: ok\n");
}

static void test_seq_rejection(void)
{
    scene_store *s = scene_store_new(NULL);
    next_seq(); seq_counter++; /* skip one */
    int r = create(s, SCENE_NO_PARENT, 1, SCENE_ROLE_WINDOW, 0, 0, 10, 10,
                   SCENE_FLAG_VISIBLE);
    CHECK(r < 0); /* -SCENE_ERR_SEQ */
    CHECK_EQ(r, -(int)SCENE_ERR_SEQ);
    const uint8_t *p; uint32_t n;
    CHECK_EQ(out_next(s, &p, &n), SCENE_SRV_ERROR);
    CHECK_EQ(scene_get_u16(p), SCENE_ERR_SEQ);
    /* store is dead: further ops rejected */
    next_seq();
    r = create(s, SCENE_NO_PARENT, 2, SCENE_ROLE_WINDOW, 0, 0, 10, 10, 0);
    CHECK(r < 0);
    scene_store_free(s);
    printf("test_seq_rejection: ok\n");
}

static void test_unknown_opcode(void)
{
    scene_store *s = scene_store_new(NULL);
    next_seq();
    put_u64_at(0, seq_counter);
    int r = do_op(s, 0x7777, 8);
    CHECK_EQ(r, -(int)SCENE_ERR_PROTOCOL);
    scene_store_free(s);
    printf("test_unknown_opcode: ok\n");
}

static void test_present_and_ack_flow(void)
{
    scene_store *s = scene_store_new(NULL);
    build_app(s);
    drain_out(s);
    /* pointer delivered; activate resolves region */
    int r = scene_store_input_pointer(s, 0, 150, 55, 0x01);
    CHECK_EQ(r, 0);
    const uint8_t *p; uint32_t n;
    CHECK_EQ(out_next(s, &p, &n), SCENE_SRV_INPUT_POINTER);
    CHECK_EQ(scene_get_u64(p), 11); /* scene_seq */
    CHECK_EQ(out_next(s, &p, &n), SCENE_SRV_INPUT_ACTIVATE);
    CHECK_EQ(scene_get_u32(p + 8), 201);
    /* flow control: second delivery dropped until ack */
    r = scene_store_input_pointer(s, 0, 150, 55, 0x01);
    CHECK_EQ(r, 0);
    CHECK_EQ(out_next(s, &p, &n), 0); /* nothing queued */
    /* ack unblocks (consumed must cover the delivered scene_seq) */
    r = ack(s, 11, 0);
    CHECK_EQ(r, 0);
    r = scene_store_input_pointer(s, 0, 250, 55, 0x00);
    CHECK_EQ(r, 0);
    CHECK_EQ(out_next(s, &p, &n), SCENE_SRV_INPUT_POINTER);
    /* present produces PresentDone with latency */
    present(s, 42);
    CHECK_EQ(out_next(s, &p, &n), SCENE_SRV_PRESENT_DONE);
    CHECK_EQ(scene_get_u64(p), 11);      /* seq */
    CHECK_EQ(scene_get_u64(p + 8), 42); /* token */
    scene_store_free(s);
    printf("test_present_and_ack_flow: ok\n");
}

static void test_snapshot_and_capture(void)
{
    scene_store *s = scene_store_new(NULL);
    build_app(s);
    drain_out(s);
    scene_store_register_texture(s, 7, 64, 32, 4, 1);
    snapshot(s, 555);
    const uint8_t *p; uint32_t n;
    CHECK_EQ(out_next(s, &p, &n), SCENE_SRV_SNAPSHOT);
    CHECK_EQ(scene_get_u32(p), 555);       /* req_id */
    uint64_t seq = scene_get_u64(p + 4);
    CHECK_EQ(seq, 11);
    uint32_t cnt = scene_get_u32(p + 12);
    CHECK_EQ(cnt, 6);
    uint32_t texc = scene_get_u32(p + 16);
    CHECK_EQ(texc, 1);
    capture(s, 556);
    CHECK_EQ(out_next(s, &p, &n), SCENE_SRV_CAPTURE);
    CHECK_EQ(scene_get_u32(p), 556);
    CHECK_EQ(scene_get_u64(p + 4), 11);
    scene_store_free(s);
    printf("test_snapshot_and_capture: ok\n");
}

static void test_search_reply(void)
{
    scene_store *s = scene_store_new(NULL);
    build_app(s);
    drain_out(s);
    search(s, 777, "quit");
    const uint8_t *p; uint32_t n;
    CHECK_EQ(out_next(s, &p, &n), SCENE_SRV_SEARCH_RESULT);
    CHECK_EQ(scene_get_u32(p), 777);
    CHECK_EQ(scene_get_u32(p + 4), 1);
    CHECK_EQ(scene_get_u32(p + 8), 202);  /* node id */
    scene_store_free(s);
    printf("test_search_reply: ok\n");
}

static void test_ping_pong(void)
{
    scene_store *s = scene_store_new(NULL);
    ping(s, 0xdeadbeef);
    const uint8_t *p; uint32_t n;
    CHECK_EQ(out_next(s, &p, &n), SCENE_SRV_PONG);
    CHECK_EQ(scene_get_u64(p), UINT64_C(0xdeadbeef));
    scene_store_free(s);
    printf("test_ping_pong: ok\n");
}

static void test_replay_determinism(void)
{
    scene_store *a = scene_store_new(NULL);
    build_app(a);
    drain_out(a);
    /* 11 mutating records in the log */
    set_input_mode(a, SCENE_MODE_REPLAY);
    seek(a, 4); /* after window+titlebar+text+panel */
    CHECK_EQ(scene_store_node_count(a), 6); /* live untouched */
    const uint8_t *p; uint32_t n;
    CHECK_EQ(out_next(a, &p, &n), 0);
    snapshot(a, 9); /* still in replay mode: serves replay arena */
    CHECK_EQ(out_next(a, &p, &n), SCENE_SRV_SNAPSHOT);
    uint32_t rcnt = scene_get_u32(p + 12);
    CHECK_EQ(rcnt, 3); /* node_count at seq 4 */
    /* drain */
    drain_out(a);
    /* byte-compare the live scene of b against a's replay at full seq */
    seek(a, 11);
    /* drain previous snapshot remnants */
    set_input_mode(a, SCENE_MODE_LIVE);   /* replay arena torn down */
    /* rebuilt playback: re-enter replay and seek to full scene */
    set_input_mode(a, SCENE_MODE_REPLAY);
    seek(a, 11);
    seq_counter = 0;                       /* b is a fresh session */
    scene_store *b = scene_store_new(NULL);
    build_app(b);
    CHECK_EQ(scene_store_node_count(b), 6);
    CHECK_EQ(scene_store_committed_seq(b), 11);
    /* drain both pending outbound, then request identical snapshots.
     * a's next seq is 19, b's is 12: per-store seqs, hand-built.      */
    drain_out(a);
    drain_out(b);
    put_u64_at(0, 19); put_u32_at(8, 1);
    do_op(a, SCENE_OP_SNAPSHOT, 12);
    put_u64_at(0, 12); put_u32_at(8, 1);
    do_op(b, SCENE_OP_SNAPSHOT, 12);
    const uint8_t *pa, *pb; uint32_t la, lb;
    uint16_t oa = out_next(a, &pa, &la);
    uint16_t ob = out_next(b, &pb, &lb);
    CHECK_EQ(oa, SCENE_SRV_SNAPSHOT);
    CHECK_EQ(ob, SCENE_SRV_SNAPSHOT);
    CHECK_EQ(la, lb);
    CHECK(memcmp(pa, pb, la) == 0);
    scene_store_free(a);
    scene_store_free(b);
    printf("test_replay_determinism: ok\n");
}

static void test_ghost_crash(void)
{
    scene_store *s = scene_store_new(NULL);
    build_app(s);
    /* app dies: everything retained, marked stale */
    CHECK_EQ(scene_store_ghost_mark(s), 0);
    /* reconnect: client re-issues its ops (same ids); engine diffs */
    create(s, SCENE_NO_PARENT, 100, SCENE_ROLE_WINDOW, 0, 0, 800, 600,
           SCENE_FLAG_VISIBLE);
    create(s, 100, 101, SCENE_ROLE_TITLEBAR, 0, 0, 800, 30,
           SCENE_FLAG_VISIBLE);
    set_text(s, 101, 1, "My App v2"); /* delta: text updated */
    create(s, 100, 102, SCENE_ROLE_PANEL, 0, 30, 800, 570,
           SCENE_FLAG_VISIBLE);
    create(s, 102, 200, SCENE_ROLE_BUTTON, 10, 40, 100, 30,
           SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    /* text on 200 was NOT re-sent: retained from before the crash */
    set_text(s, 200, 1, "Open");
    create(s, 102, 201, SCENE_ROLE_BUTTON, 120, 40, 100, 30,
           SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    CHECK_EQ(scene_store_node_count(s), 6);
    /* verify retained text survived without re-send */
    scene_node_id nodes[8];
    scene_text_id texts[8];
    size_t tcap = 8;
    size_t hits = scene_store_search(s, "Save", 4, nodes, 8, texts, &tcap);
    CHECK_EQ(hits, 1);   /* text survived */
    CHECK_EQ(nodes[0], 201);
    /* and the delta was applied */
    hits = scene_store_search(s, "v2", 2, nodes, 8, texts, &tcap);
    CHECK_EQ(hits, 1);
    CHECK_EQ(nodes[0], 101);
    /* region resolution still works after resurrection */
    CHECK_EQ(scene_store_region_at(s, 150, 55), 201);
    scene_store_free(s);
    printf("test_ghost_crash: ok\n");
}

static void test_macro_region_resolution(void)
{
    scene_store *src = scene_store_new(NULL);
    build_app(src);
    /* record: press Save (focus + click is an activation; record mutation) */
    macro_begin(src, 42);
    set_rect(src, 201, 120, 40, 100, 30); /* move Save */
    set_text(src, 201, 2, "Save As");
    macro_end(src, 42);
    /* apply the macro in a second app where the same rect is a DIFFERENT
     * button id (region-resolved activation, cross-app automation) */
    scene_store *dst = scene_store_new(NULL);
    seq_counter = 0;                       /* dst is a fresh session */
    create(dst, SCENE_NO_PARENT, 1000, SCENE_ROLE_WINDOW, 0, 0, 800, 600,
           SCENE_FLAG_VISIBLE);
    create(dst, 1000, 1001, SCENE_ROLE_PANEL, 0, 30, 800, 570,
           SCENE_FLAG_VISIBLE);
    /* button at the recorded rect but with a different id */
    create(dst, 1001, 900, SCENE_ROLE_BUTTON, 120, 40, 100, 30,
           SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    /* transfer the recorded definition (OS-service cross-app automation) */
    CHECK_EQ(scene_store_import_macro(dst, src, 42, 42), 0);
    exec_macro(dst, 42);
    /* the macro op targeted id 201 which does not exist in dst; the engine
     * re-resolved by rect (120,40) to node 900 and applied the delta */
    scene_node_id nodes[8];
    scene_text_id texts[8];
    size_t tcap = 8;
    size_t hits = scene_store_search(dst, "Save As", 7, nodes, 8, texts, &tcap);
    CHECK_EQ(hits, 1);
    CHECK_EQ(nodes[0], 900);
    scene_store_free(src);
    scene_store_free(dst);
    printf("test_macro_region_resolution: ok\n");
}

static void test_frame_check(void)
{
    scene_store *s = scene_store_new(NULL);
    uint8_t payload[8];
    scene_put_u64(payload, 1);
    uint32_t flen;
    uint8_t *f = mk_frame(SCENE_OP_PING, payload, 8, &flen);
    scene_frame_header h;
    h.magic = scene_get_u32(f + 0);
    h.version = scene_get_u16(f + 4);
    h.opcode = scene_get_u16(f + 6);
    h.length = scene_get_u32(f + 8);
    h.checksum = scene_get_u32(f + 12);
    CHECK_EQ(scene_frame_check(&h, f, flen), 0);
    f[20] ^= 0xFF; /* corrupt payload */
    h.checksum = scene_get_u32(f + 12); /* stale checksum */
    CHECK(scene_frame_check(&h, f, flen) != 0);
    free(f);
    scene_store_free(s);
    printf("test_frame_check: ok\n");
}

static void test_limits(void)
{
    scene_limits lim = {
        .max_nodes_per_session = 6,
        .max_text_bytes_per_slot = 64,
        .max_text_slots_per_node = 4,
        .max_record_length = 1024,
        .input_latency_budget_us = 16667,
    };
    scene_store *s = scene_store_new(&lim);
    build_app(s); /* 6 nodes: fits exactly */
    drain_out(s);
    create(s, SCENE_NO_PARENT, 999, SCENE_ROLE_WINDOW, 0, 0, 10, 10, 0);
    CHECK_EQ(scene_store_node_count(s), 6); /* rejected: limit */
    const uint8_t *p; uint32_t n;
    CHECK_EQ(out_next(s, &p, &n), SCENE_SRV_ERROR);
    CHECK_EQ(scene_get_u16(p), SCENE_ERR_LIMIT);
    scene_store_free(s);
    printf("test_limits: ok\n");
}

static void test_ghost_resurrect_wrong_parent(void)
{
    scene_store *s = scene_store_new(NULL);
    build_app(s);
    drain_out(s);
    CHECK_EQ(scene_store_ghost_mark(s), 0);
    /* re-create with a parent that is itself stale: must fail */
    create(s, 102, 555, SCENE_ROLE_BUTTON, 5, 5, 10, 10, 0);
    const uint8_t *p; uint32_t n;
    CHECK_EQ(out_next(s, &p, &n), SCENE_SRV_ERROR);
    CHECK_EQ(scene_get_u16(p), SCENE_ERR_BAD_PARENT);
    scene_store_free(s);
    printf("test_ghost_resurrect_wrong_parent: ok\n");
}

int main(void)
{
    seq_counter = 0;
    test_welcome();
    seq_counter = 0;
    test_build_and_query();
    seq_counter = 0;
    test_destroy_subtree();
    seq_counter = 0;
    test_focus_and_focus_next();
    seq_counter = 0;
    test_seq_rejection();
    seq_counter = 0;
    test_unknown_opcode();
    seq_counter = 0;
    test_present_and_ack_flow();
    seq_counter = 0;
    test_snapshot_and_capture();
    seq_counter = 0;
    test_search_reply();
    seq_counter = 0;
    test_ping_pong();
    seq_counter = 0;
    test_replay_determinism();
    seq_counter = 0;
    test_ghost_crash();
    seq_counter = 0;
    test_macro_region_resolution();
    seq_counter = 0;
    test_frame_check();
    seq_counter = 0;
    test_limits();
    seq_counter = 0;
    test_ghost_resurrect_wrong_parent();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
