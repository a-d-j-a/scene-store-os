/* test_hostwm.c — host-side WM service APIs (OS interventions).
 *
 * Pure engine tests: the three new host mutations
 * (scene_store_host_focus / host_set_visible / host_set_rect) must
 * behave as OS interventions — no wire bytes, no peer-seq consumption —
 * while committing through the engine's own path so the compositor's
 * per-layer diff fires (committed seq advances). They refuse unknown /
 * stale nodes, dead stores, and replay mode. Link set: scene_fmt +
 * scene_store only (as build/test_store).
 */
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

/* ---- op builders (mirror test_store.c) -------------------------------- */

static uint8_t pl_buf[1 << 20];
static uint64_t seq_counter;

static void next_seq(void) { seq_counter++; }

static int do_op(scene_store *s, uint16_t opcode, uint32_t plen)
{
    return scene_store_ingest(s, opcode, pl_buf, plen);
}

static int create(scene_store *s, uint32_t parent, uint32_t id,
                  uint16_t role, int32_t x, int32_t y, int32_t w, int32_t h,
                  uint8_t flags)
{
    next_seq();
    scene_put_u64(pl_buf + 0, seq_counter);
    scene_put_u32(pl_buf + 8, parent);
    scene_put_u32(pl_buf + 12, id);
    scene_put_u16(pl_buf + 16, role);
    scene_put_i32(pl_buf + 18, x);
    scene_put_i32(pl_buf + 22, y);
    scene_put_i32(pl_buf + 26, w);
    scene_put_i32(pl_buf + 30, h);
    pl_buf[34] = flags;
    return do_op(s, SCENE_OP_CREATE_NODE, 35);
}

static int destroy(scene_store *s, uint32_t id)
{
    next_seq();
    scene_put_u64(pl_buf + 0, seq_counter);
    scene_put_u32(pl_buf + 8, id);
    return do_op(s, SCENE_OP_DESTROY_NODE, 12);
}

static int set_text(scene_store *s, uint32_t id, uint32_t tid,
                    const char *txt)
{
    uint32_t n = (uint32_t)strlen(txt);
    next_seq();
    scene_put_u64(pl_buf + 0, seq_counter);
    scene_put_u32(pl_buf + 8, id);
    scene_put_u32(pl_buf + 12, tid);
    scene_put_u32(pl_buf + 16, n);
    memcpy(pl_buf + 20, txt, n);
    return do_op(s, SCENE_OP_SET_TEXT, 20 + n);
}

/* ---- helpers ---------------------------------------------------------- */

#define WID 41000u      /* window node id (per-session namespace) */
#define BID 41001u      /* its child button */

/* A 3-node session: WINDOW (with title text) + child BUTTON + label. */
static void build_scene(scene_store *s)
{
    seq_counter = 0;
    CHECK_EQ(create(s, SCENE_NO_PARENT, WID, SCENE_ROLE_WINDOW,
                    100, 50, 240, 160,
                    SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE), 0);
    CHECK_EQ(create(s, WID, BID, SCENE_ROLE_BUTTON,
                    120, 182, 80, 24,
                    SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE), 0);
    CHECK_EQ(create(s, WID, 41002, SCENE_ROLE_LABEL,
                    104, 56, 200, 16, SCENE_FLAG_VISIBLE), 0);
    CHECK_EQ(set_text(s, WID, 1, "AppWin"), 0);
}

/* ---- tests ------------------------------------------------------------ */

/* host_focus assigns the engine focus to a live node (exactly like the
 * focus assignment the engine does on InputActivate) and refuses
 * unknown, stale, dead, and replay-mode stores. */
static void test_host_focus(void)
{
    scene_store *s = scene_store_new(NULL);
    CHECK(s != NULL);
    build_scene(s);

    CHECK_EQ(scene_store_focus(s), SCENE_NO_PARENT);

    uint64_t seq_before = scene_store_committed_seq(s);
    CHECK_EQ(scene_store_host_focus(s, WID), 0);
    CHECK_EQ(scene_store_focus(s), WID);
    /* committed seq advanced through the engine's own commit path */
    CHECK_EQ(scene_store_committed_seq(s), seq_before + 1);
    CHECK_EQ(scene_store_committed_seq(s), scene_store_view_seq(s));

    /* refocusing the same node is a no-op: no extra commit */
    uint64_t seq2 = scene_store_committed_seq(s);
    CHECK_EQ(scene_store_host_focus(s, WID), 0);
    CHECK_EQ(scene_store_focus(s), WID);
    CHECK_EQ(scene_store_committed_seq(s), seq2);

    /* unknown id refuses */
    CHECK_EQ(scene_store_host_focus(s, 99999), -1);
    CHECK_EQ(scene_store_focus(s), WID);

    /* destroyed node (id gone) refuses */
    CHECK_EQ(destroy(s, BID), 0);
    CHECK_EQ(scene_store_host_focus(s, BID), -1);

    /* stale (ghost-crash) node refuses */
    CHECK_EQ(scene_store_ghost_mark(s), 0);
    CHECK_EQ(scene_store_host_focus(s, WID), -1);

    /* dead store refuses */
    CHECK_EQ(scene_store_fail(s, SCENE_ERR_STATE, "test"), 0);
    CHECK_EQ(scene_store_host_focus(s, WID), -1);

    scene_store_free(s);
    printf("test_host_focus: ok\n");
}

