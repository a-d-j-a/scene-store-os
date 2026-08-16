/*
 * scene_compositor.h — compositor core: first consumer of the store.
 *
 * The compositor is the OS layer that draws the semantic scene. It owns
 * one session (a scene_server seam), reads the store directly for the
 * per-node visual state, diff-maps that state onto an internal render
 * model to compute damage, and repaints only the damaged rects into its
 * framebuffer. Backends (DRM/KMS, GDI, validation) copy the damage and
 * need no scene knowledge.
 *
 * Composition happens on scene_compositor_frame(): the store's committed
 * seq is diffed against the last rendered state; changed/deleted nodes
 * add their old+new rects to the damage list; each damage rect is then
 * repainted (clear to desktop color, then every intersecting visible
 * node in document order, children over parents). No frame is repainted
 * when nothing changed (damage = 0, backend skips the flush).
 *
 * Styles are server-owned (spec §7 re-theme): the compositor holds the
 * style table; a node with style_ref 0 uses its role's default style.
 * Changing a style entry dirties every node referencing it on the next
 * frame — the OS re-themes the running app without touching it.
 *
 * Effects (v1, compositor-side): scene_compositor_set_effects(1) enables
 * deterministic tick-driven transitions. A node that first appears in
 * the render model fades in over 8 compositor ticks while sliding from
 * 6px below its final position; a node removed from the store fades out
 * over 8 ticks (a phantom snapshot of its last visuals, drawn after the
 * live scene) before the model entry is dropped. Both share the per-
 * channel premul blend used everywhere (src*a + dst*(255-a))/255 and are
 * driven only by the internal tick counter — never wall-clock — so the
 * whole pipeline stays deterministic. Replay mode (seek) and ghost-
 * reconnect re-issues never animate: only genuinely new/deleted live
 * nodes do. With effects off, output is byte-identical to the v1
 * identity paint. Rounded corners are a style property: scene_style.radius
 * clips the fill corners to a circle (radius 0 = square).
 *
 * Input is forwarded to the store (scene_server_input_pointer): fully
 * flow-controlled (§8), semantics resolved by the engine (InputActivate
 * goes to the app, not the compositor). The compositor renders no
 * cursor in v1.
 *
 * Multi-session composition: the compositor merges the shell session
 * (layer 0, created by scene_compositor_new) with any number of foreign
 * app sessions added via scene_compositor_add_session. Each layer is one
 * scene_server+store pair with its own render model and transition
 * table; layers paint bottom-up into the shared framebuffer (app windows
 * over the desktop) and input is routed by hit-test across layers,
 * topmost first (scene_store_region_at on each app layer). Keyboard
 * input goes to the layer that last received a pointer event (click-to-
 * focus; the shell is the default). Layer 0 cannot be removed; its death
 * kills the compositor. An app layer's death freezes nothing: the layer
 * stops diffing and painting (its area repaints as desktop) until the
 * host removes or replaces it.
 *
 * Threading: one compositor, many sessions, one thread (as the store).
 */
#ifndef SCENE_COMPOSITOR_H
#define SCENE_COMPOSITOR_H

#include "scene_fb.h"
#include "scene_font.h"
#include "scene_server.h"

typedef struct scene_compositor scene_compositor;

typedef struct scene_style {
    uint32_t fill, border, text;  /* premultiplied ARGB; border 0 = none  */
    uint8_t  border_w;            /* px stroke width                      */
    uint8_t  pad_x, pad_y;        /* layout reserved, 0 in v1             */
    uint8_t  radius;              /* corner radius px; 0 = square         */
} scene_style;

scene_compositor *scene_compositor_new(const scene_limits *limits,
                                       uint32_t fb_w, uint32_t fb_h);
void scene_compositor_free(scene_compositor *cp);

/* The session store (read-only queries) and the wire seam (feed apps).
 * These are the shell session (layer 0).                               */
scene_store  *scene_compositor_store(scene_compositor *cp);
scene_server *scene_compositor_server(scene_compositor *cp);

/* ---- multi-session composition -------------------------------------- */
/* Attach a foreign session (an app's server) as a new layer above the
 * shell session. The compositor takes ownership of `sv` (freed on
 * remove_session/compositor_free). Returns the layer index (> 0) on
 * success, 0 on failure. The app's store content starts rendering on
 * the next frame (all pre-existing nodes damage in).                  */
int  scene_compositor_add_session(scene_compositor *cp, scene_server *sv);
/* Detach and free the layer hosting `sv`. Layer 0 cannot be removed.
 * Removal is by server identity, not by layer number: earlier removals
 * shift the layer array, so stale indices must never be replayed.
 * Returns the removed layer index, or -1 when no layer hosts `sv`.
 * The layer's area repaints as the desktop.                              */
int  scene_compositor_remove_session(scene_compositor *cp, scene_server *sv);
/* The store of a foreign session layer (layer > 0), or NULL when the
 * layer does not exist. The host uses this to run OS-side services on
 * app stores (e.g. the media importer registering texture refs).      */
scene_store *scene_compositor_layer_store(scene_compositor *cp, int layer);
/* The session server of a layer, or NULL. The host uses this to hook
 * per-session services (e.g. scene_server_set_import_cb on join).     */
scene_server *scene_compositor_layer_server(scene_compositor *cp,
                                            int layer);
