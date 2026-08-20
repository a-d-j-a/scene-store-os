/* iso_wlshim.c - minimal legacy-wayland compositor seam (one client).
 * See iso_wlshim.h for the protocol surface and wire facts. The core
 * (wlshim_ingest) is a strict streaming decoder + state machine for
 * exactly the request set libnsfb's wld.c sends. POSIX glue is
 * Linux-only; the core stays testable on Windows via ingest/out_drain.
 */
#include "iso_wlshim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define WLSHIM_MAXID 2048u
#define WLSHIM_MAXFB 8u

enum {
    WK_NONE = 0, WK_REGISTRY, WK_COMPOSITOR, WK_SHELL, WK_SHM,
    WK_SURFACE, WK_SHELL_SURFACE, WK_CALLBACK, WK_POOL, WK_BUFFER
};

#define WL_SHM_FORMAT_XRGB8888 1u

struct wlshim {
    uint8_t  ibuf[65536];
    size_t   ilen;
    uint8_t  obuf[4096];
    size_t   olen;
    int      dead;
    wlshim_frame_fn on_frame;
    wlshim_died_fn  on_died;
    void    *ud;
    wlshim_map_fn   mapfn;
    void    *mapud;

    uint32_t kind[WLSHIM_MAXID];
    uint32_t s_buf[WLSHIM_MAXID];
    uint32_t s_fbcb[WLSHIM_MAXID][WLSHIM_MAXFB];
    uint32_t s_ncb[WLSHIM_MAXID];
    uint8_t *p_map[WLSHIM_MAXID];
    uint32_t p_size[WLSHIM_MAXID];
    uint32_t b_pool[WLSHIM_MAXID];
    uint32_t b_off[WLSHIM_MAXID];
    uint32_t b_w[WLSHIM_MAXID], b_h[WLSHIM_MAXID];
    uint32_t b_stride[WLSHIM_MAXID];
    uint32_t b_fmt[WLSHIM_MAXID];

    uint32_t reg_id;
    uint32_t id_compositor, id_shell, id_shm;

    uint32_t *stage;
    uint32_t  stage_cap;
    uint32_t  stage_w, stage_h;
    int       stage_valid;
    wlshim_frame stage_frame;

    int      lfd;
    int      cfd;
    char     sockpath[128];
};

static uint32_t ev_at;               /* outbound event size backpatch pos */

static void o_hdr(wlshim *w, uint32_t obj, uint32_t opcode)
{
    uint32_t hdr[2];
    hdr[0] = 0;                      /* size backpatched at o_end */
    hdr[1] = (obj << 16) | opcode;
    if (w->olen + 8 > sizeof w->obuf) {
        w->dead = 1;
        return;
    }
    ev_at = (uint32_t)w->olen;
    memcpy(w->obuf + w->olen, hdr, 8);
    w->olen += 8;
}

static void o_end(wlshim *w)
{
    uint32_t sz = (uint32_t)(w->olen - ev_at);
    memcpy(w->obuf + ev_at, &sz, 4);
}

static void o_u32(wlshim *w, uint32_t v)
{
    memcpy(w->obuf + w->olen, &v, 4);
    w->olen += 4;
}

static void o_string(wlshim *w, const char *s)
{
    size_t n = strlen(s) + 1;
    size_t padded = (n + 3u) & ~3u;
    uint32_t len = (uint32_t)n;
    memcpy(w->obuf + w->olen, &len, 4);
    w->olen += 4;
    memcpy(w->obuf + w->olen, s, n);
    memset(w->obuf + w->olen + n, 0, padded - n);
    w->olen += padded;
}

static void emit_globals(wlshim *w)
{
    static const char *ifs[3] = { "wl_compositor", "wl_shell", "wl_shm" };
    uint32_t i;
    for (i = 0; i < 3; i++) {
        o_hdr(w, w->reg_id, 0);
        o_u32(w, i);
        o_string(w, ifs[i]);
        o_u32(w, 1);
        o_end(w);
    }
}

static void emit_format(wlshim *w)
{
    o_hdr(w, w->id_shm, 0);
    o_u32(w, WL_SHM_FORMAT_XRGB8888);
    o_end(w);
}

