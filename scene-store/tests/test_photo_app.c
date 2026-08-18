/*
 * test_photo_app.c — the photo consumer's end-to-end proof.
 *
 * Same architecture as test_video_app.c: a REAL child process
 * (iso_photo.exe) is spawned with SCENE_STORE_PORT set, connects back
 * over real TCP into a harness-owned app session (scene_server,
 * composited as layer 1 above a shell-less layer-0 canvas), builds a
 * window with a status label, and requests its image over the WIRE:
 * the child sends 0x0017 (ref + path, no pixels — the locked v0 wire
 * never carries pixels), the harness plays the OS importer through
 * scene_server_set_import_cb, decodes the JPEG fixture (tests/fixtures/
 * jpeg_fix.jpg — 16x16, left RGB(228,74,43), right RGB(43,106,228))
 * with scene_image_load, registers the ref into the session store and
 * the pixels into the compositor, and answers 0x800D ok=1. The child
 * then pushes only the texture REFERENCE; the engine validates it
 * against the importer-registered table. A failed decode answers ok=0
 * and the child reports "bad image" with no texture.
 *
 * The old ISO_PHOTO_PATH env pre-seed is dead: this test proves the
 * honest wire path end-to-end (open-with territory — any path, any
 * session, at runtime).
 *
 * Tests:
 *   1. test_photo_paint: content pixels are the imported JPEG exactly.
 *      Expectations computed FROM the harness's own decoded buffer (the
 *      same bytes it registered) plus a tolerance sanity-check against
 *      the fixture's source colors (JPEG is lossy; ±6/channel). Status
 *      reads "ok" through the session store (node 40012), the label
 *      geometry is exact, close → exit 0 → reap → desktop restored.
 *   2. test_photo_bad_image: nonexistent path — the importer cb fails,
 *      the server answers ok=0, the app joins, reports "bad image" via
 *      the import result, no texture is applied (content shows the
 *      window fill), and close exits 0.
 *   3. test_photo_close_reap: close button click → child exits 0 → the
 *      session is reaped → the desktop repaints over the dead window.
 *
 * Effects off: identity paint, so expectations hold on the frame the
 * damage commits. All status assertions read the app session's store
 * directly (node 40012 texts) — no timing guesses.
 */
#include "scene_compositor.h"
#include "scene_client.h"
#include "scene_transport.h"
#include "scene_server.h"
#include "scene_store.h"
#include "scene_image.h"

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

/* ---- shared constants (src/iso_photo.c) --------------------------------- */

#define PHOTO_REF    2u        /* ref the app requests via 0x0017        */
#define PHOTO_STATUS 40012u    /* app base+12 (clear of chrome) */
#define PHOTO_X 100
#define PHOTO_Y 50
#define PHOTO_W 300
#define PHOTO_H 220            /* window; content = (100,82,300,188)      */

/* The fixture (tests/fixtures/jpeg_fix.jpg): 16x16, top half
 * RGB(228,74,43), bottom half RGB(43,106,228). Ground truth from the
 * GDI+ writer (pass: jpeg_fix.jpg generated with System.Drawing). */
#define FIX_PATH "tests/fixtures/jpeg_fix.jpg"
#define FIX_W    16
#define FIX_H    16
#define FIX_L_R  228
#define FIX_L_G  74
#define FIX_L_B  43
#define FIX_R_R  43
#define FIX_R_G  106
#define FIX_R_B  228
#define FIX_TOL  6            /* JPEG is lossy: tolerance per channel     */

/* pixel → ARGB channel extraction */
#define PX_R(p) (((p) >> 16) & 0xFFu)
#define PX_G(p) (((p) >> 8) & 0xFFu)
#define PX_B(p) (((p) >> 0) & 0xFFu)
#define CH_DELTA(v, e) ((unsigned)((v) > (e) ? (v) - (e) : (e) - (v)))

/* ---- harness ----------------------------------------------------------- */

struct harness {
    scene_compositor *cp;       /* compositor (owns the app session)   */
    scene_server     *sv;       /* app session server (NULL once reaped)*/
    scene_loopback   *lb;       /* layer-0 shell link                  */
    scene_transport  *server_ts;
    scene_client     *sh_cl;    /* layer-0 shell client                */

    scene_tcp_listener *listener;  /* child accept socket (launcher-style) */
    scene_transport *peer;       /* child connection (NULL until joined)   */
    uint16_t          port;      /* the child's SCENE_STORE_PORT           */
    uint32_t          pid;
    uintptr_t         hproc;
    int               joined;
    int               reaped;
    char              log_path[160];

