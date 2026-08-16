/*
 * iso_edit.c — the first-party text editor demo guest app.
 *
 * A real app process hosted by the scene launcher: started with
 * SCENE_STORE_PORT set, connects back over TCP on 127.0.0.1, builds a
 * window through the full scene_app stack (titlebar + close + content)
 * and edits a text file:
 *
 *   argv[1] = file path (titlebar shows the base file name; max 32 KB,
 *             larger files are truncated at the last newline within the
 *             limit and the status shows "truncated"); argv[2] = event
 *             log path (tests) or nothing (stderr).
 *
 * Buffer model: a line array (max 128 lines, 256 chars each) built by
 * splitting the loaded file on '\n'. The view shows 12 LABEL rows
 * (ids base+10..base+21) that follow a view_y window; the view tracks
 * the cursor row (shifted so the cursor is always visible). A status
 * LABEL (base+7) reports "changed"/"saved"/"bad file"/"save failed"/
 * "truncated"; a Save BUTTON (base+6) writes the buffer back ("wb",
 * lines joined with '\n', final newline) — Ctrl+S (scancode 31 +
 * SCENE_MOD_CTRL) does the same.
 *
 * Editing is driven purely by the INPUT_KEY record stream (cbs.key):
 * printable scancodes insert at the cursor (letters + SHIFT uppercase,
 * digits, space, period), BACKSPACE deletes before the cursor (joining
 * lines at col 0), ENTER splits the line, arrows move the clamped
 * cursor. Every input record is acked (the flow-control gate reopens).
 * No timers: the app is record-driven and deterministic. Clicking the
 * close button destroys the window and exits 0; the host reaps the
 * session and the desktop repaints. No scene_app_pump inside input
 * callbacks (pass-17 rule: re-entering scene_client_pump while the
 * record is in flight recurses forever, 0xC00000FD).
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
/* window (100,50,440,300): titlebar 32px, content (100,82,440,268).
 * scene_app allocates base..base+4 (window/titlebar/title/close/content). */
#define EDIT_X 100
#define EDIT_Y 50
#define EDIT_W 440
#define EDIT_H 300

#define SAVE_NODE   40006u      /* base+6: save button         */
#define STATUS_NODE 40007u      /* base+7: status label        */
#define ROW_BASE    40010u      /* base+10..base+21: view rows */

#define EDIT_MAX_BYTES  32768u
#define EDIT_MAX_LINES  128
#define EDIT_MAX_CHARS  255      /* 256-byte lines incl NUL    */
#define EDIT_VIEW_ROWS  12

static scene_app *g_app;
static FILE      *g_log;
static scene_node_id g_content, g_close;
static char       g_path[512];
static char       g_lines[EDIT_MAX_LINES][EDIT_MAX_CHARS + 1];
static int        g_line_count;
static int        g_cursor_row, g_cursor_col;
static int        g_view_y;
static char       g_status[64];
static int        g_status_dirty;

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

/* ---- buffer ----------------------------------------------------------- */

static int line_len(int r) { return (int)strlen(g_lines[r]); }

static void set_status(const char *s)
{
    if (strcmp(g_status, s) == 0) return;
    snprintf(g_status, sizeof(g_status), "%s", s);
    g_status_dirty = 1;
    dlog("iso-edit: status=%s\n", g_status);
}

/* Load the file: max 32 KB; larger files truncate at the last newline
 * within the limit ("truncated"). Split on '\n'; the final empty
 * segment of a trailing newline is not created. Max 128 lines /
 * 256 chars per line, clamped.                                        */
static void load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t buf[EDIT_MAX_BYTES];
    long size = -1;
    size_t got = 0, len, start, i;
    int truncated = 0;

    g_line_count = 0;
    g_lines[0][0] = '\0';
    if (!f) { set_status("bad file"); return; }
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size >= 0 && fseek(f, 0, SEEK_SET) != 0) size = -1;
    if (size >= 0) got = fread(buf, 1, EDIT_MAX_BYTES, f);
    fclose(f);
    len = got;
    if (size > (long)EDIT_MAX_BYTES) {
        truncated = 1;
        while (len > 0 && buf[len - 1] != '\n') len--;
    }
    start = 0;
    for (i = 0; i < len && g_line_count < EDIT_MAX_LINES; i++) {
        if (buf[i] == '\n') {
            size_t l = i - start;
            if (l > EDIT_MAX_CHARS) l = EDIT_MAX_CHARS;
            memcpy(g_lines[g_line_count], buf + start, l);
            g_lines[g_line_count][l] = '\0';
            g_line_count++;
            start = i + 1;
        }
    }
    if (start < len && g_line_count < EDIT_MAX_LINES) {
        size_t l = len - start;
        if (l > EDIT_MAX_CHARS) l = EDIT_MAX_CHARS;
        memcpy(g_lines[g_line_count], buf + start, l);
        g_lines[g_line_count][l] = '\0';
        g_line_count++;
    }
    if (g_line_count == 0) { g_lines[0][0] = '\0'; g_line_count = 1; }
    g_cursor_row = 0;
    g_cursor_col = 0;
    g_view_y = 0;
    if (truncated) set_status("truncated");
    dlog("iso-edit: loaded %s: %d lines%s\n", path, g_line_count,
         truncated ? " (truncated)" : "");
}

