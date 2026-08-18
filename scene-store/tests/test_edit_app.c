/*
 * test_edit_app.c — the text editor guest-app proof.
 *
 * A REAL child process (iso_edit.exe, the text editor demo app) is
 * spawned with SCENE_STORE_PORT set, connects back over real TCP into a
 * launcher-style app session (harness-owned scene_server, composited as
 * layer 1 above a shell-less layer-0 canvas), loads a fixture file, and
 * edits it through injected compositor input. Tests:
 *
 *   1. test_edit_load: iso_edit build/edit_fix.txt joins; the window
 *      paints; store rows 0..2 read "hello","world","foo", row 3 is
 *      empty; the title label carries the base file name; the Save
 *      button sits at (308,270,80,22); close exits 0, desktop restored.
 *   2. test_edit_type_and_save: click the content (click-to-focus),
 *      type 'x' then 'y' at cursor (0,0) -> row 0 becomes "xyhello"
 *      (each key acked before the next is injected, flow control), the
 *      status reads "changed", then Ctrl+S -> the fixture FILE ON DISK
 *      reads "xyhello\nworld\nfoo\n" and the status reads "saved".
 *   3. test_edit_backspace: backspace at (0,0) is a no-op (status stays
 *      empty); 'a' then backspace leaves row 0 "hello" and a save
 *      writes the original content; DOWN + RIGHT x5 + ENTER splits
 *      line 1 at its end -> row 1 keeps "world", row 2 becomes "";
 *      a save then writes "hello\nworld\n\nfoo\n".
 *   4. test_edit_save_button: type 'z', click the Save button at its
 *      absolute rect center (read from node_vis) -> the disk file
 *      updates to "zhello\nworld\nfoo\n", status "saved".
 *   5. test_edit_bad_file: a nonexistent path still joins, builds the
 *      UI, shows "bad file", and exits 0 on close.
 *
 * All assertions read the app session's store directly or the fixture
 * file on disk — no timing guesses. Effects off: identity paint.
 */
#include "scene_compositor.h"
#include "scene_client.h"
#include "scene_transport.h"
#include "scene_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
static void msleep(unsigned m) { Sleep(m); }
#else
#include <unistd.h>
#include <sys/types.h>
#include <signal.h>
static void msleep(unsigned m) { usleep(m * 1000); }
#endif

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do {                                                    \
    checks++;                                                               \
    if (!(cond)) {                                                          \
        failures++;                                                         \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
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

static char *g_argv0;

/* ---- the app's node ids (scene_app APP_ID_BASE 40000 + layout) ---------- */
#define SAVE_NODE   40030u      /* base+30 */
#define STATUS_NODE 40031u      /* base+31 */
#define ROW_BASE    40010u      /* base+10..base+21 */
#define FIXTURE     "build/edit_fix.txt"
#define BADFILE     "no_such_edit_file_never_exists.txt"

/* ---- harness ------------------------------------------------------------ */

struct harness {
    scene_compositor *cp;
    scene_server     *sv;      /* app session server (NULL once reaped) */
    scene_loopback   *lb;      /* layer-0 shell link                    */
    scene_transport  *server_ts;
    scene_client     *sh_cl;   /* layer-0 shell client                  */

    scene_tcp_listener *listener;
    scene_transport *peer;
    uint16_t          port;
    uint32_t          pid;
    uintptr_t         hproc;
    int               joined;
    int               reaped;
    char              log_path[160];
};

static void kill_child(struct harness *h)
{
    if (!h->pid) return;
#if defined(_WIN32)
    TerminateProcess((HANDLE)h->hproc, 1);
    CloseHandle((HANDLE)h->hproc);
    h->hproc = 0;
#else
    kill((pid_t)h->pid, SIGTERM);
#endif
    h->pid = 0;
}

/* Child executable path: argv[0]-relative, then CWD-relative. */
static const char *sibling_exe_path(const char *name)
{
    static char path[1024];
    const char *a0 = g_argv0 ? g_argv0 : "";
    const char *slash = strrchr(a0, '/');
    const char *bs = strrchr(a0, '\\');
    const char *sep = NULL;
    if (slash && bs) sep = slash > bs ? slash : bs;
    else if (slash) sep = slash;
    else sep = bs;
    if (sep) {
        size_t n = (size_t)(sep - a0) + 1;
        if (n < sizeof(path)) {
            snprintf(path, sizeof(path), "%.*s%s", (int)n, a0, name);
            FILE *f = fopen(path, "rb");
            if (f) { fclose(f); return path; }
        }
    }
    snprintf(path, sizeof(path), "%s", name);
    return path;
}

static void spawn_child(struct harness *h, const char *file, const char *logarg)
{
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)h->port);
#if defined(_WIN32)
    SetEnvironmentVariableA("SCENE_STORE_PORT", portstr);
    char cmd[1400];
    snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\" \"%s\"",
             sibling_exe_path("iso_edit.exe"), file, logarg);
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi))
        return;
    CloseHandle(pi.hThread);
    h->pid = (uint32_t)pi.dwProcessId;
    h->hproc = (uintptr_t)pi.hProcess;
