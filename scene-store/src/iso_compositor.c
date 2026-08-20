/* iso_compositor.c — custom wlroots compositor integrating the scene engine.
 *
 * Architecture:
 *   wlroots              ->  Wayland protocol, outputs, input
 *   iso_server           ->  owns the scene engine (scene_compositor + its
 *                            layer-0 store), the desktop shell, and the
 *                            bridge from Wayland clients into the scene
 *   scene_compositor     ->  software-renders the semantic scene to a fb
 *   wlroots output      <-  presents that fb as one XRGB8888 texture
 *
 * The honest boundary (stated, never hidden): legacy Wayland client frames
 * are COMPOSITED TEXTURES, not semantic nodes. A wl client's surface is
 * imported as a texture into the scene store's layer-0 registry and blitted
 * into a window node the compositor owns. The scene store owns meaning for
 * native apps; wl clients arrive as pixels plus a window rect and title.
 *
 * The wl-window node layout (layer 0, the shell session):
 *   WINDOW node  (role SCENE_ROLE_WINDOW, id < ID_BACKGROUND so the shell
 *                 tracks it as a task button), text slot 0 = client title
 *   IMAGE child  (role SCENE_ROLE_IMAGE, rect (0,0,w,h) in window space),
 *                 texture ref = the client's frame, refreshed every commit
 * The client draws its own chrome (NetSurf's framebuffer UI has its own
 * titlebar), so the OS adds none.
 *
 * wlroots API facts this file rests on (verified against Debian trixie
 * wlroots 0.17 headers, 2026-08-20): wlr_renderer_read_pixels reads
 * back in the requested DRM format (XY conversions supported);
 * wlr_surface_get_texture lives in wlr/types/wlr_compositor.h;
 * wlr_render_pass_submit/pass options struct is wlr_render_texture_options
 * (alpha is a const float*, blend_mode=0 is premultiplied, filter 0 =
 * bilinear, transform 0 = normal); wlr_output_begin_render_pass takes
 * (output, state-or-NULL, buffer_age-or-NULL, timer-or-NULL);
 * wlr_output_preferred_mode (not wlr_output_get_preferred_mode);
 * wlr_output_layout was removed in 0.17 (single-output, none used);
 * wlr_headless_backend_create + wlr_headless_add_output are behind
 * -DWLR_USE_UNSTABLE; DRM format macros come from drm_fourcc.h (fallback
 * literals are defined below if the header is absent).
 *
 * Build: Linux only, -DWLR_USE_UNSTABLE -D_POSIX_C_SOURCE=200809L,
 * queried via `pkg-config --cflags --libs wlroots wayland-server xkbcommon
 * libdrm`. Not compiled by the Windows tree; the ISO build links the
 * standalone binary `iso-wl` (ISO_WL_MAIN, guarded below).
 *
 * This file is Linux-only (wlroots dependency).                              */

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "iso_compositor.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#ifndef DRM_FORMAT_XRGB8888
#define DRM_FORMAT_XRGB8888 0x34325258u
#endif

#ifndef WLR_COMPOSITOR_VERSION
#define WLR_COMPOSITOR_VERSION 5u
#endif
#ifndef WLR_XDG_SHELL_VERSION
#define WLR_XDG_SHELL_VERSION 3u
#endif

/* Window position constants for the open cascade. */
#define WL_WIN_TITLE_H    32u      /* unused in v1 (client draws chrome)  */
#define WL_WIN_CASCADE_X  24       /* px between successive window origins */
#define WL_WIN_CASCADE_Y  24
#define WL_WIN_START_X    96
#define WL_WIN_START_Y    88

/* Node/texture id spaces (layer-0 store). Window nodes must sit below
 * ID_BACKGROUND (10000) so the shell's task reconciliation tracks them. */
#define WL_WIN_ID_BASE   9000u
#define WL_WIN_ID_CAP    (10000u - WL_WIN_ID_BASE)
#define WL_TEX_BASE      0x70000000u
#define WL_TEX_CAP       4096u

typedef struct iso_server iso_server;

/* ---- per-xdg-toplevel window ------------------------------------------ */

