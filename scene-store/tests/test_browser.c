/* test_browser.c — black-box tests for the scene_browser host module.
 *
 * A fake libwayland client (the same raw-wire marshaller test_wlshim.c
 * uses) drives the browser's wlshim through the exact path a NetSurf
 * `-f wld` client takes; the browser's scene session runs on a loopback
 * inside scene_compositor, so the tests assert what the browser really
 * does at the OS seam: window nodes + texture in the store, surface-
 * relative input translation into wayland events, and clean desktop
 * recovery when either the client or the server dies.
 */
#include "scene_browser.h"
#include "scene_compositor.h"
#include "scene_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;
#define CHECK(expr) do {                                                    \
    checks++;                                                               \
    if (!(expr)) {                                                          \
        failures++;                                                         \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);              \
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

/* ---- fake wire marshaller (identical to test_wlshim.c) ---------------- */
typedef struct { uint8_t b[8192]; size_t n, sz_at; } m;
static void m_start(m *mb, uint32_t obj, uint32_t op, uint32_t newid)
{
    mb->sz_at = mb->n;
    mb->n += 8;
    uint32_t h = (obj << 16) | op;
    memcpy(mb->b + mb->n - 4, &h, 4);
    if (newid) { memcpy(mb->b + mb->n, &newid, 4); mb->n += 4; }
}
static void m_u32(m *mb, uint32_t v){ memcpy(mb->b + mb->n, &v, 4); mb->n += 4; }
static void m_str(m *mb, const char *s)
{
    size_t n = strlen(s) + 1, pad = (n + 3) & ~3;
    m_u32(mb, (uint32_t)n);
    memcpy(mb->b + mb->n, s, n);
    memset(mb->b + mb->n + n, 0, pad - n);
    mb->n += pad;
}
static void m_end(m *mb)
{
    uint32_t sz = (uint32_t)(mb->n - mb->sz_at);
    memcpy(mb->b + mb->sz_at, &sz, 4);
}
static void m_put(m *mb, uint32_t obj, uint32_t op, uint32_t newid,
                  const uint8_t *args, size_t alen)
{
    m_start(mb, obj, op, newid);
    if (alen) { memcpy(mb->b + mb->n, args, alen); mb->n += alen; }
    m_end(mb);
}

/* ---- fake shm backing store ------------------------------------------- */
static uint8_t fake_pool[1 << 20];
static uint32_t fake_pool_size;

static void *fake_map(void *ud, int fd, uint32_t size)
{
    (void)ud; (void)fd;
    fake_pool_size = size;
    memset(fake_pool, 0, size);
    return fake_pool;
}

/* ---- event stream parser (server -> client) --------------------------- */
typedef struct { const uint8_t *p; size_t left; } e;
static const uint8_t *ev_next(e *ev, uint32_t *obj, uint32_t *op)
{
    const uint8_t *p = ev->p;
    uint32_t sz;
    if (ev->left < 8) { ev->left = 0; return NULL; }
    sz = ((const uint32_t *)p)[0];
    if (sz < 8 || sz > ev->left) { ev->left = 0; return NULL; }
    *obj = ((const uint32_t *)p)[1] >> 16;
    *op  = ((const uint32_t *)p)[1] & 0xFFFFu;
    ev->p += sz;
    ev->left -= sz;
    return p + 8;
}

/* ---- wire request opcodes --------------------------------------------- */
#define OP_REG_BIND             0
#define OP_COMP_CREATE_SURFACE  0
#define OP_SHELL_GET_SHELL_SURFACE 0
#define OP_SH_SHELL_SET_TOPLEVEL 3
#define OP_SH_SHELL_SET_TITLE    8
#define OP_SHM_CREATE_POOL       0
#define OP_POOL_CREATE_BUFFER    0
#define OP_SURFACE_ATTACH        1
#define OP_SURFACE_DAMAGE        2
#define OP_SURFACE_FRAME         3
#define OP_SURFACE_COMMIT        6
#define OP_SEAT_GET_POINTER      0
#define OP_SEAT_GET_KEYBOARD     1

static void bind_global(m *mb, uint32_t reg, const char *iface,
                        uint32_t name, uint32_t version, uint32_t newid)
{
    m a; a.n = 0;
    m_u32(&a, name);
    m_str(&a, iface);
    m_u32(&a, version);
    m_u32(&a, newid);
    m_put(mb, reg, 0, 0, a.b, a.n);
}

