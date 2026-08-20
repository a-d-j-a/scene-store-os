/* test_wlshim.c - black-box tests for the legacy-wayland shim.
 *
 * A fake libwayland client marshals raw wire messages (the same byte
 * stream libwayland produces: LE u32 header, args, strings padded to
 * 4, fds passed out-of-band) into wlshim_ingest, which is the actual
 * path the real NetSurf wld client exercises on the ISO. The fake shm
 * pool backing store is injected via wlshim_set_mapfn.
 */
#include "iso_wlshim.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int checks, failures;
#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)
#define CHECK_EQ(a, b) do { \
    checks++; \
    if ((a) != (b)) { failures++; printf("FAIL %s:%d: %s (%ld) != %s (%ld)\n", \
        __FILE__, __LINE__, #a, (long)(a), #b, (long)(b)); } \
} while(0)

/* ---- fake wire marshaller ----------------------------------------- */
typedef struct { uint8_t b[8192]; size_t n, sz_at; } m;
static void m_start(m *mb, uint32_t obj, uint32_t op, uint32_t newid)
{
    mb->sz_at = mb->n;
    mb->n += 8;                        /* size backpatched at m_end */
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

/* ---- fake shm backing store ---------------------------------------- */
static uint8_t fake_pool[1 << 20];
static uint32_t fake_pool_size;
static uint32_t framecalls, last_w, last_h;
static uint32_t last_px;               /* first pixel of last frame */

static void *fake_map(void *ud, int fd, uint32_t size)
{
    (void)ud; (void)fd;
    fake_pool_size = size;
    memset(fake_pool, 0, size);
    return fake_pool;
}
static void on_frame(void *ud, const wlshim_frame *f)
{
    (void)ud;
    framecalls++;
    last_w = f->w;
    last_h = f->h;
    last_px = f->px[0];
}

/* ---- event stream parser (server -> client) ------------------------ */
typedef struct { const uint8_t *p; size_t left; } e;

/* Advance past one event; return its payload pointer (NULL on garbage). */
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

static wlshim *make_shim(void)
{
    wlshim *w = wlshim_new(NULL, NULL, on_frame, NULL, NULL);
    wlshim_set_mapfn(w, fake_map, NULL);
    return w;
}

/* bind is the one request whose new_id is its LAST argument
 * (wayland wire doc: string + uint precede the new_id there). */
static void bind_global(m *mb, uint32_t reg, const char *iface,
                        uint32_t name, uint32_t version, uint32_t newid)
{
    m a; a.n = 0;
    m_u32(&a, name);
    m_str(&a, iface);
    m_u32(&a, version);
    m_u32(&a, newid);
    m_put(mb, reg, 0, 0, a.b, a.n);    /* bind opcode = 0 */
}

/* wire request opcodes (client side) */
#define OP_REG_BIND       0
#define OP_COMP_CREATE_SURFACE 0
#define OP_SHELL_GET_SHELL_SURFACE 0
#define OP_SH_SHELL_SET_TOPLEVEL 3
#define OP_SH_SHELL_SET_TITLE 8
#define OP_SHM_CREATE_POOL 0
#define OP_POOL_CREATE_BUFFER 0
#define OP_SURFACE_ATTACH  1
#define OP_SURFACE_DAMAGE  2
#define OP_SURFACE_FRAME   3
#define OP_SURFACE_COMMIT  6

static void test_globals_and_bind(void)
{
    wlshim *w = make_shim();
    m mb;
    uint8_t out[4096]; size_t on;
    e ev;
    uint32_t n = 0, names[8], versions[8];
    char ifaces[8][32];
    uint32_t formats = 0, unknown = 0;

    mb.n = 0;
    m_put(&mb, 1, 1, 2, NULL, 0);          /* get_registry -> 2 */
    bind_global(&mb, 2, "wl_compositor", 0, 1, 3);
    bind_global(&mb, 2, "wl_shell", 1, 1, 4);
    bind_global(&mb, 2, "wl_shm", 2, 1, 5);
    wlshim_ingest(w, mb.b, mb.n, NULL, 0);

    on = wlshim_out_drain(w, out, sizeof out);
    ev.p = out; ev.left = on;
    while (ev.left > 0) {
        uint32_t op = 0, obj = 0;
        const uint8_t *pl = ev_next(&ev, &obj, &op);
        if (!pl) break;
        uint32_t len;
        if (obj == 2 && op == 0) {         /* wl_registry.global */
            names[n] = ((const uint32_t *)pl)[0];
            len = ((const uint32_t *)(pl + 4))[0];
            memcpy(ifaces[n], pl + 8, len - 1);
            ifaces[n][len - 1] = 0;
            versions[n] = ((const uint32_t *)(pl + 8 + ((len + 3) & ~3)))[0];
            n++;
        } else if (obj == 5 && op == 0) {  /* wl_shm.format */
            formats++;
            CHECK_EQ(((const uint32_t *)pl)[0], 1u);   /* XRGB8888 */
        } else unknown++;
    }
    CHECK_EQ(n, 3u);
    CHECK_EQ(names[0], 0u); CHECK_EQ(names[1], 1u); CHECK_EQ(names[2], 2u);
    CHECK(strcmp(ifaces[0], "wl_compositor") == 0);
    CHECK(strcmp(ifaces[1], "wl_shell") == 0);
    CHECK(strcmp(ifaces[2], "wl_shm") == 0);
    CHECK_EQ(versions[0], 1u); CHECK_EQ(versions[1], 1u); CHECK_EQ(versions[2], 1u);
    CHECK_EQ(formats, 1u);
    CHECK_EQ(unknown, 0u);
    wlshim_free(w);
}

#define W 16u
#define H 8u

static void test_surface_shell_title_sync(void)
{
    wlshim *w = make_shim();
    m mb, ab;
    uint8_t out[256]; size_t on;
    uint32_t done = 0, known = 0, unknown = 0;

    mb.n = 0;
    m_put(&mb, 1, 1, 2, NULL, 0);          /* registry -> 2 */
    bind_global(&mb, 2, "wl_compositor", 0, 1, 3);
    bind_global(&mb, 2, "wl_shell", 1, 1, 4);
    bind_global(&mb, 2, "wl_shm", 2, 1, 5);
    m_put(&mb, 3, OP_COMP_CREATE_SURFACE, 6, NULL, 0);   /* surface -> 6 */
    ab.n = 0; m_u32(&ab, 6);
    m_put(&mb, 4, OP_SHELL_GET_SHELL_SURFACE, 7, ab.b, ab.n);/* ss -> 7 */
    m_put(&mb, 7, OP_SH_SHELL_SET_TOPLEVEL, 0, NULL, 0);
    ab.n = 0; m_str(&ab, "Test Window");
    m_put(&mb, 7, OP_SH_SHELL_SET_TITLE, 0, ab.b, ab.n);
    m_put(&mb, 1, 0, 10, NULL, 0);                 /* sync -> 10 */
    wlshim_ingest(w, mb.b, mb.n, NULL, 0);
    CHECK(wlshim_last_frame(w) == NULL);           /* no commit yet */

    on = wlshim_out_drain(w, out, sizeof out);
    e ev; ev.p = out; ev.left = on;
    while (ev.left > 0) {
        uint32_t op = 0, obj = 0;
        const uint8_t *pl = ev_next(&ev, &obj, &op);
        (void)pl; (void)obj;
        if (obj == 10 && op == 0) done++;          /* wl_callback.done */
        else if (obj == 2 || obj == 5) known++;    /* registry.global / shm.format */
        else unknown++;
    }
    CHECK_EQ(done, 1u);
    CHECK_EQ(known, 4u);
    CHECK_EQ(unknown, 0u);
    wlshim_free(w);
}

static void feed_surface_setup(wlshim *w, m *mb)
{
    m_put(mb, 1, 1, 2, NULL, 0);
    bind_global(mb, 2, "wl_compositor", 0, 1, 3);
    bind_global(mb, 2, "wl_shell", 1, 1, 4);
    bind_global(mb, 2, "wl_shm", 2, 1, 5);
    m_put(mb, 3, OP_COMP_CREATE_SURFACE, 6, NULL, 0);
    (void)w;
}

static void test_full_pipeline(void)
{
    wlshim *w = make_shim();
    m mb, ab;
    int fds[4]; size_t nfd = 0;
    uint8_t out[1024]; size_t on;
    uint32_t done = 0, released = 0;
    uint32_t i;

    mb.n = 0;
    feed_surface_setup(w, &mb);
    /* pool -> 8, size W*H*4, fd 0 */
    ab.n = 0; m_u32(&ab, W * H * 4);
    m_put(&mb, 5, OP_SHM_CREATE_POOL, 8, ab.b, ab.n);
    fds[0] = 3; nfd = 1;                      /* pretend fd 3 came via SCM_RIGHTS */
    wlshim_ingest(w, mb.b, mb.n, fds, nfd);
    for (i = 0; i < W * H; i++)
        ((uint32_t *)fake_pool)[i] = 0xFF302010u;

    mb.n = 0;
    ab.n = 0; m_u32(&ab, 0); m_u32(&ab, W); m_u32(&ab, H);
    m_u32(&ab, W * 4); m_u32(&ab, 1);
    m_put(&mb, 8, OP_POOL_CREATE_BUFFER, 9, ab.b, ab.n);
    ab.n = 0; m_u32(&ab, 9); m_u32(&ab, 0); m_u32(&ab, 0);
    m_put(&mb, 6, OP_SURFACE_ATTACH, 0, ab.b, ab.n);
    ab.n = 0; m_u32(&ab, 0); m_u32(&ab, 0); m_u32(&ab, W); m_u32(&ab, H);
    m_put(&mb, 6, OP_SURFACE_DAMAGE, 0, ab.b, ab.n);
    m_put(&mb, 6, OP_SURFACE_COMMIT, 0, NULL, 0);
    m_put(&mb, 1, 0, 10, NULL, 0);
    wlshim_ingest(w, mb.b, mb.n, NULL, 0);

    CHECK_EQ(framecalls, 1u);
    CHECK_EQ(last_w, W); CHECK_EQ(last_h, H);
    CHECK_EQ(last_px, 0xFF302010u);      /* byte order preserved */

    on = wlshim_out_drain(w, out, sizeof out);
    e ev; ev.p = out; ev.left = on;
    while (ev.left > 0) {
        uint32_t op = 0, obj = 0;
        const uint8_t *pl = ev_next(&ev, &obj, &op);
        (void)pl;
        if (op == 0 && obj == 10) done++;          /* callback.done */
        else if (obj == 9 && op == 0) released++;  /* buffer.release */
    }
    CHECK_EQ(done, 1u);
    CHECK_EQ(released, 1u);
    {
        const wlshim_frame *lf = wlshim_last_frame(w);
        CHECK(lf != NULL);
        if (lf) { CHECK_EQ(lf->w, W); CHECK_EQ(lf->h, H); }
    }
    wlshim_free(w);
}

static void test_frame_callback(void)
{
    wlshim *w = make_shim();
    m mb, ab;
    int fds[1]; size_t nfd = 0;
    uint8_t out[1024]; size_t on;
    uint32_t done13 = 0, i;

    mb.n = 0;
    feed_surface_setup(w, &mb);
    ab.n = 0; m_u32(&ab, W * H * 4);
    m_put(&mb, 5, OP_SHM_CREATE_POOL, 8, ab.b, ab.n);
    fds[0] = 3; nfd = 1;
    wlshim_ingest(w, mb.b, mb.n, fds, nfd);
    for (i = 0; i < W * H; i++)
        ((uint32_t *)fake_pool)[i] = 0xFF0000FFu;

    mb.n = 0;
    ab.n = 0; m_u32(&ab, 0); m_u32(&ab, W); m_u32(&ab, H);
    m_u32(&ab, W * 4); m_u32(&ab, 1);
    m_put(&mb, 8, OP_POOL_CREATE_BUFFER, 9, ab.b, ab.n);
    ab.n = 0; m_u32(&ab, 9); m_u32(&ab, 0); m_u32(&ab, 0);
    m_put(&mb, 6, OP_SURFACE_ATTACH, 0, ab.b, ab.n);
    m_put(&mb, 6, OP_SURFACE_FRAME, 13, NULL, 0);    /* frame -> 13 */
    m_put(&mb, 6, OP_SURFACE_COMMIT, 0, NULL, 0);
    wlshim_ingest(w, mb.b, mb.n, NULL, 0);

    CHECK_EQ(framecalls, 2u);
    on = wlshim_out_drain(w, out, sizeof out);
    e ev; ev.p = out; ev.left = on;
    while (ev.left > 0) {
        uint32_t op = 0, obj = 0;
        const uint8_t *pl = ev_next(&ev, &obj, &op);
        (void)pl;
        if (obj == 13 && op == 0) done13++;
    }
    CHECK_EQ(done13, 1u);               /* frame callback fired at commit */
    wlshim_free(w);
}

static void test_buffer_destroy_and_release(void)
{
    wlshim *w = make_shim();
    m mb, ab;
    int fds[1]; size_t nfd = 0;
    uint32_t i, before = framecalls;

    mb.n = 0;
    feed_surface_setup(w, &mb);
    ab.n = 0; m_u32(&ab, W * H * 4);
    m_put(&mb, 5, OP_SHM_CREATE_POOL, 8, ab.b, ab.n);
    fds[0] = 3; nfd = 1;
    wlshim_ingest(w, mb.b, mb.n, fds, nfd);
    for (i = 0; i < W * H; i++)
        ((uint32_t *)fake_pool)[i] = 0xFF00FF00u;

    mb.n = 0;
    ab.n = 0; m_u32(&ab, 0); m_u32(&ab, W); m_u32(&ab, H);
    m_u32(&ab, W * 4); m_u32(&ab, 1);
    m_put(&mb, 8, OP_POOL_CREATE_BUFFER, 9, ab.b, ab.n);
    m_put(&mb, 9, 0, 0, NULL, 0);              /* buffer.destroy */
    ab.n = 0; m_u32(&ab, 0); m_u32(&ab, 0); m_u32(&ab, 0);
    m_put(&mb, 6, OP_SURFACE_ATTACH, 0, ab.b, ab.n);
    m_put(&mb, 6, OP_SURFACE_COMMIT, 0, NULL, 0);
    wlshim_ingest(w, mb.b, mb.n, NULL, 0);

    CHECK_EQ(framecalls, before);              /* destroyed buffer = no frame */
    wlshim_free(w);
}

static void test_pool_overflow_skips_frame(void)
{
    wlshim *w = make_shim();
    m mb, ab;
    int fds[1]; size_t nfd = 0;
    uint32_t before = framecalls;

    mb.n = 0;
    feed_surface_setup(w, &mb);
    ab.n = 0; m_u32(&ab, 64);                    /* pool too small for 16x16 */
    m_put(&mb, 5, OP_SHM_CREATE_POOL, 8, ab.b, ab.n);
    fds[0] = 3; nfd = 1;
    wlshim_ingest(w, mb.b, mb.n, fds, nfd);

    mb.n = 0;
    ab.n = 0; m_u32(&ab, 0); m_u32(&ab, 16); m_u32(&ab, 16);
    m_u32(&ab, 64); m_u32(&ab, 1);
    m_put(&mb, 8, OP_POOL_CREATE_BUFFER, 9, ab.b, ab.n);
    ab.n = 0; m_u32(&ab, 9); m_u32(&ab, 0); m_u32(&ab, 0);
    m_put(&mb, 6, OP_SURFACE_ATTACH, 0, ab.b, ab.n);
    m_put(&mb, 6, OP_SURFACE_COMMIT, 0, NULL, 0);
    wlshim_ingest(w, mb.b, mb.n, NULL, 0);

    CHECK_EQ(framecalls, before);
    CHECK(!wlshim_dead(w));                    /* overflow is not fatal */
    wlshim_free(w);
}

static void test_protocol_errors(void)
{
    wlshim *w = make_shim();
    m mb;
    uint8_t tiny[8] = { 4, 0, 0, 0, 0, 0, 0, 0 };  /* declared size 4 < 8 */

    wlshim_ingest(w, tiny, sizeof tiny, NULL, 0);
    CHECK(wlshim_dead(w));

    wlshim_free(w);
    w = make_shim();
    mb.n = 0;
    m_put(&mb, 50, 0, 0, NULL, 0);         /* request on unknown object */
    wlshim_ingest(w, mb.b, mb.n, NULL, 0);
    CHECK(wlshim_dead(w));
    wlshim_free(w);

    w = make_shim();
    mb.n = 0;
    m_put(&mb, 1, 0, 11, NULL, 0);         /* sync -> 11 */
    m_put(&mb, 3, 9, 0, NULL, 0);          /* bad opcode on compositor */
    wlshim_ingest(w, mb.b, mb.n, NULL, 0);
    CHECK(wlshim_dead(w));
    wlshim_free(w);

    w = make_shim();
    mb.n = 0;
    m_put(&mb, 1, 1, 2, NULL, 0);
    m_put(&mb, 2, OP_REG_BIND, 3, NULL, 0);/* bind with no payload */
    wlshim_ingest(w, mb.b, mb.n, NULL, 0);
    CHECK(wlshim_dead(w));
    wlshim_free(w);
}

static void test_misc_opcodes_accepted(void)
{
    wlshim *w = make_shim();
    m mb, ab;
    int fds[1]; size_t nfd = 0;

    mb.n = 0;
    feed_surface_setup(w, &mb);
    ab.n = 0; m_u32(&ab, W * H * 4);
    m_put(&mb, 5, OP_SHM_CREATE_POOL, 8, ab.b, ab.n);
    fds[0] = 3; nfd = 1;
    wlshim_ingest(w, mb.b, mb.n, fds, nfd);

    mb.n = 0;
    ab.n = 0; m_u32(&ab, 0); m_u32(&ab, W); m_u32(&ab, H);
    m_u32(&ab, W * 4); m_u32(&ab, 1);
    m_put(&mb, 8, OP_POOL_CREATE_BUFFER, 9, ab.b, ab.n);
    ab.n = 0; m_u32(&ab, 0); m_u32(&ab, 0); m_u32(&ab, W); m_u32(&ab, H);
    m_put(&mb, 6, 9, 0, ab.b, ab.n);       /* damage_buffer (op 9) */
    ab.n = 0; m_u32(&ab, 1);
    m_put(&mb, 6, 7, 0, ab.b, ab.n);       /* set_buffer_transform */
    m_put(&mb, 6, 8, 0, ab.b, ab.n);       /* set_buffer_scale (op8, u32 1) */
    ab.n = 0; m_u32(&ab, 2); m_u32(&ab, 3);
    m_put(&mb, 6, 10, 0, ab.b, ab.n);      /* offset */
    ab.n = 0; m_u32(&ab, 5); m_u32(&ab, 6);
    m_put(&mb, 8, 2, 0, ab.b, ab.n);       /* pool resize */
    ab.n = 0; m_u32(&ab, 9); m_u32(&ab, 0); m_u32(&ab, 0);
    m_put(&mb, 6, OP_SURFACE_ATTACH, 0, ab.b, ab.n);
    m_put(&mb, 6, OP_SURFACE_COMMIT, 0, NULL, 0);
    wlshim_ingest(w, mb.b, mb.n, NULL, 0);

    CHECK(!wlshim_dead(w));
    CHECK_EQ(framecalls, 3u);              /* commit still delivered */
    wlshim_free(w);
}

static void test_unknown_global_bound(void)
{
    wlshim *w = make_shim();
    m mb;

    mb.n = 0;
    m_put(&mb, 1, 1, 2, NULL, 0);
    bind_global(&mb, 2, "wl_seat", 7, 1, 9);   /* unknown name: tolerated */
    wlshim_ingest(w, mb.b, mb.n, NULL, 0);
    CHECK(!wlshim_dead(w));
    wlshim_free(w);
}

int main(void)
{
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("t1\n"); fflush(stdout);
    test_globals_and_bind();
    printf("t2\n"); fflush(stdout);
    test_surface_shell_title_sync();
    printf("t3\n"); fflush(stdout);
    test_full_pipeline();
    printf("t4\n"); fflush(stdout);
    test_frame_callback();
    printf("t5\n"); fflush(stdout);
    test_buffer_destroy_and_release();
    printf("t6\n"); fflush(stdout);
    test_pool_overflow_skips_frame();
    printf("t7\n"); fflush(stdout);
    test_protocol_errors();
    printf("t8\n"); fflush(stdout);
    test_misc_opcodes_accepted();
    printf("t9\n"); fflush(stdout);
    test_unknown_global_bound();
    printf("wlshim: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}