typedef struct iso_window {
    struct iso_server   *srv;
    struct wlr_xdg_toplevel *toplevel;
    struct wlr_surface  *surface;      /* xdg_surface->surface            */
    uint32_t              node_id;
    uint32_t              content_id;  /* IMAGE child                     */
    scene_texture_ref     tex_ref;     /* SCENE_NO_TEXTURE = none yet     */
    uint32_t              buf_w, buf_h;/* dims of the currently held ref  */
    int                   mapped;
    int                   dead;
    int                   slot;        /* index in the window id table    */
    struct wl_listener    map;
    struct wl_listener    unmap;
    struct wl_listener    destroy;
    struct wl_listener    commit;      /* wl_surface commit (new frame)   */
    struct wl_listener    toplevel_unmap; /* xdg toplevel internal unmap  */
    struct wl_listener    surface_destroy; /* wl_surface destroy          */
    struct wl_list        link;
} iso_window;

/* ---- server ----------------------------------------------------------- */

struct iso_server {
    struct wl_display   *wl_display;
    struct wlr_backend  *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_compositor *compositor;
    struct wlr_xdg_shell *xdg_shell;
    struct wlr_seat     *seat;

    /* scene engine + shell (layer 0) */
    scene_compositor    *cp;
    scene_loopback      *lb;
    scene_transport     *server_ts;    /* server side of the loopback     */
    scene_client        *cli;          /* owner client (shell + wl win)   */
    scene_shell         *sh;
    scene_shell_config   sh_cfg;
    int                   welcomed;

    /* wl windows */
    struct wl_list        windows;
    uint8_t               win_slots[WL_WIN_ID_CAP]; /* 1 = id in use      */
    uint32_t              win_next;                 /* slot candidate     */
    uint32_t              tx_next;                  /* texture ref counter */
    uint32_t              win_count;                /* cascade order       */

    /* output */
    struct wlr_output    *output;      /* primary output (first one wins) */
    struct wl_listener    new_output;
    struct wl_listener    output_frame;
    struct wl_listener    output_destroy;

    /* input */
    struct wl_listener    new_input;
    struct wl_listener    pointer_motion;
    struct wl_listener    pointer_button;
    struct wl_listener    keyboard_key;
    struct wl_listener    keyboard_modifiers;
    struct wl_listener    new_xdg_surface;
    struct wlr_keyboard  *keyboard;    /* active keyboard, for seat focus */

    double                ptr_x, ptr_y;
    struct wlr_surface   *focus_surface; /* seat-focused wl surface       */

    uint64_t              frames;
    const char           *dump_ppm;    /* env ISO_DUMP_PPM: write each fb */
};

/* ======================================================================
 * Scene seam helpers
 * ====================================================================== */

/* ---- owner client callbacks (events the engine pushes at us) ---------- */

static void cb_activate(void *ud, uint64_t seq, scene_node_id id)
{
    iso_server *srv = ud;
    if (srv->sh)
        scene_shell_handle_activate(srv->sh, id);
    scene_client_ack(srv->cli, seq);
}

static void cb_pointer(void *ud, uint64_t seq, uint8_t device,
                       int32_t x, int32_t y, uint8_t buttons)
{
    iso_server *srv = ud;
    if (srv->sh)
        scene_shell_handle_pointer(srv->sh, x, y, buttons);
    scene_client_ack(srv->cli, seq);
}

static void cb_focus(void *ud, uint64_t seq, scene_node_id id, uint8_t state)
{
    iso_server *srv = ud;
    (void)id; (void)state;
    scene_client_ack(srv->cli, seq);
}

static void cb_key(void *ud, uint64_t seq, uint32_t code, uint8_t state,
                   uint8_t mods)
{
    iso_server *srv = ud;
    if (srv->sh)
        scene_shell_handle_key(srv->sh, code, state, mods);
    scene_client_ack(srv->cli, seq);
}

static void cb_text(void *ud, uint64_t seq, const char *text, uint32_t len)
{
    iso_server *srv = ud;
    (void)text; (void)len;
    scene_client_ack(srv->cli, seq);
}

static void cb_welcome(void *ud, uint32_t scene_id, uint16_t version,
                       const scene_limits *lim)
{
    iso_server *srv = ud;
    (void)scene_id; (void)version; (void)lim;
    srv->welcomed = 1;
}

static const scene_client_cbs owner_cbs = {
    .welcome        = cb_welcome,
    .input_activate = cb_activate,
    .input_pointer  = cb_pointer,
    .input_focus    = cb_focus,
    .input_key      = cb_key,
    .input_text     = cb_text,
};

