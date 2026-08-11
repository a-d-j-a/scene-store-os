/*
 * scene_launcher.c — host service: spawn app processes as sessions.
 *
 * One session slot per spawned app. Lifecycle:
 *
 *   spawn:  bind a non-blocking TCP listener on an ephemeral port,
 *           start the app with SCENE_STORE_PORT set, keep the listener
 *           in the slot. The app connects back asynchronously.
 *   pump:   accept (non-blocking) -> scene_server_new + attach +
 *           scene_compositor_add_session -> session live.
 *           Live sessions: recv (non-blocking, would-block = drained),
 *           feed the server, drain outbound frames back to the app.
 *           Dead sessions (recv error/close or feed violation) are
 *           reaped: scene_compositor_remove_session (by server identity;
 *           transport close, slot compacted, session_exited callback.
 *   kill:   TerminateProcess (Windows) / SIGTERM (POSIX); the socket
 *           close on app death is what the pump observes.
 *
 * Pending spawns whose app never connects are dropped after the
 * configured timeout (wall clock).
 */
#define _POSIX_C_SOURCE 200809L
#include "scene_launcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#endif

#define MAX_SESSIONS 16
#define DEFAULT_SPAWN_TIMEOUT 10

typedef struct session {
    scene_transport       *peer;      /* live connection (NULL until joined) */
    scene_server          *sv;        /* owned by the compositor once added   */
    scene_tcp_listener    *listener;  /* pending spawn accept socket          */
    uintptr_t              hproc;     /* Windows process handle / POSIX pid   */
    uint32_t               pid;
    uint32_t               layer;
    time_t                 spawned_at;
    int                    dead;
} session;

struct scene_launcher {
    scene_compositor      *cp;
    const scene_limits    *limits;   /* NULL = engine defaults               */
    scene_launcher_cbs     cbs;
    void                  *ud;
    session                sessions[MAX_SESSIONS];
    int                    count;
    int                    spawn_timeout;
};

/* ==================================================================== */
/* Process spawn (platform)                                              */
/* ==================================================================== */

static int spawn_proc(const char *exe, const char *arg, uint16_t port,
                      uint32_t *out_pid, uintptr_t *out_hproc)
{
#if defined(_WIN32)
    char env[32];
    snprintf(env, sizeof(env), "%u", (unsigned)port);
    SetEnvironmentVariableA("SCENE_STORE_PORT", env);
    char cmd[1152];
    if (arg && arg[0])
        snprintf(cmd, sizeof(cmd), "\"%s\" %s", exe, arg);
    else
        snprintf(cmd, sizeof(cmd), "\"%s\"", exe);
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi))
        return -1;
    CloseHandle(pi.hThread);
    *out_pid = (uint32_t)pi.dwProcessId;
    *out_hproc = (uintptr_t)pi.hProcess;
    return 0;
#else
    char env[32];
    snprintf(env, sizeof(env), "%u", (unsigned)port);
    pid_t p = fork();
    if (p == 0) {
        setenv("SCENE_STORE_PORT", env, 1);
        if (arg && arg[0])
            execl(exe, exe, arg, (char *)NULL);
        else
            execl(exe, exe, (char *)NULL);
        _exit(127);               /* exec failed: the socket never opens */
    }
    if (p < 0) return -1;
    *out_pid = (uint32_t)p;
    *out_hproc = (uintptr_t)p;
    return 0;
#endif
}

static void kill_proc(uintptr_t hproc, uint32_t pid)
{
#if defined(_WIN32)
    (void)pid;
    TerminateProcess((HANDLE)hproc, 1);
    CloseHandle((HANDLE)hproc);
#else
    (void)hproc;
    kill((pid_t)pid, SIGTERM);
#endif
}

/* ==================================================================== */
/* Launcher                                                              */
/* ==================================================================== */

scene_launcher *scene_launcher_new(scene_compositor *cp,
                                   const scene_limits *limits,
                                   const scene_launcher_cbs *cbs, void *ud)
{
    if (!cp) return NULL;
    scene_launcher *sl = (scene_launcher *)calloc(1, sizeof(*sl));
    if (!sl) return NULL;
    sl->cp = cp;
    sl->limits = limits;          /* NULL = engine defaults, per scene_server_new */
    if (cbs) sl->cbs = *cbs;
    sl->ud = ud;
    sl->spawn_timeout = DEFAULT_SPAWN_TIMEOUT;
#if !defined(_WIN32)
    signal(SIGCHLD, SIG_IGN);     /* reap children, no zombies */
#endif
    return sl;
}

void scene_launcher_free(scene_launcher *sl)
{
    if (!sl) return;
    int i;
    for (i = 0; i < sl->count; i++) {
        session *s = &sl->sessions[i];
        if (s->listener) scene_tcp_listen_destroy(s->listener);
        if (s->peer) {
            scene_compositor_remove_session(sl->cp, s->sv);
            scene_transport_close(s->peer);
        }
    }
    free(sl);
}

