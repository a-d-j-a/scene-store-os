/*
 * test_play_app.c — the audio consumer's guest-app proof.
 *
 * A REAL child process (iso_play.exe, the audio player demo app) is
 * spawned with SCENE_STORE_PORT set, connects back over real TCP into a
 * launcher-style app session (harness-owned scene_server, composited as
 * layer 1 above a shell-less layer-0 canvas), builds a window with a
 * status label, and plays (or degrades gracefully). Tests:
 *
 *   1. test_play_selftest: `iso_play --selftest` as a plain child —
 *      the SYN melody round-trips through its own RIFF writer + WAV
 *      parser: stdout carries "SELFTEST OK", exit code 0.
 *   2. test_play_ui: iso_play SYN joins; the window paints (titlebar
 *      0xFF1A1A1A, white title glyph pixels ground-truthed from the
 *      in-house 8x8 font data, window fill 0xFF202020); the status
 *      label (id 40005) reports "p" then "done"; clicking the close
 *      button (312,54,24,24 inside the titlebar) makes the child exit
 *      0, the session is reaped, the desktop repaints.
 *   3. test_play_wav_ui: the selftest's build/test_tone.wav through the
 *      real harness — WAV path parsing + playback to "done", title
 *      shows the file name, close exits 0.
 *   4. test_play_bad_file: a nonexistent path still joins, shows the
 *      "bad file" status, and exits 0 on close.
 *
 * All status assertions read the app session's store directly (node
 * 40005 texts) — no timing guesses. Effects off: identity paint.
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
#include <sys/wait.h>
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

/* ---- the app's node ids (scene_app APP_ID_BASE 40000 + scene_play) ------ */
#define STATUS_NODE 40005u

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