/* ---- loopback pumping (one thread drives both ends) -------------------- */

static void scene_tick(iso_server *srv)
{
    scene_server *sv = scene_compositor_server(srv->cp);

    /* server -> client: drain the server adapter's outbound into the
     * loopback; the owner client's pump() then dispatches it. */
    scene_client_pump(srv->cli);
    const uint8_t *frame = NULL;
    uint32_t flen = 0;
    while (scene_server_out_next_frame(sv, &frame, &flen) == 0 && flen > 0) {
        if (scene_transport_send(srv->server_ts, frame, flen) != 0) return;
    }
    scene_client_flush(srv->cli);

    /* client -> server: read whatever the client put on the loopback and
     * feed it into the server adapter (frame reassembly inside). */
    for (;;) {
        uint8_t buf[8192];
        uint32_t got = 0;
        int r = scene_transport_recv(srv->server_ts, buf, sizeof(buf), &got);
        if (r != 0 || got == 0) break;
        if (scene_server_feed(sv, buf, got) != 0) {
            fprintf(stderr, "iso-wl: scene server engine error (fatal)\n");
            break;
        }
    }

    if (srv->sh)
        scene_shell_tick(srv->sh);
}

/* ---- node ops through the owner client --------------------------------- */

static int wl_create_window_nodes(iso_server *srv, iso_window *win,
                                  uint32_t x, uint32_t y,
                                  uint32_t w, uint32_t h)
{
    scene_rect r;

    r.x = (int32_t)x; r.y = (int32_t)y;
    r.w = (uint32_t)w; r.h = (uint32_t)h;
    if (scene_client_create_node(srv->cli, SCENE_NO_PARENT, win->node_id,
            SCENE_ROLE_WINDOW, &r,
            SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE) != 0)
        return -1;

    r.x = 0; r.y = 0; r.w = w; r.h = h;
    if (scene_client_create_node(srv->cli, win->node_id, win->content_id,
            SCENE_ROLE_IMAGE, &r, SCENE_FLAG_VISIBLE) != 0)
        return -1;
    return 0;
}

static void wl_set_title(iso_server *srv, iso_window *win)
{
    const char *title = win->toplevel ? win->toplevel->title : NULL;
    const char *t = title ? title : "";
    size_t len = strlen(t);
    if (len > 512) len = 512;
    scene_client_set_text(srv->cli, win->node_id, 0, t, (uint32_t)len);
}

static int wl_place_texture(iso_server *srv, iso_window *win,
                            uint32_t w, uint32_t h)
{
    scene_rect r = { 0, 0, w, h };
    return scene_client_set_texture(srv->cli, win->content_id, win->tex_ref,
                                    &r, 0, 255);
}

/* Import the surface's current frame into the scene: read the pixels from
 * the compositor's texture (wlr_renderer_read_pixels), bump the texture ref
 * on size change, register it into the layer-0 store + compositor registry,
 * and point the content node at it. Called from the surface commit path. */
static void wl_import_frame(iso_server *srv, iso_window *win)
{
    struct wlr_surface *surf = win->surface;
    if (!surf || !surf->buffer) return;

    uint32_t w = surf->current.width;
    uint32_t h = surf->current.height;
    if (w == 0 || h == 0 || w > 8192 || h > 8192) return;

    struct wlr_texture *tex = wlr_surface_get_texture(surf);
    if (!tex) return;

    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px) return;
    if (!wlr_renderer_read_pixels(srv->renderer, DRM_FORMAT_XRGB8888,
                                  w * 4, w, h, 0, 0, 0, 0, px)) {
        free(px);
        return;
    }

    if (win->tex_ref != SCENE_NO_TEXTURE &&
        (win->buf_w != w || win->buf_h != h)) {
        /* size change: retire the old ref entirely, take a fresh one */
        scene_store_release_texture(scene_compositor_layer_store(srv->cp, 0),
                                    win->tex_ref);
        scene_compositor_release_texture(srv->cp, win->tex_ref);
        win->tex_ref = SCENE_NO_TEXTURE;
        win->buf_w = win->buf_h = 0;
    }

    if (win->tex_ref == SCENE_NO_TEXTURE) {
        if (srv->tx_next >= WL_TEX_CAP) { free(px); return; }
        win->tex_ref = WL_TEX_BASE + srv->tx_next++;
        win->buf_w = w; win->buf_h = h;
        if (scene_store_register_texture(scene_compositor_layer_store(srv->cp, 0),
                    win->tex_ref, w, h, SCENE_TEX_FMT_XRGB, 1) != 0) {
            win->tex_ref = SCENE_NO_TEXTURE;
            win->buf_w = win->buf_h = 0;
            free(px);
            return;
        }
    }

    wl_place_texture(srv, win, w, h);
    scene_compositor_register_texture(srv->cp, win->tex_ref, w, h,
                                      SCENE_TEX_FMT_XRGB, 1,
                                      (const uint32_t *)px);
    free(px);
}

