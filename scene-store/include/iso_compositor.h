/* iso_compositor.h — custom wlroots compositor integrating the scene engine.
 *
 * This compositor owns the bridge between Wayland clients and the semantic
 * scene store.  wlroots handles protocol + output + input; the scene engine
 * owns meaning (nodes, roles, text, geometry, focus).  Client surfaces are
 * converted to scene-store ops; the scene compositor paints to a framebuffer;
 * the framebuffer is presented to the wlroots output.
 *
 * This header is Linux-only (wlroots dependency).                              */
#ifndef ISO_COMPOSITOR_H
#define ISO_COMPOSITOR_H

#include "scene_store.h"
#include "scene_compositor.h"
#include "scene_shell.h"

typedef struct iso_server iso_server;

/* Create and start the compositor.  Returns NULL on failure.
 * The backend runs until the display is disconnected.                       */
iso_server *iso_server_create(void);

/* Shut down the compositor, free all resources.                             */
void iso_server_destroy(iso_server *srv);

/* Access the underlying scene store (for tests / tooling).                  */
scene_store     *iso_server_store(iso_server *srv);
scene_compositor *iso_server_scene_comp(iso_server *srv);
scene_shell     *iso_server_shell(iso_server *srv);

#endif /* ISO_COMPOSITOR_H */
