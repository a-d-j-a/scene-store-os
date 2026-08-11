/*
 * test_launcher_app.c — a real app process for the launcher tests.
 *
 * Started by scene_launcher with SCENE_STORE_PORT set; connects back
 * over TCP, builds a window (via the full scene_app stack), auto-acks
 * input events (realistic consume semantics: the flow-control gate
 * reopens), and logs every input event to argv[1] (or nothing when no
 * log path is given). Runs until killed by the host.
 */
#include "scene_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
static void msleep(unsigned m) { Sleep(m); }
#else
#include <unistd.h>
static void msleep(unsigned m) { usleep(m * 1000); }
#endif

static scene_app *g_app;
static FILE      *g_log;

static void on_pointer(void *ud, uint64_t seq, int32_t x, int32_t y,
                       uint8_t buttons)
{
    (void)ud;
    if (g_log) {
        fprintf(g_log, "P %d %d %u\n", (int)x, (int)y, buttons);
        fflush(g_log);
    }
    scene_app_ack(g_app, seq);
}

static void on_activate(void *ud, uint64_t seq, scene_node_id id)
{
    (void)ud;
    if (g_log) {
        fprintf(g_log, "A %u\n", (unsigned)id);
        fflush(g_log);
    }
    scene_app_ack(g_app, seq);
}

static void on_key(void *ud, uint64_t seq, uint32_t key_code,
                   uint8_t state, uint8_t modifiers)
{
    (void)ud;
    if (g_log) {
        fprintf(g_log, "K %u %u %u\n", (unsigned)key_code, state, modifiers);
        fflush(g_log);
    }
    scene_app_ack(g_app, seq);
}

int main(int argc, char **argv)
{
    const char *port = getenv("SCENE_STORE_PORT");
    if (!port) return 2;
    if (argc > 1 && strcmp(argv[1], "no-connect") == 0) return 0;
    if (argc > 1) g_log = fopen(argv[1], "w");
    if (g_log) { fprintf(g_log, "C start port=%s\n", port); fflush(g_log); }

    char target[64];
    snprintf(target, sizeof(target), "127.0.0.1:%s", port);
    scene_transport *t = scene_tcp_client(target);
    if (!t) return 3;

    scene_app_cbs cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.pointer = on_pointer;
    cbs.activate = on_activate;
    cbs.key = on_key;
    g_app = scene_app_new_on(t, target, &cbs, NULL);
    if (!g_app) {
        if (g_log) { fprintf(g_log, "E app_new failed\n"); fflush(g_log); }
        return 4;
    }
    if (g_log) { fprintf(g_log, "C app connected\n"); fflush(g_log); }

    /* Wait for WELCOME (scene_client_welcomed), then build the window:
     * (100,50,240,160) with a button at (120,182). */
    scene_node_id content = SCENE_NO_PARENT;
    int i;
    for (i = 0; i < 500; i++) {
        scene_app_pump(g_app);
        scene_app_flush(g_app);
        if (scene_client_welcomed(scene_app_client(g_app))) break;
        msleep(5);
    }
    if (i >= 500) {
        if (g_log) { fprintf(g_log, "E no welcome\n"); fflush(g_log); }
        return 5;
    }
    if (g_log) { fprintf(g_log, "W welcomed\n"); fflush(g_log); }
    content = scene_app_create_window(g_app, 100, 50, 240, 160,
                                      "Launcher App");
    scene_client_create_node(scene_app_client(g_app), content, 41000u,
                             SCENE_ROLE_BUTTON,
                             &(scene_rect){120, 182, 100, 32},
                             SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    scene_client_set_text(scene_app_client(g_app), 41000u, 0, "Go", 2);
    scene_app_present(g_app);
    scene_app_flush(g_app);
    if (g_log) { fprintf(g_log, "B built win=%u\n", (unsigned)content); fflush(g_log); }

    for (;;) {
        scene_app_pump(g_app);
        scene_app_flush(g_app);
        msleep(5);
    }
}
