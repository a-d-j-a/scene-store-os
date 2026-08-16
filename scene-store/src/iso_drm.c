/*
 * iso_drm.c — DRM/KMS compositor backend for the scene engine.
 *
 * Pure kernel-UAPI path: no libdrm, no wlroots, no mesa. DRM mode-setting
 * and page flips go straight through the linux/drm.h ioctls (the kernel
 * boundary — our own thin wrappers, no adopted code), input comes from
 * /dev/input/event* (evdev). The scene engine renders the desktop with
 * scene_compositor into a CPU-mapped dumb buffer; a page flip presents it.
 *
 * This is the boot path for the ISO: musl + busybox + kernel + this.
 * Wayland/wlroots remain an alternative backend for foreign-client
 * compatibility (iso_compositor.c), not a build dependency.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
/* The kernel asm-generic/ioctl.h (pulled by drm.h) redefines _IOC/_IO/...
 * that musl's bits/ioctl.h already defined — drop musl's copies first so
 * the DRM_IOCTL_* numbers come from the kernel definitions (identical on
 * x86_64, no redefinition warnings). */
#undef _IOC
#undef _IO
#undef _IOR
#undef _IOW
#undef _IOWR
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include <linux/input.h>

#include "scene_compositor.h"
#include "scene_shell.h"
#include "scene_launcher.h"
#include "scene_client.h"
#include "scene_transport.h"
#include "scene_server.h"
#include "scene_fb.h"
#include "scene_store.h"
#include "scene_image.h"

/* Kernel connector status (enum drm_connector_status): 1 = connected.
 * Not exposed as a UAPI constant — this is the uapi-visible value the
 * GETCONNECTOR ioctl fills into connection. */
#define ISO_DRM_CONNECTED 1u

/* ======================================================================
 * DRM plumbing (kernel UAPI, own wrappers)
 * ====================================================================== */

typedef struct fb_buf {
    uint32_t handle, fb_id, pitch, size;
    void    *map;
} fb_buf;

static int drm_ioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { perror("malloc"); exit(1); }
    return p;
}

/* Create a dumb CPU-mapped XRGB8888 buffer + a KMS framebuffer for it. */
static int dumb_create(int fd, uint32_t w, uint32_t h, fb_buf *b)
{
    struct drm_mode_create_dumb cd;
    memset(&cd, 0, sizeof cd);
    cd.height = h; cd.width = w; cd.bpp = 32; cd.flags = 0;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0)
        return -1;
    b->handle = cd.handle;
    b->pitch  = cd.pitch;
    b->size   = cd.size;

    struct drm_mode_map_dumb md;
    memset(&md, 0, sizeof md);
    md.handle = cd.handle;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0)
        return -1;
    b->map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  fd, md.offset);
    if (b->map == MAP_FAILED)
        return -1;

    /* ADDFB2 (modifiers) first, legacy ADDFB as fallback. */
    struct drm_mode_fb_cmd2 fb2;
    memset(&fb2, 0, sizeof fb2);
    fb2.width  = w;
    fb2.height = h;
    fb2.pixel_format = DRM_FORMAT_XRGB8888;
    fb2.handles[0] = cd.handle;
    fb2.pitches[0] = cd.pitch;
    fb2.modifier[0] = DRM_FORMAT_MOD_LINEAR;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb2) < 0) {
        struct drm_mode_fb_cmd fb1;
        memset(&fb1, 0, sizeof fb1);
        fb1.width  = w; fb1.height = h;
        fb1.pitch  = cd.pitch; fb1.bpp = 32; fb1.depth = 24;
        fb1.handle = cd.handle;
        if (drm_ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb1) < 0)
            return -1;
        b->fb_id = fb1.fb_id;
    } else {
        b->fb_id = fb2.fb_id;
    }
    return 0;
}

static void dumb_destroy(int fd, fb_buf *b)
{
    if (!b->map) return;
    drm_ioctl(fd, DRM_IOCTL_MODE_RMFB, &b->fb_id);
    struct drm_mode_destroy_dumb dd = { .handle = b->handle };
    drm_ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    munmap(b->map, b->size);
    memset(b, 0, sizeof *b);
}

