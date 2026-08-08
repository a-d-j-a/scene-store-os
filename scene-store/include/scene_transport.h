/*
 * scene_transport.h — byte transports for the scene-store wire protocol.
 *
 * The client and the server adapter are transport-agnostic: a transport
 * only moves raw bytes. Two implementations ship:
 *
 *   - loopback: in-process, single-threaded FIFO pair. Used by the
 *     integration tests and, later, by the compositor seam (one thread
 *     drives both ends of the link).
 *   - TCP: blocking sockets (WinSock on Windows, POSIX elsewhere).
 *     Blocking by design for the reference stack; a non-blocking or
 *     epoll/kqueue driver is a later transport, not this one.
 *
 * Transport contract:
 *   open(target)           0 = ready; -1 = failed (transport then unusable)
 *   send(data, len)        0 = accepted all bytes, -1 = error/closed
 *   recv(buf, cap, *got)   0 = ok (got maybe 0), 1 = would-block,
 *                          -1 = closed/error
 *   close()                 releases the transport; the object must not
 *                          be used after close. On error the transport
 *                          is already closed; call open() again to retry.
 *
 * Calling close() on a NULL pointer is allowed. A transport created by
 * the loopback pair or by scene_tcp_client / the accept callback is
 * heap-owned by the caller and freed by close().
 */
#ifndef SCENE_TRANSPORT_H
#define SCENE_TRANSPORT_H

#include <stdint.h>

typedef struct scene_transport scene_transport;

typedef struct scene_transport_ops {
    int  (*open)(scene_transport *t, const char *target);
    void (*close)(scene_transport *t);
    int  (*send)(scene_transport *t, const uint8_t *data, uint32_t len);
    int  (*recv)(scene_transport *t, uint8_t *buf, uint32_t cap,
                 uint32_t *got);
} scene_transport_ops;

struct scene_transport {
    const scene_transport_ops *ops;
};

/* Inline wrappers (no vtable indirection at the call sites that matter). */
static inline int scene_transport_open(scene_transport *t, const char *target)
{ return t->ops->open(t, target); }

static inline void scene_transport_close(scene_transport *t)
{ if (t) t->ops->close(t); }

static inline int scene_transport_send(scene_transport *t,
                                       const uint8_t *data, uint32_t len)
{ return t->ops->send(t, data, len); }

static inline int scene_transport_recv(scene_transport *t,
                                       uint8_t *buf, uint32_t cap,
                                       uint32_t *got)
{ return t->ops->recv(t, buf, cap, got); }

/* ---- loopback ---------------------------------------------------------- */

typedef struct scene_loopback scene_loopback;

/* In-process link between a client and a server. Single-threaded by
 * design: both ends are driven from the same thread (tests, and the
 * compositor's scene loop later). Free the link only after both the
 * client and the server transports have been closed. */
scene_loopback  *scene_loopback_new(void);
void             scene_loopback_free(scene_loopback *lb);

/* New transport bound to the client end of the link. Each call returns
 * a fresh heap transport; the caller owns it and close() frees it. */
scene_transport *scene_loopback_client_end(scene_loopback *lb);
scene_transport *scene_loopback_server_end(scene_loopback *lb);

/* ---- TCP ---------------------------------------------------------------- */

/* "host:port" target. Returns a heap transport owned by the caller;
 * open(target) resolves and connects (blocking). */
scene_transport *scene_tcp_client(const char *target);

/* Accept callback: owns `peer` (must close it). Return 0 to keep the
 * listen socket accepting, nonzero to stop accepting. The callback runs
 * on the thread that called scene_tcp_listen(). */
typedef int (*scene_tcp_accept_fn)(void *ud, scene_transport *peer);

/* Bind on `port` (0 = ephemeral; actual port written to *out_port before
 * any accept), then accept in a loop on this thread until
 * scene_tcp_listen_close(). Returns 0 when the loop ends cleanly. At
 * most one listener may be active at a time. */
int  scene_tcp_listen(uint16_t port, uint16_t *out_port,
                      scene_tcp_accept_fn cb, void *ud);
/* Unblock a running scene_tcp_listen loop from another thread. */
void scene_tcp_listen_close(void);

#endif /* SCENE_TRANSPORT_H */