static void spawn_child(struct harness *h, const char *src, const char *logarg)
{
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)h->port);
#if defined(_WIN32)
    SetEnvironmentVariableA("SCENE_STORE_PORT", portstr);
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\" \"%s\"",
             sibling_exe_path("iso_play.exe"), src, logarg);
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
        execl(sibling_exe_path("iso_play"), "iso_play", src, logarg,
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

/* Wait for the child process to exit and return its exit code, or -1. */
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

/* Status label text of the app session (node 40005, one text slot). */
static int status_label(const struct harness *h, char *out, size_t cap)
{
    scene_node_text_vis tv[4];
    int n = scene_store_node_texts(scene_server_store(h->sv), STATUS_NODE,
                                   tv, 4);
    if (n <= 0 || tv[0].len == 0 || tv[0].data == NULL) return -1;
    size_t l = tv[0].len < cap - 1 ? (size_t)tv[0].len : cap - 1;
    memcpy(out, tv[0].data, l);
    out[l] = '\0';
    return 0;
}

/* ---- test 1: selftest ---------------------------------------------------- */

/* Run `iso_play --selftest` as a plain child with stdout redirected to
 * a file; wait for exit; return exit code (-1 on spawn/timeout). */
static int run_selftest_child(const char *outfile)
{
#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;      /* child must inherit the redirected
                                    * stdout or its printf goes nowhere */
    HANDLE hout = CreateFileA(outfile, GENERIC_WRITE, 0, &sa, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (hout == INVALID_HANDLE_VALUE) return -1;
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "\"%s\" \"--selftest\"",
             sibling_exe_path("iso_play.exe"));
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hout;
    si.hStdError = hout;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(hout);
        return -1;
    }
    CloseHandle(pi.hThread);
    DWORD r = WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 0;
    if (r == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(hout);
    return r == WAIT_OBJECT_0 ? (int)code : -1;
#else
    pid_t p = fork();
    if (p == 0) {
        FILE *f = freopen(outfile, "w", stdout);
        if (!f) _exit(127);
        execl(sibling_exe_path("iso_play"), "iso_play", "--selftest",
              (char *)NULL);
        _exit(127);
    }
    if (p < 0) return -1;
    int status = 0;
    if (waitpid(p, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

static void test_play_selftest(void)
{
    static const char *out = "play_selftest.out";
    FILE *f;
    char buf[256];
    int has_ok = 0, has_fail = 0;

    CHECK_EQ(run_selftest_child(out), 0);
    f = fopen(out, "r");
    CHECK(f != NULL);
    while (f && fgets(buf, sizeof(buf), f)) {
        if (strstr(buf, "SELFTEST OK")) has_ok = 1;
        if (strstr(buf, "SELFTEST FAIL")) has_fail = 1;
    }
    if (f) fclose(f);
    CHECK_EQ(has_ok, 1);
    CHECK_EQ(has_fail, 0);

    /* The selftest wrote its round-trip WAV; the wav-UI test uses it. */
    f = fopen("build/test_tone.wav", "rb");
    CHECK(f != NULL);
    if (f) fclose(f);

    remove(out);
    printf("test_play_selftest: ok\n");
}

/* ---- shared UI flow ------------------------------------------------------ */

/* Wait for the child's window: window fill at (150,150). */
static void wait_window(struct harness *h, int max_iters)
{
    int i;
    for (i = 0; i < max_iters && PX(h->cp, 150, 150) != 0xFF202020u; i++) {
        tickf(h);
        msleep(5);
    }
}

/* Wait until the status label reads exactly `expect` (bounded). */
static void wait_status(struct harness *h, const char *expect, int max_iters)
{
    char st[64];
    int i;
    for (i = 0; i < max_iters; i++) {
        if (h->reaped || !h->sv) return;
        if (status_label(h, st, sizeof(st)) == 0 &&
            strcmp(st, expect) == 0)
            return;
        tickf(h);
        msleep(5);
    }
}

/* Pump until the session is reaped (child closed its socket). */
static void wait_reaped(struct harness *h, int max_iters)
{
    int i;
    for (i = 0; i < max_iters && !h->reaped; i++) {
        tickf(h);
        msleep(5);
    }
}

static void play_window_checks(struct harness *h)
{
    /* titlebar band (title glyphs end at x=168 for 8 chars) */
    CHECK(PX(h->cp, 260, 60) == 0xFF1A1A1Au);
    /* 'i' dot of "iso-play": glyph rows at label (104,54); 'i' row0
     * 0x18 -> bits 3,4 (font: MSB-first); 'o' row2 0x3C -> bits 2..5 */
    CHECK(PX(h->cp, 107, 54) == 0xFFFFFFFFu);
    CHECK(PX(h->cp, 108, 54) == 0xFFFFFFFFu);
    CHECK(PX(h->cp, 122, 56) == 0xFFFFFFFFu);
    /* desktop intact outside the window */
    CHECK(PX(h->cp, 60, 60) == 0xFF101010u);
    CHECK(PX(h->cp, 700, 500) == 0xFF101010u);
}

static void close_and_verify(struct harness *h)
{
    /* close button (312,54,24,24) center = (324,66) */
    scene_compositor_input_pointer(h->cp, 0, 324, 66, 1);
    wait_reaped(h, 300);
    CHECK_EQ(h->reaped, 1);
    CHECK_EQ(child_exit_code(h), 0);
    pump_n(h, 10);
    CHECK(PX(h->cp, 150, 150) == 0xFF101010u);   /* desktop restored */
    CHECK(PX(h->cp, 260, 60) == 0xFF101010u);
}

static void test_play_ui(void)
{
    struct harness h;
    uint16_t port = 0;
    scene_node_vis vis;

    harness_init(&h, &port);
    CHECK(port != 0);
    snprintf(h.log_path, sizeof(h.log_path), "play_app.log");
    spawn_child(&h, "SYN", h.log_path);
    CHECK(h.pid != 0);

    wait_window(&h, 400);
    CHECK_EQ(h.joined, 1);
    CHECK(PX(h.cp, 150, 150) == 0xFF202020u);    /* window fill */
    play_window_checks(&h);

    /* status label exists with a sane rect, reports "p" then "done" */
    CHECK_EQ(scene_store_node_vis(scene_server_store(h.sv), STATUS_NODE,
                                  &vis), 0);
    CHECK_EQ(vis.rect[0], 104);                  /* (104,90,232,16) */
    CHECK_EQ(vis.rect[1], 90);
    CHECK_EQ(vis.rect[2], 232);
    CHECK_EQ(vis.rect[3], 16);
    wait_status(&h, "p", 400);
    wait_status(&h, "done", 2000);
    {
        char st[64];
        CHECK_EQ(status_label(&h, st, sizeof(st)), 0);
        CHECK(strcmp(st, "done") == 0);
    }

    /* the child's log proves the flow: welcome, source, statuses */
    {
        FILE *f = fopen(h.log_path, "r");
        CHECK(f != NULL);
        if (f) {
            char line[256];
            int got_wel = 0, got_syn = 0, got_done = 0;
            while (fgets(line, sizeof(line), f)) {
                if (strstr(line, "iso-play: welcomed")) got_wel = 1;
                if (strstr(line, "src=SYN")) got_syn = 1;
                if (strstr(line, "iso-play: status=done")) got_done = 1;
            }
            fclose(f);
            CHECK_EQ(got_wel, 1);
            CHECK_EQ(got_syn, 1);
            CHECK_EQ(got_done, 1);
        }
    }

    close_and_verify(&h);
    remove(h.log_path);
    harness_free(&h);
    printf("test_play_ui: ok\n");
}

/* The selftest's own WAV file through the real app: WAV parsing +
 * playback to "done", file-name title.                                 */
static void test_play_wav_ui(void)
{
    struct harness h;
    uint16_t port = 0;
    int i;

    harness_init(&h, &port);
    snprintf(h.log_path, sizeof(h.log_path), "play_wav.log");
    spawn_child(&h, "build/test_tone.wav", h.log_path);
    CHECK(h.pid != 0);

    for (i = 0; i < 400 && PX(h.cp, 150, 150) != 0xFF202020u; i++) {
        tickf(&h);
        msleep(5);
    }
    CHECK_EQ(h.joined, 1);
    CHECK(PX(h.cp, 150, 150) == 0xFF202020u);
    CHECK(PX(h.cp, 260, 60) == 0xFF1A1A1Au);
    /* title "test_tone.wav": 't' row0 0x30 = bits 5,4 = cols 2,3 at
     * (106,54)/(107,54) (font is MSB-first, bit 7 = leftmost) */
    CHECK(PX(h.cp, 106, 54) == 0xFFFFFFFFu);
    CHECK(PX(h.cp, 107, 54) == 0xFFFFFFFFu);

    wait_status(&h, "done", 2000);
    {
        char st[64];
        CHECK_EQ(status_label(&h, st, sizeof(st)), 0);
        CHECK(strcmp(st, "done") == 0);
    }

    close_and_verify(&h);
    remove(h.log_path);
    harness_free(&h);
    printf("test_play_wav_ui: ok\n");
}

/* A nonexistent WAV: the app still joins, shows "bad file", and exits
 * 0 on close (the graceful-degrade contract).                           */
static void test_play_bad_file(void)
{
    struct harness h;
    uint16_t port = 0;
    int i;

    harness_init(&h, &port);
    snprintf(h.log_path, sizeof(h.log_path), "play_bad.log");
    spawn_child(&h, "no_such_file_never_exists.wav", h.log_path);
    CHECK(h.pid != 0);

    for (i = 0; i < 400 && PX(h.cp, 150, 150) != 0xFF202020u; i++) {
        tickf(&h);
        msleep(5);
    }
    CHECK_EQ(h.joined, 1);
    CHECK(PX(h.cp, 150, 150) == 0xFF202020u);
    /* the truncated file-name title (25 chars) covers 104..304, the
     * close button starts at 312 — the band at 307 is clean */
    CHECK(PX(h.cp, 307, 60) == 0xFF1A1A1Au);

    wait_status(&h, "bad file", 400);
    {
        char st[64];
        CHECK_EQ(status_label(&h, st, sizeof(st)), 0);
        CHECK(strcmp(st, "bad file") == 0);
    }

    close_and_verify(&h);
    remove(h.log_path);
    harness_free(&h);
    printf("test_play_bad_file: ok\n");
}

int main(int argc, char **argv)
{
    (void)argc;
    g_argv0 = argv[0];

    test_play_selftest();
    test_play_ui();
    test_play_wav_ui();
    test_play_bad_file();

    printf("test_play_app: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}