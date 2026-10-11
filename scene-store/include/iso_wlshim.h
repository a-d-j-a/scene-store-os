/* iso_wlshim.h - minimal Wayland compositor seam for one legacy client.
 *
 * The honest boundary at the OS seam: browsers arrive as composited
 * textures. NetSurf's framebuffer frontend (-f wld) speaks the LEGACY
 * wayland protocol (wl_compositor/wl_shell/wl_shm/wl_seat at version 1,
 * one reused wl_shm buffer, XRGB8888) - see libnsfb/src/surface/wld.c.
 * This module is the tiny server it connects to: it announces exactly
 * those four globals, services the handful of requests the client makes,
 * and on every wl_surface.commit hands the attached buffer's pixels
 * (copied at commit time into a one-frame staging buffer) to the
 * consumer. Input is injected host-side (see wlshim_input_* below): the
 * OS translates raw evdev into whatever key/pointer event the client
 * wants, and the shim frames it as the matching wayland event. The
 * sockets are POSIX (linux); the core (ingest/parse/state) is
 * platform-neutral and unit-tested on Windows.
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
 *   - wl_seat: the client binds it and calls get_pointer/get_keyboard
 *     (wld.c active handlers: pointer motion+button; keyboard is all
 *     #if 0 in stock wld - the shim still frames key events and the
 *     sender's patched keyboard_handle_key consumes them). The shim
 *     emits wl_seat.capabilities (1|2 = pointer|keyboard) on bind.
 *   - input events the shim can emit (wayland v1 wire):
 *     wl_pointer.motion (op 2: time, fixed x, fixed y)
 *     wl_pointer.button (op 3: serial, time, button, state)
 *     wl_keyboard.key   (op 3: serial, time, key, state)
 *     All but the wire must-haves; enter/leave/keymap/modifiers are NOT
 *     needed (client handlers for them are no-ops in the patched wld).
 *     wl_fixed = value << 8. Pointer surfaces track wld's only surface.
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

/* Input injection: the OS layer drives the client's pointer and
 * keyboard by framing events against the bound wl_pointer/wl_keyboard
 * objects. Coordinates are surface-relative (host translates absolute
 * scene coords -> window-relative before calling). All are no-ops if
 * the seat/pointer/keyboard isn't bound yet (or the shim is dead) -
 * injection never crashes a client that hasn't finished binding.
 * `button` is the Linux input code (BTN_LEFT=0x110, BTN_RIGHT=0x111,
 * BTN_MIDDLE=0x112); `state` is 0 released / 1 pressed; `key` is the
 * ASCII/NSFB keysym the patched wld.c keyboard handler forwards
 * verbatim. Serials and time are generated internally. */
void wlshim_input_pointer_move(wlshim *w, int32_t x, int32_t y);
void wlshim_input_pointer_button(wlshim *w, uint32_t button,
                                 uint32_t state);
void wlshim_input_keyboard_key(wlshim *w, uint32_t key, uint32_t state);

/* Non-zero after a protocol error (connection was/kill be killed). */
int wlshim_dead(const wlshim *w);

void wlshim_stop(wlshim *w);
void wlshim_free(wlshim *w);

#ifdef __cplusplus
}
#endif

#endif /* ISO_WLSHIM_H */