#else
    setenv("SCENE_STORE_PORT", portstr, 1);
    pid_t p = fork();
    if (p == 0) {
        execl(sibling_exe_path("iso_edit"), "iso_edit", file, logarg,
              (char *)NULL);
        _exit(127);
    }
    if (p < 0) return;
    h->pid = (uint32_t)p;
    h->hproc = (uintptr_t)p;
#endif
}

static void tickf(struct harness *h);

static void harness_init(struct harness *h, uint16_t *out_port)
{
    memset(h, 0, sizeof(*h));
    h->cp = scene_compositor_new(NULL, 800, 600);
    CHECK(h->cp != NULL);
    scene_compositor_set_clear(h->cp, 0xFF101010);
    scene_compositor_set_effects(h->cp, 0);   /* identity paint */

    /* Layer 0 (shell-less): bare desktop canvas. */
    h->lb = scene_loopback_new();
    h->server_ts = scene_loopback_server_end(h->lb);
    h->sh_cl = scene_client_new();
    scene_server_attach(scene_compositor_server(h->cp));
    scene_client_connect(h->sh_cl, scene_loopback_client_end(h->lb),
                         "shell", NULL, NULL);
    tickf(h);                                  /* WELCOME */
    static const scene_rect bg = {0, 0, 800, 600};
    CHECK(scene_client_create_node(h->sh_cl, SCENE_NO_PARENT, 10000,
            SCENE_ROLE_CANVAS, &bg, SCENE_FLAG_VISIBLE) == 0);
    CHECK(scene_client_present(h->sh_cl, 0) == 0);
    tickf(h);

    /* App session (layer 1): harness-owned server. */
    h->sv = scene_server_new(NULL);
    CHECK(h->sv != NULL);
    CHECK_EQ(scene_compositor_add_session(h->cp, h->sv), 1);
    scene_server_attach(h->sv);

    h->listener = scene_tcp_listen_new(0, out_port);
    CHECK(h->listener != NULL);
    h->port = *out_port;
}

static void reap_app(struct harness *h)
{
    if (h->reaped) return;
    scene_compositor_remove_session(h->cp, h->sv);
    scene_transport_close(h->peer);
    h->peer = NULL;
    h->sv = NULL;
    h->reaped = 1;
}

