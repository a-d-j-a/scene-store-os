/*
 * scene_transport.c — loopback and TCP transports.
 *
 * Loopback: FIFO pairs, single-threaded, no locking. The FIFO is a
 * growable byte queue; recv on an empty queue reports would-block.
 *
 * TCP: blocking sockets. WinSock on Windows (the w64devkit toolchain),
 * POSIX sockets elsewhere (written to the same contract; this project's
 * build targets w64devkit, the POSIX path is kept for portability but is
 * not exercised by this toolchain).
 */
#define _POSIX_C_SOURCE 200809L
#include "scene_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#endif

/* ==================================================================== */
/* Byte FIFO                                                             */
/* ==================================================================== */

typedef struct fifo {
    uint8_t *data;
    uint32_t len, cap, off;
} fifo;

static int fifo_push(fifo *f, const uint8_t *d, uint32_t n)
{
    if (f->off && (f->off > 4096 || f->off == f->len)) {
        memmove(f->data, f->data + f->off, f->len - f->off);
        f->len -= f->off;
        f->off = 0;
    }
    if (f->cap - f->len < n) {
        uint32_t nc = f->cap ? f->cap : 256;
        while (nc - f->len < n) nc <<= 1;
        uint8_t *nd = (uint8_t *)realloc(f->data, nc);
        if (!nd) return -1;
        f->data = nd;
        f->cap = nc;
    }
    memcpy(f->data + f->len, d, n);
    f->len += n;
    return 0;
}

/* 0 = bytes copied, 1 = empty (would-block). */
static int fifo_pop(fifo *f, uint8_t *d, uint32_t cap, uint32_t *got)
{
    uint32_t avail = f->len - f->off;
    if (avail == 0) { f->off = f->len = 0; return 1; }
    uint32_t n = avail < cap ? avail : cap;
    memcpy(d, f->data + f->off, n);
    f->off += n;
    if (f->off == f->len) f->off = f->len = 0;
    *got = n;
    return 0;
}

static void fifo_free(fifo *f)
{
    free(f->data);
    f->data = NULL; f->len = f->cap = f->off = 0;
}

/* ==================================================================== */
/* Loopback                                                              */
/* ==================================================================== */

typedef struct loop_transport {
    scene_transport base;
    fifo *rx;                 /* queue we read from                       */
    fifo *tx;                 /* queue we write to                        */
} loop_transport;

static int loop_open(scene_transport *t, const char *target)
{
    (void)t; (void)target;
    return 0;
}

static void loop_close(scene_transport *t)
{
    free(t);                  /* rx/tx belong to the link, not the end    */
}

static int loop_send(scene_transport *t, const uint8_t *data, uint32_t len)
{
    return fifo_push(((loop_transport *)t)->tx, data, len);
}

static int loop_recv(scene_transport *t, uint8_t *buf, uint32_t cap,
                     uint32_t *got)
{
    return fifo_pop(((loop_transport *)t)->rx, buf, cap, got);
}

static const scene_transport_ops loop_ops = {
    loop_open, loop_close, loop_send, loop_recv
};

struct scene_loopback {
    fifo a2b, b2a;
};

scene_loopback *scene_loopback_new(void)
{
    return (scene_loopback *)calloc(1, sizeof(scene_loopback));
}

void scene_loopback_free(scene_loopback *lb)
{
    if (!lb) return;
    fifo_free(&lb->a2b);
    fifo_free(&lb->b2a);
    free(lb);
}

/* Client end: reads b2a, writes a2b. Server end: reads a2b, writes b2a. */
static scene_transport *loop_end(fifo *rx, fifo *tx)
{
    loop_transport *lt = (loop_transport *)calloc(1, sizeof(*lt));
    if (!lt) return NULL;
    lt->base.ops = &loop_ops;
    lt->rx = rx;
    lt->tx = tx;
    return (scene_transport *)lt;
}

scene_transport *scene_loopback_client_end(scene_loopback *lb)
{
    return loop_end(&lb->b2a, &lb->a2b);
}

scene_transport *scene_loopback_server_end(scene_loopback *lb)
{
    return loop_end(&lb->a2b, &lb->b2a);
}

/* ==================================================================== */
/* TCP                                                                   */
/* ==================================================================== */