static void emit_done(wlshim *w, uint32_t cb)
{
    o_hdr(w, cb, 0);
    o_u32(w, 0);
    o_end(w);
}

static void emit_release(wlshim *w, uint32_t buf)
{
    o_hdr(w, buf, 0);
    o_end(w);
}

/* ----------------------------------------------------------------------
 * Core: streaming decoder. Requests arrive as whole messages from the
 * client; wlshim_ingest buffers bytes and decodes one message at a
 * time. Any malformed message or unknown object is a protocol error:
 * the connection is killed (spec-correct; the one real client never
 * triggers it).
 * ---------------------------------------------------------------------- */

typedef struct {
    const uint8_t *p;
    size_t         left;
    int            err;
} dec;

static uint32_t d_u32(dec *d)
{
    uint32_t v;
    if (d->left < 4) { d->err = 1; return 0; }
    memcpy(&v, d->p, 4);
    d->p += 4; d->left -= 4;
    return v;
}

static int32_t d_i32(dec *d)
{
    return (int32_t)d_u32(d);
}

static void d_string(dec *d, char *out, size_t cap)
{
    uint32_t n = d_u32(d);
    if (d->err) return;
    if (n > cap || n == 0) { d->err = 1; return; }
    size_t padded = ((size_t)n + 3u) & ~3u;
    if (d->left < padded) { d->err = 1; return; }
    memcpy(out, d->p, n - 1);
    out[n - 1] = 0;
    d->p += padded; d->left -= padded;
}

static void drop_obj(wlshim *w, uint32_t id)
{
    if (id >= WLSHIM_MAXID) return;
    if (w->kind[id] == WK_POOL && w->p_map[id]) {
#ifndef _WIN32
        munmap(w->p_map[id], w->p_size[id]);
#endif
        w->p_map[id] = NULL;
    }
    w->kind[id] = WK_NONE;
}

static void protocol_error(wlshim *w)
{
    w->dead = 1;
}