void scene_launcher_spawn_timeout(scene_launcher *sl, int seconds)
{
    if (sl && seconds > 0) sl->spawn_timeout = seconds;
}

int scene_launcher_spawn(scene_launcher *sl, const char *exe,
                         const char *arg, uint32_t *out_pid)
{
    if (!sl || !exe || sl->count >= MAX_SESSIONS) return -1;
    uint16_t port = 0;
    scene_tcp_listener *l = scene_tcp_listen_new(0, &port);
    if (!l) return -1;
    uint32_t pid = 0;
    uintptr_t hproc = 0;
    if (spawn_proc(exe, arg, port, &pid, &hproc) != 0) {
        scene_tcp_listen_destroy(l);
        return -1;
    }
    session *s = &sl->sessions[sl->count++];
    memset(s, 0, sizeof(*s));
    s->listener = l;
    s->pid = pid;
    s->hproc = hproc;
    s->spawned_at = time(NULL);
    if (out_pid) *out_pid = pid;
    return 0;
}

int scene_launcher_kill(scene_launcher *sl, uint32_t pid)
{
    if (!sl) return -1;
    int i;
    for (i = 0; i < sl->count; i++) {
        session *s = &sl->sessions[i];
        if (s->pid == pid && !s->dead) {
            kill_proc(s->hproc, s->pid);
            return 0;
        }
    }
    return -1;
}

int scene_launcher_session_count(const scene_launcher *sl)
{
    return sl ? sl->count : 0;
}

int scene_launcher_layer_at(const scene_launcher *sl, int i)
{
    if (!sl || i < 0 || i >= sl->count) return 0;
    return (int)sl->sessions[i].layer;
}

uint32_t scene_launcher_pid_at(const scene_launcher *sl, int i)
{
    if (!sl || i < 0 || i >= sl->count) return 0;
    return sl->sessions[i].pid;
}

/* Join a pending spawn: accept its connection and wire the session. */
static void join_session(scene_launcher *sl, session *s)
{
    scene_transport *peer;
    while ((peer = scene_tcp_listen_accept(s->listener)) != NULL) {
        scene_tcp_set_nonblock(peer, 1);
        scene_server *sv = scene_server_new(sl->limits);
        if (!sv) { scene_transport_close(peer); break; }
        int layer = scene_compositor_add_session(sl->cp, sv);
        if (layer <= 0) {
            scene_server_free(sv);
            scene_transport_close(peer);
            break;
        }
        scene_server_attach(sv);
        scene_tcp_listener *l = s->listener;
        s->listener = NULL;
        scene_tcp_listen_destroy(l);
        s->peer = peer;
        s->sv = sv;
        s->layer = (uint32_t)layer;
        if (sl->cbs.session_added)
            sl->cbs.session_added(sl->ud, layer, s->pid);
    }
}

void scene_launcher_pump(scene_launcher *sl)
{
    if (!sl) return;
    time_t now = time(NULL);
    uint8_t buf[4096];
    int i;

    /* 1. pending spawns: accept, or drop when the app never connects */
    for (i = 0; i < sl->count; i++) {
        session *s = &sl->sessions[i];
        if (!s->listener || s->dead) continue;
        if (now - s->spawned_at >= sl->spawn_timeout) {
            scene_tcp_listen_destroy(s->listener);
            s->listener = NULL;
            s->dead = 1;          /* reaped by the compaction below */
            continue;
        }
        join_session(sl, s);
    }

    /* 2. live sessions: feed inbound, drain outbound */
    for (i = 0; i < sl->count; i++) {
        session *s = &sl->sessions[i];
        if (!s->peer || s->dead) continue;
        for (;;) {
            uint32_t got = 0;
            int r = scene_transport_recv(s->peer, buf, sizeof(buf), &got);
            if (r == 0) {
                if (scene_server_feed(s->sv, buf, got) != 0) {
                    s->dead = 1;  /* protocol violation: ERROR already sent */
                    break;
                }
                continue;
            }
            if (r == 1) break;    /* would-block: drained */
            s->dead = 1;          /* connection closed/error */
            break;
        }
        if (s->dead) continue;
        const uint8_t *frame;
        uint32_t flen;
        while (scene_server_out_next_frame(s->sv, &frame, &flen) == 1)
            scene_transport_send(s->peer, frame, flen);
    }

    /* 3. reap dead sessions (compact the slot array) */
    i = 0;
    while (i < sl->count) {
        session *s = &sl->sessions[i];
        if (!s->dead) { i++; continue; }
        uint32_t pid = s->pid;
        int joined = s->peer != NULL;
        int layer = -1;
        if (joined) {
            layer = scene_compositor_remove_session(sl->cp, s->sv);
            scene_transport_close(s->peer);   /* frees the transport */
            s->peer = NULL;
            s->sv = NULL;
        }
        memmove(&sl->sessions[i], &sl->sessions[i + 1],
                (size_t)(sl->count - i - 1) * sizeof(session));
        sl->count--;
        if (joined && sl->cbs.session_exited)
            sl->cbs.session_exited(sl->ud, layer, pid);
    }
}
