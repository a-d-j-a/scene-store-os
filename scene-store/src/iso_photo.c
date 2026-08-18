/*
 * iso_photo.c — the first-party photo viewer demo guest app.
 *
 * The honest-boundary shape of iso_video.c, for still images. argv[1] is
 * the image path but the app NEVER decodes the file — the OS-side
 * importer owns the pixels (iso_drm.c iso_import_cb, hooked to every
 * session via scene_server_set_import_cb; the harness mirrors it in
 * test_photo_app.c). The app requests the image over the wire
 * (scene_app_import_texture: 0x0017 carries only a ref + path), and
 * when the 0x800D result comes back ok, pushes only the texture
 * REFERENCE — scene_client_set_texture(content, ref, full src, blend 0,
 * opacity 255) + present + flush. The locked v0 wire carries no pixels;
 * the ref was registered into the session store by the host importer,
 * so the engine validates the op. At opacity 255 the ARGB/XRGB texel
 * replaces the pixel outright, and the content node is SCENE_ROLE_GENERIC
 * (transparent fill) so the displayed image equals the imported texels
 * byte-for-byte.
 *
 * Status texts (the semantic scene owns the state, an OS service or any
 * consumer reads it back):
 *   "ok"         0x800D arrived with ok=1 and the texture is pushed
 *   "bad image"  0x800D arrived with ok=0 — the host importer could not
 *                decode argv[1] (missing file, unsupported format)
 *   "no importer" the 0x0017 op could not even be emitted
 *                (transport dead / not welcomed / oom)
 *
 * Close → destroy + flush + exit(0), the host reaps the session.
 * NO scene_app_pump inside the input callback (the pass-17 recursion
 * lesson: pump re-enters dispatch on the in-flight record — seen
 * 0xC00000FD under the suite, proven under gdb). Deterministic: no
 * timers, no decoding, no per-frame work.
 */