/* Id plan for the fake client: registry 2, compositor 3, shell 4, shm 5,
 * seat 6, surface 7, shell_surface 8, pointer 9, keyboard 10, pool 11,
 * buffer 12, frame callback 13, sync callback 14. */

/* Global binds + surface + shell + seat + one shm pool of fw*fh*4.
 * Fills the fake backing store with `fill` AFTER the pool maps it.   */
static void wire_setup_a(wlshim *w, uint32_t fw, uint32_t fh, uint32_t fill)
{
    m mb, a;
    int fds[1]; size_t nfd = 0;
    uint32_t n = fw * fh, i;

    mb.n = 0;
    m_put(&mb, 1, 1, 2, NULL, 0);
    bind_global(&mb, 2, "wl_compositor", 0, 1, 3);
    bind_global(&mb, 2, "wl_shell", 1, 1, 4);
    bind_global(&mb, 2, "wl_shm", 2, 1, 5);
    bind_global(&mb, 2, "wl_seat", 3, 1, 6);
    m_put(&mb, 3, OP_COMP_CREATE_SURFACE, 7, NULL, 0);
    a.n = 0; m_u32(&a, 7);
    m_put(&mb, 4, OP_SHELL_GET_SHELL_SURFACE, 8, a.b, a.n);
    m_put(&mb, 8, OP_SH_SHELL_SET_TOPLEVEL, 0, NULL, 0);
    a.n = 0; m_str(&a, "Test Page");
    m_put(&mb, 8, OP_SH_SHELL_SET_TITLE, 0, a.b, a.n);
    a.n = 0;
    m_put(&mb, 6, OP_SEAT_GET_POINTER, 9, a.b, a.n);
    m_put(&mb, 6, OP_SEAT_GET_KEYBOARD, 10, a.b, a.n);
    a.n = 0; m_u32(&a, fw * fh * 4);
    m_put(&mb, 5, OP_SHM_CREATE_POOL, 11, a.b, a.n);
    fds[0] = 3; nfd = 1;
    wlshim_ingest(w, mb.b, mb.n, fds, nfd);
    for (i = 0; i < n; i++)
        ((uint32_t *)fake_pool)[i] = fill;
}

/* One commit: new buffer on the pool, attach, damage, frame, commit,
 * then a wl_display.sync roundtrip (the shim must answer it).       */
static void wire_frame(wlshim *w, uint32_t fw, uint32_t fh)
{
    m mb, a;

    mb.n = 0;
    a.n = 0; m_u32(&a, 0); m_u32(&a, fw); m_u32(&a, fh);
    m_u32(&a, fw * 4); m_u32(&a, 1);
    m_put(&mb, 11, OP_POOL_CREATE_BUFFER, 12, a.b, a.n);
    a.n = 0; m_u32(&a, 12); m_u32(&a, 0); m_u32(&a, 0);
    m_put(&mb, 7, OP_SURFACE_ATTACH, 0, a.b, a.n);
    a.n = 0; m_u32(&a, 0); m_u32(&a, 0); m_u32(&a, fw); m_u32(&a, fh);
    m_put(&mb, 7, OP_SURFACE_DAMAGE, 0, a.b, a.n);
    m_put(&mb, 7, OP_SURFACE_FRAME, 13, NULL, 0);
    m_put(&mb, 7, OP_SURFACE_COMMIT, 0, NULL, 0);
    m_put(&mb, 1, 0, 14, NULL, 0);          /* sync -> 14 */
    wlshim_ingest(w, mb.b, mb.n, NULL, 0);
}

/* ---- harness ----------------------------------------------------------- */

static scene_browser_config g_cfg = {
    60, 40, 16, 8, "Test Page", "t", "wayland-0"
};

struct harness {
    scene_compositor *cp;
    scene_browser    *b;
};

static void harness_init(struct harness *h)
{
    h->cp = scene_compositor_new(NULL, 320, 200);
    h->b = scene_browser_new(h->cp, &g_cfg);
    CHECK(h->b != NULL);
    if (h->b) CHECK_EQ(scene_compositor_layer_count(h->cp), 2);
}

static void harness_free(struct harness *h)
{
    scene_browser_free(h->b);
    scene_compositor_free(h->cp);
}

