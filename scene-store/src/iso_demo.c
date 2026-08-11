/*
 * iso_demo.c — the first guest app on the ISO.
 *
 * A real app process hosted by the scene launcher: started by iso-drm
 * with SCENE_STORE_PORT set, connects back over TCP on 127.0.0.1, builds
 * a window through the full scene_app stack (titlebar + close + content),
 * and responds to input: clicking the button counts up (label + title
 * update), clicking the close button destroys the window and exits —
 * the host reaps the session and the desktop repaints. Events are logged
 * to a file given as argv[1] (tests) or stderr (ISO serial evidence).
 *
 * Flow control: every input event is acked (the gate reopens), so the
 * app keeps receiving at interactive rates.
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

static scene_app *g_app;
static FILE      *g_log;

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

/* ---- window layout --------------------------------------------------- */
/* window (120,60,300,220): titlebar 32px, content (120,92,300,188).
 * scene_app allocates base..base+4 (window/titlebar/title/close/content). */
#define WIN_X 120
#define WIN_Y 60
#define WIN_W 300
#define WIN_H 220

static scene_node_id g_content, g_close, g_button, g_label;
static int           g_count;

static void on_pointer(void *ud, uint64_t seq, int32_t x, int32_t y,
                       uint8_t buttons)
{
    (void)ud;
    dlog("iso-demo: pointer %d,%d buttons=%u\n", (int)x, (int)y, buttons);
    scene_app_ack(g_app, seq);
}

static void on_activate(void *ud, uint64_t seq, scene_node_id id)
{
    (void)ud;
    dlog("iso-demo: activate id=%u\n", (unsigned)id);
    if (id == g_close) {
        dlog("iso-demo: close clicked, exiting\n");
        scene_app_destroy_window(g_app, g_content);
        scene_app_flush(g_app);
        scene_app_pump(g_app);
        exit(0);
    }
    if (id == g_button) {
        char buf[64];
        g_count++;
        snprintf(buf, sizeof(buf), "count: %d", g_count);
        scene_app_set_text(g_app, g_label, 0, buf);
        snprintf(buf, sizeof(buf), "Demo (%d)", g_count);
        scene_app_set_title(g_app, g_content, buf);
        scene_app_present(g_app);
        scene_app_flush(g_app);
        dlog("iso-demo: count=%d\n", g_count);
    }
    scene_app_ack(g_app, seq);
}

static void on_key(void *ud, uint64_t seq, uint32_t key_code,
                   uint8_t state, uint8_t modifiers)
{
    (void)ud;
    dlog("iso-demo: key %u state=%u mods=%u\n", key_code, state, modifiers);
    scene_app_ack(g_app, seq);
}

int main(int argc, char **argv)
{
    const char *port = getenv("SCENE_STORE_PORT");
    if (!port) return 2;
    if (argc > 1 && strcmp(argv[1], "no-connect") == 0) return 0;
    if (argc > 1) g_log = fopen(argv[1], "w");
    dlog("iso-demo: start port=%s\n", port);

    char target[64];
    snprintf(target, sizeof(target), "127.0.0.1:%s", port);
    scene_transport *t = scene_tcp_client(target);
    if (!t) { dlog("iso-demo: tcp client failed\n"); return 3; }

    scene_app_cbs cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.pointer = on_pointer;
    cbs.activate = on_activate;
    cbs.key = on_key;
    g_app = scene_app_new_on(t, target, &cbs, NULL);
    if (!g_app) { dlog("iso-demo: app_new failed\n"); return 4; }
    dlog("iso-demo: connected, waiting for welcome\n");

    int i;
    for (i = 0; i < 500; i++) {
        scene_app_pump(g_app);
        scene_app_flush(g_app);
        if (scene_client_welcomed(scene_app_client(g_app))) break;
        msleep(5);
    }
    if (i >= 500) { dlog("iso-demo: no welcome\n"); return 5; }
    dlog("iso-demo: welcomed\n");

    g_content = scene_app_create_window(g_app, WIN_X, WIN_Y, WIN_W, WIN_H,
                                        "Demo");
    if (g_content == SCENE_NO_PARENT)
        { dlog("iso-demo: window create failed\n"); return 6; }
    g_close  = g_content - 1;    /* scene_app: close = base+3, content = base+4 */
    g_label  = g_content + 5;
    g_button = g_content + 6;

    scene_client *cl = scene_app_client(g_app);
    scene_client_create_node(cl, g_content, g_label,
                             SCENE_ROLE_LABEL,
                             &(scene_rect){136, 120, 200, 20},
                             SCENE_FLAG_VISIBLE);
    scene_client_set_text(cl, g_label, 0, "count: 0", 8);
    scene_client_create_node(cl, g_content, g_button,
                             SCENE_ROLE_BUTTON,
                             &(scene_rect){136, 160, 120, 34},
                             SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    scene_client_set_text(cl, g_button, 0, "Click me", 8);
    scene_app_present(g_app);
    scene_app_flush(g_app);
    dlog("iso-demo: window built content=%u\n", (unsigned)g_content);

    for (;;) {
        scene_app_pump(g_app);
        scene_app_flush(g_app);
        msleep(5);
    }
}
