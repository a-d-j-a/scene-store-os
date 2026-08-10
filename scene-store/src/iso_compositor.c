/* iso_compositor.c — custom wlroots compositor integrating the scene engine.
 *
 * Architecture:
 *   wlroots          ->  Wayland protocol, output, input, GPU
 *   iso_compositor   ->  maps wlroots surfaces to scene-store ops
 *   scene_compositor ->  software-renders the semantic scene to a framebuffer
 *   wlroots output   <-  presents the framebuffer
 *
 * The scene_compositor owns both the store and the server seam.
 * wlroots client frames are fed into scene_server_feed().
 * Shell nodes (desktop, panel) are created via an internal scene_client
 * connected through a loopback transport.
 *
 * This file is Linux-only (wlroots dependency).                              */

#define _POSIX_C_SOURCE 200809L
#include "iso_compositor.h"
#include "scene_shell.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/renderer.h>
#include <wlr/render/texture.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_surface.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

/* ======================================================================
 * Structures
 * ====================================================================== */

/* Per-client-surface state: maps a wlroots surface to a scene-store node.
 * The scene-store node is created by the shell (iso_compositor) and
 * populated by the actual Wayland client's frame data fed through
 * scene_server_feed().                                                     */
typedef struct iso_surface {
    struct wlr_surface    *wlr_surf;
    scene_node_id          node_id;
    struct iso_server     *server;     /* back-pointer for client/comp access */
    struct wl_listener     on_map;
    struct wl_listener     on_unmap;
    struct wl_listener     on_commit;
    struct wl_listener     on_destroy;
    struct wl_list         link;   /* iso_server.surfaces */
} iso_surface;

struct iso_server {
    /* wlroots objects */
    struct wl_display       *wl_display;
    struct wlr_backend      *backend;
    struct wlr_renderer     *renderer;
    struct wlr_allocator    *allocator;
    struct wlr_output       *output;
    struct wlr_output_layout *output_layout;
    struct wlr_compositor   *wlr_compositor;
    struct wlr_xdg_shell    *xdg_shell;
    struct wlr_seat         *seat;
    struct wlr_cursor       *cursor;
    struct wlr_cursor_manager *cursor_mgr;

    /* scene engine (scene_compositor owns store + server seam) */
    scene_compositor        *scene_comp;

    /* loopback transport + shell client for creating desktop nodes */
    scene_loopback          *loopback;
    scene_transport         *shell_ts;       /* client end */
    scene_transport         *server_ts;      /* server end (fed into server) */
    scene_client            *shell_client;

    /* desktop shell (optional, for themed shell) */
    scene_shell             *shell;
    scene_shell_config       shell_cfg;

    /* tracked surfaces */
    struct wl_list           surfaces;  /* iso_surface.link */

    /* next node ID for wlroots-mapped surfaces */
    uint32_t                 next_node_id;

    /* output dimensions */
    uint32_t                 width, height;

    /* listeners */
    struct wl_listener      on_new_output;
    struct wl_listener      on_new_xdg_surface;
    struct wl_listener      on_new_input;
    struct wl_listener      on_cursor_motion;
    struct wl_listener      on_cursor_button;
    struct wl_listener      on_keyboard_key;
    struct wl_listener      on_request_set_cursor;

    /* per-output frame listener (dynamic, attached on output creation) */
    struct wl_listener      on_frame;
};

/* Shell node IDs (owned by the compositor, not by clients). */
#define ISO_SHELL_DESKTOP   1u
#define ISO_SHELL_PANEL     2u

/* wlroots-surface node IDs start here. */
#define ISO_NODE_BASE  1000u

/* Default desktop background color (dark). */
#define ISO_DESKTOP_COLOR  0xFF1A1A1Au

/* ======================================================================
 * Server pump: flush shell client, drain server responses
 * ====================================================================== */

static void server_pump(iso_server *srv)
{
    scene_client *sc = srv->shell_client;
    scene_server *ss = scene_compositor_server(srv->scene_comp);

    scene_client_flush(sc);

    uint8_t buf[8192];
    uint32_t got;
    while (scene_transport_recv(srv->server_ts, buf, sizeof(buf), &got) == 0
           && got) {
        scene_server_feed(ss, buf, got);
    }

    const uint8_t *f;
    uint32_t flen;
    while (scene_server_out_next_frame(ss, &f, &flen) == 1)
        scene_transport_send(srv->server_ts, f, flen);

    scene_client_pump(sc);
}

