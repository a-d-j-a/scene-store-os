/*
 * test_files_app.c — the file browser's guest-app proof.
 *
 * A REAL child process (iso_files.exe, the first-party file browser)
 * is spawned with SCENE_STORE_PORT set, connects back over real TCP
 * into a launcher-style app session (harness-owned scene_server,
 * composited as layer 1 above a shell-less layer-0 canvas), builds a
 * window with an up button, a status label and 12 row labels, and
 * browses a deterministic fixture directory:
 *
*   build/files_fix/aa_dir/            (empty)
 *   build/files_fix/bb_file.txt        (4 bytes)
 *   build/files_fix/cc_dir/dd_file.bin
 *   build/files_fix/ee_file.xyz        (2 bytes, no association)
 *   build/files_fix/ff_pic.jpg         (copy of tests/fixtures/jpeg_fix.jpg)
 *
 * Tests (all store assertions read the app session's store directly —
 * node texts, not pixels; text is font-drawn):
 *
 *   1. test_files_list: rows show "D aa_dir", "D cc_dir",
 *      "F bb_file.txt", "F ee_file.xyz", "F ff_pic.jpg" (sorted, dirs
 *      first) in order; title contains "files_fix"; the ".." button and
 *      status label carry the expected texts and rects.
 *   2. test_files_navigate: clicking the aa_dir row (absolute rect
 *      from store node_vis) selects it, a second click enters it —
 *      rows empty, status "aa_dir", title "aa_dir"; clicking ".."
 *      returns to the fixture listing.
 *   3. test_files_file_status: clicking the ee_file.xyz row (unknown
 *      extension) twice sets the status to "file: ee_file.xyz".
 *   4. test_files_bad_dir: a nonexistent path joins, shows "bad dir",
 *      and exits 0 on close; the desktop repaints.
 *   5. test_files_open_with: clicking the bb_file.txt row twice spawns the
 *      real iso-edit child (inherits SCENE_STORE_PORT, reconnects to
 *      the harness listener as session 2, layer 2) which loads the
 *      file; clicking ff_pic.jpg spawns iso-photo, whose JPEG arrives
 *      over the real wire IMPORT_TEXTURE with the harness as the
 *      OS-seam importer. Both handlers close cleanly and reap.
 *
 * Effects off: identity paint. Deterministic fixture, cleaned up at
 * the end of the run.
 */
#include "scene_compositor.h"
#include "scene_client.h"
#include "scene_transport.h"
#include "scene_store.h"
#include "scene_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
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

/* ---- the app's node ids (scene_app base 40000 + iso_files layout) ------- */
#define F_TITLE  40002u
#define F_CLOSE  40003u
#define F_CONTENT 40004u
#define F_UP     40032u
#define F_STATUS 40033u
#define F_DEL    40034u
#define F_ROW0   40040u
#define EDIT_ROW0 40010u   /* iso-edit view rows (ROW_BASE in iso_edit.c) */

/* ---- fixture ------------------------------------------------------------ */

static void make_dir(const char *p)
{
#if defined(_WIN32)
    _mkdir(p);
#else
    mkdir(p, 0777);
#endif
}

static void rm_tree(const char *p)
{
    DIR *d = opendir(p);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            char full[1024];
            struct stat st;
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            snprintf(full, sizeof(full), "%s/%s", p, e->d_name);
            if (stat(full, &st) == 0 && (st.st_mode & S_IFDIR)) rm_tree(full);
            else remove(full);
        }
        closedir(d);
    }
#if defined(_WIN32)
    _rmdir(p);
#else
    rmdir(p);
#endif
}

