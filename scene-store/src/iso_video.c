/*
 * iso_video.c — the first-party video player demo guest app.
 *
 * Proves the honest boundary capability (spec §7): browser/video/WebGL
 * content arrives as composited textures. Started by the host with
 * SCENE_STORE_PORT set, connects back over TCP on 127.0.0.1, builds a
 * window through the full scene_app stack (titlebar + close + content)
 * and plays a synthetic video stream:
 *
 *   - Per frame the app (the "decoder") synthesizes an RGBA frame into
 *     its own buffer — a moving vertical gradient (frame n colors any
 *     row y with (n*29 + y) & 0xFF, halves split by a constant green
 *     delta) — and pushes it over the wire as a texture reference:
 *     scene_client_set_texture(content, ref 1, full src, blend 0,
 *     opacity 255) + present + flush. The locked v0 wire carries only
 *     the reference (spec §2: "TextureRef | u32, server-issued opaque
 *     handle"); the PIXELS live in the OS-side importer, which re-imports
 *     (re-registers) each decoded frame into the compositor's texture
 *     registry — the compositor refreshes pixels on re-register and
 *     dirties the referencing node. This mirrors the production path of
 *     the shell wallpaper (scene_shell.c registers + scene_client_set
 *     over the wire) and is exactly the architecture the honest-
 *     boundary line promises: the app owns the frame TIMELINE on the
 *     wire, the OS owns the imported pixels.
 *
 *   - The window's content node is SCENE_ROLE_GENERIC (transparent fill):
 *     at opacity 255 the XRGB texture replaces the pixel outright, so
 *     the displayed frame equals the imported texel byte-for-byte.
 *
 *   - Events are acked (the flow-control gate reopens) and logged to a
 *     file given as argv[1] (tests) or stderr. Clicking the close button
 *     destroys the window and exits; the host reaps the session.
 */
#define _POSIX_C_SOURCE 200809L
#include "scene_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#if defined(_WIN32)
#include <windows.h>
static void msleep(unsigned m) { Sleep(m); }
#else
#include <time.h>
static void msleep(unsigned m)
{
    struct timespec ts = { (time_t)(m / 1000), (long)(m % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

/* ---- window layout --------------------------------------------------- */
/* window (100,50,240,160): titlebar 32px, content (100,82,240,128). */
#define VID_X 100
#define VID_Y 50
#define VID_W 240
#define VID_H 160
#define VID_CW 240
#define VID_CH 128
#define VID_TEX_REF 1u

static scene_app *g_app;
static FILE      *g_log;
static scene_node_id g_content;     /* content = base+4 (scene_app layout) */

static const char *g_frame_algo =
    "frame n: A=0xFF, R=(n*29+y)&0xFF (vertical gradient), "
    "left half G=0 B=0x40, right half G=0xFF B=0x80";

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

/* ---- app-side decoder: synthesize frame n (XRGB pixels) -------------- */
static void gen_frame(uint32_t n, uint32_t *px)
{
    uint32_t y, x;

    for (y = 0; y < VID_CH; y++) {
        uint32_t R = (n * 29u + y) & 0xFFu;
        for (x = 0; x < VID_CW; x++) {
            uint32_t c = UINT32_C(0xFF000000) | (R << 16);
            if (x < VID_CW / 2u) c |= UINT32_C(0x00000040);  /* left  */
            else                 c |= UINT32_C(0x0000FF80);  /* right */
            px[y * VID_CW + x] = c;
        }
    }
}

static uint32_t *g_fb;              /* the app's decoded frame buffer */
static uint32_t  g_frame_n;         /* app-side stream counter        */

static void on_pointer(void *ud, uint64_t seq, int32_t x, int32_t y,
                       uint8_t buttons)
{
    (void)ud;
    dlog("iso-video: pointer %d,%d buttons=%u\n", (int)x, (int)y, buttons);
    scene_app_ack(g_app, seq);
}

static void on_activate(void *ud, uint64_t seq, scene_node_id id)
{
    (void)ud;
    dlog("iso-video: activate id=%u\n", (unsigned)id);
    if (id == g_content - 1) {      /* close button = base+3 */
        dlog("iso-video: close clicked, exiting\n");
        scene_app_destroy_window(g_app, g_content);
        scene_app_flush(g_app);
        scene_app_pump(g_app);
        exit(0);
    }
    scene_app_ack(g_app, seq);
}

static void on_key(void *ud, uint64_t seq, uint32_t key_code,
                   uint8_t state, uint8_t modifiers)
{
    (void)ud;
    dlog("iso-video: key %u state=%u mods=%u\n", key_code, state, modifiers);
    scene_app_ack(g_app, seq);
}

int main(int argc, char **argv)
{
    const char *port = getenv("SCENE_STORE_PORT");
    if (!port) return 2;
    if (argc > 1) g_log = fopen(argv[1], "w");
    dlog("iso-video: start port=%s (%s)\n", port, g_frame_algo);

    char target[64];
    snprintf(target, sizeof(target), "127.0.0.1:%s", port);
    scene_transport *t = scene_tcp_client(target);
    if (!t) { dlog("iso-video: tcp client failed\n"); return 3; }

    scene_app_cbs cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.pointer = on_pointer;
    cbs.activate = on_activate;
    cbs.key = on_key;
    g_app = scene_app_new_on(t, target, &cbs, NULL);
    if (!g_app) { dlog("iso-video: app_new failed\n"); return 4; }
    /* Non-blocking pump (the pass-17 lesson): a blocking recv on an
     * idle link hangs the app forever before its first flush.        */
    scene_tcp_set_nonblock(t, 1);
    dlog("iso-video: connected, waiting for welcome\n");

    int i;
    for (i = 0; i < 500; i++) {
        scene_app_pump(g_app);
        scene_app_flush(g_app);
        if (scene_client_welcomed(scene_app_client(g_app))) break;
        msleep(5);
    }
    if (i >= 500) { dlog("iso-video: no welcome\n"); return 5; }
    dlog("iso-video: welcomed\n");

    g_content = scene_app_create_window_role(g_app, VID_X, VID_Y, VID_W,
                                             VID_H, "Video",
                                             SCENE_ROLE_GENERIC);
    if (g_content == SCENE_NO_PARENT) {
        dlog("iso-video: window create failed\n");
        return 6;
    }
    g_fb = (uint32_t *)malloc((size_t)VID_CW * VID_CH * sizeof(uint32_t));
    if (!g_fb) { dlog("iso-video: frame buffer failed\n"); return 7; }
    scene_app_present(g_app);
    scene_app_flush(g_app);
    dlog("iso-video: window built content=%u\n", (unsigned)g_content);

    scene_client *cl = scene_app_client(g_app);
    static const scene_rect full = {0, 0, VID_CW, VID_CH};

    for (;;) {
        gen_frame(g_frame_n, g_fb);
        /* Frame push: the texture reference rides the wire (pixels are
         * OS-side: the importer re-registers the decoded frame).        */
        scene_client_set_texture(cl, g_content, VID_TEX_REF, &full, 0, 255);
        scene_app_present(g_app);
        scene_app_flush(g_app);
        scene_app_pump(g_app);
        if (g_frame_n == 0 || g_frame_n % 25 == 0)
            dlog("iso-video: frame %u px=%06X\n", g_frame_n,
                 g_fb[0] & UINT32_C(0x00FFFFFF));
        g_frame_n++;
        msleep(8);
    }
}