#define _POSIX_C_SOURCE 200809L
#include "scene_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#define PHOTO_EXISTS(p) (_access((p), 0) == 0)
static void msleep(unsigned m) { Sleep(m); }
#else
#include <unistd.h>
#include <time.h>
#define PHOTO_EXISTS(p) (access((p), R_OK) == 0)
static void msleep(unsigned m)
{
    struct timespec ts = { (time_t)(m / 1000), (long)(m % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

/* ---- window layout --------------------------------------------------- */
/* window (100,50,300,220): titlebar 32px, content (100,82,300,188). The
 * status LABEL (id = app base+12 = 40012) is a child of the CONTENT node
 * at (104,88,284,16). */
#define PHOTO_X 100
#define PHOTO_Y 50
#define PHOTO_W 300
#define PHOTO_H 220
/* The texture ref this app requests via 0x0017 and later references in
 * SET_TEXTURE. The host importer registers whatever ref the app asks
 * for; nothing is pre-agreed or pre-seeded (the wire import replaced the
 * old ISO_PHOTO_PATH env pre-seed). */
#define PHOTO_REF    2u
#define PHOTO_STATUS 40012u

static scene_app     *g_app;
static FILE          *g_log;
static scene_node_id  g_content;   /* content = base+4 (scene_app layout) */

static void dlog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_log) vfprintf(g_log, fmt, ap);
    else vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (g_log) fflush(g_log);
    else fflush(stderr);
}

static void set_status(const char *txt)
{
    scene_app_set_text(g_app, PHOTO_STATUS, 0, txt);
    scene_app_present(g_app);
    scene_app_flush(g_app);
    dlog("iso-photo: status=%s\n", txt);
}

/* 0x800D: the host importer's answer to our 0x0017 request. ok=1 means
 * the ref is registered in our session store — push the reference now.
 * ok=0 means the decode failed; report "bad image" (a flush inside a
 * callback is fine — only pump is forbidden here, the pass-17 lesson). */
static void on_import_result(void *ud, scene_texture_ref ref, uint8_t ok)
{
    (void)ud;
    dlog("iso-photo: import_result ref=%u ok=%u\n", (unsigned)ref, ok);
    if (ok) {
        static const scene_rect full = {0, 0, PHOTO_W, PHOTO_H - 32};
        if (scene_client_set_texture(scene_app_client(g_app), g_content,
                                     ref, &full, 0, 255) == 0) {
            set_status("ok");
            return;
        }
        set_status("no importer");
        return;
    }
    set_status("bad image");
}

/* ---- input ------------------------------------------------------------- */

static void on_pointer(void *ud, uint64_t seq, int32_t x, int32_t y,
                       uint8_t buttons)
{
    (void)ud;
    dlog("iso-photo: pointer %d,%d buttons=%u\n", (int)x, (int)y, buttons);
    scene_app_ack(g_app, seq);
}

static void on_activate(void *ud, uint64_t seq, scene_node_id id)
{
    (void)ud;
    dlog("iso-photo: activate id=%u\n", (unsigned)id);
    if (id == g_content - 1) {      /* close button = base+3 */
        dlog("iso-photo: close clicked, exiting\n");
        /* flush delivers the DESTROY op; exit(0) closes the socket and
         * the host reaps the session. No pump here — pump inside an
         * input callback re-enters scene_client_pump while the
         * INPUT_ACTIVATE record is still in flight (in_off advances only
         * after dispatch returns), re-dispatching the same record forever
         * (stack overflow 0xC00000FD, seen in iso_play and proven under
         * gdb). */
        scene_app_destroy_window(g_app, g_content);
        scene_app_flush(g_app);
        exit(0);
    }
    scene_app_ack(g_app, seq);
}

static void on_key(void *ud, uint64_t seq, uint32_t key_code,
                   uint8_t state, uint8_t modifiers)
{
    (void)ud;
    dlog("iso-photo: key %u state=%u mods=%u\n", key_code, state, modifiers);
    scene_app_ack(g_app, seq);
}

static void on_focus(void *ud, uint64_t seq, scene_node_id id, uint8_t state)
{
    (void)ud;
    dlog("iso-photo: focus id=%u state=%u\n", (unsigned)id, state);
    scene_app_ack(g_app, seq);
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *img = argc > 1 ? argv[1] : "";
    const char *port = getenv("SCENE_STORE_PORT");
    if (!port) return 2;
    if (argc > 2) g_log = fopen(argv[2], "w");
    dlog("iso-photo: start port=%s img=%s\n", port, img);

    char target[64];
    snprintf(target, sizeof(target), "127.0.0.1:%s", port);
    scene_transport *t = scene_tcp_client(target);
    if (!t) { dlog("iso-photo: tcp client failed\n"); return 3; }

    scene_app_cbs cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.pointer = on_pointer;
    cbs.activate = on_activate;
    cbs.key = on_key;
    cbs.focus = on_focus;
    cbs.import_result = on_import_result;
    g_app = scene_app_new_on(t, target, &cbs, NULL);
    if (!g_app) { dlog("iso-photo: app_new failed\n"); return 4; }
    /* Non-blocking pump (the pass-17 lesson): a blocking recv on an
     * idle link hangs the app forever before its first flush.        */
    scene_tcp_set_nonblock(t, 1);
    dlog("iso-photo: connected, waiting for welcome\n");

    int i;
    for (i = 0; i < 500; i++) {
        scene_app_pump(g_app);
        scene_app_flush(g_app);
        if (scene_client_welcomed(scene_app_client(g_app))) break;
        msleep(5);
    }
    if (i >= 500) { dlog("iso-photo: no welcome\n"); return 5; }
    dlog("iso-photo: welcomed\n");

    /* title shows the base file name (informational only) */
    const char *base = img, *p;
    for (p = img; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    char title[32];
    snprintf(title, sizeof(title), "%.25s", base);

    g_content = scene_app_create_window_role(g_app, PHOTO_X, PHOTO_Y,
                                             PHOTO_W, PHOTO_H, title,
                                             SCENE_ROLE_GENERIC);
    if (g_content == SCENE_NO_PARENT) {
        dlog("iso-photo: window create failed\n");
        return 6;
    }
    dlog("iso-photo: window built content=%u\n", (unsigned)g_content);

    /* status label: child of the CONTENT node, in the content area */
    scene_client *cl = scene_app_client(g_app);
    {
        static const scene_rect sr = {PHOTO_X + 4, PHOTO_Y + 38,
                                      PHOTO_W - 16, 16};
        scene_client_create_node(cl, g_content, PHOTO_STATUS,
                                 SCENE_ROLE_LABEL, &sr, SCENE_FLAG_VISIBLE);
    }
    scene_app_present(g_app);
    scene_app_flush(g_app);

    /* Request the image from the OS importer: 0x0017 carries the path;
     * pixels stay host-side. The result arrives asynchronously (0x800D
     * → on_import_result) — no exists-check here: the importer's decode
     * verdict IS the truth. */
    if (scene_app_import_texture(g_app, PHOTO_REF, img) != 0)
        set_status("no importer");

    for (;;) {
        scene_app_pump(g_app);
        scene_app_flush(g_app);
        msleep(5);
    }
}