/* ---- wl-surface / xdg lifecycle --------------------------------------- */

static void win_commit(struct wl_listener *listener, void *data)
{
    iso_window *win = wl_container_of(listener, win, commit);
    (void)data;
    if (win->dead) return;
    wl_import_frame(win->srv, win);
    wl_set_title(win->srv, win);
}

static void win_map(struct wl_listener *listener, void *data)
{
    iso_window *win = wl_container_of(listener, win, map);
    iso_server *srv = win->srv;
    (void)data;
    if (win->mapped || win->dead) return;
    win->mapped = 1;

    uint32_t w = win->surface ? win->surface->current.width : 0;
    uint32_t h = win->surface ? win->surface->current.height : 0;
    if (w == 0 || h == 0) { w = 320; h = 240; }

    uint32_t x, y;
    if (srv->output) {
        x = (uint32_t)(WL_WIN_START_X +
            ((int)(srv->win_count % 8) * WL_WIN_CASCADE_X));
        y = (uint32_t)(WL_WIN_START_Y +
            ((int)(srv->win_count % 8) * WL_WIN_CASCADE_Y));
    } else {
        x = WL_WIN_START_X; y = WL_WIN_START_Y;
    }
    srv->win_count++;

    if (wl_create_window_nodes(srv, win, x, y, w, h) != 0) {
        win->mapped = 0;
        return;
    }
    wl_set_title(srv, win);

    /* If a frame already arrived before map, its texture was imported with
     * the ref; point the freshly-created content node at it. */
    if (win->tex_ref != SCENE_NO_TEXTURE)
        wl_place_texture(srv, win, win->buf_w, win->buf_h);

    /* The client becomes the seat keyboard focus (its window is new). */
    if (srv->seat && srv->keyboard && win->surface) {
        wlr_seat_keyboard_notify_enter(srv->seat, win->surface, NULL, 0, NULL);
        srv->focus_surface = win->surface;
    }
}

static void win_unmap(struct wl_listener *listener, void *data)
{
    iso_window *win = wl_container_of(listener, win, unmap);
    (void)data;
    if (!win->mapped || win->dead) return;
    win->mapped = 0;
    if (win->srv->cli)
        scene_client_set_flags(win->srv->cli, win->node_id, 0);
}

static void win_destroy(struct wl_listener *listener, void *data)
{
    iso_window *win = wl_container_of(listener, win, destroy);
    iso_server *srv = win->srv;
    (void)data;
    if (win->dead) return;
    win->dead = 1;

    if (srv->cli && srv->welcomed) {
        scene_client_destroy_node(srv->cli, win->content_id);
        scene_client_destroy_node(srv->cli, win->node_id);
    }
    if (win->tex_ref != SCENE_NO_TEXTURE) {
        scene_store_release_texture(scene_compositor_layer_store(srv->cp, 0),
                                    win->tex_ref);
        scene_compositor_release_texture(srv->cp, win->tex_ref);
    }
    if (win->surface && srv->focus_surface == win->surface) {
        srv->focus_surface = NULL;
        if (srv->seat)
            wlr_seat_keyboard_clear_focus(srv->seat);
    }
    if (win->slot < WL_WIN_ID_CAP)
        srv->win_slots[win->slot] = 0;

    wl_list_remove(&win->map.link);
    wl_list_remove(&win->unmap.link);
    wl_list_remove(&win->destroy.link);
    wl_list_remove(&win->commit.link);
    wl_list_remove(&win->link);
    free(win);
}

