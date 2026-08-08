/*
 * test_automation.c — automation consumer: cross-app UI automation via
 * the scene-engine wire protocol.
 *
 * Demonstrates the OS-level automation service: an external tool connects
 * to the scene store, discovers nodes by text/role/region, injects input,
 * records macros, and verifies state — all through the locked v0 protocol.
 *
 * This is the fourth consumer (after compositor, effects, search).
 */
#include "scene_client.h"
#include "scene_compositor.h"
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

/* ---- captured events -------------------------------------------------- */

#define MAXE 32

struct harness {
    scene_loopback  *lb;
    scene_transport *server_ts;
    scene_client    *cl;
    scene_compositor *cp;

    /* WELCOME */
    int wel_called; uint32_t wel_sid;

    /* SEARCH_RESULT */
    int sr_calls; uint32_t sr_req, sr_count;
    scene_search_hit sr_hits[MAXE];

    /* TEXT_INDEX */
    int ti_calls; uint32_t ti_count;
    scene_text_hit ti_hits[MAXE];

    /* INPUT_ACTIVATE */
    int act_calls; uint32_t act_id[MAXE];

    /* INPUT_POINTER */
    int ptr_calls; int32_t ptr_xy[MAXE][2]; uint8_t ptr_btn[MAXE];

    /* INPUT_FOCUS */
    int foc_calls; uint32_t foc_id[MAXE]; uint8_t foc_state[MAXE];

    /* closed */
    int closed_calls;
};

static void cb_welcome(void *ud, uint32_t sid, uint16_t ver,
                        const scene_limits *lim)
{
    struct harness *h = ud;
    (void)ver; (void)lim;
    h->wel_called++;
    h->wel_sid = sid;
}

static void cb_search_result(void *ud, uint32_t req_id, uint32_t count,
                              const scene_search_hit *hits)
{
    struct harness *h = ud;
    h->sr_req = req_id;
    h->sr_count = count;
    if (count > MAXE) count = MAXE;
    if (hits && count) memcpy(h->sr_hits, hits, count * sizeof(*hits));
    h->sr_calls++;
}

static void cb_text_index(void *ud, const scene_text_hit *entries,
                           uint32_t count)
{
    struct harness *h = ud;
    h->ti_count = count;
    if (count > MAXE) count = MAXE;
    if (entries && count) memcpy(h->ti_hits, entries, count * sizeof(*entries));
    h->ti_calls++;
}

static void cb_input_activate(void *ud, uint64_t seq, scene_node_id id)
{
    struct harness *h = ud;
    (void)seq;
    if (h->act_calls < MAXE) h->act_id[h->act_calls] = id;
    h->act_calls++;
}

static void cb_input_pointer(void *ud, uint64_t seq, uint8_t device,
                              int32_t x, int32_t y, uint8_t buttons)
{
    struct harness *h = ud;
    (void)seq; (void)device;
    if (h->ptr_calls < MAXE) {
        h->ptr_xy[h->ptr_calls][0] = x;
        h->ptr_xy[h->ptr_calls][1] = y;
        h->ptr_btn[h->ptr_calls] = buttons;
    }
    h->ptr_calls++;
}

static void cb_input_focus(void *ud, uint64_t seq, scene_node_id id,
                            uint8_t state)
{
    struct harness *h = ud;
    (void)seq;
    if (h->foc_calls < MAXE) {
        h->foc_id[h->foc_calls] = id;
        h->foc_state[h->foc_calls] = state;
    }
    h->foc_calls++;
}

static void cb_closed(void *ud)
{
    struct harness *h = ud;
    h->closed_calls++;
}

static const scene_client_cbs test_cbs = {
    .welcome       = cb_welcome,
    .search_result = cb_search_result,
    .text_index    = cb_text_index,
    .input_activate = cb_input_activate,
    .input_pointer = cb_input_pointer,
    .input_focus   = cb_input_focus,
    .closed        = cb_closed,
};

/* ---- helpers ---------------------------------------------------------- */

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
    scene_compositor_frame(h->cp);
}

static void harness_init(struct harness *h)
{
    memset(h, 0, sizeof(*h));
    h->lb = scene_loopback_new();
    h->server_ts = scene_loopback_server_end(h->lb);
    h->cl = scene_client_new();
    h->cp = scene_compositor_new(NULL, 800, 600);
    scene_server_attach(scene_compositor_server(h->cp));
    scene_client_connect(h->cl, scene_loopback_client_end(h->lb),
                         "auto", &test_cbs, h);
    /* pump WELCOME */
    tickf(h);
}