/* Connector lookup: returns 0 on success. */
typedef struct drm_mode {
    int32_t            x, y;
    struct drm_mode_modeinfo info;
} drm_mode;

static int drm_get_connector(int fd, uint32_t id, uint32_t *conn_type,
                             uint32_t *connection, uint32_t *encoder_id,
                             struct drm_mode_modeinfo **modes,
                             uint32_t *mode_count)
{
    struct drm_mode_get_connector gc;
    memset(&gc, 0, sizeof gc);
    gc.connector_id = id;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) < 0)
        return -1;
    uint32_t nmodes = gc.count_modes;
    struct drm_mode_modeinfo *m =
        xmalloc((nmodes ? nmodes : 1) * sizeof(struct drm_mode_modeinfo));
    memset(&gc, 0, sizeof gc);
    gc.connector_id  = id;
    gc.modes_ptr     = (uint64_t)(uintptr_t)m;
    gc.count_modes   = nmodes;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) < 0) {
        free(m);
        return -1;
    }
    *conn_type    = gc.connector_type;
    *connection   = gc.connection;
    *encoder_id   = gc.encoder_id;
    *modes        = m;
    *mode_count   = gc.count_modes;
    return 0;
}

static int drm_get_encoder(int fd, uint32_t id, uint32_t *crtc_id,
                           uint32_t *possible_crtcs)
{
    struct drm_mode_get_encoder ge;
    memset(&ge, 0, sizeof ge);
    ge.encoder_id = id;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &ge) < 0)
        return -1;
    *crtc_id        = ge.crtc_id;
    *possible_crtcs = ge.possible_crtcs;
    return 0;
}

static int drm_set_crtc(int fd, uint32_t crtc_id, uint32_t fb_id,
                        uint32_t conn_id, const struct drm_mode_modeinfo *m)
{
    struct drm_mode_crtc c;
    memset(&c, 0, sizeof c);
    c.set_connectors_ptr = (uint64_t)(uintptr_t)&conn_id;
    c.count_connectors   = 1;
    c.crtc_id            = crtc_id;
    c.fb_id              = fb_id;
    c.mode_valid         = 1;
    c.mode               = *m;
    c.x = 0; c.y = 0;
    return drm_ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &c);
}

static int drm_page_flip(int fd, uint32_t crtc_id, uint32_t fb_id)
{
    struct drm_mode_crtc_page_flip pf;
    memset(&pf, 0, sizeof pf);
    pf.crtc_id = crtc_id;
    pf.fb_id   = fb_id;
    pf.flags   = DRM_MODE_PAGE_FLIP_EVENT;
    return drm_ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf);
}

static int drm_wait_flip(int fd, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int r = poll(&pfd, 1, timeout_ms);
    if (r <= 0) return -1;
    /* Page-flip events are full drm_event_vblank records (32 bytes);
     * reading only sizeof(struct drm_event) (8) desyncs the event queue
     * permanently — every later event read fails the type check and the
     * fd stays readable, spinning the main loop at 100% CPU. */
    struct drm_event_vblank ev;
    ssize_t n;
    do { n = read(fd, &ev, sizeof ev); } while (n < 0 && errno == EINTR);
    if (n < (ssize_t)sizeof ev) return -1;
    return ev.base.type == DRM_EVENT_FLIP_COMPLETE ? 0 : -1;
}

/* ======================================================================
 * evdev input
 * ====================================================================== */

#define MAX_DEV 16
typedef struct evdev_set {
    int      fds[MAX_DEV];
    int      n;
} evdev_set;

static int evdev_open(evdev_set *ev)
{
    ev->n = 0;
    DIR *d = opendir("/dev/input");
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d)) && ev->n < MAX_DEV) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        char path[300]; /* "/dev/input/" (11) + NAME_MAX (255) + NUL */
        snprintf(path, sizeof path, "/dev/input/%s", de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) ev->fds[ev->n++] = fd;
    }
    closedir(d);
    return ev->n > 0 ? 0 : -1;
}

/* ======================================================================
 * Shell client callbacks
 * ====================================================================== */

