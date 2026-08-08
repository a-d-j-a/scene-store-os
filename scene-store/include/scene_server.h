/*
 * scene_server.h — server adapter: the compositor seam.
 *
 * Wraps one scene_store (one session) with the wire-facing half of the
 * v0 protocol: frame reassembly, frame validation, ingest, and the
 * outbound frame drain. The compositor hosts this adapter in its loop:
 *
 *   sv = scene_server_new(limits);
 *   scene_server_attach(sv);          // client connected: WELCOME
 *   loop {
 *       read transport -> scene_server_feed(sv, bytes, n);
 *       while (scene_server_out_next_frame(sv, &f, &flen)) send(f, flen);
 *       scene_server_input_pointer(...);  // compositor input feeder
 *   }
 *   scene_server_detach(sv);          // client gone: ghost_mark
 *
 * Frame-level violations (magic/version/length/checksum, oversized
 * records) are fatal per spec §4: ERROR is emitted and the session
 * closes. Ingest-level errors are emitted by the engine itself; the
 * adapter surfaces them the same way (feed returns -1, session dead).
 *
 * Threading: one adapter, one session, one thread (same rule as the
 * store: scene_store.h).
 */
#ifndef SCENE_SERVER_H
#define SCENE_SERVER_H

#include "scene_fmt.h"
#include "scene_store.h"

typedef struct scene_server scene_server;

scene_server *scene_server_new(const scene_limits *limits);
void          scene_server_free(scene_server *sv);

/* New client connection: emits WELCOME as the first outbound record.   */
int  scene_server_attach(scene_server *sv);
/* Client gone: mark the retained scene stale (ghost-crash, §7).        */
void scene_server_detach(scene_server *sv);

/* Feed raw transport bytes (partial frames allowed; reassembled and
 * validated internally). Returns 0 on success, -1 on a fatal protocol
 * violation (ERROR already emitted, session dead, connection should
 * close). */
int  scene_server_feed(scene_server *sv, const uint8_t *bytes, uint32_t len);

/* Outbound drain: complete framed records. 0 = drained.                */
int  scene_server_out_next_frame(scene_server *sv,
                                 const uint8_t **frame, uint32_t *frame_len);

/* Compositor input feeder (flow-controlled, §8).                       */
int  scene_server_input_pointer(scene_server *sv, uint8_t device,
                                int32_t x, int32_t y, uint8_t buttons);

/* Key input feeder (flow-controlled, shares gate with pointer).         */
int  scene_server_input_key(scene_server *sv, uint32_t key_code,
                            uint8_t state, uint8_t modifiers);

/* 1 = session closed by a fatal error (or never attached).             */
int  scene_server_dead(const scene_server *sv);

/* The wrapped store (tests, compositor consumers).                     */
scene_store *scene_server_store(scene_server *sv);

#endif /* SCENE_SERVER_H */