static void xdg_toplevel_new(struct wl_listener *listener, void *data)
{
    iso_server *srv = wl_container_of(listener, srv, new_xdg_surface);
    struct wlr_xdg_surface *xdg_surface = data;
    if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL)
        return;

    uint32_t slot;
    for (slot = 0; slot < WL_WIN_ID_CAP; slot++) {
        uint32_t c = (srv->win_next + slot) % WL_WIN_ID_CAP;
        if (!srv->win_slots[c]) break;
    }
    if (slot >= WL_WIN_ID_CAP) return;

    iso_window *win = calloc(1, sizeof(*win));
    if (!win) return;
    win->srv      = srv;
    win->toplevel = xdg_surface->toplevel;
    win->surface  = xdg_surface->surface;
    win->node_id  = WL_WIN_ID_BASE + (slot * 3);
    win->content_id = win->node_id + 1;
    win->tex_ref  = SCENE_NO_TEXTURE;
    win->slot     = (uint32_t)slot;
    srv->win_slots[slot] = 1;
    srv->win_next = (slot + 1) % WL_WIN_ID_CAP;
    wl_list_insert(&srv->windows, &win->link);

    wl_signal_add(&win->toplevel->events.map, &win->map);
    win->map.notify = win_map;
    wl_signal_add(&win->toplevel->events.unmap, &win->unmap);
    win->unmap.notify = win_unmap;
    wl_signal_add(&xdg_surface->events.destroy, &win->destroy);
    win->destroy.notify = win_destroy;
    wl_signal_add(&win->surface->events.commit, &win->commit);
    win->commit.notify = win_commit;
}

/* ======================================================================
 * Output
 * ====================================================================== */

static void output_frame(struct wl_listener *listener, void *data)
{
    iso_server *srv = wl_container_of(listener, srv, output_frame);
    (void)data;

    scene_tick(srv);
    if (scene_compositor_frame(srv->cp) != 0)
        return;

    const scene_fb *fb = scene_compositor_fb(srv->cp);
    if (!fb || !srv->output)
        return;
    srv->frames++;

    /* Optional pixel proof: dump the scene fb to a PPM every frame. */
    if (srv->dump_ppm) {
        FILE *pf = fopen(srv->dump_ppm, "wb");
        if (pf) {
            fprintf(pf, "P6\n%u %u\n255\n", fb->w, fb->h);
            for (uint32_t i = 0; i < (uint32_t)(fb->w * fb->h); i++) {
                uint32_t p = fb->px[i];
                const uint8_t c[3] = {
                    (uint8_t)((p >> 16) & 0xFF),
                    (uint8_t)((p >> 8)  & 0xFF),
                    (uint8_t)(p & 0xFF)
                };
                fwrite(c, 1, 3, pf);
            }
            fclose(pf);
        }
    }

    struct wlr_texture *tex = wlr_texture_from_pixels(srv->renderer,
            DRM_FORMAT_XRGB8888, fb->w * 4, fb->w, fb->h, fb->px);
    if (!tex)
        return;

    struct wlr_render_pass *pass = wlr_output_begin_render_pass(srv->output,
            NULL, NULL, NULL);
    if (!pass) {
        wlr_texture_destroy(tex);
        return;
    }

    struct wlr_render_texture_options opt = { 0 };
    opt.texture = tex;
    opt.src_box = (struct wlr_fbox) {
        .width = fb->w, .height = fb->h
    };
    opt.dst_box = (struct wlr_box) {
        .width = srv->output->width, .height = srv->output->height
    };
    wlr_render_pass_add_texture(pass, &opt);
    wlr_render_pass_submit(pass);
    wlr_texture_destroy(tex);

    if (wlr_output_commit_state(srv->output, NULL) != 0)
        fprintf(stderr, "iso-wl: output commit failed\n");

    /* Software path (headless / no vblank): keep frames flowing. */
    wlr_output_schedule_frame(srv->output);
}

static void output_destroy(struct wl_listener *listener, void *data)
{
    iso_server *srv = wl_container_of(listener, srv, output_destroy);
    (void)data;
    srv->output = NULL;
}