typedef struct ctx {
    scene_compositor *cp;
    scene_shell      *sh;
    scene_client     *cli;
    scene_transport  *server_ts;
    scene_loopback   *lb;
    scene_transport  *client_ts;
    scene_launcher   *launcher;
    int               run;
} ctx;

static void cb_welcome(void *ud, uint32_t sid, uint16_t ver,
                       const scene_limits *lim)
{
    (void)ud; (void)sid; (void)ver; (void)lim;
}

static void cb_activate(void *ud, uint64_t seq, scene_node_id id)
{
    ctx *c = ud;
    (void)seq;
    if (c->sh) scene_shell_handle_activate(c->sh, id);
}

static const scene_client_cbs shell_cbs = {
    .welcome       = cb_welcome,
    .input_activate = cb_activate,
};

/* ======================================================================
 * App launcher callbacks (the shell menu hands spawns to the host)
 * ====================================================================== */

/* ---- OS-side media importer (honest boundary) --------------------------
 * The v0 wire carries only texture REFERENCEs; the pixels live here, in
 * the OS. This demo importer re-runs the demo decoder (the same formula
 * as iso_video.c: frame n colors row y with (n*29+y)&0xFF, left half
 * G=0 B=0x40, right half G=0xFF B=0x80) into the app session's store
 * every compositor frame. A real system would decode the shared media
 * stream at this same seam. ------------------------------------------- */
#define IMP_REF  1u
#define IMP_W    240u
#define IMP_H    128u
static scene_compositor *g_imp_cp;
static scene_store      *g_imp_store;
static int               g_imp_layer = -1;
static uint32_t          g_imp_frame;
static uint32_t          g_imp_tex[IMP_W * IMP_H];

static void importer_tick(void)
{
    uint32_t x, y, n;
    if (!g_imp_cp || !g_imp_store || g_imp_layer < 0) return;
    n = g_imp_frame++;
    for (y = 0; y < IMP_H; y++) {
        uint32_t R = (n * 29u + y) & 0xFFu;
        for (x = 0; x < IMP_W; x++) {
            uint32_t c = UINT32_C(0xFF000000) | (R << 16);
            if (x < IMP_W / 2u) c |= UINT32_C(0x00000040);  /* left  */
            else                c |= UINT32_C(0x0000FF80);  /* right */
            g_imp_tex[y * IMP_W + x] = c;
        }
    }
    scene_compositor_register_texture_layer(g_imp_cp, g_imp_layer,
                                            IMP_REF, IMP_W, IMP_H,
                                            SCENE_TEX_FMT_XRGB, 1,
                                            g_imp_tex);
}

/* ---- OS-side photo import -----------------------------------------------
 * REMOVED (2026-08-15): iso_photo now requests its image over the wire
 * (0x0017 → iso_import_cb below); the ISO_PHOTO_PATH env pre-seed is
 * dead. Any path, any session, at runtime — open-with territory.      */

/* ---- Wire import hook (0x0017) ------------------------------------------
 * An app requests a file by ref over the wire; the OS decodes it at
 * this seam (scene_image: BMP/TGA in-house, PNG/JPEG/GIF via vendored
 * stb_image), registers ref+pixels into the session's layer, and
 * reports the result. The app's SET_TEXTURE validates only after a
 * successful import (or against the pre-seeded demo refs above).
 * Failure reports ok=0; the session stays alive (the app decides).
 * The layer is resolved from the server identity (not the last-joined
 * global): with several sessions at once, each import lands in the
 * session that asked.                                            */
static int iso_import_cb(void *ud, scene_server *sv, scene_texture_ref ref,
                         const char *path)
{
    ctx *c = ud;
    int w = 0, h = 0;
    uint32_t *px = NULL;
    int layer = -1;
    int i;
    int n = scene_compositor_layer_count(c->cp);
    for (i = 1; i < n; i++) {
        if (scene_compositor_layer_server(c->cp, i) == sv) {
            layer = i;
            break;
        }
    }

    fprintf(stderr, "iso-drm: import ref %u path %s (layer %d)\n",
            (unsigned)ref, path, layer);
    if (layer < 0) return -1;                      /* no such session */
    if (scene_image_load(path, &w, &h, &px) != 0 || !px || w <= 0 || h <= 0) {
        fprintf(stderr, "iso-drm: import %s: %s\n", path,
                scene_image_error());
        scene_image_free(px);
        return -1;
    }
    if (scene_compositor_register_texture_layer(c->cp, layer, ref,
                                                (uint32_t)w, (uint32_t)h,
                                                SCENE_TEX_FMT_ARGB, 1,
                                                px) != 0) {
        fprintf(stderr, "iso-drm: import %s: register failed\n", path);
        scene_image_free(px);
        return -1;
    }
    scene_image_free(px);
    fprintf(stderr, "iso-drm: imported %s: %dx%d ref %u\n", path, w, h,
            (unsigned)ref);
    return scene_server_import_result(sv, ref, 1);
}

