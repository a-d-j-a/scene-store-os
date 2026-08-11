/*
 * scene_launcher.h — host service: spawn app processes as sessions.
 *
 * The launcher is the OS-side app factory. It starts an app executable,
 * listens for its TCP connection back, and wires the connection into
 * the compositor as a session layer (scene_compositor_add_session).
 * scene_launcher_pump() — called from the compositor loop — accepts
 * pending connections, feeds transport bytes into the per-app
 * scene_server, drains outbound frames back to the app, and reaps
 * sessions whose connection closed (app exited/killed) or whose session
 * died from a protocol violation. A reaped session is detached from the
 * compositor (layer area repaints as desktop) and freed.
 *
 * Spawn contract: the app is started with the environment variable
 * SCENE_STORE_PORT set to the port it must connect to on 127.0.0.1.
 * Apps link scene_app/scene_client over scene_tcp_client. A spawn whose
 * app never connects is dropped after scene_launcher_spawn_timeout()
 * seconds (default 10).
 *
 * The accepted sockets are non-blocking, so pump never blocks: it
 * processes what is available and returns. One thread drives launcher +
 * compositor (the same rule as the store). Death of an app does not
 * touch the shell session; death of the shell kills the compositor
 * (scene_compositor_add_session semantics).
 */
#ifndef SCENE_LAUNCHER_H
#define SCENE_LAUNCHER_H

#include "scene_compositor.h"
#include "scene_transport.h"

typedef struct scene_launcher scene_launcher;

typedef struct scene_launcher_cbs {
    /* App connected and composited. layer >= 1. */
    void (*session_added)(void *ud, int layer, uint32_t pid);
    /* Joined app gone (exited, killed, or protocol violation). The
     * layer is already detached from the compositor and the session
     * freed. Never fires for spawns that never joined. */
    void (*session_exited)(void *ud, int layer, uint32_t pid);
} scene_launcher_cbs;

/* cbs/ud may be NULL. limits is used for every app session (the
 * compositor was created with the same limits); NULL = engine defaults.
 * The caller must keep *limits alive for the launcher's lifetime. */
scene_launcher *scene_launcher_new(scene_compositor *cp,
                                   const scene_limits *limits,
                                   const scene_launcher_cbs *cbs, void *ud);
void scene_launcher_free(scene_launcher *sl);

/* Start an app process. The executable is resolved against the current
 * directory / PATH. Returns 0 on spawn success (pid written to
 * *out_pid), -1 on failure. The app's connection is accepted on the
 * next pump. */
int scene_launcher_spawn(scene_launcher *sl, const char *exe,
                         const char *arg, uint32_t *out_pid);

/* Terminate an app (SIGTERM / TerminateProcess). The session is reaped
 * on the next pump when the connection closes. Returns 0 on success,
 * -1 when the pid is not a live session. */
int scene_launcher_kill(scene_launcher *sl, uint32_t pid);

/* Seconds a spawn may wait for its app to connect before the pending
 * slot is dropped. Default 10; must be > 0. */
void scene_launcher_spawn_timeout(scene_launcher *sl, int seconds);

/* Driver-loop pump: accept, feed, drain, reap. Never blocks. */
void scene_launcher_pump(scene_launcher *sl);

int      scene_launcher_session_count(const scene_launcher *sl);
int      scene_launcher_layer_at(const scene_launcher *sl, int i);
uint32_t scene_launcher_pid_at(const scene_launcher *sl, int i);

#endif /* SCENE_LAUNCHER_H */