static void op_ok(struct harness *h __attribute__((unused)), int r, const char *what)
{
    if (r != 0) {
        failures++;
        printf("FAIL %s: op %s returned %d\n", __FILE__, what, r);
    }
    checks++;
}

static void harness_free(struct harness *h)
{
    scene_client_free(h->cl);
    scene_compositor_free(h->cp);
    scene_loopback_free(h->lb);
}

/* ---- build a sample app UI -------------------------------------------- */

static void build_app(struct harness *h)
{
    static const scene_rect r_win = {10, 10, 400, 300};
    static const scene_rect r_pan = {10, 40, 400, 270};
    static const scene_rect r_btn_open = {30, 60, 100, 30};
    static const scene_rect r_btn_save = {140, 60, 100, 30};
    static const scene_rect r_label   = {30, 100, 200, 20};
    static const scene_rect r_input   = {30, 130, 200, 24};

    /* Window */
    op_ok(h, scene_client_create_node(h->cl, SCENE_NO_PARENT, 100,
            SCENE_ROLE_WINDOW, &r_win, SCENE_FLAG_VISIBLE), "win");
    /* Panel */
    op_ok(h, scene_client_create_node(h->cl, 100, 101,
            SCENE_ROLE_PANEL, &r_pan, SCENE_FLAG_VISIBLE), "pan");
    /* Buttons */
    op_ok(h, scene_client_create_node(h->cl, 101, 200,
            SCENE_ROLE_BUTTON, &r_btn_open,
            SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE), "btn open");
    op_ok(h, scene_client_set_text(h->cl, 200, 1, "Open", 4), "text open");

    op_ok(h, scene_client_create_node(h->cl, 101, 201,
            SCENE_ROLE_BUTTON, &r_btn_save,
            SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE), "btn save");
    op_ok(h, scene_client_set_text(h->cl, 201, 1, "Save", 4), "text save");

    /* Label */
    op_ok(h, scene_client_create_node(h->cl, 101, 202,
            SCENE_ROLE_LABEL, &r_label, SCENE_FLAG_VISIBLE), "label");
    op_ok(h, scene_client_set_text(h->cl, 202, 1, "Hello World", 11),
          "text label");

    /* Text input (editable field) */
    op_ok(h, scene_client_create_node(h->cl, 101, 203,
            SCENE_ROLE_LABEL, &r_input,
            SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE), "input");
    op_ok(h, scene_client_set_text(h->cl, 203, 1, "initial", 7),
          "text input");
}

/* ==================================================================== */
/* Tests                                                                */
/* ==================================================================== */

/* Test 1: Discover nodes by text search.
 * An automation tool searches for "Open" and gets back the button node,
 * then searches for "Save" and gets back that button.                     */
static void test_auto_discover_by_text(void)
{
    struct harness h;
    harness_init(&h);
    build_app(&h);
    tickf(&h);

    /* Drain TEXT_INDEX records that arrive from the build_app ops. */
    h.ti_calls = 0;

    /* Search for "Open" → should find node 200 */
    op_ok(&h, scene_client_search(h.cl, 1, "Open", 4), "search open");
    tickf(&h);
    CHECK_EQ(h.sr_calls, 1);
    CHECK_EQ(h.sr_req, 1);
    CHECK_EQ(h.sr_count, 1);
    CHECK_EQ(h.sr_hits[0].id, 200);
    CHECK_EQ(h.sr_hits[0].role, SCENE_ROLE_BUTTON);

    /* Search for "Save" → should find node 201 */
    h.sr_calls = 0;
    op_ok(&h, scene_client_search(h.cl, 2, "Save", 4), "search save");
    tickf(&h);
    CHECK_EQ(h.sr_calls, 1);
    CHECK_EQ(h.sr_req, 2);
    CHECK_EQ(h.sr_count, 1);
    CHECK_EQ(h.sr_hits[0].id, 201);

    /* Search for "Hello" → should find the label */
    h.sr_calls = 0;
    op_ok(&h, scene_client_search(h.cl, 3, "Hello", 5), "search hello");
    tickf(&h);
    CHECK_EQ(h.sr_count, 1);
    CHECK_EQ(h.sr_hits[0].id, 202);
    CHECK_EQ(h.sr_hits[0].role, SCENE_ROLE_LABEL);

    /* Search for nonexistent → 0 hits */
    h.sr_calls = 0;
    op_ok(&h, scene_client_search(h.cl, 4, "zzzzz", 5), "search miss");
    tickf(&h);
    CHECK_EQ(h.sr_count, 0);

    printf("test_auto_discover_by_text: ok\n");
    harness_free(&h);
}