static void cb_session_added(void *ud, int layer, uint32_t pid)
{
    ctx *c = ud;
    scene_server *sv;
    fprintf(stderr, "iso-drm: app %u joined layer %d\n", pid, layer);
    g_imp_cp = c->cp;
    g_imp_store = scene_compositor_layer_store(c->cp, layer);
    g_imp_layer = layer;
    g_imp_frame = 0;
    sv = scene_compositor_layer_server(c->cp, layer);
    if (sv) scene_server_set_import_cb(sv, iso_import_cb, c);
    /* Pre-seed the ref in the store so the app's wire SET_TEXTURE ops
     * validate (same seam as the test harness: store register once,
     * compositor register refreshes pixels per frame).              */
    scene_store_register_texture(g_imp_store, IMP_REF, IMP_W, IMP_H,
                                 SCENE_TEX_FMT_XRGB, 1);
}

static void cb_session_exited(void *ud, int layer, uint32_t pid)
{
    (void)ud;
    fprintf(stderr, "iso-drm: app %u exited layer %d\n", pid, layer);
    g_imp_store = NULL;
    g_imp_layer = -1;
    g_photo_store = NULL;   /* pixels are kept: reused by the next session */
}

static const scene_launcher_cbs launcher_cbs = {
    .session_added  = cb_session_added,
    .session_exited = cb_session_exited,
};

static void cb_launch(void *ud, uint32_t idx, const char *name)
{
    ctx *c = ud;
    (void)idx;
    uint32_t pid = 0;
    if (scene_launcher_spawn(c->launcher, name, NULL, &pid) != 0)
        fprintf(stderr, "iso-drm: spawn '%s' failed\n", name);
    else
        fprintf(stderr, "iso-drm: spawned '%s' pid=%u\n", name, pid);
}

/* Autostart: apps listed with --autolaunch=NAME on the command line are
 * spawned once the launcher is up (kernel cmdline autolaunch=NAME tokens
 * are forwarded by the init script). */
#define MAX_AUTOLAUNCH 4
static const char *g_autolaunch[MAX_AUTOLAUNCH];
static int g_autolaunch_n;

static void autolaunch_each(ctx *c)
{
    int i;
    for (i = 0; i < g_autolaunch_n; i++)
        cb_launch(c, (uint32_t)i, g_autolaunch[i]);
}

/* ---- debug diagnostics (--debug on the command line) ---------------- */

static int g_dbg;
static unsigned long long g_flips_ok, g_flips_fail, g_frames;

static void dbg_line(ctx *c, const fb_buf *bufs, int cur,
                     const struct drm_mode_modeinfo *mode)
{
    const scene_fb *fb = scene_compositor_fb(c->cp);
    scene_rect dr[8];
    uint32_t nd = scene_compositor_damage(c->cp, dr, 8);
    uint32_t fb_w = scene_fb_get(fb, 150, 150) & 0xFFFFFFu;
    uint32_t fb_b = scene_fb_get(fb, 170, 175) & 0xFFFFFFu;
    uint32_t db_w = 0xFFFFFFu, db_o = 0xFFFFFFu;
    if (bufs[cur].map && mode->hdisplay > 150 && mode->vdisplay > 175) {
        db_w = *(uint32_t *)((uint8_t *)bufs[cur].map +
                             (size_t)150 * bufs[cur].pitch + (size_t)150 * 4)
               & 0xFFFFFFu;
        db_o = *(uint32_t *)((uint8_t *)bufs[1 - cur].map +
                             (size_t)150 * bufs[1 - cur].pitch + (size_t)150 * 4)
               & 0xFFFFFFu;
    }
    fprintf(stderr,
            "iso-drm: D frames=%llu anims=%u damage=%u pxFb=0x%06X/0x%06X "
            "pxDumb=0x%06X/0x%06X flips=%llu/%llu cur=%d "
            "l0=%llu l1=%llu l2=%llu l3=%llu\n",
            g_frames, scene_compositor_anim_count(c->cp), nd, fb_w, fb_b,
            db_w, db_o, g_flips_ok, g_flips_fail, cur,
            (unsigned long long)scene_compositor_layer_seq(c->cp, 0),
            (unsigned long long)scene_compositor_layer_seq(c->cp, 1),
            (unsigned long long)scene_compositor_layer_seq(c->cp, 2),
            (unsigned long long)scene_compositor_layer_seq(c->cp, 3));
}

