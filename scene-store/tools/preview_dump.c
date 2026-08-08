/*
 * tools/preview_dump.c — Console tool: run full stack (compositor + shell + wallpaper),
 * dump framebuffer to PPM. No GUI, no GDI, just the compositor paint path.
 */
#include "scene_shell.h"
#include "scene_compositor.h"
#include "scene_transport.h"
#include "scene_client.h"
#include "scene_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void pump_loop(scene_client *cl, scene_transport *server_ts,
                      scene_compositor *cp)
{
    scene_client_flush(cl);
    uint8_t buf[8192];
    uint32_t got;
    while (scene_transport_recv(server_ts, buf, sizeof(buf), &got) == 0 && got)
        scene_server_feed(scene_compositor_server(cp), buf, got);
    const uint8_t *f;
    uint32_t flen;
    while (scene_server_out_next_frame(scene_compositor_server(cp), &f, &flen) == 1)
        scene_transport_send(server_ts, f, flen);
    scene_client_pump(cl);
}

static void dump_ppm(const scene_fb *fb, const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) { fprintf(stderr, "cannot create %s\n", path); return; }
    fprintf(fp, "P6\n%u %u\n255\n", fb->w, fb->h);
    uint32_t y, x;
    for (y = 0; y < fb->h; y++) {
        const uint32_t *row = fb->px + y * fb->pitch;
        for (x = 0; x < fb->w; x++) {
            uint32_t p = row[x];
            uint32_t a = (p >> 24) & 0xFF;
            uint32_t r = (p >> 16) & 0xFF;
            uint32_t g = (p >>  8) & 0xFF;
            uint32_t b = (p      ) & 0xFF;
            if (a > 0 && a < 255) {
                r = r * 255 / a; if (r > 255) r = 255;
                g = g * 255 / a; if (g > 255) g = 255;
                b = b * 255 / a; if (b > 255) b = 255;
            }
            unsigned char rgb[3] = { (unsigned char)r, (unsigned char)g, (unsigned char)b };
            fwrite(rgb, 1, 3, fp);
        }
    }
    fclose(fp);
    fprintf(stderr, "wrote %s (%ux%u)\n", path, fb->w, fb->h);
}

int main(int argc, char **argv)
{
    uint32_t W = 1024, H = 768;
    int wp_mode = 4; /* SCENE_WP_PLASMA */
    const char *outpath = "preview.ppm";

    if (argc > 1) { W = (uint32_t)atoi(argv[1]); H = (uint32_t)atoi(argv[2]); }
    if (argc > 3) wp_mode = atoi(argv[3]);
    if (argc > 4) outpath = argv[4];
    const char *wp_path = NULL;
    if (argc > 5) wp_path = argv[5];

    scene_loopback *lb = scene_loopback_new();
    scene_transport *server_ts = scene_loopback_server_end(lb);
    scene_client *cl = scene_client_new();
    scene_compositor *cp = scene_compositor_new(NULL, W, H);

    scene_compositor_set_effects(cp, 1);
    scene_compositor_set_clear(cp, 0xFF1A1A2E);

    scene_server_attach(scene_compositor_server(cp));
    scene_client_connect(cl, scene_loopback_client_end(lb), "preview-dump", NULL, NULL);

    scene_shell_config cfg;
    scene_shell_config_defaults(&cfg);
    cfg.wallpaper_mode = (uint8_t)wp_mode;
    cfg.wallpaper_speed = 1.0f;
    if (wp_mode == 0) { /* SCENE_WP_STATIC */
        if (wp_path)
            strncpy(cfg.wallpaper_path, wp_path, sizeof(cfg.wallpaper_path)-1);
        else
            strncpy(cfg.wallpaper_path, "wallpaper.bmp", sizeof(cfg.wallpaper_path)-1);
    }
    cfg.panel_height = 32;
    cfg.clock_12h = 0;

    fprintf(stderr, "sizeof(scene_shell_config)=%zu\n", sizeof(scene_shell_config));

    scene_shell *sh = scene_shell_new(cl, scene_compositor_store(cp), cp, &cfg);

    /* Must pump WELCOME before shell build — cli_emit requires welcomed=1 */
    pump_loop(cl, server_ts, cp);
    fprintf(stderr, "WELCOME pump done\n");

    int br = scene_shell_build(sh, W, H);
    fprintf(stderr, "shell_build returned: %d\n", br);

    fprintf(stderr, "shell built, pumping...\n");

    /* Need WELCOME first — pump enough rounds for connect handshake */
    int i;
    for (i = 0; i < 4; i++)
        pump_loop(cl, server_ts, cp);
    fprintf(stderr, "post-connect pump done\n");

    /* Pump build ops */
    for (i = 0; i < 15; i++)
        pump_loop(cl, server_ts, cp);
    fprintf(stderr, "pumped 15, ticking...\n");

    /* Tick (clock, wallpaper) */
    scene_shell_tick(sh);

    for (i = 0; i < 15; i++)
        pump_loop(cl, server_ts, cp);
    fprintf(stderr, "pumped 15 after tick\n");

    /* Render several frames to let effects settle */
    for (i = 0; i < 12; i++) {
        scene_compositor_force_repaint(cp);
        scene_compositor_frame(cp);
        pump_loop(cl, server_ts, cp);
    }

    fprintf(stderr, "rendered 12 frames\n");

    /* Check store state */
    scene_store *s = scene_compositor_store(cp);
    fprintf(stderr, "store node_count: %u\n", scene_store_node_count(s));

    scene_node_vis v;
    int r;
    r = scene_store_node_vis(s, 10000, &v);
    fprintf(stderr, "bg(10000): vis=%d rect=[%d,%d,%d,%d] flags=0x%02X tex=%u blend=%u opacity=%u\n",
            r, v.rect[0], v.rect[1], v.rect[2], v.rect[3], v.flags, v.tex, v.blend, v.opacity);
    r = scene_store_node_vis(s, 10001, &v);
    fprintf(stderr, "panel(10001): vis=%d rect=[%d,%d,%d,%d] flags=0x%02X tex=%u blend=%u opacity=%u\n",
            r, v.rect[0], v.rect[1], v.rect[2], v.rect[3], v.flags, v.tex, v.blend, v.opacity);
    r = scene_store_node_vis(s, 10002, &v);
    fprintf(stderr, "start(10002): vis=%d rect=[%d,%d,%d,%d] flags=0x%02X blend=%u opacity=%u\n",
            r, v.rect[0], v.rect[1], v.rect[2], v.rect[3], v.flags, v.blend, v.opacity);
    r = scene_store_node_vis(s, 10003, &v);
    fprintf(stderr, "clock(10003): vis=%d rect=[%d,%d,%d,%d] flags=0x%02X blend=%u opacity=%u\n",
            r, v.rect[0], v.rect[1], v.rect[2], v.rect[3], v.flags, v.blend, v.opacity);

    const scene_fb *fb = scene_compositor_fb(cp);
    fprintf(stderr, "fb: %ux%u pitch=%u\n", fb->w, fb->h, fb->pitch);
    fprintf(stderr, "rendered_seq: %lu\n", (unsigned long)scene_compositor_rendered_seq(cp));

    /* Dump framebuffer */
    dump_ppm(scene_compositor_fb(cp), outpath);

    /* Cleanup */
    scene_shell_free(sh);
    scene_client_free(cl);
    scene_compositor_free(cp);
    scene_loopback_free(lb);

    fprintf(stderr, "done.\n");
    return 0;
}