static void output_new(struct wl_listener *listener, void *data)
{
    iso_server *srv = wl_container_of(listener, srv, new_output);
    struct wlr_output *out = data;

    /* Single output: keep the first one, ignore the rest. */
    if (srv->output)
        return;
    srv->output = out;

    struct wlr_output_state state = { 0 };
    if (!wl_list_empty(&out->modes)) {
        struct wlr_output_mode *mode = wlr_output_preferred_mode(out);
        if (mode) wlr_output_state_set_mode(&state, mode);
    } else {
        /* Headless outputs carry their size already. */
    }
    wlr_output_state_set_scale(&state, 1);
    wlr_output_commit_state(out, &state);

    wl_signal_add(&out->events.frame, &srv->output_frame);
    wl_signal_add(&out->events.destroy, &srv->output_destroy);

    /* Size the scene engine to this output and build the desktop. */
    scene_compositor_resize(srv->cp, out->width, out->height);
    if (srv->sh) {
        scene_shell_resize(srv->sh, out->width, out->height);
    } else if (srv->welcomed) {
        scene_shell_config_defaults(&srv->sh_cfg);
        srv->sh_cfg.panel_height = 40;
        srv->sh = scene_shell_new(srv->cli,
                                  scene_compositor_layer_store(srv->cp, 0),
                                  srv->cp, &srv->sh_cfg);
        if (srv->sh) {
            scene_compositor_setup_hover_style(srv->cp,
                    srv->sh_cfg.hover_color, srv->sh_cfg.button_text);
            scene_compositor_setup_active_style(srv->cp,
                    srv->sh_cfg.button_color, srv->sh_cfg.button_text);
            scene_shell_set_hover_style(srv->sh, 1);
            scene_shell_set_active_style(srv->sh, 2);
            scene_shell_build(srv->sh, out->width, out->height);
            scene_client_flush(srv->cli);
        }
    }
}

/* ======================================================================
 * Input
 * ====================================================================== */

static uint8_t key_mods_to_scene(struct wlr_keyboard *kb)
{
    uint8_t m = 0;
    xkb_mod_mask_t dep = kb->modifiers.depressed;
    struct xkb_keymap *km = kb->keymap;
    if (!km) return 0;
    xkb_mod_index_t ci = xkb_keymap_mod_get_index(km, XKB_MOD_NAME_CTRL);
    xkb_mod_index_t si = xkb_keymap_mod_get_index(km, XKB_MOD_NAME_SHIFT);
    xkb_mod_index_t ai = xkb_keymap_mod_get_index(km, XKB_MOD_NAME_ALT);
    xkb_mod_index_t li = xkb_keymap_mod_get_index(km, XKB_MOD_NAME_LOGO);
    if (ci != XKB_MOD_INVALID && (dep & (1u << ci))) m |= SCENE_MOD_CTRL;
    if (si != XKB_MOD_INVALID && (dep & (1u << si))) m |= SCENE_MOD_SHIFT;
    if (ai != XKB_MOD_INVALID && (dep & (1u << ai))) m |= SCENE_MOD_ALT;
    if (li != XKB_MOD_INVALID && (dep & (1u << li))) m |= SCENE_MOD_SUPER;
    return m;
}

static void pointer_motion(struct wl_listener *listener, void *data)
{
    iso_server *srv = wl_container_of(listener, srv, pointer_motion);
    struct wlr_pointer_motion_event *ev = data;
    srv->ptr_x += ev->delta_x;
    srv->ptr_y += ev->delta_y;
    if (srv->cp)
        scene_compositor_input_pointer(srv->cp, 0,
                (int32_t)srv->ptr_x, (int32_t)srv->ptr_y, 0);
    if (srv->seat && srv->focus_surface)
        wlr_seat_pointer_notify_motion(srv->seat, ev->time_msec,
                srv->ptr_x, srv->ptr_y);
}

static void pointer_button(struct wl_listener *listener, void *data)
{
    iso_server *srv = wl_container_of(listener, srv, pointer_button);
    struct wlr_pointer_button_event *ev = data;
    uint8_t btns = (ev->state == WLR_BUTTON_PRESSED) ? 1 : 0;
    if (srv->cp)
        scene_compositor_input_pointer(srv->cp, 0,
                (int32_t)srv->ptr_x, (int32_t)srv->ptr_y, btns);

    /* Seat routing: find the wl window the cursor is inside (topmost last-
     * mapped wins) and give it pointer + keyboard focus. */
    struct wlr_surface *hit = NULL;
    iso_window *win;
    wl_list_for_each(win, &srv->windows, link) {
        if (!win->mapped || win->dead || !win->surface) continue;
        struct wlr_surface *s = win->surface;
        if (srv->ptr_x >= 0 && srv->ptr_y >= 0 &&
            srv->ptr_x < s->current.width && srv->ptr_y < s->current.height)
            hit = s;
    }
    if (srv->seat) {
        if (hit && hit != srv->focus_surface) {
            wlr_seat_pointer_notify_enter(srv->seat, hit, srv->ptr_x,
                                          srv->ptr_y);
            srv->focus_surface = hit;
            if (srv->keyboard)
                wlr_seat_keyboard_notify_enter(srv->seat, hit, NULL, 0, NULL);
        }
        wlr_seat_pointer_notify_button(srv->seat, ev->time_msec,
                ev->button, ev->state);
    }
}