#if defined(_WIN32)
typedef SOCKET sc_sock;
#define SC_SOCK_INVALID INVALID_SOCKET
static int sc_sock_close(sc_sock s) { return closesocket(s) == 0 ? 0 : -1; }
#else
typedef int sc_sock;
#define SC_SOCK_INVALID (-1)
static int sc_sock_close(sc_sock s) { return close(s) == 0 ? 0 : -1; }
#endif

typedef struct tcp_transport {
    scene_transport base;
    sc_sock s;
} tcp_transport;

static int ws_ready;          /* Winsock initialized (Windows)            */

static int sc_net_init(void)
{
#if defined(_WIN32)
    if (ws_ready) return 0;
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return -1;
    ws_ready = 1;
#endif
    return 0;
}

static int tcp_open(scene_transport *t, const char *target)
{
    tcp_transport *tc = (tcp_transport *)t;
    tc->s = SC_SOCK_INVALID;
    const char *colon = target ? strrchr(target, ':') : NULL;
    if (!colon) return -1;
    char host[256];
    size_t hlen = (size_t)(colon - target);
    if (hlen >= sizeof(host)) return -1;
    memcpy(host, target, hlen);
    host[hlen] = '\0';
    if (colon[1] == '\0') return -1;
    if (sc_net_init() != 0) return -1;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, colon + 1, &hints, &res) != 0) return -1;
    sc_sock s = SC_SOCK_INVALID;
    struct addrinfo *ai;
    for (ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == SC_SOCK_INVALID) continue;
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        sc_sock_close(s);
        s = SC_SOCK_INVALID;
    }
    freeaddrinfo(res);
    if (s == SC_SOCK_INVALID) return -1;
    tc->s = s;
    return 0;
}

static void tcp_close(scene_transport *t)
{
    tcp_transport *tc = (tcp_transport *)t;
    if (tc->s != SC_SOCK_INVALID) {
        sc_sock_close(tc->s);
        tc->s = SC_SOCK_INVALID;
    }
    free(tc);
}

static int tcp_send(scene_transport *t, const uint8_t *data, uint32_t len)
{
    tcp_transport *tc = (tcp_transport *)t;
    if (tc->s == SC_SOCK_INVALID) return -1;
    uint32_t off = 0;
    while (off < len) {
#if defined(_WIN32)
        int n = send(tc->s, (const char *)data + off, (int)(len - off), 0);
#else
        ssize_t n = send(tc->s, data + off, len - off, 0);
#endif
        if (n <= 0) return -1;
        off += (uint32_t)n;
    }
    return 0;
}

static int tcp_recv(scene_transport *t, uint8_t *buf, uint32_t cap,
                    uint32_t *got)
{
    tcp_transport *tc = (tcp_transport *)t;
    if (tc->s == SC_SOCK_INVALID) return -1;
#if defined(_WIN32)
    int n = recv(tc->s, (char *)buf, (int)cap, 0);
    if (n > 0) { *got = (uint32_t)n; return 0; }
    if (n == 0) return -1;        /* peer closed                            */
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK || err == WSAEINTR) return 1;  /* would-block */
    return -1;
#else
    ssize_t n = recv(tc->s, buf, cap, 0);
    if (n > 0) { *got = (uint32_t)n; return 0; }
    if (n == 0) return -1;        /* peer closed                            */
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return 1;                 /* would-block                            */
    return -1;
#endif
}

static const scene_transport_ops tcp_ops = {
    tcp_open, tcp_close, tcp_send, tcp_recv
};