/* host_set_visible toggles SCENE_FLAG_VISIBLE on a live node and bumps
 * the committed seq visible to scene_store_node_vis, without ingest. */
static void test_host_visible(void)
{
    scene_store *s = scene_store_new(NULL);
    CHECK(s != NULL);
    build_scene(s);

    scene_node_vis v;
    CHECK_EQ(scene_store_node_vis(s, WID, &v), 0);
    CHECK(v.flags & SCENE_FLAG_VISIBLE);

    uint64_t seq_before = scene_store_committed_seq(s);
    CHECK_EQ(scene_store_host_set_visible(s, WID, 0), 0);
    CHECK_EQ(scene_store_committed_seq(s), seq_before + 1);
    CHECK_EQ(scene_store_node_vis(s, WID, &v), 0);
    CHECK(!(v.flags & SCENE_FLAG_VISIBLE));

    /* re-hiding a hidden node is a no-op */
    uint64_t seq2 = scene_store_committed_seq(s);
    CHECK_EQ(scene_store_host_set_visible(s, WID, 0), 0);
    CHECK_EQ(scene_store_committed_seq(s), seq2);

    CHECK_EQ(scene_store_host_set_visible(s, WID, 1), 0);
    CHECK_EQ(scene_store_committed_seq(s), seq2 + 1);
    CHECK_EQ(scene_store_node_vis(s, WID, &v), 0);
    CHECK(v.flags & SCENE_FLAG_VISIBLE);

    /* unknown / stale refuse */
    CHECK_EQ(scene_store_host_set_visible(s, 99999, 0), -1);
    CHECK_EQ(scene_store_ghost_mark(s), 0);
    CHECK_EQ(scene_store_host_set_visible(s, WID, 0), -1);

    /* dead store refuses */
    CHECK_EQ(scene_store_fail(s, SCENE_ERR_STATE, "test"), 0);
    CHECK_EQ(scene_store_host_set_visible(s, WID, 0), -1);

    scene_store_free(s);
    printf("test_host_visible: ok\n");
}

/* host_set_rect sets the absolute rect on a live node and bumps seq. */
static void test_host_rect(void)
{
    scene_store *s = scene_store_new(NULL);
    CHECK(s != NULL);
    build_scene(s);

    scene_node_vis v;
    CHECK_EQ(scene_store_node_vis(s, WID, &v), 0);
    CHECK_EQ(v.rect[0], 100);
    CHECK_EQ(v.rect[1], 50);
    CHECK_EQ(v.rect[2], 240);
    CHECK_EQ(v.rect[3], 160);

    uint64_t seq_before = scene_store_committed_seq(s);
    CHECK_EQ(scene_store_host_set_rect(s, WID, 30, 40, 120, 60), 0);
    CHECK_EQ(scene_store_committed_seq(s), seq_before + 1);
    CHECK_EQ(scene_store_node_vis(s, WID, &v), 0);
    CHECK_EQ(v.rect[0], 30);
    CHECK_EQ(v.rect[1], 40);
    CHECK_EQ(v.rect[2], 120);
    CHECK_EQ(v.rect[3], 60);

    /* an identical rect is a no-op */
    uint64_t seq2 = scene_store_committed_seq(s);
    CHECK_EQ(scene_store_host_set_rect(s, WID, 30, 40, 120, 60), 0);
    CHECK_EQ(scene_store_committed_seq(s), seq2);

    /* unknown / stale refuse */
    CHECK_EQ(scene_store_host_set_rect(s, 99999, 0, 0, 1, 1), -1);
    CHECK_EQ(scene_store_ghost_mark(s), 0);
    CHECK_EQ(scene_store_host_set_rect(s, WID, 1, 1, 1, 1), -1);

    /* dead store refuses */
    CHECK_EQ(scene_store_fail(s, SCENE_ERR_STATE, "test"), 0);
    CHECK_EQ(scene_store_host_set_rect(s, WID, 1, 1, 1, 1), -1);

    scene_store_free(s);
    printf("test_host_rect: ok\n");
}