static void make_fixture(void)
{
    FILE *f;
    make_dir("build");
    rm_tree("build/files_fix");
    make_dir("build/files_fix");
    make_dir("build/files_fix/aa_dir");            /* empty dir */
    f = fopen("build/files_fix/bb_file.txt", "wb");
    if (f) { fwrite("data", 1, 4, f); fclose(f); }
    make_dir("build/files_fix/cc_dir");
    f = fopen("build/files_fix/cc_dir/dd_file.bin", "wb");
    if (f) { fwrite("dddd", 1, 4, f); fclose(f); }
    f = fopen("build/files_fix/ee_file.xyz", "wb");
    if (f) { fwrite("ee", 1, 2, f); fclose(f); }
    f = fopen("build/files_fix/ff_pic.jpg", "wb");
    if (f) {
        FILE *g = fopen("tests/fixtures/jpeg_fix.jpg", "rb");
        if (g) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), g)) > 0)
                fwrite(buf, 1, n, f);
            fclose(g);
        }
        fclose(f);
    }
}

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

    /* open-with sessions (spawned handler apps, one at a time). */
    scene_server     *sv2;     /* handler session server                */
    scene_transport  *peer2;
    int               layer2;  /* compositor layer of the handler       */
    int               joined2;
    int               reaped2;
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