/* ======================================================================
 * Shell: desktop background + panel (fallback without scene_shell)
 * ====================================================================== */

static void shell_create_desktop(iso_server *srv)
{
    scene_client *c = srv->shell_client;
    scene_rect r = { 0, 0, (int32_t)srv->width, (int32_t)srv->height };

    scene_client_create_node(c, SCENE_NO_PARENT, ISO_SHELL_DESKTOP,
                             SCENE_ROLE_WINDOW, &r, SCENE_FLAG_VISIBLE);
    scene_client_set_style(c, ISO_SHELL_DESKTOP, 0);
}

static void shell_create_panel(iso_server *srv)
{
    scene_client *c = srv->shell_client;
    int32_t ph = 36;
    scene_rect r = { 0, (int32_t)srv->height - ph,
                     (int32_t)srv->width, ph };

    scene_client_create_node(c, ISO_SHELL_DESKTOP, ISO_SHELL_PANEL,
                             SCENE_ROLE_PANEL, &r, SCENE_FLAG_VISIBLE);
}

static void shell_resize(iso_server *srv, uint32_t w, uint32_t h)
{
    scene_client *c = srv->shell_client;
    int32_t ph = 36;

    scene_rect desktop_r = { 0, 0, (int32_t)w, (int32_t)h };
    scene_client_set_rect(c, ISO_SHELL_DESKTOP, &desktop_r);

    scene_rect panel_r = { 0, (int32_t)h - ph, (int32_t)w, ph };
    scene_client_set_rect(c, ISO_SHELL_PANEL, &panel_r);
}

/* ======================================================================
 * Surface to scene-store mapping
 * ====================================================================== */

static void surface_update_rect(iso_surface *surf)
{
    struct wlr_surface *ws = surf->wlr_surf;
    scene_rect r = {
        ws->current.x,
        ws->current.y,
        ws->current.width,
        ws->current.height
    };
    scene_client_set_rect(surf->server->shell_client, surf->node_id, &r);
}

static void surface_update_texture(iso_surface *surf)
{
    struct wlr_surface *ws = surf->wlr_surf;
    struct wlr_texture *tex;

    if (!ws->buffer) return;
    tex = wlr_surface_get_texture(ws);
    if (!tex) return;

    uint32_t pw = ws->current.width;
    uint32_t ph = ws->current.height;
    if (pw == 0 || ph == 0) return;

    void *pixels = calloc((size_t)pw * (size_t)ph, 4);
    if (!pixels) return;

    wlr_texture_read_pixels(tex, WL_OUTPUT_FORMAT_ARGB8888,
                            0, 0, pw, ph, pixels);

    scene_texture_ref tex_ref = surf->node_id;
    scene_compositor_register_texture(surf->server->scene_comp, tex_ref,
                                      pw, ph,
                                      1, /* SCENE_TEX_FMT_ARGB */
                                      0, /* not opaque */
                                      (const uint32_t *)pixels);
    scene_client_set_texture(surf->server->shell_client, surf->node_id,
                             tex_ref, NULL, 1, 255);

    free(pixels);
}

/* ---- wlroots callbacks for surface lifecycle ---- */

