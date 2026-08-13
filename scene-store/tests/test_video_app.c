/*
 * test_video_app.c — the honest-boundary proof, end to end.
 *
 * A REAL child process (iso_video.exe, the video player demo app) is
 * spawned with SCENE_STORE_PORT set, connects back over real TCP into a
 * launcher-style app session (harness-owned scene_server, composited as
 * layer 1 above a shell-less layer-0 canvas), builds a window, and then
 * PUSHES a frame per iteration over the wire: scene_client_set_texture
 * (the locked v0 wire carries only the texture REFERENCE; pixels live in
 * the OS-side importer). This harness plays the OS importer: it re-runs
 * the app's decoder and re-registers each decoded frame into the
 * compositor's texture registry — the compositor refreshes pixels on
 * re-register and dirties the referencing node. Assertions:
 *
 *   1. join + paint: the child's window paints over the desktop; the
 *      content shows importer frame 0 byte-exact (XRGB @ opacity 255
 *      replaces: texel == pixel, transparent GENERIC fill under it).
 *   2. live refresh: the importer's next frame re-registers -> a content
 *      pixel CHANGED from the first frame's value, exact per-channel.
 *   3. release (boundary removal) reverts the content to the window
 *      fill; re-register resumes the stream.
 *   4. protocol behavior: clicks deliver pointer+activate to the child
 *      (acked, gate reopens; two clicks = two deliveries), the child's
 *      own decoded frames provably differ, kill -> reap -> desktop.
 *
 * Every expected pixel is derived from the same generator the harness
 * imported — no guessed colors. Effects off: identity paint (the
 * launcher-harness default), so expectations hold on the frame the
 * damage commits.
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

/* ---- the video stream (must match src/iso_video.c's decoder) ---------- */

#define VID_X 100
#define VID_Y 50
#define VID_W 240
#define VID_H 160
#define VID_CW 240
#define VID_CH 128
#define VID_TEX_REF 1u

/* Decoded frame n: A=0xFF, R=(n*29 + y)&0xFF (vertical gradient), left
 * half G=0 B=0x40, right half G=0xFF B=0x80. XRGB, opaque.             */
static void gen_frame(uint32_t n, uint32_t *px)
{
    uint32_t y, x;

    for (y = 0; y < VID_CH; y++) {
        uint32_t R = (n * 29u + y) & 0xFFu;
        for (x = 0; x < VID_CW; x++) {
            uint32_t c = UINT32_C(0xFF000000) | (R << 16);
            if (x < VID_CW / 2u) c |= UINT32_C(0x00000040);
            else                 c |= UINT32_C(0x00FF0080);
            px[y * VID_CW + x] = c;
        }
    }
}

/* Content-local probe at row 18 (screen y=100, content starts y=82):
 * R = (n*29 + 18) & 0xFF. */
static uint32_t frame_probe_left(uint32_t n)
{
    return UINT32_C(0xFF000000) | (((n * 29u + 18u) & 0xFFu) << 16)
           | UINT32_C(0x00000040);
}

static uint32_t frame_probe_right(uint32_t n)
{
    return UINT32_C(0xFF000000) | (((n * 29u + 18u) & 0xFFu) << 16)
           | UINT32_C(0x00FF0080);
}

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

    uint32_t dec[VID_CW * VID_CH];  /* importer pixel buffer            */
    uint32_t imp_frame;             /* importer stream index            */
};

/* OS-side importer: decode frame n and re-import it into the compositor
 * texture registry. Re-register refreshes pixels and dirties the node;
 * the engine store was seeded with the same ref in harness_init.       */
static void importer_step(struct harness *h, uint32_t n)
{
    gen_frame(n, h->dec);
    CHECK_EQ(scene_compositor_register_texture(h->cp, VID_TEX_REF,
             VID_CW, VID_CH, SCENE_TEX_FMT_XRGB, 1, h->dec), 0);
    h->imp_frame = n;
}

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