static void tickf(struct harness *h)
{
    if (h->peer) {
        uint8_t buf[8192];
        for (;;) {
            uint32_t got = 0;
            int r = scene_transport_recv(h->peer, buf, sizeof(buf), &got);
            if (r == 1) break;                       /* would-block */
            if (r != 0 || got == 0) { reap_app(h); break; }  /* closed */
            if (scene_server_feed(h->sv, buf, got) != 0)
                { reap_app(h); break; }
        }
        if (!h->reaped && h->sv) {
            const uint8_t *f;
            uint32_t flen;
            while (scene_server_out_next_frame(h->sv, &f, &flen) == 1)
                scene_transport_send(h->peer, f, flen);
        }
    } else if (h->listener) {
        scene_transport *peer = scene_tcp_listen_accept(h->listener);
        if (peer) {
            scene_tcp_set_nonblock(peer, 1);
            h->peer = peer;
            h->joined = 1;
        }
    }

    scene_client_flush(h->sh_cl);
    uint8_t buf[8192];
    uint32_t got;
    while (scene_transport_recv(h->server_ts, buf, sizeof(buf), &got) == 0
           && got) {
        if (scene_server_feed(scene_compositor_server(h->cp), buf, got)
            != 0) break;
    }
    const uint8_t *f;
    uint32_t flen;
    while (scene_server_out_next_frame(scene_compositor_server(h->cp),
                                       &f, &flen) == 1)
        scene_transport_send(h->server_ts, f, flen);
    scene_client_pump(h->sh_cl);
    scene_compositor_frame(h->cp);
}

static void pump_n(struct harness *h, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        tickf(h);
        msleep(5);
    }
}

static void harness_free(struct harness *h)
{
    if (!h->reaped && h->sv) {
        kill_child(h);
        pump_n(h, 50);
    } else if (h->pid) {
        kill_child(h);
    }
    if (h->peer) scene_transport_close(h->peer);
    if (h->listener) scene_tcp_listen_destroy(h->listener);
    if (h->sv) scene_compositor_remove_session(h->cp, h->sv);
    scene_client_free(h->sh_cl);
    scene_transport_close(h->server_ts);
    scene_loopback_free(h->lb);
    scene_compositor_free(h->cp);
}