/* Handle one full request message. hdr[0]=size, hdr[1]=id<<16|opcode. */
static void handle_msg(wlshim *w, const uint8_t *msg, size_t len,
                       const int *fds, size_t nfd)
{
    dec d = { msg + 8, len - 8, 0 };
    uint32_t obj = ((const uint32_t *)msg)[1] >> 16;
    uint32_t op  = ((const uint32_t *)msg)[1] & 0xFFFFu;
    char s[128];
    uint32_t id, buf, size, ww, hh, stride, fmt;

    if (obj == 1) {                    /* wl_display */
        if (op == 0) {                 /* sync: answer done immediately */
            id = d_u32(&d);
            if (d.err || id == 0) { protocol_error(w); return; }
            emit_done(w, id);
        } else if (op == 1) {          /* get_registry */
            w->reg_id = d_u32(&d);
            if (d.err || w->reg_id == 0) { protocol_error(w); return; }
            if (w->reg_id < WLSHIM_MAXID) w->kind[w->reg_id] = WK_REGISTRY;
            emit_globals(w);
        } else protocol_error(w);
        return;
    }
    if (obj >= WLSHIM_MAXID) { protocol_error(w); return; }

    switch (w->kind[obj]) {
    case WK_REGISTRY:
        if (op != 0) { protocol_error(w); return; }
        (void)d_u32(&d);                /* name (global index) */
        d_string(&d, s, sizeof s);
        (void)d_u32(&d);                /* version: ignored, v1 only */
        id = d_u32(&d);                 /* new object id (LAST arg: bind
                                           is the one new_id-last request) */
        if (d.err || id == 0 || id >= WLSHIM_MAXID) { protocol_error(w); return; }
        if (strcmp(s, "wl_compositor") == 0) {
            w->kind[id] = WK_COMPOSITOR; w->id_compositor = id;
        } else if (strcmp(s, "wl_shell") == 0) {
            w->kind[id] = WK_SHELL; w->id_shell = id;
        } else if (strcmp(s, "wl_shm") == 0) {
            w->kind[id] = WK_SHM; w->id_shm = id;
            emit_format(w);
        } else { w->kind[id] = WK_NONE; }
        return;

    case WK_COMPOSITOR:
        if (op == 0) {                 /* create_surface */
            id = d_u32(&d);
            if (d.err || id == 0 || id >= WLSHIM_MAXID) { protocol_error(w); return; }
            w->kind[id] = WK_SURFACE;
        } else if (op == 1) {          /* create_region: accepted, unused */
            id = d_u32(&d);
            if (d.err || id == 0 || id >= WLSHIM_MAXID) { protocol_error(w); return; }
            w->kind[id] = WK_NONE;
        } else protocol_error(w);
        return;

    case WK_SHELL:
        if (op != 0) { protocol_error(w); return; }
        id = d_u32(&d);                /* new shell_surface id */
        buf = d_u32(&d);               /* surface id */
        if (d.err || id == 0 || id >= WLSHIM_MAXID ||
            w->kind[buf] != WK_SURFACE) { protocol_error(w); return; }
        w->kind[id] = WK_SHELL_SURFACE;
        return;

    case WK_SHELL_SURFACE:
        if (op == 0) drop_obj(w, obj);            /* destroy */
        else if (op == 3 || op == 4 || op == 5 ||
                 op == 6 || op == 7 || op == 9) { /* toplevel/transient/
                     fullscreen/popup/maximized/class: no payload to skip */
        }
        else if (op == 8) d_string(&d, s, sizeof s);  /* set_title */
        else protocol_error(w);
        return;

    case WK_SHM:
        if (op == 0) {                 /* create_pool: fd + i32 size */
            id = d_u32(&d);
            size = (uint32_t)d_i32(&d);
            if (d.err || id == 0 || id >= WLSHIM_MAXID || nfd < 1) {
                protocol_error(w); return;
            }
            w->kind[id] = WK_POOL;
            w->p_map[id] = NULL;
            w->p_size[id] = size;
            if (w->mapfn) w->p_map[id] = w->mapfn(w->mapud, fds[0], size);
        } else if (op == 1) {          /* release: ignore */
        } else protocol_error(w);
        return;

    case WK_POOL:
        if (op == 0) {                 /* create_buffer: newid, then
                                         * offset, width, height, stride,
                                         * format (offset is a real arg) */
            id = d_u32(&d);
            int32_t off = d_i32(&d);
            ww = (uint32_t)d_i32(&d);
            hh = (uint32_t)d_i32(&d);
            stride = (uint32_t)d_i32(&d);
            fmt = d_u32(&d);
            if (d.err || id == 0 || id >= WLSHIM_MAXID || off < 0 ||
                fmt != WL_SHM_FORMAT_XRGB8888 || ww == 0 || hh == 0 ||
                stride < ww * 4u) { protocol_error(w); return; }
            w->kind[id] = WK_BUFFER;
            w->b_pool[id] = obj;
            w->b_off[id] = (uint32_t)off;
            w->b_w[id] = ww;
            w->b_h[id] = hh;
            w->b_stride[id] = stride;
            w->b_fmt[id] = fmt;
        } else if (op == 1) drop_obj(w, obj);     /* destroy */
        else if (op == 2) { d_i32(&d); d_i32(&d); }  /* resize: ignored */
        else protocol_error(w);
        return;

    case WK_BUFFER:
        if (op == 0) drop_obj(w, obj);
        else protocol_error(w);
        return;

    case WK_SURFACE:
        if (op == 0) {                 /* destroy */
            drop_obj(w, obj);
        } else if (op == 1) {          /* attach */
            buf = d_u32(&d);
            d_i32(&d); d_i32(&d);      /* x, y */
            if (d.err) { protocol_error(w); return; }
            if (buf != 0 && w->kind[buf] != WK_BUFFER) {
                protocol_error(w); return;
            }
            w->s_buf[obj] = buf;
        } else if (op == 2) {          /* damage: ignored */
            d_i32(&d); d_i32(&d); d_i32(&d); d_i32(&d);
        } else if (op == 3) {          /* frame: queue callback */
            id = d_u32(&d);
            if (d.err || id == 0 || id >= WLSHIM_MAXID) {
                protocol_error(w); return;
            }
            if (w->s_ncb[obj] < WLSHIM_MAXFB)
                w->s_fbcb[obj][w->s_ncb[obj]++] = id;
        } else if (op == 6) {          /* commit */
            uint32_t bi = w->s_buf[obj];
            if (bi != 0 && w->kind[bi] == WK_BUFFER) {
                uint32_t pool = w->b_pool[bi];
                uint32_t need = w->b_stride[bi] * w->b_h[bi];
                if (w->kind[pool] == WK_POOL && w->p_map[pool] &&
                    need <= w->p_size[pool] &&
                    w->b_off[bi] + need <= w->p_size[pool]) {
                    if (need > w->stage_cap) {
                        uint32_t *ns = (uint32_t *)realloc(
                            w->stage, need ? need : 1u);
                        if (!ns) { protocol_error(w); return; }
                        w->stage = ns;
                        w->stage_cap = need;
                    }
                    memcpy(w->stage, w->p_map[pool] + w->b_off[bi], need);
                    w->stage_w = w->b_w[bi];
                    w->stage_h = w->b_h[bi];
                    w->stage_valid = 1;
                    w->stage_frame.w = w->stage_w;
                    w->stage_frame.h = w->stage_h;
                    w->stage_frame.px = w->stage;
                    if (w->on_frame) w->on_frame(w->ud, &w->stage_frame);
                    emit_release(w, bi);
                }
            }
            while (w->s_ncb[obj] > 0) {
                emit_done(w, w->s_fbcb[obj][--w->s_ncb[obj]]);
            }
        } else if (op >= 7) {          /* transform/scale/damage_buffer/offset */
            if (op == 7 || op == 8) d_u32(&d);
            else if (op == 9) { d_i32(&d); d_i32(&d); d_i32(&d); d_i32(&d); }
            else if (op == 10) { d_i32(&d); d_i32(&d); }
            else protocol_error(w);
        } else protocol_error(w);
        return;

    default:
        protocol_error(w);
        return;
    }
}