/* ======================================================================
 * Main loop plumbing: pump the shell client + compose + present
 * ====================================================================== */

static void pump_shell(ctx *c)
{
    scene_client_flush(c->cli);
    uint8_t buf[16384];
    uint32_t got;
    scene_server *srv = scene_compositor_server(c->cp);
    while (scene_transport_recv(c->server_ts, buf, sizeof buf, &got) == 0
           && got)
        scene_server_feed(srv, buf, got);
    const uint8_t *f;
    uint32_t flen;
    while (scene_server_out_next_frame(srv, &f, &flen) == 1)
        scene_transport_send(c->server_ts, f, flen);
    scene_client_pump(c->cli);
}

/* Present the whole framebuffer. The double-buffer scheme MUST NOT patch
 * damage rects into the back buffer: the back buffer holds the content
 * from two flips ago (each buffer alternates), so patching only the new
 * damage resurrects stale pixels from the older state — observed on the
 * ISO as the app window frozen at the enter animation's t=7 alpha after
 * a later shell repaint re-presented the stale buffer. A full copy per
 * damaged frame is cheap at desktop sizes (4 MB @ 60 Hz) and correct by
 * construction; damage-based present can return with a proper
 * per-buffer accumulation scheme. */
static void present_full(const scene_fb *fb, fb_buf *bufs, int back,
                         size_t fb_bytes)
{
    memcpy(bufs[back].map, fb->px, fb_bytes);
}

/* ======================================================================
 * Keyboard state → scene modifiers
 * ====================================================================== */

static uint8_t mods = 0;

static void key_event(ctx *c, uint16_t code, int32_t value)
{
    uint8_t state = value ? 1 : 0;
    uint8_t old = mods;
    switch (code) {
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT:
        mods = state ? (mods | SCENE_MOD_SHIFT) : (mods & ~SCENE_MOD_SHIFT);
        break;
    case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
        mods = state ? (mods | SCENE_MOD_CTRL) : (mods & ~SCENE_MOD_CTRL);
        break;
    case KEY_LEFTALT: case KEY_RIGHTALT:
        mods = state ? (mods | SCENE_MOD_ALT) : (mods & ~SCENE_MOD_ALT);
        break;
    case KEY_LEFTMETA: case KEY_RIGHTMETA:
        mods = state ? (mods | SCENE_MOD_SUPER) : (mods & ~SCENE_MOD_SUPER);
        break;
    default:
        break;
    }
    if (c->sh && scene_compositor_focus_is_shell(c->cp)
        && mods == 0 && (code == KEY_TAB || code == KEY_ENTER ||
                         code == KEY_ESC)) {
        /* shell hotkeys when no modifier is held and the shell has
         * keyboard focus (an app session's typing must reach the app) */
        if (scene_shell_handle_key(c->sh, code, state, 0))
            return;
    }
    scene_compositor_input_key(c->cp, code, state, mods);
    (void)old;
}

static void pointer_event(ctx *c, int32_t *x, int32_t *y, uint8_t *btns)
{
    scene_compositor_input_pointer(c->cp, 0, *x, *y, *btns);
    if (c->sh) scene_shell_handle_pointer(c->sh, *x, *y, *btns);
}