static void spawn_child(struct harness *h, const char *logarg)
{
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)h->port);
#if defined(_WIN32)
    SetEnvironmentVariableA("SCENE_STORE_PORT", portstr);
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\"", sibling_exe_path("iso_video.exe"),
             logarg);
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
        execl(sibling_exe_path("iso_video"), "iso_video", logarg,
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
     * The OS-side importer seeds the texture into the APP session's
     * store first — the engine validates the child's wire SET_TEXTURE
     * against this table — and registers the pixels compositor-side.   */
    h->sv = scene_server_new(NULL);
    CHECK(h->sv != NULL);
    CHECK_EQ(scene_compositor_add_session(h->cp, h->sv), 1);
    scene_server_attach(h->sv);
    CHECK_EQ(scene_store_register_texture(scene_server_store(h->sv),
             VID_TEX_REF, VID_CW, VID_CH, SCENE_TEX_FMT_XRGB, 1), 0);
    importer_step(h, 0);                       /* initial decode+import */

    /* Launcher-style spawn: bind an ephemeral non-blocking listener,
     * start the child with SCENE_STORE_PORT. */
    h->listener = scene_tcp_listen_new(0, out_port);
    CHECK(h->listener != NULL);
    h->port = *out_port;
    if (!h->listener) return;
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
    scene_compositor_release_texture(h->cp, VID_TEX_REF);
    scene_client_free(h->sh_cl);
    scene_transport_close(h->server_ts);
    scene_loopback_free(h->lb);
    scene_compositor_free(h->cp);
}

/* ---- the test ---------------------------------------------------------- */