/* Blind pump + frame, n rounds: no asserts inside (a dead browser just
 * returns -1; the death tests assert that outside). */
static void settle(struct harness *h, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        scene_browser_pump(h->b);
        scene_compositor_frame(h->cp);
    }
}

static void drain_shim(wlshim *w)
{
    uint8_t out[4096];
    wlshim_out_drain(w, out, sizeof out);
}

/* ---- pointer/keyboard event scanners ----------------------------------- */

static void scan_pointer(wlshim *w, int *had_motion, int32_t *mx, int32_t *my,
                         uint32_t *press, uint32_t *rel)
{
    uint8_t out[4096];
    size_t on;
    e ev;
    on = wlshim_out_drain(w, out, sizeof out);
    ev.p = out; ev.left = on;
    *had_motion = 0; *mx = 0; *my = 0; *press = 0; *rel = 0;
    while (ev.left > 0) {
        uint32_t obj = 0, op = 0;
        const uint8_t *pl = ev_next(&ev, &obj, &op);
        if (!pl) break;
        if (obj == 9 && op == 2) {          /* wl_pointer.motion */
            *had_motion = 1;
            *mx = (int32_t)(((const uint32_t *)pl)[1]) >> 8;
            *my = (int32_t)(((const uint32_t *)pl)[2]) >> 8;
        } else if (obj == 9 && op == 3) {   /* wl_pointer.button */
            if (((const uint32_t *)pl)[3] == 1) (*press)++;
            else (*rel)++;
        }
    }
}

static int collect_keys(wlshim *w, uint32_t *codes, uint8_t *states, int cap)
{
    uint8_t out[4096];
    size_t on;
    e ev;
    int n = 0;
    on = wlshim_out_drain(w, out, sizeof out);
    ev.p = out; ev.left = on;
    while (ev.left > 0) {
        uint32_t obj = 0, op = 0;
        const uint8_t *pl = ev_next(&ev, &obj, &op);
        if (!pl) break;
        if (obj == 10 && op == 3) {         /* wl_keyboard.key */
            if (n < cap) {
                codes[n] = ((const uint32_t *)pl)[2];
                states[n] = ((const uint32_t *)pl)[3];
            }
            n++;
        }
    }
    return n;
}

static void inject_key(struct harness *h, uint32_t code, uint8_t mods)
{
    scene_compositor_input_key(h->cp, code, 1, mods);
    settle(h, 6);
    scene_compositor_input_key(h->cp, code, 0, mods);
    settle(h, 6);
}

/* ---- tests ------------------------------------------------------------- */

static void test_window_and_store(void)
{
    struct harness h;
    scene_rect r;
    scene_store *store;
    scene_node_vis v;
    scene_node_text_vis tv[4];
    int n;

    harness_init(&h);
    wlshim_set_mapfn(scene_browser_shim(h.b), fake_map, NULL);
    wire_setup_a(scene_browser_shim(h.b), 16, 8, 0xFF00FF00);
    wire_frame(scene_browser_shim(h.b), 16, 8);
    settle(&h, 6);

    scene_browser_window_rect(h.b, &r);
    CHECK_EQ(r.x, 60); CHECK_EQ(r.y, 40);
    CHECK_EQ(r.w, 16); CHECK_EQ(r.h, 8);

    store = scene_compositor_layer_store(h.cp, scene_browser_layer(h.b));
    CHECK(store != NULL);
    CHECK_EQ(scene_store_node_vis(store, 60000, &v), 0);
    CHECK_EQ(v.role, (uint16_t)SCENE_ROLE_WINDOW);
    CHECK_EQ(v.parent, (uint32_t)SCENE_NO_PARENT);
    CHECK_EQ(v.rect[0], 60); CHECK_EQ(v.rect[1], 40);
    CHECK_EQ(v.rect[2], 16); CHECK_EQ(v.rect[3], 8);
    n = scene_store_node_texts(store, 60000, tv, 4);
    CHECK(n >= 1);
    CHECK_EQ(tv[0].len, 9u);
    CHECK(memcmp(tv[0].data, "Test Page", 9) == 0);

    CHECK_EQ(scene_store_node_vis(store, 60001, &v), 0);
    CHECK_EQ(v.role, (uint16_t)SCENE_ROLE_GENERIC);
    CHECK_EQ(v.parent, 60000u);

    CHECK_EQ(PX(h.cp, 60, 40), 0xFF00FF00u);
    CHECK_EQ(PX(h.cp, 75, 47), 0xFF00FF00u);   /* bottom-right inside */
    CHECK_EQ(PX(h.cp, 76, 48), 0u);            /* one px outside      */
    CHECK_EQ(PX(h.cp, 300, 100), 0u);          /* empty desktop       */
    harness_free(&h);
}