/* Number of attached layers (>= 1; layer 0 = shell always present).      */
int  scene_compositor_layer_count(scene_compositor *cp);
/* 1 = keyboard focus is on the shell session (layer 0). The host uses
 * this to keep OS-level key grabs (shell hotkeys) away from apps.      */
int  scene_compositor_focus_is_shell(scene_compositor *cp);

/* OS-level key grabs: a grabbed (key_code, mods) chord routes to the
 * shell session (layer 0) regardless of keyboard focus. The table is a
 * small fixed array; grabs are registered once at shell build time, not
 * per gesture. Matching is exact on (key_code, modifiers).             */
#define SCENE_COMPOSITOR_KEY_GRABS 8u
int scene_compositor_key_grab(scene_compositor *cp, uint32_t key_code,
                              uint8_t mods);
int scene_compositor_ungrab(scene_compositor *cp, uint32_t key_code,
                            uint8_t mods);

/* Set the keyboard-focus layer index (0 = shell). scene_compositor_
 * input_key routes to it when no grab matches. This is how the search
 * overlay keeps typing after it opens (the pointer hit-test may have
 * left focus on an app layer). Returns 0 on success, -1 on an invalid
 * layer index. */
int  scene_compositor_set_focus_layer(scene_compositor *cp, int layer);

void scene_compositor_resize(scene_compositor *cp, uint32_t w, uint32_t h);
void scene_compositor_set_clear(scene_compositor *cp, uint32_t color);

/* Texture pixels: w*h of premultiplied ARGB or XRGB per fmt. Refs are
 * per-layer: each session's store owns its own ref space, so two app
 * sessions may both use ref 1 without colliding in the compositor
 * (the engine validates SET_TEXTURE against the session's own store).
 * The layer-0 variants below are the shell/wallpaper path.           */
int scene_compositor_register_texture(scene_compositor *cp,
                                      scene_texture_ref ref,
                                      uint32_t w, uint32_t h, uint16_t fmt,
                                      uint8_t opaque, const uint32_t *pixels);
int scene_compositor_release_texture(scene_compositor *cp,
                                     scene_texture_ref ref);
/* Layer-aware variants: register into the app session's layer (the
 * media importer path; layer comes from a scene_launcher session_added
 * callback). Registers the ref into the layer's store (validation)
 * and the layer's pixel registry. */
int scene_compositor_register_texture_layer(scene_compositor *cp, int layer,
                                            scene_texture_ref ref,
                                            uint32_t w, uint32_t h,
                                            uint16_t fmt, uint8_t opaque,
                                            const uint32_t *pixels);
int scene_compositor_release_texture_layer(scene_compositor *cp, int layer,
                                           scene_texture_ref ref);

/* Server-owned style table; ref must be < style_count (set it first).   */
void scene_compositor_set_style_count(scene_compositor *cp, uint32_t n);
int  scene_compositor_set_style(scene_compositor *cp, scene_style_ref ref,
                                const scene_style *st);

/* Forward to the store: flow-controlled, engine-resolved (§8, §7).      */
int scene_compositor_input_pointer(scene_compositor *cp, uint8_t device,
                                   int32_t x, int32_t y, uint8_t buttons);

/* Key input feeder (flow-controlled, shares gate with pointer).         */
int scene_compositor_input_key(scene_compositor *cp, uint32_t key_code,
                               uint8_t state, uint8_t modifiers);

/* One composition cycle. Returns 0 on success, -1 on internal failure.  */
int  scene_compositor_frame(scene_compositor *cp);
const scene_fb *scene_compositor_fb(scene_compositor *cp);
/* Damage of the last frame (count written into out, capped by cap).     */
uint32_t scene_compositor_damage(scene_compositor *cp, scene_rect *out,
                                 uint32_t cap);
/* Committed seq the framebuffer currently shows.                        */
uint64_t scene_compositor_rendered_seq(scene_compositor *cp);
/* Committed seq of one layer's store (0 = shell). Debug/diagnostics.   */
uint64_t scene_compositor_layer_seq(scene_compositor *cp, uint32_t layer);
void scene_compositor_force_repaint(scene_compositor *cp);

/* Effects (v1): enter/exit fade+slide transitions, tick-driven and fully
 * deterministic. on=1 enables (default off = identity paint).           */
void scene_compositor_set_effects(scene_compositor *cp, int on);
/* Deterministic frame counter (incremented by every frame()).           */
uint64_t scene_compositor_tick(scene_compositor *cp);
/* Number of running transitions (0 = fully settled).                    */
uint32_t scene_compositor_anim_count(scene_compositor *cp);

/* Hover style: allocates style slot 1 with the given fill/text colors.
 * Shell nodes set style_ref=1 when hovered, 0 when not. Returns the
 * style_ref (always 1) or 0 on failure.                                */
scene_style_ref scene_compositor_setup_hover_style(scene_compositor *cp,
                                                   uint32_t fill,
                                                   uint32_t text);

/* Active-focus style: allocates style slot 2 with the given fill/text
 * colors. Shell nodes set style_ref=2 when their window is focused.
 * Returns the style_ref (always 2) or 0 on failure.                    */
scene_style_ref scene_compositor_setup_active_style(scene_compositor *cp,
                                                    uint32_t fill,
                                                    uint32_t text);

#endif /* SCENE_COMPOSITOR_H */