static int child_exit_code(struct harness *h)
{
#if defined(_WIN32)
    DWORD r = WaitForSingleObject((HANDLE)h->hproc, 10000);
    if (r != WAIT_OBJECT_0) return -1;
    DWORD code = 0;
    if (!GetExitCodeProcess((HANDLE)h->hproc, &code)) return -1;
    CloseHandle((HANDLE)h->hproc);
    h->hproc = 0;
    if (code != 0) printf("DBG child exit code = 0x%08lX\n",
                          (unsigned long)code);
    return (int)code;
#else
    int status = 0;
    if (waitpid((pid_t)h->pid, &status, 0) < 0) return -1;
    h->pid = 0;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

/* ---- store read helpers -------------------------------------------------- */

static int row_text(const struct harness *h, int row, char *out, size_t cap)
{
    scene_node_text_vis tv[1];
    int n = scene_store_node_texts(scene_server_store(h->sv),
                                   ROW_BASE + (scene_node_id)row, tv, 1);
    if (n <= 0) { out[0] = '\0'; return -1; }
    if (tv[0].len == 0) { out[0] = '\0'; return 0; }
    if (tv[0].data == NULL) { out[0] = '\0'; return -1; }
    size_t l = tv[0].len < cap - 1 ? (size_t)tv[0].len : cap - 1;
    memcpy(out, tv[0].data, l);
    out[l] = '\0';
    return 0;
}

static int status_text(const struct harness *h, char *out, size_t cap)
{
    scene_node_text_vis tv[4];
    int n = scene_store_node_texts(scene_server_store(h->sv), STATUS_NODE,
                                   tv, 4);
    if (n <= 0) { out[0] = '\0'; return -1; }
    if (tv[0].len == 0) { out[0] = '\0'; return 0; }
    if (tv[0].data == NULL) { out[0] = '\0'; return -1; }
    size_t l = tv[0].len < cap - 1 ? (size_t)tv[0].len : cap - 1;
    memcpy(out, tv[0].data, l);
    out[l] = '\0';
    return 0;
}

/* Pump until store row `row` reads exactly `expect` (bounded). */
static void wait_row(struct harness *h, int row, const char *expect, int max)
{
    char got[300];
    int i;
    for (i = 0; i < max; i++) {
        if (h->reaped || !h->sv) return;
        if (row_text(h, row, got, sizeof(got)) == 0 &&
            strcmp(got, expect) == 0)
            return;
        tickf(h);
        msleep(5);
    }
}

/* Pump until the status label reads exactly `expect` (bounded). */
static void wait_status(struct harness *h, const char *expect, int max)
{
    char st[64];
    int i;
    for (i = 0; i < max; i++) {
        if (h->reaped || !h->sv) return;
        if (status_text(h, st, sizeof(st)) == 0 &&
            strcmp(st, expect) == 0)
            return;
        tickf(h);
        msleep(5);
    }
}

/* Pump until the fixture file on disk reads exactly `expect` (bounded). */
static void wait_file(struct harness *h, const char *expect, size_t elen,
                      int max)
{
    char buf[512];
    int i;
    for (i = 0; i < max; i++) {
        FILE *f = fopen(FIXTURE, "rb");
        size_t got = 0;
        if (f) {
            got = fread(buf, 1, sizeof(buf), f);
            fclose(f);
        }
        if (got == elen && memcmp(buf, expect, elen) == 0) return;
        tickf(h);
        msleep(5);
    }
}

/* ---- input injection ----------------------------------------------------- */

/* Click (down+up); each pointer record is acked before the next goes
 * through the flow gate. */
static void click_at(struct harness *h, int32_t x, int32_t y)
{
    scene_compositor_input_pointer(h->cp, 0, x, y, 1);
    pump_n(h, 8);
    scene_compositor_input_pointer(h->cp, 0, x, y, 0);
    pump_n(h, 8);
}

/* Inject a key press; pump until the ack reopens the flow gate before
 * the next injection (one key in flight). */
static void press_key(struct harness *h, uint32_t code, uint8_t mods)
{
    scene_compositor_input_key(h->cp, code, 1, mods);
    pump_n(h, 8);
}

/* ---- shared flows -------------------------------------------------------- */

static void write_fixture(void)
{
    FILE *f = fopen(FIXTURE, "wb");
    CHECK(f != NULL);
    if (f) {
        fputs("hello\nworld\nfoo", f);
        fclose(f);
    }
}

static void wait_window(struct harness *h, int max_iters)
{
    int i;
    for (i = 0; i < max_iters && PX(h->cp, 150, 150) != 0xFF202020u; i++) {
        tickf(h);
        msleep(5);
    }
}

static void wait_reaped(struct harness *h, int max_iters)
{
    int i;
    for (i = 0; i < max_iters && !h->reaped; i++) {
        tickf(h);
        msleep(5);
    }
}

/* Click the close button (512,54,24,24), wait for the reap, verify the
 * child exited 0 and the desktop repainted over the window. */
static void close_and_verify(struct harness *h)
{
    scene_compositor_input_pointer(h->cp, 0, 524, 66, 1);
    wait_reaped(h, 300);
    CHECK_EQ(h->reaped, 1);
    CHECK_EQ(child_exit_code(h), 0);
    pump_n(h, 10);
    CHECK(PX(h->cp, 150, 150) == 0xFF101010u);   /* desktop restored */
    CHECK(PX(h->cp, 260, 60) == 0xFF101010u);
}

/* ---- test 1: load -------------------------------------------------------- */

static void test_edit_load(void)
{
    struct harness h;
    uint16_t port = 0;
    scene_node_vis vis;
    scene_node_text_vis tv[1];

    write_fixture();
    harness_init(&h, &port);
    CHECK(port != 0);
    snprintf(h.log_path, sizeof(h.log_path), "edit_load.log");
    spawn_child(&h, FIXTURE, h.log_path);
    CHECK(h.pid != 0);

    wait_window(&h, 400);
    CHECK_EQ(h.joined, 1);
    CHECK(PX(h.cp, 150, 150) == 0xFF202020u);    /* window fill */
    CHECK(PX(h.cp, 260, 60) == 0xFF1A1A1Au);     /* titlebar */

    /* rows 0..2 carry the fixture lines; row 3 is empty */
    wait_row(&h, 0, "hello", 200);
    wait_row(&h, 1, "world", 200);
    wait_row(&h, 2, "foo", 200);
    {
        char got[64];
        CHECK(row_text(&h, 0, got, sizeof(got)) == 0 &&
              strcmp(got, "hello") == 0);
        CHECK(row_text(&h, 1, got, sizeof(got)) == 0 &&
              strcmp(got, "world") == 0);
        CHECK(row_text(&h, 2, got, sizeof(got)) == 0 &&
              strcmp(got, "foo") == 0);
        wait_row(&h, 3, "", 200);
        CHECK(row_text(&h, 3, got, sizeof(got)) == 0 && got[0] == '\0');
    }

    /* title label (base+2) carries the base file name */
    CHECK(scene_store_node_vis(scene_server_store(h.sv), 40002u, &vis) == 0);
    CHECK_EQ(scene_store_node_texts(scene_server_store(h.sv), 40002u, tv, 1), 1);
    {
        char tbuf[64];
        size_t tl = tv[0].len < sizeof(tbuf) - 1 ? (size_t)tv[0].len
                                                 : sizeof(tbuf) - 1;
        memcpy(tbuf, tv[0].data, tl);
        tbuf[tl] = '\0';
        CHECK(strstr(tbuf, "edit_fix.txt") != NULL);
    }

    /* save button geometry (used by test 4's click) */
    CHECK(scene_store_node_vis(scene_server_store(h.sv), SAVE_NODE, &vis) == 0);
    CHECK_EQ(vis.rect[0], 308);
    CHECK_EQ(vis.rect[1], 270);
    CHECK_EQ(vis.rect[2], 80);
    CHECK_EQ(vis.rect[3], 22);
    /* status label geometry */
    CHECK(scene_store_node_vis(scene_server_store(h.sv), STATUS_NODE, &vis) == 0);
    CHECK_EQ(vis.rect[0], 104);
    CHECK_EQ(vis.rect[1], 272);
    CHECK_EQ(vis.rect[2], 200);
    CHECK_EQ(vis.rect[3], 20);

    close_and_verify(&h);
    remove(h.log_path);
    remove(FIXTURE);
    harness_free(&h);
    printf("test_edit_load: ok\n");
}

/* ---- test 2: type + Ctrl+S ---------------------------------------------- */

static void test_edit_type_and_save(void)
{
    struct harness h;
    uint16_t port = 0;

    write_fixture();
    harness_init(&h, &port);
    snprintf(h.log_path, sizeof(h.log_path), "edit_type.log");
    spawn_child(&h, FIXTURE, h.log_path);
    CHECK(h.pid != 0);

    wait_window(&h, 400);
    CHECK_EQ(h.joined, 1);
    CHECK(PX(h.cp, 150, 150) == 0xFF202020u);

    /* click the content (click-to-focus: keys route to this layer) */
    click_at(&h, 300, 340);
    CHECK_EQ(scene_compositor_focus_is_shell(h.cp), 0);

    /* 'x' (45) then 'y' (21) at cursor (0,0): "xyhello" */
    press_key(&h, 45, 0);
    wait_row(&h, 0, "xhello", 200);
    press_key(&h, 21, 0);
    wait_row(&h, 0, "xyhello", 200);
    {
        char st[64];
        CHECK(status_text(&h, st, sizeof(st)) == 0 &&
              strcmp(st, "changed") == 0);
    }

    /* Ctrl+S: the fixture file on disk now ends with a final newline */
    press_key(&h, 31, SCENE_MOD_CTRL);
    wait_file(&h, "xyhello\nworld\nfoo\n", 18, 300);
    {
        FILE *f = fopen(FIXTURE, "rb");
        char buf[64];
        size_t got = 0;
        if (f) { got = fread(buf, 1, sizeof(buf), f); fclose(f); }
        CHECK(got == 18 && memcmp(buf, "xyhello\nworld\nfoo\n", 18) == 0);
    }
    {
        char st[64];
        CHECK(status_text(&h, st, sizeof(st)) == 0 &&
              strcmp(st, "saved") == 0);
    }
    /* the child's log proves welcome + both keys + the save */
    {
        FILE *f = fopen(h.log_path, "r");
        CHECK(f != NULL);
        if (f) {
            char line[256];
            int got_wel = 0, got_x = 0, got_y = 0, got_saved = 0;
            while (fgets(line, sizeof(line), f)) {
                if (strstr(line, "iso-edit: welcomed")) got_wel = 1;
                if (strstr(line, "key 45 state=1")) got_x = 1;
                if (strstr(line, "key 21 state=1")) got_y = 1;
                if (strstr(line, "iso-edit: saved")) got_saved = 1;
            }
            fclose(f);
            CHECK_EQ(got_wel, 1);
            CHECK_EQ(got_x, 1);
            CHECK_EQ(got_y, 1);
            CHECK_EQ(got_saved, 1);
        }
    }

    close_and_verify(&h);
    remove(h.log_path);
    remove(FIXTURE);
    harness_free(&h);
    printf("test_edit_type_and_save: ok\n");
}

/* ---- test 3: backspace, navigation, enter split ------------------------- */

static void test_edit_backspace(void)
{
    struct harness h;
    uint16_t port = 0;
    int i;

    write_fixture();
    harness_init(&h, &port);
    snprintf(h.log_path, sizeof(h.log_path), "edit_bs.log");
    spawn_child(&h, FIXTURE, h.log_path);
    CHECK(h.pid != 0);

    wait_window(&h, 400);
    CHECK_EQ(h.joined, 1);
    click_at(&h, 300, 340);

    /* backspace at (0,0) is a no-op: row 0 unchanged, status untouched */
    press_key(&h, SCENE_KEY_BACKSPACE, 0);
    wait_row(&h, 0, "hello", 200);
    {
        char got[64], st[64];
        CHECK(row_text(&h, 0, got, sizeof(got)) == 0 &&
              strcmp(got, "hello") == 0);
        CHECK(status_text(&h, st, sizeof(st)) == 0 && st[0] == '\0');
    }

    /* 'a' (30) then backspace: the buffer is back to "hello" */
    press_key(&h, 30, 0);
    wait_row(&h, 0, "ahello", 200);
    press_key(&h, SCENE_KEY_BACKSPACE, 0);
    wait_row(&h, 0, "hello", 200);
    {
        char got[64];
        CHECK(row_text(&h, 0, got, sizeof(got)) == 0 &&
              strcmp(got, "hello") == 0);
    }

    /* save: the disk file is the ORIGINAL content (nothing left) */
    press_key(&h, 31, SCENE_MOD_CTRL);
    wait_file(&h, "hello\nworld\nfoo\n", 16, 300);
    {
        FILE *f = fopen(FIXTURE, "rb");
        char buf[64];
        size_t got = 0;
        if (f) { got = fread(buf, 1, sizeof(buf), f); fclose(f); }
        CHECK(got == 16 && memcmp(buf, "hello\nworld\nfoo\n", 16) == 0);
    }

    /* DOWN, RIGHT x5 (end of "world"), ENTER: line 1 keeps the rest,
     * line 2 becomes empty, line 3 holds "foo" */
    press_key(&h, SCENE_KEY_DOWN, 0);
    for (i = 0; i < 5; i++) press_key(&h, SCENE_KEY_RIGHT, 0);
    press_key(&h, SCENE_KEY_ENTER, 0);
    wait_row(&h, 1, "world", 200);
    wait_row(&h, 2, "", 200);
    wait_row(&h, 3, "foo", 200);
    {
        char got[64];
        CHECK(row_text(&h, 1, got, sizeof(got)) == 0 &&
              strcmp(got, "world") == 0);
        CHECK(row_text(&h, 2, got, sizeof(got)) == 0 && got[0] == '\0');
        CHECK(row_text(&h, 3, got, sizeof(got)) == 0 &&
              strcmp(got, "foo") == 0);
    }

    /* save again: the empty line is on disk */
    press_key(&h, 31, SCENE_MOD_CTRL);
    wait_file(&h, "hello\nworld\n\nfoo\n", 17, 300);

    close_and_verify(&h);
    remove(h.log_path);
    remove(FIXTURE);
    harness_free(&h);
    printf("test_edit_backspace: ok\n");
}

/* ---- test 4: the Save button -------------------------------------------- */

static void test_edit_save_button(void)
{
    struct harness h;
    uint16_t port = 0;
    scene_node_vis vis;
    int32_t cx, cy;

    write_fixture();
    harness_init(&h, &port);
    snprintf(h.log_path, sizeof(h.log_path), "edit_savebtn.log");
    spawn_child(&h, FIXTURE, h.log_path);
    CHECK(h.pid != 0);

    wait_window(&h, 400);
    CHECK_EQ(h.joined, 1);
    click_at(&h, 300, 340);

    /* type 'z' (44): "zhello" */
    press_key(&h, 44, 0);
    wait_row(&h, 0, "zhello", 200);
    {
        char got[64];
        CHECK(row_text(&h, 0, got, sizeof(got)) == 0 &&
              strcmp(got, "zhello") == 0);
    }

    /* click the Save button at its absolute rect center */
    CHECK(scene_store_node_vis(scene_server_store(h.sv), SAVE_NODE, &vis) == 0);
    cx = vis.rect[0] + vis.rect[2] / 2;
    cy = vis.rect[1] + vis.rect[3] / 2;
    CHECK_EQ(cx, 348);
    CHECK_EQ(cy, 281);
    click_at(&h, cx, cy);

    wait_file(&h, "zhello\nworld\nfoo\n", 17, 300);
    {
        char st[64];
        CHECK(status_text(&h, st, sizeof(st)) == 0 &&
              strcmp(st, "saved") == 0);
    }

    close_and_verify(&h);
    remove(h.log_path);
    remove(FIXTURE);
    harness_free(&h);
    printf("test_edit_save_button: ok\n");
}

/* ---- test 5: bad file ---------------------------------------------------- */

static void test_edit_bad_file(void)
{
    struct harness h;
    uint16_t port = 0;
    int i;

    harness_init(&h, &port);
    snprintf(h.log_path, sizeof(h.log_path), "edit_bad.log");
    spawn_child(&h, BADFILE, h.log_path);
    CHECK(h.pid != 0);

    /* the UI still builds and paints */
    for (i = 0; i < 400 && PX(h.cp, 150, 150) != 0xFF202020u; i++) {
        tickf(&h);
        msleep(5);
    }
    CHECK_EQ(h.joined, 1);
    CHECK(PX(h.cp, 150, 150) == 0xFF202020u);

    wait_status(&h, "bad file", 400);
    {
        char st[64];
        CHECK(status_text(&h, st, sizeof(st)) == 0 &&
              strcmp(st, "bad file") == 0);
    }
    /* the buffer is a single empty line (nothing loaded) */
    wait_row(&h, 0, "", 200);
    {
        char got[64];
        CHECK(row_text(&h, 0, got, sizeof(got)) == 0 && got[0] == '\0');
    }

    close_and_verify(&h);
    remove(h.log_path);
    harness_free(&h);
    printf("test_edit_bad_file: ok\n");
}

int main(int argc, char **argv)
{
    (void)argc;
    g_argv0 = argv[0];

    test_edit_load();
    test_edit_type_and_save();
    test_edit_backspace();
    test_edit_save_button();
    test_edit_bad_file();

    printf("test_edit_app: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