/* Test 2: Click a discovered node.
 * Search for "Open", click at its center, verify the click arrives at
 * the server with the correct coordinates.                               */
static void test_auto_click_discovered(void)
{
    struct harness h;
    harness_init(&h);
    build_app(&h);
    tickf(&h);
    h.ti_calls = 0;

    /* Find the "Open" button */
    op_ok(&h, scene_client_search(h.cl, 1, "Open", 4), "search");
    tickf(&h);
    CHECK_EQ(h.sr_count, 1);
    scene_node_id open_btn = h.sr_hits[0].id;

    /* Click at the center of the button: rect {30,60,100,30} → center (80,75) */
    int32_t cx = h.sr_hits[0].rect.x + h.sr_hits[0].rect.w / 2;
    int32_t cy = h.sr_hits[0].rect.y + h.sr_hits[0].rect.h / 2;
    CHECK_EQ(cx, 80);
    CHECK_EQ(cy, 75);

    /* Forward the click to the compositor (simulates automation input) */
    scene_compositor_input_pointer(h.cp, 0, cx, cy, 0x01);
    tickf(&h);

    /* The compositor should have forwarded the pointer event to the server,
     * which resolves it to the node under the cursor. The server sends
     * INPUT_ACTIVATE back to the client. */
    CHECK(h.act_calls >= 1);
    CHECK_EQ(h.act_id[h.act_calls - 1], open_btn);

    printf("test_auto_click_discovered: ok\n");
    harness_free(&h);
}

/* Test 3: Type into a field.
 * Find the text input field by search, then update its text to simulate
 * typing "typed_text" into it. Verify the text changed.                  */
static void test_auto_type_into_field(void)
{
    struct harness h;
    harness_init(&h);
    build_app(&h);
    tickf(&h);
    h.ti_calls = 0;

    /* Find the input field by its initial text */
    op_ok(&h, scene_client_search(h.cl, 1, "initial", 7), "search input");
    tickf(&h);
    CHECK_EQ(h.sr_count, 1);
    CHECK_EQ(h.sr_hits[0].id, 203);

    /* Type "typed_text" into it (simulate: set_text replaces content) */
    op_ok(&h, scene_client_set_text(h.cl, 203, 1, "typed_text", 10),
          "type into field");
    tickf(&h);

    /* Verify: search for the new text → should find node 203 */
    h.sr_calls = 0;
    op_ok(&h, scene_client_search(h.cl, 2, "typed_text", 10),
          "search typed");
    tickf(&h);
    CHECK_EQ(h.sr_count, 1);
    CHECK_EQ(h.sr_hits[0].id, 203);

    /* Verify: search for the old text → should NOT find it */
    h.sr_calls = 0;
    op_ok(&h, scene_client_search(h.cl, 3, "initial", 7),
          "search old text");
    tickf(&h);
    CHECK_EQ(h.sr_count, 0);

    printf("test_auto_type_into_field: ok\n");
    harness_free(&h);
}

/* Test 4: Record and replay a macro.
 * Record: set_text on two nodes. Replay the macro. Verify both
 * mutations occurred by searching for the new text.                    */
static void test_auto_macro_record_replay(void)
{
    struct harness h;
    harness_init(&h);
    build_app(&h);
    tickf(&h);
    h.ti_calls = 0;

    /* Record a macro: change text of node 200 to "MacroA" and 201 to "MacroB" */
    op_ok(&h, scene_client_macro_begin(h.cl, 42), "macro begin");
    op_ok(&h, scene_client_set_text(h.cl, 200, 1, "MacroA", 6), "text open");
    op_ok(&h, scene_client_set_text(h.cl, 201, 1, "MacroB", 6), "text save");
    op_ok(&h, scene_client_macro_end(h.cl, 42), "macro end");
    tickf(&h);

    /* Execute the macro */
    op_ok(&h, scene_client_exec_macro(h.cl, 42), "exec macro");
    tickf(&h);

    /* Verify: search for "MacroA" → should find node 200 */
    h.sr_calls = 0;
    op_ok(&h, scene_client_search(h.cl, 1, "MacroA", 6), "search macroa");
    tickf(&h);
    CHECK_EQ(h.sr_count, 1);
    CHECK_EQ(h.sr_hits[0].id, 200);

    /* Verify: search for "MacroB" → should find node 201 */
    h.sr_calls = 0;
    op_ok(&h, scene_client_search(h.cl, 2, "MacroB", 6), "search macrob");
    tickf(&h);
    CHECK_EQ(h.sr_count, 1);
    CHECK_EQ(h.sr_hits[0].id, 201);

    printf("test_auto_macro_record_replay: ok\n");
    harness_free(&h);
}

