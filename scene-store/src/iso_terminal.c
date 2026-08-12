/*
 * iso_terminal.c — the terminal app for the ISO.
 *
 * A real shell in a scene window: a guest app hosted by the scene
 * launcher (started with SCENE_STORE_PORT set, connects back over
 * TCP on 127.0.0.1), builds a window through scene_app, embeds a
 * scene_terminal (PTY-backed busybox ash) into the content node,
 * and renders the visible buffer lines into the node's text slots.
 *
 * Keyboard: evdev key codes forwarded by the compositor; the terminal
 * maps them to ASCII and writes them to the PTY. The shell echoes.
 *
 * Flow control: every input event is acked (the gate reopens).
 * The render loop runs on the app's own tick; text slot count is
 * capped at 16 by the compositor (SCENE_COMPOSITOR_TEXT_CAP), so the
 * terminal shows 16 rows of 80 columns.
 */
#define _POSIX_C_SOURCE 200809L
#include "scene_app.h"
#include "scene_terminal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define COLS 80
#define ROWS 16
#define WIN_X 60
#define WIN_Y 40
#define WIN_W (COLS * 8 + 8)
#define WIN_H (32 + ROWS * 8 + 8)

static scene_app      *g_app;
static scene_terminal *g_term;
static scene_node_id   g_content, g_close;

/* ---- callbacks -------------------------------------------------------- */

static void on_pointer(void *ud, uint64_t seq, int32_t x, int32_t y,
                       uint8_t buttons)
{
    (void)ud; (void)x; (void)y; (void)buttons;
    scene_app_ack(g_app, seq);
}

static void on_activate(void *ud, uint64_t seq, scene_node_id id)
{
    (void)ud;
    if (id == g_close) {
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
    scene_terminal_input_key(g_term, key_code, state, modifiers);
    scene_app_ack(g_app, seq);
}

/* ---- render ------------------------------------------------------------ */

static void render_screen(void)
{
    int32_t top = scene_terminal_view_top(g_term);
    int32_t row;
    for (row = 0; row < ROWS; row++) {
        char *line = scene_terminal_line(g_term, top + row);
        if (line) {
            scene_app_set_text(g_app, g_content, (scene_text_id)row, line);
            free(line);
        } else {
            scene_app_set_text(g_app, g_content, (scene_text_id)row, " ");
        }
    }
    scene_app_present(g_app);
    scene_app_flush(g_app);
}

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *port = getenv("SCENE_STORE_PORT");
    fprintf(stderr, "iso-terminal: port=%s\n", port ? port : "(none)");
    if (!port) return 2;
    (void)argc; (void)argv;

    char target[64];
    snprintf(target, sizeof(target), "127.0.0.1:%s", port);
    scene_transport *t = scene_tcp_client(target);
    fprintf(stderr, "iso-terminal: transport=%s\n", t ? "ok" : "FAIL");
    if (!t) return 3;

    scene_app_cbs cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.pointer = on_pointer;
    cbs.activate = on_activate;
    cbs.key = on_key;
    g_app = scene_app_new_on(t, target, &cbs, NULL);
    fprintf(stderr, "iso-terminal: app=%s\n", g_app ? "ok" : "FAIL");
    if (!g_app) return 4;

    int i;
    for (i = 0; i < 500; i++) {
        scene_app_pump(g_app);
        scene_app_flush(g_app);
        if (scene_client_welcomed(scene_app_client(g_app))) break;
        msleep(5);
    }
    fprintf(stderr, "iso-terminal: welcome=%s (i=%d)\n",
            i < 500 ? "ok" : "TIMEOUT", i);
    if (i >= 500) return 5;

    g_content = scene_app_create_window(g_app, WIN_X, WIN_Y, WIN_W, WIN_H,
                                        "Terminal");
    fprintf(stderr, "iso-terminal: window id=%u\n", (unsigned)g_content);
    if (g_content == SCENE_NO_PARENT) return 6;
    g_close = g_content - 1;    /* scene_app: close = base+3, content = base+4 */

    scene_terminal_config tcfg;
    scene_terminal_config_defaults(&tcfg);
    tcfg.cols = COLS;
    tcfg.rows = ROWS;
    tcfg.bg_color = 0xFF0C0C0C;
    g_term = scene_terminal_new(g_app, g_content, &tcfg);
    fprintf(stderr, "iso-terminal: terminal=%s\n", g_term ? "ok" : "FAIL");
    if (!g_term) return 7;

    fprintf(stderr, "iso-terminal: running\n");
    for (;;) {
        scene_app_pump(g_app);
        scene_terminal_pump(g_term);
        render_screen();
        scene_app_flush(g_app);
        msleep(5);
    }
}
