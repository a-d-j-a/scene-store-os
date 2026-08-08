/*
 * test_rewind.c — black-box tests for the rewind (deterministic replay) service.
 *
 * Builds a scene, then exercises:
 *   - enter/exit replay mode
 *   - seek forward and backward
 *   - step forward and backward
 *   - tell/head/tail queries
 *   - diff between two seq points
 *   - determinism: same ops → same diffs
 */
#include "scene_rewind.h"
#include "scene_a11y.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int checks, failures;
static uint64_t seq;

#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)
#define CHECK_EQ(a, b) do { \
    checks++; \
    if ((a) != (b)) { failures++; printf("FAIL %s:%d: %s (%lu) != %s (%lu)\n", \
        __FILE__, __LINE__, #a, (unsigned long)(a), #b, (unsigned long)(b)); } \
} while(0)

static void cr(scene_store *s, uint32_t p, uint32_t id, uint16_t r,
               int32_t x, int32_t y, int32_t w, int32_t h, uint8_t f)
{
    uint8_t b[35]; memset(b, 0, 35);
    scene_put_u64(b, seq++);
    scene_put_u32(b + 8, p); scene_put_u32(b + 12, id);
    scene_put_u16(b + 16, r);
    scene_put_i32(b + 18, x); scene_put_i32(b + 22, y);
    scene_put_i32(b + 26, w); scene_put_i32(b + 30, h);
    b[34] = f;
    scene_store_ingest(s, SCENE_OP_CREATE_NODE, b, 35);
}

static void st(scene_store *s, uint32_t id, uint32_t tid,
               const char *t, uint32_t len)
{
    uint8_t b[128]; memset(b, 0, 20);
    scene_put_u64(b, seq++);
    scene_put_u32(b + 8, id); scene_put_u32(b + 12, tid);
    scene_put_u32(b + 16, len);
    memcpy(b + 20, t, len);
    scene_store_ingest(s, SCENE_OP_SET_TEXT, b, 20 + len);
}

static void build_app(scene_store *s)
{
    seq = 1;
    cr(s, SCENE_NO_PARENT, 100, SCENE_ROLE_WINDOW,   10,  10, 400, 300, SCENE_FLAG_VISIBLE);
    cr(s, 100, 101, SCENE_ROLE_PANEL,   10,  40, 400, 270, SCENE_FLAG_VISIBLE);
    cr(s, 101, 200, SCENE_ROLE_BUTTON,  30,  60, 100,  30, SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    st(s, 200, 1, "Open", 4);
    cr(s, 101, 201, SCENE_ROLE_BUTTON, 140,  60, 100,  30, SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    st(s, 201, 1, "Save", 4);
    cr(s, 101, 202, SCENE_ROLE_LABEL,   30, 100, 200,  20, SCENE_FLAG_VISIBLE);
    st(s, 202, 1, "Hello", 5);
}

/* ---- Tests ----------------------------------------------------------- */

static void test_rewind_enter_exit(void)
{
    scene_store *s = scene_store_new(NULL);
    build_app(s);
    scene_rewind *rw = scene_rewind_new(s);
    CHECK(rw != NULL);

    /* Enter replay mode */
    CHECK_EQ(scene_rewind_enter_replay(rw), 0);
    CHECK(scene_store_in_replay(s));

    /* Exit replay mode */
    CHECK_EQ(scene_rewind_exit_replay(rw), 0);
    CHECK(!scene_store_in_replay(s));

    scene_rewind_free(rw);
    scene_store_free(s);
    printf("test_rewind_enter_exit: ok\n");
}

static void test_rewind_seek(void)
{
    scene_store *s = scene_store_new(NULL);
    build_app(s);
    scene_rewind *rw = scene_rewind_new(s);
    scene_rewind_enter_replay(rw);

    /* Initial position is 0 */
    CHECK_EQ(scene_rewind_tell(rw), 0);
    CHECK_EQ(scene_rewind_head(rw), 8); /* 8 ops: 4 creates + 4 texts */
    CHECK_EQ(scene_rewind_tail(rw), 0);

    /* Seek to seq 4 */
    CHECK_EQ(scene_rewind_seek(rw, 4), 0);
    CHECK_EQ(scene_rewind_tell(rw), 4);

    /* Verify: scene at seq 4 should have some nodes */
    scene_a11y_node an;
    CHECK_EQ(scene_store_a11y_node(s, 100, &an), 0); /* window exists */
    CHECK_EQ(scene_store_a11y_node(s, 200, &an), 0); /* Open button exists */

    /* Seek backward to seq 2 */
    CHECK_EQ(scene_rewind_seek(rw, 2), 0);
    CHECK_EQ(scene_rewind_tell(rw), 2);

    /* Return to live */
    scene_rewind_exit_replay(rw);

    scene_rewind_free(rw);
    scene_store_free(s);
    printf("test_rewind_seek: ok\n");
}

static void test_rewind_step(void)
{
    scene_store *s = scene_store_new(NULL);
    build_app(s);
    scene_rewind *rw = scene_rewind_new(s);
    scene_rewind_enter_replay(rw);

    /* Step forward 3 */
    CHECK_EQ(scene_rewind_step_forward(rw, 3), 0);
    CHECK_EQ(scene_rewind_tell(rw), 3);

    /* Step forward 2 more */
    CHECK_EQ(scene_rewind_step_forward(rw, 2), 0);
    CHECK_EQ(scene_rewind_tell(rw), 5);

    /* Step backward 4 */
    CHECK_EQ(scene_rewind_step_backward(rw, 4), 0);
    CHECK_EQ(scene_rewind_tell(rw), 1);

    /* Step backward 10 — clamped at 1 */
    CHECK_EQ(scene_rewind_step_backward(rw, 10), 0);
    CHECK_EQ(scene_rewind_tell(rw), 1);

    /* Step forward past head — clamped at head */
    CHECK_EQ(scene_rewind_step_forward(rw, 100), 0);
    CHECK_EQ(scene_rewind_tell(rw), scene_rewind_head(rw));

    scene_rewind_exit_replay(rw);
    scene_rewind_free(rw);
    scene_store_free(s);
    printf("test_rewind_step: ok\n");
}

static void test_rewind_diff_create_destroy(void)
{
    scene_store *s = scene_store_new(NULL);
    build_app(s);
    scene_rewind *rw = scene_rewind_new(s);
    scene_rewind_enter_replay(rw);

    /* At seq 2: window + panel only (2 ops).
     * At seq 8: full app (8 ops).
     * Diff should show nodes created between seq 2 and seq 8. */
    CHECK_EQ(scene_rewind_seek(rw, 2), 0);
    scene_rewind_diff_result d;
    memset(&d, 0, sizeof(d));
    CHECK_EQ(scene_rewind_diff(rw, 8, &d), 0);

    /* Nodes in seq 7 but not seq 2 = created */
    CHECK(d.created_count > 0);
    /* Nodes in seq 2 but not seq 7 = none (we only added, never removed) */
    CHECK_EQ(d.destroyed_count, 0);

    scene_rewind_diff_free(&d);
    scene_rewind_exit_replay(rw);
    scene_rewind_free(rw);
    scene_store_free(s);
    printf("test_rewind_diff_create_destroy: ok\n");
}

static void test_rewind_diff_modify(void)
{
    scene_store *s = scene_store_new(NULL);
    build_app(s);

    /* After building, change text of node 200 from "Open" to "Opened" */
    st(s, 200, 1, "Opened", 6);

    scene_rewind *rw = scene_rewind_new(s);
    scene_rewind_enter_replay(rw);

    /* Diff between seq 4 (before text change) and seq 9 (after) */
    CHECK_EQ(scene_rewind_seek(rw, 4), 0);
    scene_rewind_diff_result d;
    memset(&d, 0, sizeof(d));
    CHECK_EQ(scene_rewind_diff(rw, 9, &d), 0);

    /* Node 200 should appear in modified (text changed) */
    int found = 0;
    uint32_t i;
    for (i = 0; i < d.modified_count; i++) {
        if (d.modified[i].id == 200) { found = 1; break; }
    }
    CHECK(found);

    scene_rewind_diff_free(&d);
    scene_rewind_exit_replay(rw);
    scene_rewind_free(rw);
    scene_store_free(s);
    printf("test_rewind_diff_modify: ok\n");
}

static void test_rewind_diff_determinism(void)
{
    /* Test: can two independent stores both enter replay? */
    scene_store *a = scene_store_new(NULL);
    scene_store *b = scene_store_new(NULL);
    build_app(a);
    seq = 1;
    build_app(b);

    /* Add a text change to both (reset seq for each store) */
    seq = 9;
    st(a, 200, 1, "Opened", 6);
    seq = 9;
    st(b, 200, 1, "Opened", 6);

    CHECK_EQ(scene_store_committed_seq(a), scene_store_committed_seq(b));
    CHECK_EQ(scene_store_node_count(a), scene_store_node_count(b));

    /* Enter replay on both stores independently */
    scene_rewind *ra = scene_rewind_new(a);
    scene_rewind *rb = scene_rewind_new(b);
    CHECK_EQ(scene_rewind_enter_replay(ra), 0);
    CHECK_EQ(scene_store_in_replay(a), 1);
    CHECK_EQ(scene_rewind_enter_replay(rb), 0);
    CHECK_EQ(scene_store_in_replay(b), 1);

    /* Get diffs from both */
    CHECK_EQ(scene_rewind_seek(ra, 4), 0);
    CHECK_EQ(scene_rewind_seek(rb, 4), 0);
    scene_rewind_diff_result da, db;
    memset(&da, 0, sizeof(da));
    memset(&db, 0, sizeof(db));
    CHECK_EQ(scene_rewind_diff(ra, 9, &da), 0);
    CHECK_EQ(scene_rewind_diff(rb, 9, &db), 0);

    CHECK_EQ(da.created_count, db.created_count);
    CHECK_EQ(da.destroyed_count, db.destroyed_count);
    CHECK_EQ(da.modified_count, db.modified_count);

    if (da.modified_count == db.modified_count) {
        uint32_t i;
        for (i = 0; i < da.modified_count; i++)
            CHECK_EQ(da.modified[i].id, db.modified[i].id);
    }

    scene_rewind_diff_free(&da);
    scene_rewind_diff_free(&db);
    scene_rewind_exit_replay(ra);
    scene_rewind_exit_replay(rb);
    scene_rewind_free(ra);
    scene_rewind_free(rb);
    scene_store_free(a);
    scene_store_free(b);
    printf("test_rewind_diff_determinism: ok\n");
}

int main(void)
{
    test_rewind_enter_exit();
    test_rewind_seek();
    test_rewind_step();
    test_rewind_diff_create_destroy();
    test_rewind_diff_modify();
    test_rewind_diff_determinism();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