size_t wlshim_ingest(wlshim *w, const uint8_t *bytes, size_t len,
                     const int *fds, size_t nfd)
{
    size_t consumed = 0;
    while (consumed + 8 <= len && !w->dead) {
        uint32_t msize = ((const uint32_t *)(bytes + consumed))[0];
        if (msize < 8) { w->dead = 1; break; }
        if (msize > len - consumed) break;   /* need more bytes */
        handle_msg(w, bytes + consumed, msize, fds, nfd);
        consumed += msize;
    }
    return consumed;
}

size_t wlshim_out_drain(wlshim *w, uint8_t *out, size_t cap)
{
    size_t n = w->olen < cap ? w->olen : cap;
    if (n) memcpy(out, w->obuf, n);
    if (w->olen >= cap) {
        memmove(w->obuf, w->obuf + cap, w->olen - cap);
        w->olen -= cap;
    } else w->olen = 0;
    return n;
}

const wlshim_frame *wlshim_last_frame(const wlshim *w)
{
    if (!w->stage_valid) return NULL;
    return &w->stage_frame;
}

void wlshim_set_mapfn(wlshim *w, wlshim_map_fn fn, void *ud)
{
    w->mapfn = fn;
    w->mapud = ud;
}

int wlshim_dead(const wlshim *w)
{
    return w->dead;
}

/* ----------------------------------------------------------------------
 * POSIX glue
 * ---------------------------------------------------------------------- */
#ifndef _WIN32

static void wlshim_close_client(wlshim *w)
{
    int died = (w->cfd >= 0);
    if (w->cfd >= 0) { close(w->cfd); w->cfd = -1; }
    if (died && w->on_died) w->on_died(w->ud);
}

static void *posix_map(void *ud, int fd_index, uint32_t size)
{
    (void)ud;
    void *p;
    int mfd;
    if (size == 0) return NULL;
    mfd = dup(fd_index);
    if (mfd < 0) return NULL;
    p = mmap(NULL, size, PROT_READ, MAP_SHARED, mfd, 0);
    close(mfd);
    if (p == MAP_FAILED) return NULL;
    return p;
}

