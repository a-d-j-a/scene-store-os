/*
 * scene_app.h — native app client library.
 *
 * Provides a clean API for apps to connect to the scene store, create
 * windows with title bars and close buttons, handle input events, and
 * render their UI. Wraps the low-level scene_client wire API.
 *
 * Lifecycle:
 *   app = scene_app_new(transport, &cbs, ud)
 *   wid = scene_app_create_window(app, x, y, w, h, "title")
 *   loop { scene_app_pump(app); scene_app_flush(app); }
 *   scene_app_free(app)
 */
#ifndef SCENE_APP_H
#define SCENE_APP_H

#include "scene_fmt.h"
#include "scene_client.h"
#include "scene_transport.h"

typedef struct scene_app scene_app;

/* ---- input callbacks -------------------------------------------------- */

typedef struct scene_app_cbs {
    void (*pointer)(void *ud, uint64_t seq, int32_t x, int32_t y,
                    uint8_t buttons);
    void (*activate)(void *ud, uint64_t seq, scene_node_id id);
    void (*key)(void *ud, uint64_t seq, uint32_t key_code,
                uint8_t state, uint8_t modifiers);
    void (*focus)(void *ud, uint64_t seq, scene_node_id id, uint8_t state);
} scene_app_cbs;

/* ---- lifecycle -------------------------------------------------------- */

/* Create an app connected via the given transport. Takes ownership of t.
 * cbs/ud are called for input events; may be NULL. Opens the transport
 * on the loopback target ("local"); use scene_app_new_on for TCP.       */
scene_app *scene_app_new(scene_transport *t,
                         const scene_app_cbs *cbs, void *ud);

/* Like scene_app_new, but opens the transport on `target` explicitly
 * (TCP: "127.0.0.1:port"). */
scene_app *scene_app_new_on(scene_transport *t, const char *target,
                            const scene_app_cbs *cbs, void *ud);

/* Free the app and its internal state. Does NOT close the transport.     */
void scene_app_free(scene_app *app);

/* ---- window management ------------------------------------------------ */

/* Create a window: WINDOW node + TITLEBAR + TITLE_LABEL + CLOSE_BUTTON
 * + CONTENT area. Returns the CONTENT node ID (apps draw into this).
 * The window is fully visible and focusable.                             */
scene_node_id scene_app_create_window(scene_app *app,
                                      int32_t x, int32_t y,
                                      int32_t w, int32_t h,
                                      const char *title);

/* Destroy a window and all its child nodes.                              */
int scene_app_destroy_window(scene_app *app, scene_node_id content_id);

/* Get the WINDOW node ID from a content ID.                              */
scene_node_id scene_app_content_to_window(scene_app *app,
                                          scene_node_id content_id);

/* Update a window's title text (reaches the TITLE_LABEL child).          */
int scene_app_set_title(scene_app *app, scene_node_id content_id,
                        const char *title);

/* Resize a window: updates WINDOW, TITLEBAR, and CONTENT rects.          */
int scene_app_resize_window(scene_app *app, scene_node_id content_id,
                            int32_t w, int32_t h);

/* Minimize a window (hides it).                                          */
int scene_app_minimize(scene_app *app, scene_node_id content_id);

/* Maximize a window (fills given area, typically screen minus panel).     */
int scene_app_maximize(scene_app *app, scene_node_id content_id,
                       int32_t screen_w, int32_t screen_h, int32_t panel_h);

/* ---- node operations -------------------------------------------------- */

int scene_app_set_text(scene_app *app, scene_node_id id,
                       scene_text_id slot, const char *text);
int scene_app_set_rect(scene_app *app, scene_node_id id,
                       int32_t x, int32_t y, int32_t w, int32_t h);
int scene_app_set_flags(scene_app *app, scene_node_id id, uint8_t flags);

/* ---- frame flow ------------------------------------------------------- */

/* Signal frame complete (present).                                       */
int scene_app_present(scene_app *app);

/* Ack an input event to release flow control.                            */
int scene_app_ack(scene_app *app, uint64_t seq);

/* Pump inbound events (calls cbs).                                       */
int scene_app_pump(scene_app *app);

/* Flush outbound ops to the transport.                                   */
int scene_app_flush(scene_app *app);

/* ---- accessors -------------------------------------------------------- */

scene_client *scene_app_client(scene_app *app);
uint32_t     scene_app_id(const scene_app *app);

#endif /* SCENE_APP_H */
