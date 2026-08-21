/* iso_wl_client.c — minimal Wayland xdg client proving the scene-engine
 * compositor's honest boundary: the client window arrives as a composited
 * texture, pixel-for-pixel, into the OS scene.
 *
 * Uses real libwayland-client + the generated xdg-shell client code
 * (third_party/wayland/xdg-shell-{protocol.c,client.h}, produced by
 * wayland-scanner from the stable protocol) — the same protocol stack any
 * native Wayland application (e.g. NetSurf's fb frontend) speaks.
 *
 * The window draws a deterministic pattern in a wl_shm XRGB8888 buffer:
 *   rows 0..31        -> red     0x00CC0000
 *   rows h-32..h-1    -> blue    0x00000088
 *   center rectangle  -> white   0x00FFFFFF
 *   otherwise         -> green   0x0077AA33
 * The compositor imports the frame and blits it into the OS scene. The
 * ISO proof reads the scene framebuffer back and checks these exact
 * colors at probe coordinates (XRGB8888 little-endian).
 *
 * Build (host, any Linux with wayland-client):
 *   cc -o build/iso_wl_client tools/iso_wl_client.c \
 *        third_party/wayland/xdg-shell-protocol.c \
 *        -Ithird_party/wayland \
 *        $(pkg-config --cflags --libs wayland-client)
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include "xdg-shell-client.h"

#define WIN_W 480u
#define WIN_H 300u
#define TITLE "iso-wl-test"

struct client {
    struct wl_display   *display;
    struct wl_registry  *registry;
    struct wl_compositor *compositor;
    struct wl_shm       *shm;
    struct xdg_wm_base  *wm_base;
    struct wl_surface   *surface;
    struct xdg_surface  *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_shm_pool  *pool;
    struct wl_buffer    *buffer;
    void                *data;
    size_t               data_size;
    int                  configured;
    int                  run;
};

/* ---- registry / interface globals -------------------------------------- */

static const struct xdg_wm_base_listener wm_base_listener;

static void registry_handle_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version)
{
    struct client *c = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        c->compositor = wl_registry_bind(registry, name,
                &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        c->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        c->wm_base = wl_registry_bind(registry, name,
                &xdg_wm_base_interface, version < 1 ? version : 1);
        xdg_wm_base_add_listener(c->wm_base, &wm_base_listener, c);
    }
}

static void registry_handle_global_remove(void *data,
        struct wl_registry *registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

/* ---- xdg-shell listeners ------------------------------------------------ */

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base,
        uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface,
        uint32_t serial)
{
    struct client *c = data;
    xdg_surface_ack_configure(surface, serial);
    c->configured = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
        int32_t width, int32_t height, struct wl_array *states)
{
    (void)data; (void)toplevel; (void)states;
    /* We draw our fixed-size pattern; ignore compositor sizing. */
    (void)width; (void)height;
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    struct client *c = data;
    (void)toplevel;
    c->run = 0;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

/* ---- buffer helpers ----------------------------------------------------- */

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    (void)buffer;
    /* Single static frame: nothing to redraw. */
    (void)data;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static int make_shm_file(size_t size)
{
    char name[] = "/tmp/iso-wl-shm-XXXXXX";
    int fd = mkstemp(name);
    if (fd < 0) return -1;
    unlink(name);
    if (ftruncate(fd, (off_t)size) != 0) { close(fd); return -1; }
    return fd;
}

static int create_buffer(struct client *c)
{
    const uint32_t stride = WIN_W * 4;
    c->data_size = (size_t)stride * WIN_H;

    int fd = make_shm_file(c->data_size);
    if (fd < 0) return -1;
    c->data = mmap(NULL, c->data_size, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (c->data == MAP_FAILED) { close(fd); return -1; }

    c->pool = wl_shm_create_pool(c->shm, fd, (int32_t)c->data_size);
    close(fd);
    c->buffer = wl_shm_pool_create_buffer(c->pool, 0, WIN_W, WIN_H,
            (int32_t)stride, WL_SHM_FORMAT_XRGB8888);
    wl_buffer_add_listener(c->buffer, &buffer_listener, c);
    return 0;
}

/* Draw the deterministic pattern described in the file header. */
static void draw_pattern(struct client *c)
{
    uint32_t *px = c->data;

    for (uint32_t y = 0; y < WIN_H; y++) {
        for (uint32_t x = 0; x < WIN_W; x++) {
            uint32_t col;
            if (y < 32) {
                col = 0x00CC0000u;                       /* top: red      */
            } else if (y >= WIN_H - 32) {
                col = 0x00000088u;                       /* bottom: blue  */
            } else if (x >= 120 && x < 360 && y >= 100 && y < 200) {
                col = 0x00FFFFFFu;                       /* center: white */
            } else {
                col = 0x0077AA33u;                       /* else: green   */
            }
            px[y * WIN_W + x] = col;
        }
    }
}

/* ---- main ---------------------------------------------------------------- */

static int run(struct client *c)
{
    /* registry + globals */
    c->registry = wl_display_get_registry(c->display);
    wl_registry_add_listener(c->registry, &registry_listener, c);
    wl_display_roundtrip(c->display);
    if (!c->compositor || !c->shm || !c->wm_base) {
        fprintf(stderr, "iso-wl-client: missing globals\n");
        return 1;
    }

    c->surface = wl_compositor_create_surface(c->compositor);
    c->xdg_surface = xdg_wm_base_get_xdg_surface(c->wm_base, c->surface);
    xdg_surface_add_listener(c->xdg_surface, &xdg_surface_listener, c);
    c->toplevel = xdg_surface_get_toplevel(c->xdg_surface);
    xdg_toplevel_add_listener(c->toplevel, &xdg_toplevel_listener, c);
    xdg_toplevel_set_title(c->toplevel, TITLE);
    xdg_toplevel_set_app_id(c->toplevel, "org.scene.iso-wl-test");

    wl_surface_commit(c->surface);
    wl_display_roundtrip(c->display);   /* deliver xdg configure */

    if (create_buffer(c) != 0) {
        fprintf(stderr, "iso-wl-client: shm buffer failed\n");
        return 1;
    }
    draw_pattern(c);

    wl_surface_attach(c->surface, c->buffer, 0, 0);
    wl_surface_damage_buffer(c->surface, 0, 0, (int32_t)WIN_W,
                             (int32_t)WIN_H);
    wl_surface_commit(c->surface);

    fprintf(stdout, "iso-wl-client: window %ux%u committed\n", WIN_W, WIN_H);
    fflush(stdout);

    while (c->run && wl_display_dispatch(c->display) != -1) { }
    return 0;
}

int main(void)
{
    struct client c;
    memset(&c, 0, sizeof(c));
    c.run = 1;
    fprintf(stderr, "iso-wl-client: WAYLAND_DISPLAY=%s XDG_RUNTIME_DIR=%s\n", getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(null)", getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "(null)"); fflush(stderr);
    c.display = wl_display_connect(NULL);
    if (!c.display) {
        fprintf(stderr, "iso-wl-client: cannot connect to WAYLAND_DISPLAY (%s)\n", strerror(errno));
        return 1;
    }
    int rc = run(&c);
    wl_display_disconnect(c.display);
    return rc;
}