    /* OS importer state (the wire import cb writes into this) */
    char              imp_path[160];   /* path the child must have sent  */
    int               imp_calls;
    scene_texture_ref imp_ref;
    int               imp_w, imp_h;    /* imported image dims            */
    uint32_t          dec[FIX_W * FIX_H]; /* importer pixel buffer (cap) */
    int               dec_cap_ok;      /* decoded within the buffer cap  */
    int               cb_err;          /* complaint flag from the cb     */
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

/* argv[1] = image path, argv[2] = event log (iso_photo.c convention). */
static void spawn_child(struct harness *h, const char *imgarg,
                        const char *logarg)
{
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)h->port);
    if (imgarg) snprintf(h->imp_path, sizeof(h->imp_path), "%s", imgarg);
#if defined(_WIN32)
    SetEnvironmentVariableA("SCENE_STORE_PORT", portstr);
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\" \"%s\"",
             sibling_exe_path("iso_photo.exe"), imgarg, logarg);
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
        execl(sibling_exe_path("iso_photo"), "iso_photo", imgarg, logarg,
              (char *)NULL);
        _exit(127);
    }
    if (p < 0) return;
    h->pid = (uint32_t)p;
    h->hproc = (uintptr_t)p;
#endif
}

/* ---- the OS importer (wire seam) ---------------------------------------- */

/* 0x0017 handler: decode the path (scene_image: PNG/JPEG/GIF via stb),
 * register ref+pixels into the SESSION store + compositor layer, and
 * answer ok=1; any failure answers -1 -> the server emits ok=0 and the
 * session stays alive. Mirrors src/iso_drm.c iso_import_cb. */
static int cb_import(void *ud, scene_server *sv, scene_texture_ref ref,
                     const char *path)
{
    struct harness *h = (struct harness *)ud;
    int w = 0, hh = 0;
    uint32_t *px = NULL;
    int rc = -1;

    h->imp_calls++;
    h->imp_ref = ref;
    if (path && strcmp(path, h->imp_path) == 0 &&
        scene_image_load(path, &w, &hh, &px) == 0 && px != NULL &&
        w > 0 && hh > 0 &&
        w * hh <= (int)(sizeof(h->dec) / sizeof(h->dec[0]))) {
        memcpy(h->dec, px, (size_t)w * hh * sizeof(uint32_t));
        h->dec_cap_ok = 1;
        h->imp_w = w;
        h->imp_h = hh;
        if (scene_compositor_register_texture_layer(h->cp, 1, ref,
                                                    (uint32_t)w,
                                                    (uint32_t)hh,
                                                    SCENE_TEX_FMT_ARGB, 1,
                                                    h->dec) == 0)
            rc = scene_server_import_result(sv, ref, 1);
    }
    if (rc != 0) h->cb_err++;
    scene_image_free(px);
    return rc;
}

static void tickf(struct harness *h);

