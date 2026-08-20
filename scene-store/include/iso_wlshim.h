/* iso_wlshim.h - minimal Wayland compositor seam for one legacy client.
 *
 * The honest boundary at the OS seam: browsers arrive as composited
 * textures. NetSurf's framebuffer frontend (-f wld) speaks the LEGACY
 * wayland protocol (wl_compositor/wl_shell/wl_shm at version 1, one
 * reused wl_shm buffer, XRGB8888) - see libnsfb/src/surface/wld.c. This
 * module is the tiny server it connects to: it announces exactly those
 * three globals (no seat, no output), services the handful of requests the
 * client makes, and on every wl_surface.commit hands the attached
 * buffer's pixels (copied at commit time into a one-frame staging
 * buffer) to the consumer. Nothing else is implemented because nothing
 * else arrives: no xdg, no seat, no output, no regions, no frame
 * callbacks. The sockets are POSIX (linux); the core (ingest/parse/
 * state) is platform-neutral and unit-tested on Windows.
 *
 * Wire facts the implementation rests on (verified against
 * libnsfb b701cdc src/surface/wld.c + wayland.xml):
 *   - connection: wl_display_connect(NULL) -> XDG_RUNTIME_DIR/WAYLAND_DISPLAY
 *   - globals bound at version 1: wl_compositor, wl_shell, wl_shm
 *     (versions announced by the server are ignored - UNUSED(version))
 *   - wl_shell (legacy): get_shell_surface + set_toplevel + set_title;
 *     ping/configure events are never required (handlers UNUSED)
 *   - one wl_shm pool, XRGB8888, stride = width*4; one buffer; the
 *     client re-attaches it forever and never waits for release
 *   - a wl_display.sync roundtrip blocks after EVERY commit - the shim
 *     must answer every sync with wl_callback.done or the client hangs
 *   - wl_shm.format (XRGB8888 = 1) must be announced or the client
 *     refuses to start ("WL_SHM_FORMAT_XRGB8888 not available")
 *   - first commit happens at init, before any page content
 */
#ifndef ISO_WLSHIM_H
#define ISO_WLSHIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wlshim wlshim;

/* A committed frame: one staging buffer, XRGB8888 little-endian
 * (u32 = 0xFFRRGGBB), stride == width*4, valid until the next frame. */
typedef struct wlshim_frame {
    uint32_t        w, h;
    const uint32_t *px;      /* width*height pixels (staging) */
} wlshim_frame;

typedef void (*wlshim_frame_fn)(void *ud, const wlshim_frame *f);
typedef void (*wlshim_died_fn)(void *ud);   /* client socket closed */

/* Platforms hook pool backing stores: given a transferred fd index and
 * the pool size, return a pointer to the mapping (POSIX: dup+mmap;
 * tests: a fake table). NULL = no map for this pool. */
typedef void *(*wlshim_map_fn)(void *ud, int fd_index, uint32_t size);

/* Start the shim on XDG_RUNTIME_DIR/<display> (display default
 * wayland-0). On non-POSIX builds the listener is not created (lfd
 * stays -1) and the core state is still usable through wlshim_ingest. */
wlshim *wlshim_new(const char *xdg_runtime, const char *display,
                   wlshim_frame_fn on_frame, wlshim_died_fn on_died,
                   void *ud);

void wlshim_set_mapfn(wlshim *w, wlshim_map_fn fn, void *ud);

/* Service the client socket (POSIX only): recv, ingest, flush outbound.
 * Call from the main loop each frame. 0 = alive, -1 = EOF/error. */
int wlshim_pump(wlshim *w);

/* Core ingest: feed raw client bytes (fds ride in the `fds` array, one
 * per fd arg in the message). Returns bytes consumed; the remainder
 * must be fed again later (message framing). Platform-neutral. */
size_t wlshim_ingest(wlshim *w, const uint8_t *bytes, size_t len,
                     const int *fds, size_t nfd);

/* Drain pending server->client events (registry globals, shm formats,
 * callback done, buffer release, delete_id). Returns bytes written. */
size_t wlshim_out_drain(wlshim *w, uint8_t *out, size_t cap);

/* The last committed frame (NULL if none yet). */
const wlshim_frame *wlshim_last_frame(const wlshim *w);

/* Non-zero after a protocol error (connection was/kill be killed). */
int wlshim_dead(const wlshim *w);

void wlshim_stop(wlshim *w);
void wlshim_free(wlshim *w);

#ifdef __cplusplus
}
#endif

#endif /* ISO_WLSHIM_H */