/* Write the buffer back: lines joined with '\n', final newline. */
static void save_file(void)
{
    FILE *f = fopen(g_path, "wb");
    int i;

    if (!f) { set_status("save failed"); return; }
    for (i = 0; i < g_line_count; i++) {
        if (fputs(g_lines[i], f) == EOF || fputc('\n', f) == EOF) {
            fclose(f);
            set_status("save failed");
            return;
        }
    }
    if (fclose(f) != 0) { set_status("save failed"); return; }
    set_status("saved");
    dlog("iso-edit: saved %d lines to %s\n", g_line_count, g_path);
}

/* ---- editing ops (return 1 when the buffer changed) -------------------- */

static int edit_insert(char ch)
{
    int len = line_len(g_cursor_row);

    if (g_cursor_col > len) g_cursor_col = len;
    if (len >= EDIT_MAX_CHARS) return 0;
    memmove(g_lines[g_cursor_row] + g_cursor_col + 1,
            g_lines[g_cursor_row] + g_cursor_col, (size_t)len - g_cursor_col + 1);
    g_lines[g_cursor_row][g_cursor_col] = ch;
    g_cursor_col++;
    return 1;
}

static int edit_backspace(void)
{
    if (g_cursor_col > 0) {
        int len = line_len(g_cursor_row);
        memmove(g_lines[g_cursor_row] + g_cursor_col - 1,
                g_lines[g_cursor_row] + g_cursor_col, (size_t)len - g_cursor_col + 1);
        g_cursor_col--;
        return 1;
    }
    if (g_cursor_row > 0) {
        int prev_len = line_len(g_cursor_row - 1);
        int cur_len = line_len(g_cursor_row);
        int i;
        if (prev_len + cur_len > EDIT_MAX_CHARS) cur_len = EDIT_MAX_CHARS - prev_len;
        memcpy(g_lines[g_cursor_row - 1] + prev_len, g_lines[g_cursor_row],
               (size_t)cur_len);
        g_lines[g_cursor_row - 1][prev_len + cur_len] = '\0';
        for (i = g_cursor_row; i < g_line_count - 1; i++) {
            size_t l = strlen(g_lines[i + 1]) + 1;
            memmove(g_lines[i], g_lines[i + 1], l);
        }
        g_line_count--;
        g_cursor_row--;
        g_cursor_col = prev_len;
        return 1;
    }
    return 0;
}

static int edit_enter(void)
{
    int len = line_len(g_cursor_row);
    int r = g_cursor_row, i;

    if (g_cursor_col > len) g_cursor_col = len;
    if (g_line_count >= EDIT_MAX_LINES) return 0;
    for (i = g_line_count; i > r + 1; i--) {
        size_t l = strlen(g_lines[i - 1]) + 1;
        memmove(g_lines[i], g_lines[i - 1], l);
    }
    memmove(g_lines[r + 1], g_lines[r] + g_cursor_col,
            strlen(g_lines[r] + g_cursor_col) + 1);
    g_lines[r][g_cursor_col] = '\0';
    g_line_count++;
    g_cursor_row = r + 1;
    g_cursor_col = 0;
    return 1;
}

static int cursor_left(void)
{
    if (g_cursor_col > 0) { g_cursor_col--; return 1; }
    if (g_cursor_row > 0) {
        g_cursor_row--;
        g_cursor_col = line_len(g_cursor_row);
        return 1;
    }
    return 0;
}

static int cursor_right(void)
{
    if (g_cursor_col < line_len(g_cursor_row)) { g_cursor_col++; return 1; }
    if (g_cursor_row < g_line_count - 1) {
        g_cursor_row++;
        g_cursor_col = 0;
        return 1;
    }
    return 0;
}