/* Test 5: Cross-node workflow.
 * Full automation sequence: discover → click → verify state change →
 * type → verify → search again.                                           */
static void test_auto_cross_node_workflow(void)
{
    struct harness h;
    harness_init(&h);
    build_app(&h);
    tickf(&h);
    h.ti_calls = 0;

    /* Step 1: Discover all buttons by searching for "a" (Open, Save both
     * contain 'a'... actually "Open" doesn't contain 'a'. Search for "v"
     * to find "Save". Let's search for "Save".) */
    op_ok(&h, scene_client_search(h.cl, 1, "Save", 4), "find save");
    tickf(&h);
    CHECK_EQ(h.sr_count, 1);
    uint32_t save_id = h.sr_hits[0].id;
    CHECK_EQ(save_id, 201);

    /* Step 2: Click Save → verify activation */
    h.act_calls = 0;
    int32_t sx = h.sr_hits[0].rect.x + h.sr_hits[0].rect.w / 2;
    int32_t sy = h.sr_hits[0].rect.y + h.sr_hits[0].rect.h / 2;
    scene_compositor_input_pointer(h.cp, 0, sx, sy, 0x01);
    tickf(&h);
    CHECK(h.act_calls >= 1);
    CHECK_EQ(h.act_id[h.act_calls - 1], save_id);

    /* Step 3: Change Save button's text to "Saved!" */
    op_ok(&h, scene_client_set_text(h.cl, 201, 1, "Saved!", 6),
          "update text");
    tickf(&h);

    /* Step 4: Verify old search still finds it (substring: "Save" ⊂ "Saved!") */
    h.sr_calls = 0;
    op_ok(&h, scene_client_search(h.cl, 2, "Save", 4), "search old");
    tickf(&h);
    CHECK_EQ(h.sr_count, 1);

    /* Step 4b: Verify a term NOT in "Saved!" returns 0 */
    h.sr_calls = 0;
    op_ok(&h, scene_client_search(h.cl, 22, "Savez", 5), "search gone");
    tickf(&h);
    CHECK_EQ(h.sr_count, 0);

    /* Step 5: New search finds updated text */
    h.sr_calls = 0;
    op_ok(&h, scene_client_search(h.cl, 3, "Saved!", 6), "search new");
    tickf(&h);
    CHECK_EQ(h.sr_count, 1);
    CHECK_EQ(h.sr_hits[0].id, save_id);

    printf("test_auto_cross_node_workflow: ok\n");
    harness_free(&h);
}

/* Test 6: Visibility toggle as automation primitive.
 * Hide a node, verify it's no longer found by search (invisible nodes
 * shouldn't be discoverable). Then show it again and verify.             */
static void test_auto_visibility_toggle(void)
{
    struct harness h;
    harness_init(&h);
    build_app(&h);
    tickf(&h);
    h.ti_calls = 0;

    /* Verify "Open" is discoverable */
    op_ok(&h, scene_client_search(h.cl, 1, "Open", 4), "search before");
    tickf(&h);
    CHECK_EQ(h.sr_count, 1);

    /* Hide the Open button (clear VISIBLE flag) */
    op_ok(&h, scene_client_set_flags(h.cl, 200, 0), "hide open");
    tickf(&h);

    /* Verify it's no longer discoverable (invisible) */
    h.sr_calls = 0;
    op_ok(&h, scene_client_search(h.cl, 2, "Open", 4), "search hidden");
    tickf(&h);
    CHECK_EQ(h.sr_count, 0);

    /* Show it again */
    op_ok(&h, scene_client_set_flags(h.cl, 200, SCENE_FLAG_VISIBLE),
          "show open");
    tickf(&h);

    /* Verify it's discoverable again */
    h.sr_calls = 0;
    op_ok(&h, scene_client_search(h.cl, 3, "Open", 4), "search shown");
    tickf(&h);
    CHECK_EQ(h.sr_count, 1);
    CHECK_EQ(h.sr_hits[0].id, 200);

    printf("test_auto_visibility_toggle: ok\n");
    harness_free(&h);
}