static void test_resize(void)
{
    struct harness h;
    scene_rect r;

    harness_init(&h);
    wlshim_set_mapfn(scene_browser_shim(h.b), fake_map, NULL);
    wire_setup_a(scene_browser_shim(h.b), 32, 16, 0xFF0000FF);
    wire_frame(scene_browser_shim(h.b), 32, 16);
    settle(&h, 6);

    scene_browser_window_rect(h.b, &r);
    CHECK_EQ(r.x, 60); CHECK_EQ(r.y, 40);
    CHECK_EQ(r.w, 32); CHECK_EQ(r.h, 16);      /* frame size wins      */

    CHECK_EQ(PX(h.cp, 60, 40), 0xFF0000FFu);
    CHECK_EQ(PX(h.cp, 91, 55), 0xFF0000FFu);   /* bottom-right inside */
    CHECK_EQ(PX(h.cp, 92, 56), 0u);            /* just outside        */
    harness_free(&h);
}

static void test_input_click(void)
{
    struct harness h;
    int got;
    int32_t mx, my;
    uint32_t press, rel;

    harness_init(&h);
    wlshim_set_mapfn(scene_browser_shim(h.b), fake_map, NULL);
    wire_setup_a(scene_browser_shim(h.b), 16, 8, 0xFF00FF00);
    wire_frame(scene_browser_shim(h.b), 16, 8);
    settle(&h, 6);
    drain_shim(scene_browser_shim(h.b));       /* consume setup events */

    scene_compositor_input_pointer(h.cp, 0, 65, 43, SCENE_BTN_LEFT);
    settle(&h, 6);
    scan_pointer(scene_browser_shim(h.b), &got, &mx, &my, &press, &rel);
    CHECK(got);
    CHECK_EQ(mx, 5); CHECK_EQ(my, 3);          /* window-relative */
    CHECK_EQ(press, 1u); CHECK_EQ(rel, 0u);

    scene_compositor_input_pointer(h.cp, 0, 70, 45, 0);
    settle(&h, 6);
    scan_pointer(scene_browser_shim(h.b), &got, &mx, &my, &press, &rel);
    CHECK_EQ(press, 0u); CHECK_EQ(rel, 1u);

    /* click far outside the window: routed to the shell layer, the
     * browser must see nothing */
    scene_compositor_input_pointer(h.cp, 0, 300, 100, SCENE_BTN_LEFT);
    scene_compositor_input_pointer(h.cp, 0, 300, 100, 0);
    settle(&h, 6);
    {
        uint8_t out[2048];
        CHECK_EQ(wlshim_out_drain(scene_browser_shim(h.b), out, sizeof out),
                 0u);
    }
    harness_free(&h);
}

static void test_input_keys(void)
{
    struct harness h;
    uint32_t codes[16];
    uint8_t states[16];
    uint32_t i;
    int n;

    harness_init(&h);
    wlshim_set_mapfn(scene_browser_shim(h.b), fake_map, NULL);
    wire_setup_a(scene_browser_shim(h.b), 16, 8, 0xFF00FF00);
    wire_frame(scene_browser_shim(h.b), 16, 8);
    settle(&h, 6);
    drain_shim(scene_browser_shim(h.b));

    /* click inside to focus the browser layer */
    scene_compositor_input_pointer(h.cp, 0, 65, 43, SCENE_BTN_LEFT);
    settle(&h, 6);
    scene_compositor_input_pointer(h.cp, 0, 65, 43, 0);
    settle(&h, 6);
    drain_shim(scene_browser_shim(h.b));

    inject_key(&h, 30, 0);                     /* a            */
    inject_key(&h, 30, SCENE_MOD_SHIFT);       /* A            */
    inject_key(&h, 14, 0);                     /* Backspace    */
    inject_key(&h, 28, 0);                     /* Enter        */
    inject_key(&h, 105, 0);                    /* Left         */
    inject_key(&h, 59, 0);                     /* F1           */

    n = collect_keys(scene_browser_shim(h.b), codes, states, 16);
    CHECK_EQ(n, 12);
    {
        static const struct { uint32_t c; uint8_t s; } exp[12] = {
            {0x61, 1}, {0x61, 0}, {0x41, 1}, {0x41, 0},
            {8, 1},    {8, 0},    {13, 1},   {13, 0},
            {275, 1},  {275, 0},  {282, 1},  {282, 0}
        };
        for (i = 0; i < 12; i++) {
            CHECK_EQ(codes[i], exp[i].c);
            CHECK_EQ(states[i], exp[i].s);
        }
    }
    harness_free(&h);
}