static void keyboard_key(struct wl_listener *listener, void *data)
{
    iso_server *srv = wl_container_of(listener, srv, keyboard_key);
    struct wlr_keyboard_key_event *ev = data;
    uint8_t state = (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED) ? 1 : 0;
    if (srv->cp)
        scene_compositor_input_key(srv->cp, ev->keycode, state,
                srv->keyboard ? key_mods_to_scene(srv->keyboard) : 0);
    if (srv->seat)
        wlr_seat_keyboard_notify_key(srv->seat, ev->time_msec,
                ev->keycode, ev->state);
}

static void keyboard_modifiers(struct wl_listener *listener, void *data)
{
    iso_server *srv = wl_container_of(listener, srv, keyboard_modifiers);
    (void)data;
    if (srv->seat && srv->keyboard)
        wlr_seat_keyboard_notify_modifiers(srv->seat, &srv->keyboard->modifiers);
}

static void new_input(struct wl_listener *listener, void *data)
{
    iso_server *srv = wl_container_of(listener, srv, new_input);
    struct wlr_input_device *dev = data;
    switch (dev->type) {
    case WLR_INPUT_DEVICE_POINTER: {
        struct wlr_pointer *p = wlr_pointer_from_input_device(dev);
        wl_signal_add(&p->events.motion, &srv->pointer_motion);
        wl_signal_add(&p->events.button, &srv->pointer_button);
        break;
    }
    case WLR_INPUT_DEVICE_KEYBOARD: {
        struct wlr_keyboard *kb = wlr_keyboard_from_input_device(dev);
        struct xkb_rule_names rules = {
            .rules = NULL, .model = NULL, .layout = "us",
            .variant = NULL, .options = NULL
        };
        struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        struct xkb_keymap *km = ctx ?
            xkb_keymap_new_from_names(ctx, &rules,
                                      XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
        if (km) {
            wlr_keyboard_set_keymap(kb, km);
            xkb_keymap_unref(km);
        }
        xkb_context_unref(ctx);
        wlr_keyboard_set_repeat_info(kb, 25, 600);
        srv->keyboard = kb;
        wl_signal_add(&kb->events.key, &srv->keyboard_key);
        wl_signal_add(&kb->events.modifiers, &srv->keyboard_modifiers);
        break;
    }
    default:
        break;
    }
}

/* ======================================================================
 * Server lifecycle
 * ====================================================================== */

iso_server *iso_server_create(void)
{
    iso_server *srv = calloc(1, sizeof(*srv));
    if (!srv) return NULL;
    wl_list_init(&srv->windows);

    srv->wl_display = wl_display_create();
    if (!srv->wl_display) { free(srv); return NULL; }

    /* Scene engine first: the desktop + all window nodes land in layer 0. */
    srv->cp = scene_compositor_new(NULL, 1280, 800);
    if (!srv->cp) { wl_display_destroy(srv->wl_display); free(srv); return NULL; }
    scene_compositor_set_clear(srv->cp, 0xFF1A1A2E);
    scene_compositor_set_effects(srv->cp, 1);

    srv->lb = scene_loopback_new();
    srv->server_ts = scene_loopback_server_end(srv->lb);
    scene_server_attach(scene_compositor_server(srv->cp)); /* -> WELCOME */
    srv->cli = scene_client_new();
    if (scene_client_connect(srv->cli, scene_loopback_client_end(srv->lb),
                             "shell", &owner_cbs, srv) != 0) {
        fprintf(stderr, "iso-wl: scene client connect failed\n");
        scene_client_free(srv->cli); srv->cli = NULL;
    }
    scene_tick(srv);   /* deliver WELCOME (client pump needs the loopback
                          wiring; server_ts is set before this call) */

    srv->dump_ppm = getenv("ISO_DUMP_PPM");
    if (getenv("ISO_HEADLESS")) {
        srv->backend = wlr_headless_backend_create(srv->wl_display);
    } else {
        srv->backend = wlr_backend_autocreate(srv->wl_display, NULL);
    }
    if (!srv->backend) {
        fprintf(stderr, "iso-wl: failed to create the wlroots backend\n");
        goto fail;
    }

    srv->renderer = wlr_renderer_autocreate(srv->backend);
    if (!srv->renderer) {
        fprintf(stderr, "iso-wl: failed to create the renderer\n");
        goto fail;
    }
    wlr_renderer_init_wl_display(srv->renderer, srv->wl_display);

    srv->allocator = wlr_allocator_autocreate(srv->backend, srv->renderer);
    if (!srv->allocator) {
        fprintf(stderr, "iso-wl: failed to create the allocator\n");
        goto fail;
    }

    srv->compositor = wlr_compositor_create(srv->wl_display,
            WLR_COMPOSITOR_VERSION, srv->renderer);
    srv->xdg_shell = wlr_xdg_shell_create(srv->wl_display,
            WLR_XDG_SHELL_VERSION);
    srv->seat = wlr_seat_create(srv->wl_display, "seat0");
    if (!srv->compositor || !srv->xdg_shell || !srv->seat) {
        fprintf(stderr, "iso-wl: failed to create protocol globals\n");
        goto fail;
    }

    /* Register listeners before creating the headless output / starting
     * the backend so new_output/new_input fire into wired handlers. */
    srv->new_output.notify = output_new;
    srv->new_input.notify = new_input;
    srv->new_xdg_surface.notify = xdg_toplevel_new;
    srv->output_frame.notify = output_frame;
    srv->output_destroy.notify = output_destroy;
    srv->pointer_motion.notify = pointer_motion;
    srv->pointer_button.notify = pointer_button;
    srv->keyboard_key.notify = keyboard_key;
    srv->keyboard_modifiers.notify = keyboard_modifiers;
    wl_signal_add(&srv->backend->events.new_output, &srv->new_output);
    wl_signal_add(&srv->backend->events.new_input, &srv->new_input);
    wl_signal_add(&srv->xdg_shell->events.new_surface, &srv->new_xdg_surface);

    if (getenv("ISO_HEADLESS"))
        wlr_headless_add_output(srv->backend, 1280, 800);

    if (!wlr_backend_start(srv->backend)) {
        fprintf(stderr, "iso-wl: failed to start the backend\n");
        goto fail;
    }
    return srv;

fail:
    iso_server_destroy(srv);
    return NULL;
}

void iso_server_destroy(iso_server *srv)
{
    if (!srv) return;
    if (srv->backend)
        wlr_backend_destroy(srv->backend);
    if (srv->sh)
        scene_shell_free(srv->sh);
    if (srv->cli)
        scene_client_free(srv->cli);
    if (srv->lb)
        scene_loopback_free(srv->lb);
    if (srv->cp)
        scene_compositor_free(srv->cp);
    if (srv->wl_display)
        wl_display_destroy(srv->wl_display);
    free(srv);
}

scene_store *iso_server_store(iso_server *srv)
{
    return srv ? scene_compositor_layer_store(srv->cp, 0) : NULL;
}

scene_compositor *iso_server_scene_comp(iso_server *srv)
{
    return srv ? srv->cp : NULL;
}

scene_shell *iso_server_shell(iso_server *srv)
{
    return srv ? srv->sh : NULL;
}

/* ======================================================================
 * Standalone entry point (proves the compositor on the host/ISO)
 * ====================================================================== */
#ifdef ISO_WL_MAIN

static void child_term(int sig)
{
    (void)sig;
    /* no-op; SIGINT/SIGTERM end wl_display_run via the display listener */
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    wlr_log_init(WLR_DEBUG, NULL);

    signal(SIGINT, child_term);
    signal(SIGTERM, child_term);

    iso_server *srv = iso_server_create();
    if (!srv) return 1;

    const char *socket = wl_display_add_socket_auto(srv->wl_display);
    if (!socket) {
        fprintf(stderr, "iso-wl: no WAYLAND socket available (%s)\n",
                strerror(errno));
        iso_server_destroy(srv);
        return 1;
    }
    printf("iso-wl: WAYLAND_DISPLAY=%s\n", socket);
    fflush(stdout);

    wl_display_run(srv->wl_display);
    iso_server_destroy(srv);
    return 0;
}

#endif /* ISO_WL_MAIN */
