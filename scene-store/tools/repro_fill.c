/* repro_fill.c — isolate the panel fill byte order in the compositor fb.
 * Build: cc -std=c11 -O0 -g -Iinclude src/scene_fmt.c src/scene_store.c
 *        src/scene_transport.c src/scene_client.c src/scene_server.c
 *        src/scene_fb.c src/scene_font.c src/scene_font_data.c
 *        src/scene_compositor.c src/scene_shell.c repro_fill.c ... */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "scene_transport.h"
#include "scene_client.h"
#include "scene_server.h"
#include "scene_compositor.h"
#include "scene_shell.h"

#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { failures++; printf("FAIL %s:%d: %s\n", \
        __FILE__, __LINE__, #expr); } \
} while (0)
#define CHECK_EQ(a, b) do { \
    checks++; \
    if ((a) != (b)) { failures++; printf("FAIL %s:%d: %s (%lu) != %s (%lu)\n", \
        __FILE__, __LINE__, #a, (unsigned long)(a), #b, (unsigned long)(b)); } \
} while (0)

static int checks, failures;

struct harness {
    scene_loopback *lb;
    scene_transport *server_ts;
    scene_client *cl;
    scene_compositor *cp;
};

static void tickf(struct harness *h)
{
    int round;
    for (round = 0; round < 4; round++) {
        scene_client_flush(h->cl);
        uint8_t buf[8192];
        uint32_t got;
        while (scene_transport_recv(h->server_ts, buf, sizeof(buf), &got) == 0
               && got)
            scene_server_feed(scene_compositor_server(h->cp), buf, got);
        const uint8_t *f;
        uint32_t flen;
        while (scene_server_out_next_frame(scene_compositor_server(h->cp),
                                           &f, &flen) == 1)
            scene_transport_send(h->server_ts, f, flen);
        scene_client_pump(h->cl);
    }
    scene_compositor_frame(h->cp);
}

static void harness_init(struct harness *h)
{
    memset(h, 0, sizeof(*h));
    h->lb = scene_loopback_new();
    h->server_ts = scene_loopback_server_end(h->lb);
    h->cl = scene_client_new();
    h->cp = scene_compositor_new(NULL, 800, 600);
    scene_server_attach(scene_compositor_server(h->cp));
    scene_client_connect(h->cl, scene_loopback_client_end(h->lb),
                         "shell", NULL, NULL);
    tickf(h);
}

int main(void)
{
    struct harness h;
    harness_init(&h);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    cfg.bg_color    = 0xFF1A1A2Eu;
    cfg.panel_color = 0xFF16213Eu;
    cfg.panel_border = 0xFF0F3460u;
    cfg.panel_border_w = 1;
    cfg.panel_radius = 4;
    cfg.button_color = 0xFF2A2A4Eu;

    scene_shell *sh = scene_shell_new(h.cl,
        scene_compositor_store(h.cp), h.cp, &cfg);
    CHECK(sh != NULL);
    CHECK_EQ(scene_shell_build(sh, 800, 600), 0);
    tickf(&h);

    const scene_fb *fb = scene_compositor_fb(h.cp);
    CHECK(fb != NULL && fb->px != NULL);
    if (fb && fb->px) {
        /* panel: (0, 568, 800, 32) with shell defaults (h=32, panel at
         * y = 600-32). Probe interior pixel + the border row. */
        uint32_t body = fb->px[580u * fb->pitch + 400u];
        uint32_t top  = fb->px[568u * fb->pitch + 400u];
        fprintf(stderr, "panel body pixel  = 0x%08X\n", body);
        fprintf(stderr, "panel top row     = 0x%08X\n", top);
        CHECK_EQ(body, 0xFF16213Eu);
        CHECK_EQ(top, 0xFF0F3460u);
        /* background interior */
        uint32_t bg = fb->px[300u * fb->pitch + 400u];
        fprintf(stderr, "desktop pixel     = 0x%08X\n", bg);
        CHECK_EQ(bg, 0xFF1A1A2Eu);
    }
    scene_shell_free(sh);
    scene_client_free(h.cl);
    scene_compositor_free(h.cp);
    scene_loopback_free(h.lb);

    printf("repro_fill: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}