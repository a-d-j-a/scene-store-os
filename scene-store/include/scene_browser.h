/*
 * scene_browser.h — host module bridging a legacy Wayland client into
 * the compositor at the honest boundary.
 *
 * A browser renders its own pixels. The OS's job is to show them as a
 * window — a composited texture — and to translate host input into
 * events the browser understands, nothing more. This module is that
 * seam: it owns one wlshim (the tiny legacy-wayland compositor that a
 * NetSurf `-f wld` client connects to), one scene session (a
 * scene_server on a loopback transport), and one scene_compositor
 * layer. Each committed wayland frame becomes a registered texture on
 * the browser's layer and the per-session window nodes get sized to it;
 * web content therefore arrives exactly like the media importer's
 * output — pixels at the OS seam, effects applied, semantics never
 * crossing (browser/video/WebGL composition is the documented honest
 * boundary of this project).
 *
 * Per-session identifiers (session-private by design, like any app's
 * node namespace):
 *   node 60000  — SCENE_ROLE_WINDOW   (the window chrome + geometry)
 *   node 60001  — SCENE_ROLE_GENERIC  (transparent fill; the texture)
 *   tex ref 1   — the composited browser frame (XRGB)
 * A host on POSIX must install the wlshim pool mapping so committed
 * buffers are readable:
 *   wlshim_set_mapfn(scene_browser_shim(b), fn, ud);
 * (On Windows no POSIX socket exists, so no real mapping is installed;
 * the shim core still ingests fake-wire bytes, which the tests use.)
 *
 * Lifecycle: scene_browser_new attaches the session; the host calls
 * scene_browser_pump once per frame (it services the wayland socket,
 * pulls committed frames, and drives the loopback link). A dead browser
 * returns -1 from pump and is freed with scene_browser_free. The host
 * must never free the wlshim it passed in (that object belongs to the
 * host's session).
 */
#ifndef SCENE_BROWSER_H
#define SCENE_BROWSER_H

#include "scene_fmt.h"
#include "iso_wlshim.h"

typedef struct scene_compositor scene_compositor;
typedef struct scene_server     scene_server;

/* Placement + initial geometry of the browser window, and the wayland
 * side the client connects to (XDG_RUNTIME_DIR/<display>). x/y are
 * absolute scene coordinates; init_w/init_h are the window size before
 * the first committed frame arrives (the browser sets its own size). */
typedef struct scene_browser_config {
    int32_t      x, y;
    uint32_t     init_w, init_h;
    const char  *title;      /* window title, UTF-8 */
    const char  *xdg_runtime; /* may be NULL (default) */
    const char  *display;     /* wayland-0 default when NULL */
} scene_browser_config;

typedef struct scene_browser scene_browser;

/* Create the browser session: a wlshim listener (POSIX), a scene_server
 * attached as a new compositor layer, and a loopback client running that
 * session's store. Returns NULL on allocation failure. */
scene_browser *scene_browser_new(scene_compositor *cp,
                                 const scene_browser_config *cfg);

/* Service one frame slice. Returns 0 while alive, -1 once dead. A dead
 * browser has been removed from the compositor (its area repaints as
 * desktop) and must be freed, not reused. */
int  scene_browser_pump(scene_browser *b);
int  scene_browser_dead(const scene_browser *b);

/* Host handles: the compositor layer index, the session server (host
 * services like import hooks), and the wlshim (mapfn/input injection). */
int              scene_browser_layer(const scene_browser *b);
scene_server    *scene_browser_server(const scene_browser *b);
wlshim          *scene_browser_shim(const scene_browser *b);

/* Current window rect: x/y from the config, w/h = last committed frame
 * size (or the init size before the first frame). */
void scene_browser_window_rect(const scene_browser *b, scene_rect *r);

/* Detach the layer, close the loopback, free the client and the module.
 * Never frees the wlshim. Safe to call on a dead browser. */
void scene_browser_free(scene_browser *b);

#endif /* SCENE_BROWSER_H */