static int cursor_up(void)
{
    if (g_cursor_row > 0) {
        g_cursor_row--;
        if (g_cursor_col > line_len(g_cursor_row))
            g_cursor_col = line_len(g_cursor_row);
        return 1;
    }
    return 0;
}

static int cursor_down(void)
{
    if (g_cursor_row < g_line_count - 1) {
        g_cursor_row++;
        if (g_cursor_col > line_len(g_cursor_row))
            g_cursor_col = line_len(g_cursor_row);
        return 1;
    }
    return 0;
}

/* ---- view --------------------------------------------------------------- */

/* The view tracks the cursor: if the cursor row leaves the 12-row
 * window, shift view_y so it is visible again (deterministic rule).   */
static void redraw(void)
{
    scene_client *cl;
    int i;

    if (g_cursor_row < g_view_y) g_view_y = g_cursor_row;
    else if (g_cursor_row >= g_view_y + EDIT_VIEW_ROWS)
        g_view_y = g_cursor_row - EDIT_VIEW_ROWS + 1;

    cl = scene_app_client(g_app);
    for (i = 0; i < EDIT_VIEW_ROWS; i++) {
        int line = g_view_y + i;
        const char *t = line < g_line_count ? g_lines[line] : "";
        scene_client_set_text(cl, ROW_BASE + (scene_node_id)i, 0, t,
                              (uint32_t)strlen(t));
    }
    if (g_status_dirty) {
        g_status_dirty = 0;
        scene_client_set_text(cl, STATUS_NODE, 0, g_status,
                              (uint32_t)strlen(g_status));
    }
    scene_app_present(g_app);
    scene_app_flush(g_app);
}

/* ---- input --------------------------------------------------------------- */

/* Printable scancodes: letters (evdev KEY_* values, SHIFT -> uppercase),
 * digits (spread 2..11), space, period. Anything else maps to '\0'.    */
static const struct { uint32_t code; char lo; } letter_map[] = {
    {30,'a'},{48,'b'},{46,'c'},{32,'d'},{18,'e'},{33,'f'},{34,'g'},{35,'h'},
    {23,'i'},{36,'j'},{37,'k'},{38,'l'},{50,'m'},{49,'n'},{24,'o'},{25,'p'},
    {16,'q'},{19,'r'},{31,'s'},{20,'t'},{22,'u'},{47,'v'},{17,'w'},{45,'x'},
    {21,'y'},{44,'z'}
};

static char printable_from(uint32_t code, uint8_t mods)
{
    size_t li;

    for (li = 0; li < sizeof(letter_map) / sizeof(letter_map[0]); li++) {
        if (code == letter_map[li].code) {
            char c = letter_map[li].lo;
            if (mods & SCENE_MOD_SHIFT) c = (char)(c - 'a' + 'A');
            return c;
        }
    }
    if (code >= 2 && code <= 11) return (char)('1' + (int)(code - 2));
    if (code == 57) return ' ';
    if (code == 52) return '.';
    return '\0';
}

static void on_key(void *ud, uint64_t seq, uint32_t key_code,
                   uint8_t state, uint8_t modifiers)
{
    char c;

    (void)ud;
    dlog("iso-edit: key %u state=%u mods=%u\n", key_code, state, modifiers);
    if (state == 1) {
        if (key_code == 31 && (modifiers & SCENE_MOD_CTRL)) {
            save_file();
            redraw();
        } else if (key_code == SCENE_KEY_BACKSPACE) {
            if (edit_backspace()) { set_status("changed"); redraw(); }
        } else if (key_code == SCENE_KEY_ENTER) {
            if (edit_enter()) { set_status("changed"); redraw(); }
        } else if (key_code == SCENE_KEY_LEFT) {
            if (cursor_left()) redraw();
        } else if (key_code == SCENE_KEY_RIGHT) {
            if (cursor_right()) redraw();
        } else if (key_code == SCENE_KEY_UP) {
            if (cursor_up()) redraw();
        } else if (key_code == SCENE_KEY_DOWN) {
            if (cursor_down()) redraw();
        } else if (!(modifiers & (SCENE_MOD_CTRL | SCENE_MOD_ALT |
                                  SCENE_MOD_SUPER))) {
            c = printable_from(key_code, modifiers);
            if (c) { edit_insert(c); set_status("changed"); redraw(); }
        }
    }
    scene_app_ack(g_app, seq);
}