static void test_video_stream(void)
{
    struct harness h;
    uint16_t port = 0;

    harness_init(&h, &port);
    CHECK(port != 0);

    snprintf(h.log_path, sizeof(h.log_path), "video_app.log");
    spawn_child(&h, h.log_path);
    CHECK(h.pid != 0);
    int i;

    /* 1. The child joins and its window paints: titlebar band exact
     *    (title conveys no glyphs at x=260), desktop intact.          */
    for (i = 0; i < 400 && PX(h.cp, 260, 60) != 0xFF1A1A1Au; i++) {
        tickf(&h);
        msleep(5);
    }
    CHECK_EQ(h.joined, 1);
    CHECK(PX(h.cp, 260, 60) == 0xFF1A1A1Au);       /* titlebar band  */
    CHECK(PX(h.cp, 60, 60) == 0xFF101010u);        /* desktop        */
    CHECK(PX(h.cp, 700, 500) == 0xFF101010u);      /* desktop        */

    /* 2. First frame: the content shows importer frame 0 byte-exact
     *    (XRGB @ opacity 255 replaces texel over the transparent
     *    GENERIC fill).                                                */
    for (i = 0; i < 400 && PX(h.cp, 100, 100) != frame_probe_left(0); i++) {
        tickf(&h);
        msleep(5);
    }
    CHECK(PX(h.cp, 100, 100) == frame_probe_left(0));   /* 0xFF120040 */
    CHECK(PX(h.cp, 250, 100) == frame_probe_right(0));  /* 0xFF12FF80 */

    /* 3. Live refresh: the importer decodes frame 1 and re-registers —
     *    the compositor refreshes pixels and dirties the node; a probe
     *    pixel CHANGED from the first frame's value.                   */
    importer_step(&h, 1);
    pump_n(&h, 10);
    CHECK(PX(h.cp, 100, 100) == frame_probe_left(1));   /* 0xFF2F0040 */
    CHECK(PX(h.cp, 250, 100) == frame_probe_right(1));  /* 0xFF2FFF80 */
    CHECK(PX(h.cp, 100, 100) != frame_probe_left(0));

    /* 4. The stream keeps advancing through the same wire references.  */
    importer_step(&h, 2);
    pump_n(&h, 10);
    CHECK(PX(h.cp, 100, 100) == frame_probe_left(2));   /* 0xFF4C0040 */
    CHECK(PX(h.cp, 250, 100) == frame_probe_right(2));  /* 0xFF4CFF80 */

    /* 5. Boundary removal: releasing the texture unpaints the content
     *    back to the WINDOW fill under the transparent GENERIC node.   */
    CHECK_EQ(scene_compositor_release_texture(h.cp, VID_TEX_REF), 0);
    pump_n(&h, 10);
    CHECK(PX(h.cp, 100, 100) == 0xFF202020u);
    CHECK(PX(h.cp, 250, 100) == 0xFF202020u);

    /* 6. Re-import frame 3: the stream resumes with the same ref.      */
    importer_step(&h, 3);
    pump_n(&h, 10);
    CHECK(PX(h.cp, 100, 100) == frame_probe_left(3));   /* 0xFF690040 */
    CHECK(PX(h.cp, 250, 100) == frame_probe_right(3));  /* 0xFF69FF80 */

    /* 7. Protocol behavior: two clicks deliver pointer + activate to
     *    the child; the first ack reopens the flow gate for the second
     *    (focus lands on the app layer).                              */
    scene_compositor_input_pointer(h.cp, 0, 150, 100, 1);
    pump_n(&h, 30);
    CHECK_EQ(scene_compositor_focus_is_shell(h.cp), 0);
    scene_compositor_input_pointer(h.cp, 0, 150, 100, 1);
    pump_n(&h, 30);

    /* The child's log proves: welcome, both clicks, and that its own
     * decoded frames provably differ (app-side decoder runs).         */
    FILE *f = fopen(h.log_path, "r");
    CHECK(f != NULL);
    if (f) {
        char line[256];
        int got_wel = 0, got_a = 0, got_p = 0;
        uint32_t px[16];
        int npx = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "iso-video: welcomed")) got_wel = 1;
            if (strncmp(line, "iso-video: pointer 150,100",
                        sizeof("iso-video: pointer 150,100") - 1) == 0)
                got_p++;
            if (strncmp(line, "iso-video: activate id=40004",
                        sizeof("iso-video: activate id=40004") - 1) == 0)
                got_a = 1;
            if (npx < 16 &&
                sscanf(line, "iso-video: frame %*u px=%6x", &px[npx]) == 1)
                npx++;
        }
        fclose(f);
        CHECK_EQ(got_wel, 1);
        CHECK_EQ(got_p, 2);
        CHECK_EQ(got_a, 1);
        CHECK(npx >= 2);
        if (npx >= 2) CHECK(px[0] != px[1]);
    }

    /* 8. Kill: the socket closes, the session is reaped, the desktop
     *    repaints over the dead window.                                */
    kill_child(&h);
    pump_n(&h, 50);
    CHECK_EQ(h.reaped, 1);
    CHECK(h.sv == NULL);                       /* session is gone */
    pump_n(&h, 10);
    CHECK(PX(h.cp, 100, 100) == 0xFF101010u);   /* desktop restored */
    CHECK(PX(h.cp, 260, 60) == 0xFF101010u);

    remove(h.log_path);
    harness_free(&h);
    printf("test_video_stream: ok\n");
}

/* The stream start is reproducible: a fresh harness + fresh child shows
 * the same first-frame pixels on every boot.                            */
static void test_video_fresh_join(void)
{
    struct harness h;
    uint16_t port = 0;
    int i;

    harness_init(&h, &port);
    snprintf(h.log_path, sizeof(h.log_path), "video_app2.log");
    spawn_child(&h, h.log_path);
    CHECK(h.pid != 0);

    for (i = 0; i < 400 && PX(h.cp, 100, 100) != frame_probe_left(0); i++) {
        tickf(&h);
        msleep(5);
    }
    CHECK_EQ(h.joined, 1);
    CHECK(PX(h.cp, 100, 100) == frame_probe_left(0));
    CHECK(PX(h.cp, 250, 100) == frame_probe_right(0));
    CHECK(PX(h.cp, 260, 60) == 0xFF1A1A1Au);

    kill_child(&h);
    pump_n(&h, 50);
    CHECK_EQ(h.reaped, 1);

    remove(h.log_path);
    harness_free(&h);
    printf("test_video_fresh_join: ok\n");
}

int main(int argc, char **argv)
{
    (void)argc;
    g_argv0 = argv[0];

    test_video_stream();
    test_video_fresh_join();

    printf("test_video_app: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}