static void harness_init(struct harness *h, uint16_t *out_port)
{
    memset(h, 0, sizeof(*h));
    h->cp = scene_compositor_new(NULL, 800, 600);
    CHECK(h->cp != NULL);
    scene_compositor_set_clear(h->cp, 0xFF101010);
    scene_compositor_set_effects(h->cp, 0);   /* identity paint */

    /* Layer 0 (shell-less): bare desktop canvas, exactly like the
     * launcher harness (test_launcher.c harness_init). */
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

    /* App session (layer 1): harness-owned server, like test_sessions.c.
     * The OS importer is installed as a WIRE callback; nothing is
     * pre-seeded — the child's 0x0017 drives the whole import. */
    h->sv = scene_server_new(NULL);
    CHECK(h->sv != NULL);
    CHECK_EQ(scene_compositor_add_session(h->cp, h->sv), 1);
    scene_server_attach(h->sv);
    scene_server_set_import_cb(h->sv, cb_import, h);

    /* Launcher-style spawn: bind an ephemeral non-blocking listener. */
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

/* One loop iteration: accept/feed/drain the child link, pump the layer-0
 * shell link, composite one frame. */
static void tickf(struct harness *h)
{
    /* child link */
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

    /* layer-0 shell link */
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
        pump_n(h, 50);                   /* reap on the closed socket */
    } else if (h->pid) {
        kill_child(h);
    }
    if (h->peer) scene_transport_close(h->peer);
    if (h->listener) scene_tcp_listen_destroy(h->listener);
    if (h->sv) scene_compositor_remove_session(h->cp, h->sv);
    scene_compositor_release_texture_layer(h->cp, 1, PHOTO_REF);
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
    return (int)code;
#else
    int status = 0;
    if (waitpid((pid_t)h->pid, &status, 0) < 0) return -1;
    h->pid = 0;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

/* Status label text of the app session (node 40012, one text slot). */
static int status_label(const struct harness *h, char *out, size_t cap)
{
    scene_node_text_vis tv[4];
    int n;
    if (!h->sv) return -1;
    n = scene_store_node_texts(scene_server_store(h->sv), PHOTO_STATUS,
                               tv, 4);
    if (n <= 0 || tv[0].len == 0 || tv[0].data == NULL) return -1;
    size_t l = tv[0].len < cap - 1 ? (size_t)tv[0].len : cap - 1;
    memcpy(out, tv[0].data, l);
    out[l] = '\0';
    return 0;
}

/* ---- shared UI flow ------------------------------------------------------ */

/* Wait for the child's window: window fill at (150,150) (content spans
 * y 82..270; the 16x16 image only covers the top-left of the content,
 * so (150,150) shows the 0xFF202020 window fill). */
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

/* Click the close button (x+w-28, y+4, 24, 24) center = (384,66) and
 * verify: child exit 0, session reaped, desktop repainted. */
static void close_and_verify(struct harness *h)
{
    scene_compositor_input_pointer(h->cp, 0, 384, 66, 1);
    wait_reaped(h, 300);
    CHECK_EQ(h->reaped, 1);
    CHECK_EQ(child_exit_code(h), 0);
    pump_n(h, 10);
    CHECK(PX(h->cp, 150, 150) == 0xFF101010u);   /* desktop restored */
    CHECK(PX(h->cp, 260, 60) == 0xFF101010u);
}

/* ---- test 1: paint --------------------------------------------------------- */

static void test_photo_paint(void)
{
    struct harness h;
    uint16_t port = 0;
    scene_node_vis vis;
    uint32_t exp_left, exp_right;
    int i;

    harness_init(&h, &port);
    CHECK(port != 0);
    snprintf(h.log_path, sizeof(h.log_path), "photo_app.log");
    spawn_child(&h, FIX_PATH, h.log_path);
    CHECK(h.pid != 0);

    wait_window(&h, 400);
    CHECK_EQ(h.joined, 1);
    CHECK(PX(h.cp, 150, 150) == 0xFF202020u);    /* window fill */
    /* titlebar band: "jpeg_fix.jpg" glyphs end at x=208, band clean */
    CHECK(PX(h.cp, 260, 60) == 0xFF1A1A1Au);
    /* desktop intact outside the window */
    CHECK(PX(h.cp, 60, 60) == 0xFF101010u);
    CHECK(PX(h.cp, 700, 500) == 0xFF101010u);

    /* status label exists with a sane rect, reports "ok" */
    CHECK_EQ(scene_store_node_vis(scene_server_store(h.sv), PHOTO_STATUS,
                                  &vis), 0);
    CHECK_EQ(vis.rect[0], PHOTO_X + 4);          /* (104,88,284,16) */
    CHECK_EQ(vis.rect[1], PHOTO_Y + 38);
    CHECK_EQ(vis.rect[2], PHOTO_W - 16);
    CHECK_EQ(vis.rect[3], 16);
    wait_status(&h, "ok", 400);
    {
        char st[64];
        CHECK_EQ(status_label(&h, st, sizeof(st)), 0);
        CHECK(strcmp(st, "ok") == 0);
    }

    /* The import went over the wire: one cb call, correct ref+path.
     * The status can only be "ok" if the import succeeded first. */
    CHECK_EQ(h.imp_calls, 1);
    CHECK_EQ((unsigned)h.imp_ref, (unsigned)PHOTO_REF);
    CHECK_EQ(h.imp_w, FIX_W);
    CHECK_EQ(h.imp_h, FIX_H);
    CHECK_EQ(h.cb_err, 0);

    /* Decode sanity vs the writer's colors (JPEG lossy: ±TOL/channel). */
    {
        uint32_t L = h.dec[8 * FIX_W + 4];   /* left  half probe */
        uint32_t R = h.dec[8 * FIX_W + 12];  /* right half probe */
        CHECK(CH_DELTA(PX_R(L), FIX_L_R) <= FIX_TOL);
        CHECK(CH_DELTA(PX_G(L), FIX_L_G) <= FIX_TOL);
        CHECK(CH_DELTA(PX_B(L), FIX_L_B) <= FIX_TOL);
        CHECK(CH_DELTA(PX_R(R), FIX_R_R) <= FIX_TOL);
        CHECK(CH_DELTA(PX_G(R), FIX_R_G) <= FIX_TOL);
        CHECK(CH_DELTA(PX_B(R), FIX_R_B) <= FIX_TOL);
    }

    /* Content pixels are the imported image exactly: expectations FROM
     * the harness's own decoded buffer (the same bytes it registered),
     * at content-relative (4,15) and (12,15) — content at (100,82),
     * image 16x16 at its top-left. y=15 stays inside the image (rows
     * 82..97) and below the status label's "ok" glyph rows (88..96). */
    exp_left = h.dec[15 * FIX_W + 4];
    exp_right = h.dec[15 * FIX_W + 12];
    for (i = 0; i < 400 && PX(h.cp, 104, 97) != exp_left; i++) {
        tickf(&h);
        msleep(5);
    }
    CHECK(PX(h.cp, 104, 97) == exp_left);
    CHECK(PX(h.cp, 112, 97) == exp_right);
    CHECK(PX(h.cp, 104, 97) != PX(h.cp, 112, 97));

    close_and_verify(&h);
    remove(h.log_path);
    harness_free(&h);
    printf("test_photo_paint: ok\n");
}

/* ---- test 2: bad image ------------------------------------------------------ */

static void test_photo_bad_image(void)
{
    struct harness h;
    uint16_t port = 0;

    harness_init(&h, &port);
    CHECK(port != 0);
    snprintf(h.log_path, sizeof(h.log_path), "photo_bad.log");
    spawn_child(&h, "no_such_photo_never_exists.jpg", h.log_path);
    CHECK(h.pid != 0);

    wait_window(&h, 400);
    CHECK_EQ(h.joined, 1);
    CHECK(PX(h.cp, 150, 150) == 0xFF202020u);    /* window fill */

    /* The importer could not decode: cb ran, returned -1 -> the server
     * answered ok=0 -> the app reported "bad image" — and never pushed
     * a SET_TEXTURE, so the content still shows the window fill. */
    wait_status(&h, "bad image", 400);
    {
        char st[64];
        CHECK_EQ(status_label(&h, st, sizeof(st)), 0);
        CHECK(strcmp(st, "bad image") == 0);
    }
    CHECK_EQ(h.imp_calls, 1);                  /* the importer was asked */
    CHECK_EQ(h.imp_w, 0);                      /* ... and it failed       */
    CHECK(PX(h.cp, 104, 90) == 0xFF202020u);   /* no texture, window fill */
    CHECK(PX(h.cp, 136, 98) == 0xFF202020u);

    close_and_verify(&h);
    remove(h.log_path);
    harness_free(&h);
    printf("test_photo_bad_image: ok\n");
}

/* ---- test 3: close -> reap --------------------------------------------------- */

static void test_photo_close_reap(void)
{
    struct harness h;
    uint16_t port = 0;

    harness_init(&h, &port);
    CHECK(port != 0);
    snprintf(h.log_path, sizeof(h.log_path), "photo_reap.log");
    spawn_child(&h, FIX_PATH, h.log_path);
    CHECK(h.pid != 0);

    wait_window(&h, 400);
    CHECK_EQ(h.joined, 1);
    CHECK(PX(h.cp, 150, 150) == 0xFF202020u);

    /* close button click -> child exits 0 -> session reaped -> the
     * desktop repaints over the dead window */
    scene_compositor_input_pointer(h.cp, 0, 384, 66, 1);
    wait_reaped(&h, 300);
    CHECK_EQ(h.reaped, 1);
    CHECK_EQ(child_exit_code(&h), 0);
    pump_n(&h, 10);
    CHECK(PX(h.cp, 150, 150) == 0xFF101010u);   /* desktop restored */
    CHECK(PX(h.cp, 260, 60) == 0xFF101010u);

    remove(h.log_path);
    harness_free(&h);
    printf("test_photo_close_reap: ok\n");
}

int main(int argc, char **argv)
{
    (void)argc;
    g_argv0 = argv[0];

    test_photo_paint();
    test_photo_bad_image();
    test_photo_close_reap();

    printf("test_photo_app: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}