/* Test 7: Move a node and verify coordinates.
 * Automation tool finds a node, moves it, then re-discovers it and
 * verifies the new coordinates.                                            */
static void test_auto_move_and_verify(void)
{
    struct harness h;
    harness_init(&h);
    build_app(&h);
    tickf(&h);
    h.ti_calls = 0;

    /* Find Save button, check initial rect */
    op_ok(&h, scene_client_search(h.cl, 1, "Save", 4), "find save");
    tickf(&h);
    CHECK_EQ(h.sr_count, 1);
    CHECK_EQ(h.sr_hits[0].rect.x, 140);
    CHECK_EQ(h.sr_hits[0].rect.y, 60);

    /* Move it */
    static const scene_rect new_rect = {250, 80, 120, 35};
    op_ok(&h, scene_client_set_rect(h.cl, 201, &new_rect), "move save");
    tickf(&h);

    /* Re-discover and verify new coordinates */
    h.sr_calls = 0;
    op_ok(&h, scene_client_search(h.cl, 2, "Save", 4), "find moved");
    tickf(&h);
    CHECK_EQ(h.sr_count, 1);
    CHECK_EQ(h.sr_hits[0].rect.x, 250);
    CHECK_EQ(h.sr_hits[0].rect.y, 80);
    CHECK_EQ(h.sr_hits[0].rect.w, 120);
    CHECK_EQ(h.sr_hits[0].rect.h, 35);

    printf("test_auto_move_and_verify: ok\n");
    harness_free(&h);
}

/* Test 8: Multi-node search.
 * Search for a term that matches multiple nodes, verify all are found.   */
static void test_auto_multi_node_search(void)
{
    struct harness h;
    harness_init(&h);

    /* Build a UI with multiple buttons containing "btn" */
    static const scene_rect r_win = {0, 0, 800, 600};
    static const scene_rect r1 = {10, 10, 100, 30};
    static const scene_rect r2 = {10, 50, 100, 30};
    static const scene_rect r3 = {10, 90, 100, 30};

    op_ok(&h, scene_client_create_node(h.cl, SCENE_NO_PARENT, 100,
            SCENE_ROLE_WINDOW, &r_win, SCENE_FLAG_VISIBLE), "win");
    op_ok(&h, scene_client_create_node(h.cl, 100, 200,
            SCENE_ROLE_BUTTON, &r1, SCENE_FLAG_VISIBLE), "b1");
    op_ok(&h, scene_client_set_text(h.cl, 200, 1, "btn Alpha", 9), "t1");
    op_ok(&h, scene_client_create_node(h.cl, 100, 201,
            SCENE_ROLE_BUTTON, &r2, SCENE_FLAG_VISIBLE), "b2");
    op_ok(&h, scene_client_set_text(h.cl, 201, 1, "btn Beta", 8), "t2");
    op_ok(&h, scene_client_create_node(h.cl, 100, 202,
            SCENE_ROLE_BUTTON, &r3, SCENE_FLAG_VISIBLE), "b3");
    op_ok(&h, scene_client_set_text(h.cl, 202, 1, "btn Gamma", 9), "t3");

    tickf(&h);
    h.ti_calls = 0;

    /* Search for "btn" → should find all 3 */
    op_ok(&h, scene_client_search(h.cl, 1, "btn", 3), "search btn");
    tickf(&h);
    CHECK_EQ(h.sr_count, 3);

    /* Search for "Alpha" → 1 hit */
    h.sr_calls = 0;
    op_ok(&h, scene_client_search(h.cl, 2, "Alpha", 5), "search alpha");
    tickf(&h);
    CHECK_EQ(h.sr_count, 1);
    CHECK_EQ(h.sr_hits[0].id, 200);

    /* Search for "btn Beta" → 1 hit */
    h.sr_calls = 0;
    op_ok(&h, scene_client_search(h.cl, 3, "btn Beta", 8), "search exact");
    tickf(&h);
    CHECK_EQ(h.sr_count, 1);
    CHECK_EQ(h.sr_hits[0].id, 201);

    printf("test_auto_multi_node_search: ok\n");
    harness_free(&h);
}

/* ==================================================================== */
/* Main                                                                 */
/* ==================================================================== */

int main(void)
{
    test_auto_discover_by_text();
    test_auto_click_discovered();
    test_auto_type_into_field();
    test_auto_macro_record_replay();
    test_auto_cross_node_workflow();
    test_auto_visibility_toggle();
    test_auto_move_and_verify();
    test_auto_multi_node_search();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