/* ======================================================================
 * Main
 * ====================================================================== */

static volatile sig_atomic_t g_run = 1;
static void on_signal(int s) { (void)s; g_run = 0; }

int main(int argc, char **argv)
{
    const char *card = "/dev/dri/card0";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--drm") == 0 && i + 1 < argc) {
            card = argv[++i];
        } else if (strncmp(argv[i], "--autolaunch=", 13) == 0) {
            if (g_autolaunch_n < MAX_AUTOLAUNCH)
                g_autolaunch[g_autolaunch_n++] = argv[i] + 13;
        } else if (strcmp(argv[i], "--debug") == 0) {
            g_dbg = 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int fd = open(card, O_RDWR | O_CLOEXEC);
    if (fd < 0) { fprintf(stderr, "iso-drm: open %s: %s\n", card,
                          strerror(errno)); return 1; }
    drm_ioctl(fd, DRM_IOCTL_SET_MASTER, NULL);

    /* ---- find connector + mode ---- */
    struct drm_mode_card_res res;
    memset(&res, 0, sizeof res);
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        fprintf(stderr, "iso-drm: GETRESOURCES failed\n"); return 1;
    }
    uint32_t *conns = xmalloc((res.count_connectors ? res.count_connectors : 1)
                              * 4);
    uint32_t *crtcs = xmalloc((res.count_crtcs ? res.count_crtcs : 1) * 4);
    /* Kernel GETRESOURCES only fills the id arrays when the request's
     * count fields are nonzero and >= the available counts (libdrm
     * reuses the counts returned by the first call). Zeroing them here
     * made the kernel skip the writes and leave malloc garbage (0) in
     * the arrays — the "GETCONNECTOR 0 failed" symptom. */
    uint32_t want_conns = res.count_connectors;
    uint32_t want_crtcs = res.count_crtcs;
    memset(&res, 0, sizeof res);
    res.connector_id_ptr = (uint64_t)(uintptr_t)conns;
    res.crtc_id_ptr      = (uint64_t)(uintptr_t)crtcs;
    res.count_connectors = want_conns;
    res.count_crtcs      = want_crtcs;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        fprintf(stderr, "iso-drm: GETRESOURCES(2) failed\n"); return 1;
    }
    fprintf(stderr, "iso-drm: %u connectors, %u crtcs\n",
            res.count_connectors, res.count_crtcs);

    uint32_t conn_id = 0, mode_cnt = 0, conn_enc = 0, conn_type = 0;
    struct drm_mode_modeinfo *modes = NULL;
    uint32_t i;
    for (i = 0; i < res.count_connectors; i++) {
        uint32_t t, state, enc, nmodes;
        struct drm_mode_modeinfo *m;
        if (drm_get_connector(fd, conns[i], &t, &state, &enc, &m, &nmodes) < 0) {
            fprintf(stderr, "iso-drm: GETCONNECTOR %u failed: %s\n",
                    conns[i], strerror(errno));
            continue;
        }
        fprintf(stderr, "iso-drm: conn %u state=%u enc=%u nmodes=%u\n",
                conns[i], state, enc, nmodes);
        if (state == ISO_DRM_CONNECTED && nmodes > 0) {
            conn_id = conns[i]; conn_type = t;
            conn_enc = enc; modes = m; mode_cnt = nmodes;
            break;
        }
        free(m);
    }
    if (!conn_id) {
        fprintf(stderr, "iso-drm: no connected connector with modes\n");
        return 1;
    }

    /* pick preferred mode, else first */
    struct drm_mode_modeinfo mode = modes[0];
    for (i = 0; i < mode_cnt; i++)
        if (modes[i].type & DRM_MODE_TYPE_PREFERRED) { mode = modes[i]; break; }

    /* ---- crtc ---- */
    uint32_t crtc_id = 0, possible = 0;
    if (conn_enc)
        drm_get_encoder(fd, conn_enc, &crtc_id, &possible);
    if (!crtc_id) {
        for (i = 0; i < res.count_crtcs; i++) {
            if (possible == 0 || (possible & (1u << i)))
                { crtc_id = crtcs[i]; break; }
        }
        if (!crtc_id && res.count_crtcs) crtc_id = crtcs[0];
    }
    if (!crtc_id) {
        fprintf(stderr, "iso-drm: no CRTC\n"); return 1;
    }

    fprintf(stderr, "iso-drm: %ux%u@%u on %s (connector type %u)\n",
            mode.hdisplay, mode.vdisplay, mode.vrefresh, card, conn_type);

    /* ---- buffers ---- */
    fb_buf bufs[2];
    memset(bufs, 0, sizeof bufs);
    if (dumb_create(fd, mode.hdisplay, mode.vdisplay, &bufs[0]) < 0 ||
        dumb_create(fd, mode.hdisplay, mode.vdisplay, &bufs[1]) < 0) {
        fprintf(stderr, "iso-drm: dumb buffer create failed\n"); return 1;
    }

    /* ---- scene engine + shell ---- */
    ctx c;
    memset(&c, 0, sizeof c);
    c.cp = scene_compositor_new(NULL, mode.hdisplay, mode.vdisplay);
    if (!c.cp) return 1;
    scene_compositor_set_effects(c.cp, 1);
    scene_compositor_set_clear(c.cp, 0xFF1A1A2E);

    c.lb  = scene_loopback_new();
    c.client_ts = scene_loopback_client_end(c.lb);
    c.server_ts = scene_loopback_server_end(c.lb);
    c.cli = scene_client_new();
    if (scene_client_connect(c.cli, c.client_ts, "iso-drm-shell",
                             &shell_cbs, &c) != 0) {
        fprintf(stderr, "iso-drm: shell client connect failed\n"); return 1;
    }
    /* Without attach the server never sends WELCOME, the client's emit
     * guard stays shut, and the scene stays empty (clear color only). */
    if (scene_server_attach(scene_compositor_server(c.cp)) != 0) {
        fprintf(stderr, "iso-drm: server attach failed\n"); return 1;
    }
    pump_shell(&c);   /* WELCOME */

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    if (access("/etc/shell.conf", R_OK) == 0)
        scene_shell_config_load(&cfg, "/etc/shell.conf");
    c.sh = scene_shell_new(c.cli, scene_compositor_store(c.cp), c.cp, &cfg);
    if (!c.sh) return 1;
    scene_shell_build(c.sh, mode.hdisplay, mode.vdisplay);
    scene_style_ref hov = scene_compositor_setup_hover_style(
        c.cp, cfg.hover_color, 0xFFE8E8E8);
    if (hov) scene_shell_set_hover_style(c.sh, hov);
    scene_style_ref act = scene_compositor_setup_active_style(
        c.cp, 0xFF2E4E6E, 0xFFE8E8E8);
    if (act) scene_shell_set_active_style(c.sh, act);
    pump_shell(&c);

    /* App launcher: hosts guest app processes as session layers. The
     * shell's launcher menu spawns through it. */
    c.launcher = scene_launcher_new(c.cp, NULL, &launcher_cbs, &c);
    if (!c.launcher) return 1;
    scene_shell_set_launch_cb(c.sh, cb_launch, &c);
    autolaunch_each(&c);
    pump_shell(&c);
    fprintf(stderr, "iso-drm: scene nodes=%u\n",
            scene_store_node_count(scene_compositor_store(c.cp)));

    /* ---- present initial frame ---- */
    drm_set_crtc(fd, crtc_id, bufs[0].fb_id, conn_id, &mode);

    evdev_set ev = { {0}, 0 };
    int have_ev = evdev_open(&ev) == 0;
    int32_t cx = mode.hdisplay / 2, cy = mode.vdisplay / 2;
    uint8_t btns = 0;
    int cur = 0;   /* index into bufs currently displayed */

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long next_ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    scene_rect drects[32];
    long last_dbg = 0;
    while (g_run) {
        /* ---- input ---- */
        struct pollfd pfds[MAX_DEV + 1];
        nfds_t npfds = 0;
        for (i = 0; i < (uint32_t)ev.n; i++) {
            pfds[npfds].fd = ev.fds[i];
            pfds[npfds].events = POLLIN;
            npfds++;
        }
        struct pollfd flipfd = { .fd = fd, .events = POLLIN };
        if (npfds < MAX_DEV) {
            pfds[npfds] = flipfd;
            npfds++;
        }
        int pr = poll(pfds, npfds, 10);
        if (pr > 0) {
            nfds_t j;
            for (j = 0; j < npfds; j++) {
                if (!(pfds[j].revents & POLLIN)) continue;
                int devfd = pfds[j].fd;
                if (devfd == fd) {
                    drm_wait_flip(fd, 0);
                    continue;
                }
                struct input_event ie;
                ssize_t n;
                while ((n = read(devfd, &ie, sizeof ie)) ==
                       (ssize_t)sizeof ie) {
                    if (ie.type == EV_KEY) {
                        if (ie.code == BTN_LEFT || ie.code == BTN_RIGHT) {
                            uint8_t mask = (ie.code == BTN_LEFT) ? 0x01 : 0x02;
                            btns = ie.value ? (btns | mask) : (btns & ~mask);
                            pointer_event(&c, &cx, &cy, &btns);
                        } else {
                            key_event(&c, ie.code, ie.value);
                        }
                    } else if (ie.type == EV_REL) {
                        if (ie.code == REL_X) cx += ie.value;
                        else if (ie.code == REL_Y) cy += ie.value;
                        if (cx < 0) cx = 0;
                        if (cy < 0) cy = 0;
                        if (cx >= (int32_t)mode.hdisplay)
                            cx = (int32_t)mode.hdisplay - 1;
                        if (cy >= (int32_t)mode.vdisplay)
                            cy = (int32_t)mode.vdisplay - 1;
                        pointer_event(&c, &cx, &cy, &btns);
                    } else if (ie.type == EV_ABS) {
                        int32_t mx = mode.hdisplay - 1, my = mode.vdisplay - 1;
                        if (ie.code == ABS_X)
                            cx = (int64_t)ie.value * mx / 32767;
                        else if (ie.code == ABS_Y)
                            cy = (int64_t)ie.value * my / 32767;
                        pointer_event(&c, &cx, &cy, &btns);
                    }
                }
            }
        }
        if (!have_ev) {
            usleep(10000);
        }

        /* ---- frame budget ---- */
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long now = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if (now < next_ms) continue;
        next_ms = now + 16;

        pump_shell(&c);
        if (c.sh) scene_shell_tick(c.sh);
        scene_launcher_pump(c.launcher);
        importer_tick();

        if (scene_compositor_frame(c.cp) != 0)
            break;
        g_frames++;

        uint32_t nd = scene_compositor_damage(c.cp, drects, 32);
        if (nd > 0 || scene_compositor_anim_count(c.cp) > 0) {
            int back = 1 - cur;
            const scene_fb *fb = scene_compositor_fb(c.cp);
            present_full(fb, bufs, back,
                         (size_t)mode.hdisplay * 4 * mode.vdisplay);
            if (drm_page_flip(fd, crtc_id, bufs[back].fb_id) == 0) {
                g_flips_ok++;
                cur = back;
            } else {
                g_flips_fail++;
                /* no flip possible: refresh the current buffer in place,
                 * and the back buffer too so its next present starts from
                 * this state (otherwise it holds stale pixels) */
                memcpy(bufs[cur].map, fb->px,
                       (size_t)mode.hdisplay * 4 * mode.vdisplay);
                memcpy(bufs[back].map, fb->px,
                       (size_t)mode.hdisplay * 4 * mode.vdisplay);
            }
        }
        if (g_dbg && now - last_dbg >= 1000) {
            last_dbg = now;
            dbg_line(&c, bufs, cur, &mode);
        }
    }

    /* ---- cleanup ---- */
    pump_shell(&c);
    scene_launcher_free(c.launcher);
    scene_shell_free(c.sh);
    scene_client_free(c.cli);
    scene_transport_close(c.client_ts);
    scene_transport_close(c.server_ts);
    scene_loopback_free(c.lb);
    scene_compositor_free(c.cp);
    dumb_destroy(fd, &bufs[0]);
    dumb_destroy(fd, &bufs[1]);
    drm_ioctl(fd, DRM_IOCTL_DROP_MASTER, NULL);
    close(fd);
    return 0;
}