wlshim *wlshim_new(const char *xdg_runtime, const char *display,
                   wlshim_frame_fn on_frame, wlshim_died_fn on_died,
                   void *ud)
{
    wlshim *w = (wlshim *)calloc(1, sizeof *w);
    struct sockaddr_un sa;
    int s;
    if (!w) return NULL;
    w->on_frame = on_frame;
    w->on_died = on_died;
    w->ud = ud;
    w->lfd = -1;
    w->cfd = -1;
    wlshim_set_mapfn(w, posix_map, NULL);
    if (!xdg_runtime || !*xdg_runtime) { w->dead = 1; return w; }
    (void)snprintf(w->sockpath, sizeof w->sockpath, "%s/%s",
                   xdg_runtime, (display && *display) ? display : "wayland-0");
    s = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (s < 0) { w->dead = 1; return w; }
    unlink(w->sockpath);
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    if (strlen(w->sockpath) >= sizeof sa.sun_path) {
        close(s); w->dead = 1; return w;
    }
    strncpy(sa.sun_path, w->sockpath, sizeof sa.sun_path - 1);
    if (bind(s, (struct sockaddr *)&sa, sizeof sa) < 0 ||
        listen(s, 1) < 0) {
        close(s); w->dead = 1; return w;
    }
    w->lfd = s;
    return w;
}

int wlshim_pump(wlshim *w)
{
    if (w->dead) { wlshim_close_client(w); return -1; }
    if (w->cfd < 0) {
        w->cfd = accept(w->lfd, NULL, NULL);
        if (w->cfd >= 0)
            fcntl(w->cfd, F_SETFL, fcntl(w->cfd, F_GETFL, 0) | O_NONBLOCK);
    }
    if (w->cfd >= 0) {
        ssize_t got;
        for (;;) {
            got = recv(w->cfd, w->ibuf + w->ilen,
                       sizeof w->ibuf - w->ilen, 0);
            if (got > 0) {
                size_t used = wlshim_ingest(w, w->ibuf, (size_t)got,
                                            NULL, 0);
                if (used < (size_t)got) {
                    memmove(w->ibuf, w->ibuf + used, got - used);
                    w->ilen = (size_t)got - used;
                } else w->ilen = 0;
            } else if (got == 0) {
                wlshim_close_client(w);
                return -1;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else if (errno == EINTR) continue;
            else { wlshim_close_client(w); return -1; }
        }
        if (w->olen > 0 && !w->dead) {
            ssize_t sent = send(w->cfd, w->obuf, w->olen, MSG_NOSIGNAL);
            if (sent > 0) {
                memmove(w->obuf, w->obuf + sent, w->olen - (size_t)sent);
                w->olen -= (size_t)sent;
            } else if (sent < 0 && errno != EAGAIN && errno != EINTR) {
                wlshim_close_client(w);
                return -1;
            }
        }
    }
    if (w->dead) { wlshim_close_client(w); return -1; }
    return 0;
}

#else /* _WIN32 */

wlshim *wlshim_new(const char *xdg_runtime, const char *display,
                   wlshim_frame_fn on_frame, wlshim_died_fn on_died,
                   void *ud)
{
    wlshim *w = (wlshim *)calloc(1, sizeof *w);
    (void)xdg_runtime; (void)display;
    if (!w) return NULL;
    w->on_frame = on_frame;
    w->on_died = on_died;
    w->ud = ud;
    w->lfd = -1;
    w->cfd = -1;
    return w;
}

int wlshim_pump(wlshim *w)
{
    (void)w;
    return 0;
}

#endif /* _WIN32 */

void wlshim_stop(wlshim *w)
{
    if (!w) return;
#ifndef _WIN32
    if (w->cfd >= 0) close(w->cfd);
    if (w->lfd >= 0) { close(w->lfd); unlink(w->sockpath); }
#endif
}

void wlshim_free(wlshim *w)
{
    uint32_t i;
    if (!w) return;
    wlshim_stop(w);
    for (i = 0; i < WLSHIM_MAXID; i++) drop_obj(w, i);
    free(w->stage);
    free(w);
}