/* All three host APIs refuse while the store is in replay mode (the
 * precedent: scene_store_seek_to refuses in live mode), and work again
 * after scene_store_end_replay. */
static void test_host_replay_refusal(void)
{
    scene_store *s = scene_store_new(NULL);
    CHECK(s != NULL);
    build_scene(s);

    CHECK_EQ(scene_store_begin_replay(s), 0);
    CHECK_EQ(scene_store_host_focus(s, WID), -1);
    CHECK_EQ(scene_store_host_set_visible(s, WID, 0), -1);
    CHECK_EQ(scene_store_host_set_rect(s, WID, 1, 1, 1, 1), -1);

    CHECK_EQ(scene_store_end_replay(s), 0);
    CHECK_EQ(scene_store_host_focus(s, WID), 0);
    CHECK_EQ(scene_store_host_set_visible(s, WID, 0), 0);
    CHECK_EQ(scene_store_host_set_rect(s, WID, 1, 1, 1, 1), 0);

    scene_store_free(s);
    printf("test_host_replay_refusal: ok\n");
}

/* Determinism: two stores built with identical op sequences and hit with
 * identical host mutations commit identical seqs and end in identical
 * visible state. */
static void test_host_determinism(void)
{
    scene_store *a = scene_store_new(NULL);
    scene_store *b = scene_store_new(NULL);
    CHECK(a != NULL);
    CHECK(b != NULL);
    build_scene(a);
    build_scene(b);

    /* same host mutation sequence on both stores. A call that changes
     * nothing is a no-op (0, no seq bump — as pinned by test_host_visible
     * and test_host_rect); an effective mutation bumps the committed seq
     * by exactly 1. The per-k expectation table reflects engine truth:
     *   k: 0 focus(new)  1 vis(noop)  2 rect   3 focus(noop)
     *      4 vis(off)    5 rect       6 focus(noop)
     *      7 vis(on)     8 rect       9 focus(noop)                        */
    static const uint8_t expect_bump[10] = {1, 0, 1, 0, 1, 1, 0, 1, 1, 0};
    unsigned int k;
    uint64_t sa, sb;
    for (k = 0; k < 10; k++) {
        sa = scene_store_committed_seq(a);
        sb = scene_store_committed_seq(b);
        CHECK_EQ(sa, sb);
        switch (k % 3) {
        case 0: CHECK_EQ(scene_store_host_focus(a, WID), 0);
                CHECK_EQ(scene_store_host_focus(b, WID), 0); break;
        case 1: CHECK_EQ(scene_store_host_set_visible(a, WID, k % 2), 0);
                CHECK_EQ(scene_store_host_set_visible(b, WID, k % 2), 0);
                break;
        default: CHECK_EQ(scene_store_host_set_rect(a, WID,
                          (int32_t)k * 10, 20, 300, 180), 0);
                 CHECK_EQ(scene_store_host_set_rect(b, WID,
                          (int32_t)k * 10, 20, 300, 180), 0);
                 break;
        }
        /* both stores advance identically: +1 iff the mutation was
         * effective, 0 for a no-op */
        CHECK_EQ(scene_store_committed_seq(a), sa + expect_bump[k]);
        CHECK_EQ(scene_store_committed_seq(b), sb + expect_bump[k]);
    }
    CHECK_EQ(scene_store_committed_seq(a), scene_store_committed_seq(b));

    /* end state identical through the read view */
    scene_node_vis na, nb;
    CHECK_EQ(scene_store_node_vis(a, WID, &na), 0);
    CHECK_EQ(scene_store_node_vis(b, WID, &nb), 0);
    CHECK_EQ(na.rect[0], nb.rect[0]);
    CHECK_EQ(na.rect[1], nb.rect[1]);
    CHECK_EQ(na.rect[2], nb.rect[2]);
    CHECK_EQ(na.rect[3], nb.rect[3]);
    CHECK_EQ(na.flags, nb.flags);
    CHECK_EQ(scene_store_focus(a), scene_store_focus(b));

    scene_store_free(a);
    scene_store_free(b);
    printf("test_host_determinism: ok\n");
}

int main(void)
{
    test_host_focus();
    test_host_visible();
    test_host_rect();
    test_host_replay_refusal();
    test_host_determinism();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}