static void on_surface_map(struct wl_listener *l, void *data)
{
    iso_surface *surf = wl_container_of(l, surf, on_map);
    (void)data;
    surface_update_rect(surf);
    scene_client_set_flags(surf->server->shell_client, surf->node_id,
                           SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
    surface_update_texture(surf);
}

static void on_surface_unmap(struct wl_listener *l, void *data)
{
    iso_surface *surf = wl_container_of(l, surf, on_unmap);
    (void)data;
    scene_client_set_flags(surf->server->shell_client, surf->node_id, 0);
}

static void on_surface_commit(struct wl_listener *l, void *data)
{
    iso_surface *surf = wl_container_of(l, surf, on_commit);
    (void)data;
    surface_update_rect(surf);
    surface_update_texture(surf);
}

static void on_surface_destroy(struct wl_listener *l, void *data)
{
    iso_surface *surf = wl_container_of(l, surf, on_destroy);
    (void)data;

    wl_list_remove(&surf->on_map.link);
    wl_list_remove(&surf->on_unmap.link);
    wl_list_remove(&surf->on_commit.link);
    wl_list_remove(&surf->on_destroy.link);
    wl_list_remove(&surf->link);

    scene_client_destroy_node(surf->server->shell_client, surf->node_id);
    free(surf);
}

static iso_surface *iso_surface_create(iso_server *srv,
                                       struct wlr_surface *ws)
{
    iso_surface *surf = calloc(1, sizeof(*surf));
    if (!surf) return NULL;

    surf->wlr_surf  = ws;
    surf->server    = srv;
    surf->node_id   = srv->next_node_id++;

    scene_rect r = {
        ws->current.x, ws->current.y,
        ws->current.width, ws->current.height
    };
    scene_client_create_node(srv->shell_client,
                             ISO_SHELL_DESKTOP, surf->node_id,
                             SCENE_ROLE_WINDOW, &r, 0);

    surf->on_map.notify      = on_surface_map;
    surf->on_unmap.notify    = on_surface_unmap;
    surf->on_commit.notify   = on_surface_commit;
    surf->on_destroy.notify  = on_surface_destroy;
    wl_signal_add(&ws->events.map,    &surf->on_map);
    wl_signal_add(&ws->events.unmap,  &surf->on_unmap);
    wl_signal_add(&ws->events.commit, &surf->on_commit);
    wl_signal_add(&ws->events.destroy, &surf->on_destroy);

    wl_list_insert(&srv->surfaces, &surf->link);
    return surf;
}

/* ======================================================================
 * Output: present our framebuffer
 * ====================================================================== */

static void on_output_frame(struct wl_listener *l, void *data)
{
    iso_server *srv = wl_container_of(l, srv, on_frame);
    struct wlr_output *output = data;
    (void)output;

    /* Update shell (clock, task list, wallpaper). */
    if (srv->shell)
        scene_shell_tick(srv->shell);

    /* Flush shell client ops and drain server responses. */
    server_pump(srv);

    /* Compose one frame: diff, effects, repaint. */
    scene_compositor_frame(srv->scene_comp);

    /* Read back the framebuffer and present to the output. */
    const scene_fb *fb = scene_compositor_fb(srv->scene_comp);
    if (!fb || !fb->px || fb->w == 0 || fb->h == 0) return;

    uint32_t fb_w = fb->w;
    uint32_t fb_h = fb->h;

    struct wlr_render_pass *pass =
        wlr_output_begin_render_pass(output, NULL);
    if (!pass) return;

    struct wlr_texture *tex = wlr_texture_from_pixels(
        srv->renderer,
        WL_OUTPUT_FORMAT_ARGB8888,
        fb_w * 4,
        fb_w, fb_h,
        fb->px);
    if (!tex) {
        wlr_render_pass_submit(pass);
        return;
    }

    struct wlr_texture_options opts = {
        .texture = tex,
        .src_box = { 0, 0, (int)fb_w, (int)fb_h },
        .dst_box = { 0, 0, (int)fb_w, (int)fb_h },
        .alpha = 1.0f,
    };
    wlr_render_pass_add_texture(pass, &opts);
    wlr_texture_destroy(tex);
    wlr_render_pass_submit(pass);
}

/* ======================================================================
 * Input: cursor + keyboard to scene store
 * ====================================================================== */

static void on_cursor_motion(struct wl_listener *l, void *data)
{
    iso_server *srv = wl_container_of(l, srv, on_cursor_motion);
    struct wlr_pointer_motion_event *evt = data;

    wlr_cursor_move(srv->cursor, &evt->pointer->base,
                    evt->delta_x, evt->delta_y);

    /* Forward to shell for hover tracking. */
    if (srv->shell)
        scene_shell_handle_pointer(srv->shell,
                                   (int32_t)srv->cursor->x,
                                   (int32_t)srv->cursor->y, 0);

    scene_compositor_input_pointer(srv->scene_comp, 0,
                                   (int32_t)srv->cursor->x,
                                   (int32_t)srv->cursor->y, 0);
}

static void on_cursor_button(struct wl_listener *l, void *data)
{
    iso_server *srv = wl_container_of(l, srv, on_cursor_button);
    struct wlr_pointer_button_event *evt = data;

    uint8_t btns = (evt->state == WL_POINTER_BUTTON_STATE_PRESSED) ? 0x01 : 0;

    /* Forward to shell for click handling. */
    if (srv->shell) {
        scene_node_id hit = scene_shell_handle_pointer(
            srv->shell,
            (int32_t)srv->cursor->x,
            (int32_t)srv->cursor->y, btns);
        if (hit != 0 && btns) {
            scene_shell_handle_activate(srv->shell, hit);
        }
    }

    scene_compositor_input_pointer(srv->scene_comp, 0,
                                   (int32_t)srv->cursor->x,
                                   (int32_t)srv->cursor->y, btns);
}

static void on_keyboard_key(struct wl_listener *l, void *data)
{
    iso_server *srv = wl_container_of(l, srv, on_keyboard_key);
    struct wlr_keyboard_key_event *evt = data;

    struct wlr_keyboard *kb = srv->seat->keyboard_state.keyboard;
    uint8_t mod = 0;
    if (kb) {
        xkb_mod_mask_t mods = xkb_state_serialize_mods(
            kb->xkb_state, XKB_STATE_MODS_EFFECTIVE);
        xkb_mod_index_t idx;
        idx = xkb_map_mod_get_index(kb->keymap, XKB_MOD_NAME_SHIFT);
        if (mods & (1u << idx)) mod |= SCENE_MOD_SHIFT;
        idx = xkb_map_mod_get_index(kb->keymap, XKB_MOD_NAME_CTRL);
        if (mods & (1u << idx)) mod |= SCENE_MOD_CTRL;
        idx = xkb_map_mod_get_index(kb->keymap, XKB_MOD_NAME_ALT);
        if (mods & (1u << idx)) mod |= SCENE_MOD_ALT;
        idx = xkb_map_mod_get_index(kb->keymap, XKB_MOD_NAME_LOGO);
        if (mods & (1u << idx)) mod |= SCENE_MOD_SUPER;
    }

    /* xkb_keycode is evdev + 8. */
    uint32_t key_code = evt->keycode - 8;
    uint8_t state = (evt->state == WL_KEYBOARD_KEY_STATE_PRESSED) ? 1 : 0;

    /* Forward to shell for Alt+Tab, Escape, etc. */
    if (srv->shell && state) {
        if (scene_shell_handle_key(srv->shell, key_code, state, mod))
            return;
    }

    scene_compositor_input_key(srv->scene_comp, key_code, state, mod);
}

static void on_request_set_cursor(struct wl_listener *l, void *data)
{
    iso_server *srv = wl_container_of(l, srv, on_request_set_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *evt = data;
    (void)srv;
    wlr_cursor_set_surface(srv->cursor, evt->surface,
                           evt->seat_client, evt->serial);
}

/* ======================================================================
 * New-output / new-surface / new-input handlers
 * ====================================================================== */

static void on_new_output(struct wl_listener *l, void *data)
{
    iso_server *srv = wl_container_of(l, srv, on_new_output);
    struct wlr_output *output = data;

    wlr_output_layout_add(srv->output_layout, output, 0, 0);

    struct wlr_output_state state = { 0 };
    wlr_output_state_set_enabled(&state, true);
    struct wlr_output_mode *mode = wlr_output_get_preferred_mode(output);
    if (mode)
        wlr_output_state_set_mode(&state, mode);
    wlr_output_commit_state(output, &state);
    srv->output = output;

    /* Get output dimensions. */
    if (mode) {
        srv->width  = (uint32_t)mode->width;
        srv->height = (uint32_t)mode->height;
    } else {
        srv->width  = 1920;
        srv->height = 1080;
    }

    /* Resize scene engine to match output. */
    scene_compositor_resize(srv->scene_comp, srv->width, srv->height);

    /* Resize shell. */
    if (srv->shell) {
        scene_shell_resize(srv->shell, (int32_t)srv->width,
                           (int32_t)srv->height);
    } else {
        shell_resize(srv, srv->width, srv->height);
    }

    /* Attach the frame listener to this output. */
    srv->on_frame.notify = on_output_frame;
    wl_signal_add(&output->events.frame, &srv->on_frame);
}

static void on_new_xdg_surface(struct wl_listener *l, void *data)
{
    iso_server *srv = wl_container_of(l, srv, on_new_xdg_surface);
    struct wlr_xdg_surface *xdg = data;

    if (xdg->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) return;

    iso_surface_create(srv, xdg->surface);
}

static void on_new_input(struct wl_listener *l, void *data)
{
    iso_server *srv = wl_container_of(l, srv, on_new_input);
    struct wlr_input_device *dev = data;

    switch (dev->type) {
    case WLR_INPUT_DEVICE_POINTER:
        wlr_cursor_attach_input_device(srv->cursor, dev);
        wlr_cursor_set_xcursor(srv->cursor, srv->cursor_mgr, "default");
        break;
    case WLR_INPUT_DEVICE_KEYBOARD:
        wlr_seat_set_keyboard(srv->seat, dev);
        break;
    default:
        break;
    }

    wlr_seat_set_capabilities(srv->seat,
                              WL_SEAT_CAPABILITY_POINTER |
                              WL_SEAT_CAPABILITY_KEYBOARD);
}

/* ======================================================================
 * Create / destroy
 * ====================================================================== */

iso_server *iso_server_create(void)
{
    wlr_log(WLR_INFO, "iso-compositor: creating server");

    iso_server *srv = calloc(1, sizeof(*srv));
    if (!srv) return NULL;

    wl_list_init(&srv->surfaces);
    srv->next_node_id = ISO_NODE_BASE;

    /* ---- wlroots init ---- */

    srv->wl_display = wl_display_create();
    if (!srv->wl_display) goto fail;

    srv->backend = wlr_backend_autocreate(
        wl_display_get_event_loop(srv->wl_display), NULL);
    if (!srv->backend) goto fail;

    srv->renderer = wlr_renderer_autocreate(srv->backend);
    if (!srv->renderer) goto fail;
    wlr_renderer_init_wl_display(srv->renderer, srv->wl_display);

    srv->allocator = wlr_allocator_autocreate(srv->backend,
                                              srv->renderer);
    if (!srv->allocator) goto fail;

    srv->output_layout = wlr_output_layout_create(srv->wl_display);
    if (!srv->output_layout) goto fail;

    srv->wlr_compositor = wlr_compositor_create(
        srv->wl_display, 5, srv->renderer);
    if (!srv->wlr_compositor) goto fail;

    srv->xdg_shell = wlr_xdg_shell_create(srv->wl_display, 5);
    if (!srv->xdg_shell) goto fail;

    srv->seat = wlr_seat_create(srv->wl_display, "seat0");
    if (!srv->seat) goto fail;

    srv->cursor = wlr_cursor_create();
    if (!srv->cursor) goto fail;
    wlr_cursor_attach_output_layout(srv->cursor, srv->output_layout);

    srv->cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
    if (!srv->cursor_mgr) goto fail;

    /* ---- Scene engine init ---- */

    srv->scene_comp = scene_compositor_new(NULL, 1920, 1080);
    if (!srv->scene_comp) goto fail;

    /* ---- Loopback transport + shell client ---- */

    srv->loopback = scene_loopback_new();
    if (!srv->loopback) goto fail;

    srv->shell_ts  = scene_loopback_client_end(srv->loopback);
    srv->server_ts = scene_loopback_server_end(srv->loopback);
    if (!srv->shell_ts || !srv->server_ts) goto fail;

    srv->shell_client = scene_client_new();
    if (!srv->shell_client) goto fail;

    if (scene_client_connect(srv->shell_client, srv->shell_ts,
                             "iso-shell", &(scene_client_cbs){0}, srv) != 0)
        goto fail;

    /* Pump the WELCOME response. */
    server_pump(srv);

    /* ---- Create desktop shell nodes ---- */

    /* Use themed shell if available, otherwise fallback. */
    scene_shell_config_defaults(&srv->shell_cfg);
    (void)scene_shell_config_load(&srv->shell_cfg, "/etc/shell.conf");

    srv->shell = scene_shell_new(srv->shell_client,
                                 scene_compositor_store(srv->scene_comp),
                                 srv->scene_comp,
                                 &srv->shell_cfg);
    if (srv->shell) {
        /* Shell build creates its own nodes and calls apply_theme. */
        scene_shell_build(srv->shell, 1920, 1080);
        scene_compositor_set_effects(srv->scene_comp, 1);
    } else {
        /* Fallback: bare desktop + panel. */
        shell_create_desktop(srv);
        shell_create_panel(srv);

        scene_style bg_style = {
            .fill     = ISO_DESKTOP_COLOR,
            .border   = 0,
            .text     = 0,
            .border_w = 0,
            .radius   = 0
        };
        scene_compositor_set_style_count(srv->scene_comp, 1);
        scene_compositor_set_style(srv->scene_comp, 0, &bg_style);
    }

    /* ---- Listeners ---- */

    srv->on_new_output.notify    = on_new_output;
    wl_signal_add(&srv->backend->events.new_output, &srv->on_new_output);

    srv->on_new_xdg_surface.notify = on_new_xdg_surface;
    wl_signal_add(&srv->xdg_shell->events.new_surface,
                  &srv->on_new_xdg_surface);

    srv->on_new_input.notify    = on_new_input;
    wl_signal_add(&srv->backend->events.new_input, &srv->on_new_input);

    srv->on_cursor_motion.notify  = on_cursor_motion;
    wl_signal_add(&srv->cursor->events.motion, &srv->on_cursor_motion);

    srv->on_cursor_button.notify  = on_cursor_button;
    wl_signal_add(&srv->cursor->events.button, &srv->on_cursor_button);

    srv->on_keyboard_key.notify   = on_keyboard_key;
    wl_signal_add(&srv->seat->events.key, &srv->on_keyboard_key);

    srv->on_request_set_cursor.notify = on_request_set_cursor;
    wl_signal_add(&srv->seat->events.request_set_cursor,
                  &srv->on_request_set_cursor);

    wlr_log(WLR_INFO, "iso-compositor: server created");
    return srv;

fail:
    iso_server_destroy(srv);
    return NULL;
}

void iso_server_destroy(iso_server *srv)
{
    if (!srv) return;

    if (srv->shell) scene_shell_free(srv->shell);

    iso_surface *surf, *tmp;
    wl_list_for_each_safe(surf, tmp, &srv->surfaces, link) {
        scene_client_destroy_node(surf->server->shell_client, surf->node_id);
        free(surf);
    }

    if (srv->shell_client) scene_client_free(srv->shell_client);
    if (srv->shell_ts)     scene_transport_close(srv->shell_ts);
    if (srv->server_ts)    scene_transport_close(srv->server_ts);
    if (srv->loopback)     scene_loopback_free(srv->loopback);
    if (srv->scene_comp)   scene_compositor_free(srv->scene_comp);

    if (srv->cursor_mgr) wlr_xcursor_manager_destroy(srv->cursor_mgr);
    if (srv->cursor)     wlr_cursor_destroy(srv->cursor);
    if (srv->wl_display) wl_display_destroy(srv->wl_display);

    free(srv);
}

scene_store *iso_server_store(iso_server *srv)
{
    return srv ? scene_compositor_store(srv->scene_comp) : NULL;
}

scene_compositor *iso_server_scene_comp(iso_server *srv)
{
    return srv ? srv->scene_comp : NULL;
}

scene_shell *iso_server_shell(iso_server *srv)
{
    return srv ? srv->shell : NULL;
}

/* ======================================================================
 * Entry point (standalone compositor, not a library)
 * ====================================================================== */
#ifdef ISO_COMPOSITOR_STANDALONE
int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    wlr_log_init(WLR_INFO, NULL);

    iso_server *srv = iso_server_create();
    if (!srv) {
        wlr_log(WLR_ERROR, "failed to create iso-compositor");
        return 1;
    }

    wlr_log(WLR_INFO, "iso-compositor: starting backend");
    if (!wlr_backend_start(srv->backend)) {
        wlr_log(WLR_ERROR, "failed to start backend");
        iso_server_destroy(srv);
        return 1;
    }

    char *sock = wl_display_add_socket_auto(srv->wl_display);
    if (!sock) {
        wlr_log(WLR_ERROR, "failed to create Wayland socket");
        iso_server_destroy(srv);
        return 1;
    }
    wlr_log(WLR_INFO, "iso-compositor: WAYLAND_DISPLAY=%s", sock);
    setenv("WAYLAND_DISPLAY", sock, 1);

    wl_display_run(srv->wl_display);
    iso_server_destroy(srv);
    return 0;
}
#endif /* ISO_COMPOSITOR_STANDALONE */