static void spawn_child(struct harness *h, const char *dir, const char *logarg)
{
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)h->port);
#if defined(_WIN32)
    SetEnvironmentVariableA("SCENE_STORE_PORT", portstr);
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\" \"%s\"",
             sibling_exe_path("iso_files.exe"), dir, logarg);
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
        execl(sibling_exe_path("iso_files"), "iso_files", dir, logarg,
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

static void reap_one(struct harness *h, scene_server **svp,
                     scene_transport **peerp, int *reapedp)
{
    if (*reapedp) return;
    if (*svp) scene_compositor_remove_session(h->cp, *svp);
    if (*peerp) scene_transport_close(*peerp);
    *peerp = NULL;
    *svp = NULL;
    *reapedp = 1;
}

/* The open-with handler session's importer: the harness itself sits at
 * the OS seam, like iso_drm's importer_tick — decode with scene_image,
 * register the pixels into the handler's layer, answer the wire
 * IMPORT_TEXTURE with ok=1. */
static int opener_import(void *ud, scene_server *sv, scene_texture_ref ref,
                         const char *path)
{
    struct harness *h = ud;
    int w = 0, hh = 0;
    uint32_t *px = NULL;

    if (scene_image_load(path, &w, &hh, &px) != 0 || !px || w <= 0 || hh <= 0) {
        if (px) free(px);
        return -1;
    }
    if (h->layer2 > 0
        && scene_compositor_register_texture_layer(h->cp, h->layer2, ref,
                                                   (uint32_t)w, (uint32_t)hh,
                                                   SCENE_TEX_FMT_ARGB, 1,
                                                   px) != 0) {
        free(px);
        return -1;
    }
    free(px);
    return scene_server_import_result(sv, ref, 1);
}

/* Pump one connected session: drain inbound (feeding the server), push
 * outbound to the peer, reap on error/close. */
static void session_pump(struct harness *h, scene_server **svp,
                         scene_transport **peerp, int *reapedp)
{
    uint8_t buf[8192];
    uint32_t got;

    if (!*svp || !*peerp) return;
    for (;;) {
        got = 0;
        int r = scene_transport_recv(*peerp, buf, sizeof(buf), &got);
        if (r == 1) break;                       /* would-block */
        if (r != 0 || got == 0) { reap_one(h, svp, peerp, reapedp); return; }
        if (scene_server_feed(*svp, buf, got) != 0) {
            reap_one(h, svp, peerp, reapedp);
            return;
        }
    }
    if (*svp) {
        const uint8_t *f;
        uint32_t flen;
        while (scene_server_out_next_frame(*svp, &f, &flen) == 1)
            scene_transport_send(*peerp, f, flen);
    }
}

static void tickf(struct harness *h)
{
    if (h->listener) {
        scene_transport *peer;
        while ((peer = scene_tcp_listen_accept(h->listener)) != NULL) {
            scene_tcp_set_nonblock(peer, 1);
            if (!h->peer) {
                h->peer = peer;
                h->joined = 1;
            } else if (!h->peer2 && h->sv2 == NULL) {
                h->peer2 = peer;
                h->sv2 = scene_server_new(NULL);
                CHECK(h->sv2 != NULL);
                h->layer2 = scene_compositor_add_session(h->cp, h->sv2);
                CHECK(h->layer2 > 1);
                scene_server_set_import_cb(h->sv2, opener_import, h);
                scene_server_attach(h->sv2);
                h->joined2 = 1;
                h->reaped2 = 0;
            } else {
                scene_transport_close(peer);
                break;
            }
        }
    }

    session_pump(h, &h->sv, &h->peer, &h->reaped);
    session_pump(h, &h->sv2, &h->peer2, &h->reaped2);

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

static int node_text(const struct harness *h, scene_node_id id,
                     char *out, size_t cap)
{
scene_node_text_vis tv[2];
    int n = scene_store_node_texts(scene_server_store(h->sv), id, tv, 2);
    if (n <= 0) return -1;
    size_t l = tv[0].len < cap - 1 ? (size_t)tv[0].len : cap - 1;
    memcpy(out, tv[0].data ? tv[0].data : "", l);
    out[l] = '\0';
    return 0;
}

/* Same, on the open-with handler session's store. */
static int node_text2(const struct harness *h, scene_node_id id,
                      char *out, size_t cap)
{
    scene_node_text_vis tv[2];
    int n = -1;
    if (!h->sv2 || h->reaped2) return -1;
    n = scene_store_node_texts(scene_server_store(h->sv2), id, tv, 2);
    if (n <= 0) return -1;
    size_t l = tv[0].len < cap - 1 ? (size_t)tv[0].len : cap - 1;
    memcpy(out, tv[0].data ? tv[0].data : "", l);
    out[l] = '\0';
    return 0;
}

static int node_rect(const struct harness *h, scene_node_id id,
                     scene_node_vis *v)
{
    return scene_store_node_vis(scene_server_store(h->sv), id, v);
}

static int node_rect2(const struct harness *h, scene_node_id id,
                      scene_node_vis *v)
{
    if (!h->sv2 || h->reaped2) return -1;
    return scene_store_node_vis(scene_server_store(h->sv2), id, v);
}

static void wait_status(struct harness *h, const char *expect, int max_iters)
{
    char st[80];
    int i;
    for (i = 0; i < max_iters; i++) {
        if (h->reaped || !h->sv) return;
        if (node_text(h, F_STATUS, st, sizeof(st)) == 0 &&
            strcmp(st, expect) == 0)
            return;
        tickf(h);
        msleep(5);
    }
}

static void wait_status_prefix(struct harness *h, const char *prefix,
                               int max_iters)
{
    char st[80];
    int i;
    for (i = 0; i < max_iters; i++) {
        if (h->reaped || !h->sv) return;
        if (node_text(h, F_STATUS, st, sizeof(st)) == 0 &&
            strncmp(st, prefix, strlen(prefix)) == 0)
            return;
        tickf(h);
        msleep(5);
    }
}

/* ---- interaction --------------------------------------------------------- */

static void click_at(struct harness *h, int32_t x, int32_t y)
{
    scene_compositor_input_pointer(h->cp, 0, x, y, 1);   /* down */
    pump_n(h, 3);
    scene_compositor_input_pointer(h->cp, 0, x, y, 0);   /* up   */
    pump_n(h, 3);
}

/* Click the center of a node, using its store rect (absolute coords —
 * the app creates every node with absolute screen coords).           */
static void click_node(struct harness *h, scene_node_id id)
{
    scene_node_vis v;
    if (node_rect(h, id, &v) != 0) return;
    click_at(h, v.rect[0] + v.rect[2] / 2, v.rect[1] + v.rect[3] / 2);
}

static void wait_reaped(struct harness *h, int max_iters)
{
    int i;
    for (i = 0; i < max_iters && !h->reaped; i++) {
        tickf(h);
        msleep(5);
    }
}

static void close_and_verify(struct harness *h)
{
    click_node(h, F_CLOSE);
    wait_reaped(h, 300);
    CHECK_EQ(h->reaped, 1);
    CHECK_EQ(child_exit_code(h), 0);
    pump_n(h, 10);
    CHECK(PX(h->cp, 200, 250) == 0xFF101010u);   /* desktop restored */
    CHECK(PX(h->cp, 60, 60) == 0xFF101010u);
}

/* Close the open-with handler's window (scene_app close = content-1 =
 * 40003 in the handler's own store) and verify session 2 reaps. */
static void close2_and_verify(struct harness *h)
{
    scene_node_vis v;
    int i;

    if (!h->sv2 || h->reaped2) return;
    CHECK_EQ(node_rect2(h, 40003, &v), 0);
    click_at(h, v.rect[0] + v.rect[2] / 2, v.rect[1] + v.rect[3] / 2);
    for (i = 0; i < 300 && !h->reaped2; i++) {
        tickf(h);
        msleep(5);
    }
    CHECK_EQ(h->reaped2, 1);
    CHECK(h->sv2 == NULL);
    pump_n(h, 10);
}

static void harness_free(struct harness *h)
{
    if (h->sv2 && !h->reaped2) {
        /* best-effort close of a live opener session (failed-test path) */
        scene_node_vis v;
        int i;
        if (node_rect2(h, 40003, &v) == 0) {
            click_at(h, v.rect[0] + v.rect[2] / 2, v.rect[1] + v.rect[3] / 2);
            for (i = 0; i < 300 && !h->reaped2; i++) {
                tickf(h);
                msleep(5);
            }
        }
        if (!h->reaped2)
            reap_one(h, &h->sv2, &h->peer2, &h->reaped2);
    }
    if (!h->reaped && h->sv) {
        kill_child(h);
        pump_n(h, 50);
    } else if (h->pid) {
        kill_child(h);
    }
    if (h->peer) scene_transport_close(h->peer);
    if (h->peer2) scene_transport_close(h->peer2);
    if (h->listener) scene_tcp_listen_destroy(h->listener);
    if (h->sv) scene_compositor_remove_session(h->cp, h->sv);
    if (h->sv2) scene_compositor_remove_session(h->cp, h->sv2);
    scene_client_free(h->sh_cl);
    scene_transport_close(h->server_ts);
    scene_loopback_free(h->lb);
    scene_compositor_free(h->cp);
}

/* ---- test 1: listing ------------------------------------------------------ */

static void test_files_list(void)
{
    struct harness h;
    uint16_t port = 0;
    scene_node_vis v;
    char t[80];

    harness_init(&h, &port);
    CHECK(port != 0);
    snprintf(h.log_path, sizeof(h.log_path), "files_list.log");
    spawn_child(&h, "build/files_fix", h.log_path);
    CHECK(h.pid != 0);

    wait_status(&h, "files_fix", 400);
    CHECK_EQ(h.joined, 1);

    /* the 12 row labels exist in layout order with the sorted listing */
    CHECK_EQ(node_rect(&h, F_ROW0 + 0, &v), 0);
    CHECK_EQ(v.rect[0], 104);
    CHECK_EQ(v.rect[1], 112);
    CHECK_EQ(v.rect[2], 412);
    CHECK_EQ(v.rect[3], 16);
    CHECK_EQ(node_rect(&h, F_ROW0 + 11, &v), 0);      /* last row exists */
    CHECK_EQ(node_rect(&h, F_ROW0 - 1, &v), -1);      /* base+9 unused  */
    CHECK_EQ(node_rect(&h, F_ROW0 - 2, &v), -1);      /* base+8 unused  */

CHECK_EQ(node_text(&h, F_ROW0 + 0, t, sizeof(t)), 0);
    CHECK(strcmp(t, "D aa_dir") == 0);
    CHECK_EQ(node_text(&h, F_ROW0 + 1, t, sizeof(t)), 0);
    CHECK(strcmp(t, "D cc_dir") == 0);
    CHECK_EQ(node_text(&h, F_ROW0 + 2, t, sizeof(t)), 0);
    CHECK(strcmp(t, "F bb_file.txt") == 0);
    CHECK_EQ(node_text(&h, F_ROW0 + 3, t, sizeof(t)), 0);
    CHECK(strcmp(t, "F ee_file.xyz") == 0);
    CHECK_EQ(node_text(&h, F_ROW0 + 4, t, sizeof(t)), 0);
    CHECK(strcmp(t, "F ff_pic.jpg") == 0);
    CHECK_EQ(node_text(&h, F_ROW0 + 5, t, sizeof(t)), 0);
    CHECK(t[0] == '\0');                             /* unused row */

    /* up button + status label rects and texts */
    CHECK_EQ(node_rect(&h, F_UP, &v), 0);
    CHECK_EQ(v.rect[0], 104);
    CHECK_EQ(v.rect[1], 86);
    CHECK_EQ(v.rect[2], 60);
    CHECK_EQ(v.rect[3], 22);
    CHECK_EQ(node_text(&h, F_UP, t, sizeof(t)), 0);
    CHECK(strcmp(t, "..") == 0);
    CHECK_EQ(node_rect(&h, F_STATUS, &v), 0);
    CHECK_EQ(v.rect[0], 228);
    CHECK_EQ(v.rect[1], 86);
    CHECK_EQ(v.rect[2], 288);
    CHECK_EQ(v.rect[3], 22);
    CHECK_EQ(node_rect(&h, F_DEL, &v), 0);
    CHECK_EQ(v.rect[0], 172);
    CHECK_EQ(v.rect[2], 48);

    /* title label carries the base dir name */
    CHECK_EQ(node_text(&h, F_TITLE, t, sizeof(t)), 0);
    CHECK(strstr(t, "files_fix") != NULL);

    /* pixels: window fill through the transparent GENERIC content,
     * titlebar band, desktop outside the window */
    CHECK(PX(h.cp, 200, 250) == 0xFF202020u);
    CHECK(PX(h.cp, 200, 55) == 0xFF1A1A1Au);
    CHECK(PX(h.cp, 60, 60) == 0xFF101010u);

    /* the child's log proves the flow: start, welcome, window built */
    {
        FILE *f = fopen(h.log_path, "r");
        CHECK(f != NULL);
        if (f) {
            char line[256];
            int got_start = 0, got_wel = 0, got_built = 0;
            while (fgets(line, sizeof(line), f)) {
                if (strstr(line, "iso-files: start")) got_start = 1;
                if (strstr(line, "iso-files: welcomed")) got_wel = 1;
                if (strstr(line, "iso-files: window built")) got_built = 1;
            }
            fclose(f);
            CHECK_EQ(got_start, 1);
            CHECK_EQ(got_wel, 1);
            CHECK_EQ(got_built, 1);
        }
    }

    close_and_verify(&h);
    /* remove(h.log_path); */
    harness_free(&h);
    printf("test_files_list: ok\n");
}

/* ---- test 2: navigation --------------------------------------------------- */

static void test_files_navigate(void)
{
    struct harness h;
    uint16_t port = 0;
    char t[80];

    harness_init(&h, &port);
    snprintf(h.log_path, sizeof(h.log_path), "files_nav.log");
    spawn_child(&h, "build/files_fix", h.log_path);
    CHECK(h.pid != 0);

    wait_status(&h, "files_fix", 400);
    CHECK_EQ(h.joined, 1);

    /* into the empty aa_dir (click = select, second click = open) */
    click_node(&h, F_ROW0 + 0);                       /* "D aa_dir" */
    wait_status(&h, "sel: aa_dir", 400);
    click_node(&h, F_ROW0 + 0);
    wait_status(&h, "aa_dir", 400);
    CHECK_EQ(node_text(&h, F_ROW0 + 0, t, sizeof(t)), 0);
    CHECK(t[0] == '\0');                              /* empty listing */
    CHECK_EQ(node_text(&h, F_ROW0 + 1, t, sizeof(t)), 0);
    CHECK(t[0] == '\0');
    CHECK_EQ(node_text(&h, F_ROW0 + 2, t, sizeof(t)), 0);
    CHECK(t[0] == '\0');
    CHECK_EQ(node_text(&h, F_TITLE, t, sizeof(t)), 0);
    CHECK(strcmp(t, "aa_dir") == 0);

    /* ".." returns to the fixture listing */
    click_node(&h, F_UP);
    wait_status(&h, "files_fix", 400);
    CHECK_EQ(node_text(&h, F_ROW0 + 0, t, sizeof(t)), 0);
    CHECK(strcmp(t, "D aa_dir") == 0);
    CHECK_EQ(node_text(&h, F_ROW0 + 2, t, sizeof(t)), 0);
    CHECK(strcmp(t, "F bb_file.txt") == 0);

    close_and_verify(&h);
    /* remove(h.log_path); */
    harness_free(&h);
    printf("test_files_navigate: ok\n");
}

/* ---- test 3: file click (unknown extension: no association) -------------- */

static void test_files_file_status(void)
{
    struct harness h;
    uint16_t port = 0;
    char t[80];

    harness_init(&h, &port);
    snprintf(h.log_path, sizeof(h.log_path), "files_file.log");
    spawn_child(&h, "build/files_fix", h.log_path);
    CHECK(h.pid != 0);

    wait_status(&h, "files_fix", 400);
    CHECK_EQ(h.joined, 1);

    click_node(&h, F_ROW0 + 3);                       /* "F ee_file.xyz" */
    wait_status_prefix(&h, "sel: ee_file.xyz", 400);
    click_node(&h, F_ROW0 + 3);
    wait_status_prefix(&h, "file: ee_file.xyz", 400);
    CHECK_EQ(node_text(&h, F_STATUS, t, sizeof(t)), 0);
    CHECK(strcmp(t, "file: ee_file.xyz") == 0);

    close_and_verify(&h);
    /* remove(h.log_path); */
    harness_free(&h);
    printf("test_files_file_status: ok\n");
}

/* ---- test 4: bad dir ------------------------------------------------------ */

static void test_files_bad_dir(void)
{
    struct harness h;
    uint16_t port = 0;
    char t[80];
    scene_node_vis v;

    harness_init(&h, &port);
    snprintf(h.log_path, sizeof(h.log_path), "files_bad.log");
    spawn_child(&h, "build/no_such_dir_never_xyz", h.log_path);
    CHECK(h.pid != 0);

    wait_status(&h, "bad dir", 400);
    CHECK_EQ(h.joined, 1);

    /* window still alive (titlebar + content nodes present), rows empty */
    CHECK_EQ(node_rect(&h, F_CONTENT, &v), 0);
    CHECK_EQ(node_rect(&h, F_ROW0 + 0, &v), 0);
    CHECK_EQ(node_text(&h, F_ROW0 + 0, t, sizeof(t)), 0);
    CHECK(t[0] == '\0');
    CHECK_EQ(node_text(&h, F_STATUS, t, sizeof(t)), 0);
    CHECK(strcmp(t, "bad dir") == 0);
    CHECK(PX(h.cp, 200, 250) == 0xFF202020u);         /* window up */

    close_and_verify(&h);
    /* remove(h.log_path); */
    harness_free(&h);
    printf("test_files_bad_dir: ok\n");
}

/* ---- test 5: open-with ----------------------------------------------------
 * Clicking a file row spawns the associated guest app (iso-edit for
 * .txt, iso-photo for .jpg): the handler is a REAL child process that
 * inherits SCENE_STORE_PORT and connects back into the harness's
 * listener — composited as a NEW session (layer 2) above the browser.
 * The photo handler's texture arrives over the real wire IMPORT_TEXTURE
 * path, importer = the harness (the OS-seam role, like iso_drm).     */

static void test_files_open_with(void)
{
    struct harness h;
    uint16_t port = 0;
    char t[80];
    int i;

    harness_init(&h, &port);
    snprintf(h.log_path, sizeof(h.log_path), "files_open.log");
    spawn_child(&h, "build/files_fix", h.log_path);
    CHECK(h.pid != 0);

    wait_status(&h, "files_fix", 400);
    CHECK_EQ(h.joined, 1);

    /* ---- click the .txt row: iso-edit joins as session 2 ------------ */
    click_node(&h, F_ROW0 + 2);                       /* "F bb_file.txt" */
    wait_status(&h, "sel: bb_file.txt", 400);
    click_node(&h, F_ROW0 + 2);
    wait_status(&h, "open: bb_file.txt", 400);
    for (i = 0; i < 400 && h.sv2 == NULL; i++) {
        tickf(&h);
        msleep(5);
    }
    CHECK_EQ(h.sv2 != NULL, 1);
    CHECK_EQ(h.joined2, 1);

    /* iso-edit's own window: title = base name, row 0 = the file's
     * 4-byte content "data" (the editor loads the file it was given;
     * view rows live at base+10..base+21, like the browser's) */
    for (i = 0; i < 400; i++) {
        if (node_text2(&h, EDIT_ROW0, t, sizeof(t)) == 0
            && strcmp(t, "data") == 0)
            break;
        tickf(&h);
        msleep(5);
    }
    CHECK_EQ(node_text2(&h, 40002, t, sizeof(t)), 0);
    CHECK(strcmp(t, "bb_file.txt") == 0);
    CHECK_EQ(node_text2(&h, EDIT_ROW0, t, sizeof(t)), 0);
    CHECK(strcmp(t, "data") == 0);
    {
        scene_node_vis v0;
        CHECK_EQ(node_rect2(&h, EDIT_ROW0, &v0), 0);
        CHECK(v0.rect[2] > 0 && v0.rect[3] > 0);
    }

    /* the browser itself survived the spawn and stays interactive */
    CHECK_EQ(node_text(&h, F_STATUS, t, sizeof(t)), 0);
    if (strcmp(t, "open: bb_file.txt") != 0)
        printf("DBG status is '%s' (pid=%u, reaped=%d, sv=%p)\n", t,
               (unsigned)h.pid, h.reaped, (void *)h.sv);
    CHECK(strcmp(t, "open: bb_file.txt") == 0);

    close2_and_verify(&h);                            /* iso-edit exits  */

    /* ---- click the .jpg row: iso-photo joins, imports via the wire - */
    click_node(&h, F_ROW0 + 4);                       /* "F ff_pic.jpg"  */
    wait_status(&h, "sel: ff_pic.jpg", 400);
    click_node(&h, F_ROW0 + 4);
    wait_status(&h, "open: ff_pic.jpg", 400);
    for (i = 0; i < 400 && h.sv2 == NULL; i++) {
        tickf(&h);
        msleep(5);
    }
    CHECK_EQ(h.sv2 != NULL, 1);

    /* the wire IMPORT_TEXTURE round-trip: harness importer decoded the
     * JPEG and registered ref 2 into the handler's own session/store */
    for (i = 0; i < 400 && h.sv2 != NULL; i++) {
        if (scene_store_texture_registered(scene_server_store(h.sv2),
                                           (scene_texture_ref)2))
            break;
        tickf(&h);
        msleep(5);
    }
    CHECK_EQ(h.sv2 != NULL, 1);
    if (h.sv2) {
        CHECK_EQ(scene_store_texture_registered(scene_server_store(h.sv2),
                                                (scene_texture_ref)2), 1);
        CHECK_EQ(node_text2(&h, 40002, t, sizeof(t)), 0);
        CHECK(strcmp(t, "ff_pic.jpg") == 0);
    }

    /* the photo is actually composited: 16x16 fixture, so the content's
     * top-left texel now shows the JPEG's left-half color (the window
     * fill was 0xFF202020 before the texture landed; the SET_TEXTURE
     * op arrives a pump or two after the importer answered) */
    {
        scene_node_vis c2;
        uint32_t want = 0xFFE7492Cu;   /* jpeg_fix.jpg texel (1,1) */
        CHECK_EQ(node_rect2(&h, 40004, &c2), 0);
        for (i = 0; i < 400; i++) {
            if (PX(h.cp, c2.rect[0] + 1, c2.rect[1] + 1) == want) break;
            tickf(&h);
            msleep(5);
        }
        CHECK(PX(h.cp, c2.rect[0] + 1, c2.rect[1] + 1) == want);
    }

    close2_and_verify(&h);                            /* iso-photo exits */
    close_and_verify(&h);                             /* browser exits   */
    /* remove(h.log_path); */
    harness_free(&h);
    printf("test_files_open_with: ok\n");
}

/* ---- test 6: delete -------------------------------------------------------
 * The Del toolbar button removes the SELECTED row (click a row once =
 * select, again = open). Deleting a file removes it from the fs and the
 * listing; an empty dir is removed; a non-empty dir fails ("failed: ..")
 * and stays. All verified on the real fs from the harness side.       */

static void test_files_delete(void)
{
    struct harness h;
    uint16_t port = 0;
    scene_node_vis v;
    char t[80];
    FILE *f;
    int i;

    /* fixture: build/files_del/{xx_dir/file.txt, yy_delme_dir/, zz_delme.txt} */
    make_dir("build");
    rm_tree("build/files_del");
    make_dir("build/files_del");
    make_dir("build/files_del/xx_dir");
    f = fopen("build/files_del/xx_dir/file.txt", "wb");
    if (f) { fwrite("x", 1, 1, f); fclose(f); }
    make_dir("build/files_del/yy_delme_dir");
    f = fopen("build/files_del/zz_delme.txt", "wb");
    if (f) { fwrite("z", 1, 1, f); fclose(f); }

    harness_init(&h, &port);
    snprintf(h.log_path, sizeof(h.log_path), "files_del.log");
    spawn_child(&h, "build/files_del", h.log_path);
    CHECK(h.pid != 0);

    wait_status(&h, "files_del", 400);
    CHECK_EQ(h.joined, 1);

    /* sorted: row0 = D xx_dir, row1 = D yy_delme_dir, row2 = F zz_delme.txt */
    CHECK_EQ(node_text(&h, F_ROW0 + 0, t, sizeof(t)), 0);
    CHECK(strcmp(t, "D xx_dir") == 0);
    CHECK_EQ(node_text(&h, F_ROW0 + 2, t, sizeof(t)), 0);
    CHECK(strcmp(t, "F zz_delme.txt") == 0);
    CHECK_EQ(node_rect(&h, F_DEL, &v), 0);
    CHECK_EQ(v.rect[0], 172);
    CHECK_EQ(v.rect[1], 86);

    /* select the file row, then delete it */
    click_node(&h, F_ROW0 + 2);
    wait_status(&h, "sel: zz_delme.txt", 400);
    click_node(&h, F_DEL);
    wait_status(&h, "removed: zz_delme.txt", 400);

    /* row gone from the listing, gone from the fs */
    CHECK_EQ(node_text(&h, F_ROW0 + 2, t, sizeof(t)), 0);
    CHECK(t[0] == '\0');
    CHECK_EQ(access("build/files_del/zz_delme.txt", F_OK), -1);

    /* deleting an empty dir works */
    click_node(&h, F_ROW0 + 1);                       /* D yy_delme_dir */
    wait_status(&h, "sel: yy_delme_dir", 400);
    click_node(&h, F_DEL);
    wait_status(&h, "removed: yy_delme_dir", 400);
    CHECK_EQ(access("build/files_del/yy_delme_dir", F_OK), -1);

    /* deleting a non-empty dir fails and leaves it */
    click_node(&h, F_ROW0 + 0);                       /* D xx_dir */
    wait_status(&h, "sel: xx_dir", 400);
    click_node(&h, F_DEL);
    wait_status(&h, "failed: xx_dir", 400);
    CHECK_EQ(access("build/files_del/xx_dir", F_OK), 0);
    CHECK_EQ(node_text(&h, F_ROW0 + 0, t, sizeof(t)), 0);
    CHECK(strcmp(t, "D xx_dir") == 0);

    /* Del with no selection is a no-op (status unchanged) */
    for (i = 0; i < 5; i++) { tickf(&h); msleep(5); }
    click_node(&h, F_DEL);
    wait_status(&h, "failed: xx_dir", 400);           /* unchanged */
    click_node(&h, F_DEL);                            /* no-op again */
    for (i = 0; i < 5; i++) { tickf(&h); msleep(5); }
    CHECK_EQ(access("build/files_del/xx_dir", F_OK), 0);

    close_and_verify(&h);
    /* remove(h.log_path); */
    harness_free(&h);
    rm_tree("build/files_del");
    printf("test_files_delete: ok\n");
}

int main(int argc, char **argv)
{
    (void)argc;
    g_argv0 = argv[0];
    setbuf(stdout, NULL);

    make_fixture();
    test_files_list();
    test_files_navigate();
    test_files_file_status();
    test_files_bad_dir();
    test_files_open_with();
    test_files_delete();
    rm_tree("build/files_fix");

    printf("test_files_app: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