static void on_pointer(void *ud, uint64_t seq, int32_t x, int32_t y,
                       uint8_t buttons)
{
    (void)ud;
    dlog("iso-edit: pointer %d,%d buttons=%u\n", (int)x, (int)y, buttons);
    scene_app_ack(g_app, seq);
}

static void on_activate(void *ud, uint64_t seq, scene_node_id id)
{
    (void)ud;
    dlog("iso-edit: activate id=%u\n", (unsigned)id);
    if (id == g_close) {
        dlog("iso-edit: close clicked, exiting\n");
        /* flush delivers the DESTROY op, then exit(0) closes the socket
         * and the host reaps the session. NO scene_app_pump here (pass-17:
         * pump inside an input callback re-enters scene_client_pump while
         * the INPUT_ACTIVATE record is still in flight -> recursion).   */
        scene_app_destroy_window(g_app, g_content);
        scene_app_flush(g_app);
        exit(0);
    }
    if (id == SAVE_NODE) {
        save_file();
        redraw();
    }
    scene_app_ack(g_app, seq);
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *port;
    const char *path;
    const char *base, *p;
    char title[64];
    char target[64];
    scene_transport *t;
    scene_app_cbs cbs;
    scene_client *cl;
    int i, r;

    port = getenv("SCENE_STORE_PORT");
    if (!port) return 2;
    if (argc < 2) { fprintf(stderr, "usage: iso_edit FILE [log]\n"); return 2; }
    path = argv[1];
    snprintf(g_path, sizeof(g_path), "%s", path);
    if (argc > 2) g_log = fopen(argv[2], "w");
    dlog("iso-edit: start port=%s file=%s\n", port, g_path);

    base = path;
    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    snprintf(title, sizeof(title), "%.31s", base);
    load_file(path);

    snprintf(target, sizeof(target), "127.0.0.1:%s", port);
    t = scene_tcp_client(target);
    if (!t) { dlog("iso-edit: tcp client failed\n"); return 3; }

    memset(&cbs, 0, sizeof(cbs));
    cbs.pointer = on_pointer;
    cbs.activate = on_activate;
    cbs.key = on_key;
    g_app = scene_app_new_on(t, target, &cbs, NULL);
    if (!g_app) { dlog("iso-edit: app_new failed\n"); return 4; }
    scene_tcp_set_nonblock(t, 1);            /* pass-17 lesson */
    dlog("iso-edit: connected, waiting for welcome\n");

    for (i = 0; i < 500; i++) {
        scene_app_pump(g_app);
        scene_app_flush(g_app);
        if (scene_client_welcomed(scene_app_client(g_app))) break;
        msleep(5);
    }
    if (i >= 500) { dlog("iso-edit: no welcome\n"); return 5; }
    dlog("iso-edit: welcomed\n");

    g_content = scene_app_create_window_role(g_app, EDIT_X, EDIT_Y, EDIT_W,
                                             EDIT_H, title,
                                             SCENE_ROLE_GENERIC);
    if (g_content == SCENE_NO_PARENT)
        { dlog("iso-edit: window create failed\n"); return 6; }
    g_close = g_content - 1;   /* scene_app: close = base+3, content = base+4 */

    cl = scene_app_client(g_app);
    for (r = 0; r < EDIT_VIEW_ROWS; r++) {
        scene_rect rr = {EDIT_X + 4, EDIT_Y + 36 + 16 * r, EDIT_W - 20, 16};
        scene_client_create_node(cl, g_content, ROW_BASE + (scene_node_id)r,
                                 SCENE_ROLE_LABEL, &rr, SCENE_FLAG_VISIBLE);
    }
    scene_client_create_node(cl, g_content, STATUS_NODE, SCENE_ROLE_LABEL,
                             &(scene_rect){104, 272, 200, 20},
                             SCENE_FLAG_VISIBLE);
    scene_client_create_node(cl, g_content, SAVE_NODE, SCENE_ROLE_BUTTON,
                             &(scene_rect){308, 270, 80, 22},
                             SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    scene_client_set_text(cl, SAVE_NODE, 0, "save", 4);
    scene_app_present(g_app);
    scene_app_flush(g_app);
    dlog("iso-edit: window built content=%u\n", (unsigned)g_content);
    g_status_dirty = 1;          /* emit the initial status slot ("") */
    redraw();

    for (;;) {
        scene_app_pump(g_app);
        scene_app_flush(g_app);
        msleep(5);
    }
}