static void test_death(void)
{
    struct harness h;
    wlshim *w;
    scene_server *sv;
    uint8_t garbage[16];

    /* (a) wlshim protocol death removes the layer */
    harness_init(&h);
    w = scene_browser_shim(h.b);
    wlshim_set_mapfn(w, fake_map, NULL);
    wire_setup_a(w, 16, 8, 0xFF00FF00);
    wire_frame(w, 16, 8);
    settle(&h, 6);
    CHECK_EQ(PX(h.cp, 62, 42), 0xFF00FF00u);

    { uint8_t tiny[8] = { 4, 0, 0, 0, 0, 0, 0, 0 };   /* size 4 < 8 */
      wlshim_ingest(w, tiny, 8, NULL, 0); }
    CHECK(wlshim_dead(w));
    CHECK(scene_browser_pump(h.b) == -1);
    CHECK(scene_browser_dead(h.b));
    CHECK_EQ(scene_compositor_layer_count(h.cp), 1);
    CHECK(scene_browser_server(h.b) == NULL);
    scene_compositor_frame(h.cp);
    CHECK_EQ(PX(h.cp, 62, 42), 0xFF101010u);   /* desktop restored */
    harness_free(&h);

    /* (b) scene-server protocol death: detected at the next pump */
    harness_init(&h);
    sv = scene_browser_server(h.b);
    CHECK(sv != NULL);
    CHECK_EQ(scene_compositor_layer_count(h.cp), 2);
    memset(garbage, 0xFF, sizeof garbage);
    CHECK(scene_server_feed(sv, garbage, sizeof garbage) != 0);
    CHECK(scene_browser_pump(h.b) == -1);
    CHECK(scene_browser_dead(h.b));
    CHECK_EQ(scene_compositor_layer_count(h.cp), 1);
    CHECK(scene_browser_server(h.b) == NULL);
    harness_free(&h);
}

static void test_determinism(void)
{
    struct harness a, b;
    const scene_fb *fa, *fb;

    harness_init(&a);
    wlshim_set_mapfn(scene_browser_shim(a.b), fake_map, NULL);
    wire_setup_a(scene_browser_shim(a.b), 16, 8, 0xFF00FF00);
    wire_frame(scene_browser_shim(a.b), 16, 8);
    settle(&a, 6);

    harness_init(&b);
    wlshim_set_mapfn(scene_browser_shim(b.b), fake_map, NULL);
    wire_setup_a(scene_browser_shim(b.b), 16, 8, 0xFF00FF00);
    wire_frame(scene_browser_shim(b.b), 16, 8);
    settle(&b, 6);

    fa = scene_compositor_fb(a.cp);
    fb = scene_compositor_fb(b.cp);
    CHECK_EQ(fa->w, fb->w);
    CHECK_EQ(fa->h, fb->h);
    CHECK(memcmp(fa->px, fb->px,
                 (size_t)fa->w * fa->h * sizeof(uint32_t)) == 0);
    CHECK_EQ(PX(a.cp, 62, 42), 0xFF00FF00u);   /* something drew */
    CHECK(PX(a.cp, 62, 42) != 0xFF101010u);
    harness_free(&a);
    harness_free(&b);
}

int main(void)
{
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("t1\n"); fflush(stdout);
    test_window_and_store();
    printf("t2\n"); fflush(stdout);
    test_resize();
    printf("t3\n"); fflush(stdout);
    test_input_click();
    printf("t4\n"); fflush(stdout);
    test_input_keys();
    printf("t5\n"); fflush(stdout);
    test_death();
    printf("t6\n"); fflush(stdout);
    test_determinism();
    printf("browser: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}