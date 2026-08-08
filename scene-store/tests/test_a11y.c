/*
 * test_a11y.c — black-box tests for the a11y consumer layer.
 *
 * Builds a 7-node UI (window, panel, Open button, Save button,
 * label "Hello World", text input), then exercises:
 *   - per-node a11y query
 *   - full tree walk
 *   - focus chain
 *   - role-based search
 *   - child enumeration
 *   - focused node query
 */
#include "scene_a11y.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int checks, failures;
static uint64_t test_seq;

#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)
#define CHECK_EQ(a, b) do { \
    checks++; \
    if ((a) != (b)) { failures++; printf("FAIL %s:%d: %s (%u) != %s (%u)\n", \
        __FILE__, __LINE__, #a, (unsigned)(a), #b, (unsigned)(b)); } \
} while(0)

static void create(scene_store *s, scene_node_id parent, scene_node_id id,
                   uint16_t role, int32_t x, int32_t y, int32_t w, int32_t h,
                   uint8_t flags)
{
    uint8_t b[35];
    memset(b, 0, sizeof(b));
    scene_put_u64(b + 0, test_seq++);
    scene_put_u32(b + 8, parent);
    scene_put_u32(b + 12, id);
    scene_put_u16(b + 16, role);
    scene_put_i32(b + 18, x);
    scene_put_i32(b + 22, y);
    scene_put_i32(b + 26, w);
    scene_put_i32(b + 30, h);
    b[34] = flags;
    scene_store_ingest(s, SCENE_OP_CREATE_NODE, b, 35);
}

static void set_text(scene_store *s, scene_node_id id, scene_text_id tid,
                     const char *text, uint32_t len)
{
    uint8_t b[128];
    memset(b, 0, 20);
    scene_put_u64(b + 0, test_seq++);
    scene_put_u32(b + 8, id);
    scene_put_u32(b + 12, tid);
    scene_put_u32(b + 16, len);
    memcpy(b + 20, text, len);
    scene_store_ingest(s, SCENE_OP_SET_TEXT, b, 20 + len);
}

static void build_app(scene_store *s)
{
    test_seq = 1;
    create(s, SCENE_NO_PARENT, 100, SCENE_ROLE_WINDOW,   10,  10, 400, 300, SCENE_FLAG_VISIBLE);
    create(s, 100, 101, SCENE_ROLE_PANEL,   10,  40, 400, 270, SCENE_FLAG_VISIBLE);
    create(s, 101, 200, SCENE_ROLE_BUTTON,  30,  60, 100,  30, SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    set_text(s, 200, 1, "Open", 4);
    create(s, 101, 201, SCENE_ROLE_BUTTON, 140,  60, 100,  30, SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    set_text(s, 201, 1, "Save", 4);
    create(s, 101, 202, SCENE_ROLE_LABEL,   30, 100, 200,  20, SCENE_FLAG_VISIBLE);
    set_text(s, 202, 1, "Hello World", 11);
    create(s, 101, 203, SCENE_ROLE_TEXTFIELD, 30, 130, 200, 24,
           SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    set_text(s, 203, 1, "initial", 7);
}

/* ---- Tests ----------------------------------------------------------- */

static void test_a11y_node_query(void)
{
    scene_store *s = scene_store_new(NULL);
    build_app(s);
    scene_a11y_node an;

    /* Open button */
    CHECK_EQ(scene_store_a11y_node(s, 200, &an), 0);
    CHECK_EQ(an.id, 200);
    CHECK_EQ(an.role, SCENE_ROLE_BUTTON);
    CHECK_EQ(an.flags & SCENE_FLAG_VISIBLE, SCENE_FLAG_VISIBLE);
    CHECK_EQ(an.flags & SCENE_FLAG_FOCUSABLE, SCENE_FLAG_FOCUSABLE);
    CHECK_EQ(an.rect[0], 30);
    CHECK_EQ(an.rect[1], 60);
    CHECK_EQ(an.rect[2], 100);
    CHECK_EQ(an.rect[3], 30);
    CHECK_EQ(an.child_count, 0);
    CHECK(an.primary_text_len == 4);
    CHECK(memcmp(an.primary_text, "Open", 4) == 0);

    /* Window (has children) */
    CHECK_EQ(scene_store_a11y_node(s, 100, &an), 0);
    CHECK_EQ(an.role, SCENE_ROLE_WINDOW);
    CHECK_EQ(an.child_count, 1);  /* panel */

    /* Panel (has children) */
    CHECK_EQ(scene_store_a11y_node(s, 101, &an), 0);
    CHECK_EQ(an.role, SCENE_ROLE_PANEL);
    CHECK_EQ(an.child_count, 4);  /* Open, Save, label, input */

    /* Unknown node */
    CHECK_EQ(scene_store_a11y_node(s, 999, &an), -1);

    scene_store_free(s);
    printf("test_a11y_node_query: ok\n");
}

struct count_ctx { int count; };

static int count_cb(const scene_a11y_node *node, void *ud)
{
    (void)node;
    struct count_ctx *c = ud;
    c->count++;
    return 0;
}

static void test_a11y_walk(void)
{
    scene_store *s = scene_store_new(NULL);
    build_app(s);
    struct count_ctx ctx = {0};
    scene_store_a11y_walk(s, count_cb, &ctx);
    /* 7 nodes: window, panel, Open, Save, label, input = 6... plus? */
    CHECK_EQ(ctx.count, 6);
    scene_store_free(s);
    printf("test_a11y_walk: ok\n");
}

struct collect_ctx {
    scene_node_id ids[16];
    size_t count;
};

static int collect_cb(const scene_a11y_node *node, void *ud)
{
    struct collect_ctx *c = ud;
    if (c->count < 16) c->ids[c->count] = node->id;
    c->count++;
    return 0;
}

static void test_a11y_focus_chain(void)
{
    scene_store *s = scene_store_new(NULL);
    build_app(s);

    /* No focus yet */
    scene_a11y_node fn;
    CHECK_EQ(scene_store_a11y_focused(s, &fn), -1);

    /* Focus Open button */
    uint8_t b[12];
    memset(b, 0, 12);
    scene_put_u64(b + 0, test_seq++);
    scene_put_u32(b + 8, 200);
    scene_store_ingest(s, SCENE_OP_FOCUS, b, 12);

    CHECK_EQ(scene_store_a11y_focused(s, &fn), 0);
    CHECK_EQ(fn.id, 200);
    CHECK_EQ(fn.role, SCENE_ROLE_BUTTON);

    /* Focus chain: visible+focusable nodes in document order */
    scene_node_id chain[16];
    size_t n = scene_store_a11y_focus_chain(s, chain, 16);
    CHECK_EQ(n, 3);  /* Open, Save, input */
    CHECK_EQ(chain[0], 200);
    CHECK_EQ(chain[1], 201);
    CHECK_EQ(chain[2], 203);

    scene_store_free(s);
    printf("test_a11y_focus_chain: ok\n");
}

static void test_a11y_role_search(void)
{
    scene_store *s = scene_store_new(NULL);
    build_app(s);

    scene_node_id ids[16];
    size_t n;

    /* Find all buttons */
    n = scene_store_a11y_find_by_role(s, SCENE_ROLE_BUTTON, ids, 16);
    CHECK_EQ(n, 2);
    CHECK_EQ(ids[0], 200);
    CHECK_EQ(ids[1], 201);

    /* Find all labels */
    n = scene_store_a11y_find_by_role(s, SCENE_ROLE_LABEL, ids, 16);
    CHECK_EQ(n, 1);
    CHECK_EQ(ids[0], 202);

    /* Find all windows */
    n = scene_store_a11y_find_by_role(s, SCENE_ROLE_WINDOW, ids, 16);
    CHECK_EQ(n, 1);
    CHECK_EQ(ids[0], 100);

    /* Find non-existent role */
    n = scene_store_a11y_find_by_role(s, SCENE_ROLE_MENU, ids, 16);
    CHECK_EQ(n, 0);

    scene_store_free(s);
    printf("test_a11y_role_search: ok\n");
}

static void test_a11y_children(void)
{
    scene_store *s = scene_store_new(NULL);
    build_app(s);

    scene_node_id ids[16];
    size_t n;

    /* Children of window (100) */
    n = scene_store_a11y_children(s, 100, ids, 16);
    CHECK_EQ(n, 1);
    CHECK_EQ(ids[0], 101);

    /* Children of panel (101) */
    n = scene_store_a11y_children(s, 101, ids, 16);
    CHECK_EQ(n, 4);

    /* Children of leaf node */
    n = scene_store_a11y_children(s, 200, ids, 16);
    CHECK_EQ(n, 0);

    /* Children of unknown node */
    n = scene_store_a11y_children(s, 999, ids, 16);
    CHECK_EQ(n, 0);

    scene_store_free(s);
    printf("test_a11y_children: ok\n");
}

static void test_a11y_visibility_filter(void)
{
    scene_store *s = scene_store_new(NULL);
    build_app(s);

    /* Hide the Open button */
    uint8_t b[13];
    memset(b, 0, 13);
    scene_put_u64(b + 0, test_seq++);
    scene_put_u32(b + 8, 200);
    b[12] = 0;
    scene_store_ingest(s, SCENE_OP_SET_FLAGS, b, 13);

    /* Invisible node should not appear in walk */
    struct collect_ctx ctx = { {0}, 0 };
    scene_store_a11y_walk(s, collect_cb, &ctx);
    for (size_t i = 0; i < ctx.count; i++)
        CHECK(ctx.ids[i] != 200);

    /* Not in focus chain */
    scene_node_id chain[16];
    size_t n = scene_store_a11y_focus_chain(s, chain, 16);
    CHECK_EQ(n, 2);  /* Save, input only */

    /* Not in role search */
    scene_node_id ids[16];
    n = scene_store_a11y_find_by_role(s, SCENE_ROLE_BUTTON, ids, 16);
    CHECK_EQ(n, 1);
    CHECK_EQ(ids[0], 201);

    /* Not in children list */
    n = scene_store_a11y_children(s, 101, ids, 16);
    CHECK_EQ(n, 3);  /* Save, label, input only */

    scene_store_free(s);
    printf("test_a11y_visibility_filter: ok\n");
}

int main(void)
{
    test_a11y_node_query();
    test_a11y_walk();
    test_a11y_focus_chain();
    test_a11y_role_search();
    test_a11y_children();
    test_a11y_visibility_filter();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