int scene_tcp_set_nonblock(scene_transport *t, int nb)
{
    if (!t || t->ops != &tcp_ops) return -1;
    tcp_transport *tc = (tcp_transport *)t;
    if (tc->s == SC_SOCK_INVALID) return -1;
#if defined(_WIN32)
    u_long mode = nb ? 1 : 0;
    return ioctlsocket(tc->s, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int fl = fcntl(tc->s, F_GETFL, 0);
    if (fl < 0) return -1;
    fl = nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
    return fcntl(tc->s, F_SETFL, fl) == 0 ? 0 : -1;
#endif
}

scene_transport *scene_tcp_client(const char *target)
{
    tcp_transport *tc = (tcp_transport *)calloc(1, sizeof(*tc));
    if (!tc) return NULL;
    tc->base.ops = &tcp_ops;
    tc->s = SC_SOCK_INVALID;
    (void)target;                 /* resolved in open()                    */
    return (scene_transport *)tc;
}

/* ---- listener --------------------------------------------------------- */

static sc_sock g_listen = SC_SOCK_INVALID;

int scene_tcp_listen(uint16_t port, uint16_t *out_port,
                     scene_tcp_accept_fn cb, void *ud)
{
    if (sc_net_init() != 0) return -1;
    if (g_listen != SC_SOCK_INVALID) return -1;   /* one listener at a time */
    struct addrinfo hints, *res = NULL;
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(NULL, portstr, &hints, &res) != 0) return -1;
    sc_sock ls = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (ls == SC_SOCK_INVALID) { freeaddrinfo(res); return -1; }
    int one = 1;
#if defined(_WIN32)
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
#else
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
    if (bind(ls, res->ai_addr, (int)res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        sc_sock_close(ls);
        return -1;
    }
    struct sockaddr_in sa;
    socklen_t salen = sizeof(sa);
    if (getsockname(ls, (struct sockaddr *)&sa, &salen) == 0 && out_port)
        *out_port = ntohs(sa.sin_port);
    freeaddrinfo(res);
    if (listen(ls, 4) != 0) { sc_sock_close(ls); return -1; }
    g_listen = ls;
    for (;;) {
        sc_sock cs = accept(ls, NULL, NULL);
        if (cs == SC_SOCK_INVALID) break;   /* listen socket closed         */
        tcp_transport *peer = (tcp_transport *)calloc(1, sizeof(*peer));
        if (!peer) { sc_sock_close(cs); continue; }
        peer->base.ops = &tcp_ops;
        peer->s = cs;
        int keep = cb(ud, (scene_transport *)peer);
        if (keep != 0) break;
    }
    sc_sock_close(ls);
    g_listen = SC_SOCK_INVALID;
    return 0;
}

void scene_tcp_listen_close(void)
{
    if (g_listen != SC_SOCK_INVALID) {
        sc_sock ls = g_listen;
        g_listen = SC_SOCK_INVALID;
        sc_sock_close(ls);        /* unblocks accept on Windows            */
    }
}

/* ---- non-blocking listener --------------------------------------------- */

struct scene_tcp_listener {
    sc_sock s;
};

scene_tcp_listener *scene_tcp_listen_new(uint16_t port, uint16_t *out_port)
{
    if (sc_net_init() != 0) return NULL;
    struct addrinfo hints, *res = NULL;
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(NULL, portstr, &hints, &res) != 0) return NULL;
    sc_sock ls = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (ls == SC_SOCK_INVALID) { freeaddrinfo(res); return NULL; }
    int one = 1;
#if defined(_WIN32)
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
#else
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
    if (bind(ls, res->ai_addr, (int)res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        sc_sock_close(ls);
        return NULL;
    }
    struct sockaddr_in sa;
    socklen_t salen = sizeof(sa);
    if (getsockname(ls, (struct sockaddr *)&sa, &salen) == 0 && out_port)
        *out_port = ntohs(sa.sin_port);
    freeaddrinfo(res);
    if (listen(ls, 4) != 0) { sc_sock_close(ls); return NULL; }
#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(ls, FIONBIO, &mode);
#else
    int fl = fcntl(ls, F_GETFL, 0);
    if (fl >= 0) fcntl(ls, F_SETFL, fl | O_NONBLOCK);
#endif
    scene_tcp_listener *l = (scene_tcp_listener *)calloc(1, sizeof(*l));
    if (!l) { sc_sock_close(ls); return NULL; }
    l->s = ls;
    return l;
}

scene_transport *scene_tcp_listen_accept(scene_tcp_listener *l)
{
    if (!l || l->s == SC_SOCK_INVALID) return NULL;
    sc_sock cs = accept(l->s, NULL, NULL);
    if (cs == SC_SOCK_INVALID) return NULL;   /* none pending (non-blocking) */
    tcp_transport *peer = (tcp_transport *)calloc(1, sizeof(*peer));
    if (!peer) { sc_sock_close(cs); return NULL; }
    peer->base.ops = &tcp_ops;
    peer->s = cs;
    return (scene_transport *)peer;
}

void scene_tcp_listen_destroy(scene_tcp_listener *l)
{
    if (!l) return;
    if (l->s != SC_SOCK_INVALID) sc_sock_close(l->s);
    free(l);
}
