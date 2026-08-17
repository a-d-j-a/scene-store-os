/*
 * scene_store.h — the semantic scene engine (OS-layer core).
 *
 * Owns the meaning of every native app's visible UI as a versioned tree
 * with an append-only op log. Consumers (compositor, effects, search,
 * automation, a11y, rewind) read through this API. Serialization is the
 * locked v0 wire format (scene_fmt.h).
 *
 * Lifecycle:
 *   s = scene_store_new(&limits)
 *   scene_store_set_clock(s, fn, ud)        // monotonic us source
 *   loop { resolve whole frames off the transport, then:
 *          scene_store_ingest(s, hdr, payload, &repl)
 *          drain repl outbound records to the peer }
 *   scene_store_free(s)
 *
 * Threading: one store owns one session; session is single-threaded.
 */
#ifndef SCENE_STORE_H
#define SCENE_STORE_H

#include "scene_fmt.h"

typedef struct scene_store scene_store;

typedef uint64_t (*scene_clock_fn)(void *ud);

/* -------------------------------------------------------------------- */
/* Construction                                                         */
/* -------------------------------------------------------------------- */
scene_store *scene_store_new(const scene_limits *limits);
void         scene_store_free(scene_store *s);
void         scene_store_set_clock(scene_store *s, scene_clock_fn fn, void *ud);

/* -------------------------------------------------------------------- */
/* Ingest a single client->server record (header + payload already
 * reassembled). Returns 0 on success or a stable negative code mapped from
 * SCENE_ERR_*. Outbound records produced by the store are appended to the
 * store's outbound buffer; drain with scene_store_out_next().           */
int  scene_store_ingest(scene_store *s,
                        uint16_t opcode,
                        const uint8_t *payload, uint32_t payload_len);
/* Outbound drain: 0 = no more records, 1 = got one.                     */
int  scene_store_out_next(scene_store *s,
                          uint16_t *opcode,
                          const uint8_t **payload,
                          uint32_t *payload_len);

/* Full-framed-record drain: like scene_store_out_next but yields the
 * complete frame bytes (header + payload, checksum included) ready to
 * send on the wire. Drains the same outbound buffer; use one drain
 * style per poll cycle, not both mixed.                                 */
int  scene_store_out_next_frame(scene_store *s,
                                const uint8_t **frame, uint32_t *frame_len);

/* Server-side fatal failure: emit ERROR and close the session (marks
 * the store dead). Used by transport/server layers on frame-level
 * protocol violations the engine cannot see (bad magic/version/length/
 * checksum).                                                            */
int  scene_store_fail(scene_store *s, uint16_t code, const char *msg);

/* Server-side raw record emit (adapter-owned records like
 * IMPORT_RESULT; frame + checksum computed here). Server-record opcodes
 * (0x8000 range) only; oversized payloads rejected.                      */
int  scene_store_emit_record(scene_store *s, uint16_t opcode,
                             const uint8_t *payload, uint32_t plen);

/* Server-side feed: emit WELCOME as the first outbound record.          */
int  scene_store_welcome(scene_store *s);

/* Input feeder (compositor calls this; the engine resolves semantics).
 * Flow-controlled: one un-acked pointer delivery at a time (§8).        */
int  scene_store_input_pointer(scene_store *s, uint8_t device,
                               int32_t x, int32_t y, uint8_t buttons);

/* Key input feeder (compositor calls this; engine emits to client).
 * Flow-controlled: shares the un-acked gate with pointer input.         */
int  scene_store_input_key(scene_store *s, uint32_t key_code,
                           uint8_t state, uint8_t modifiers);

/* Text input feeder (compositor clipboard service calls this; engine
 * emits SCENE_SRV_INPUT_TEXT to the client). The text is the OS-
 * provided string to insert (paste content), NOT keycodes. Flow-
 * controlled: shares the un-acked gate with pointer/key input (dropped
 * silently while a previous input is unacked).                          */
int  scene_store_input_text(scene_store *s, const char *text, uint32_t len);

/* Ghost-crash: mark every retained live node stale (client died).
 * On reconnect the client re-issues its ops; the engine diffs against
 * the retained scene and applies only deltas (CreateNode resurrects).  */
int  scene_store_ghost_mark(scene_store *s);

/* Ghost rejoin: rebase the stream counter onto a re-issuing client's
 * next seq (the retained session's committed seq stays in the log; the
 * re-issued ops at seq >= the new baseline are applied as deltas).
 * Refuses when the session is dead or in replay mode, or when the given
 * seq is behind the current counter. seq == current counter is a no-op
 * success (client continued normally).                                  */
int  scene_store_rejoin(scene_store *s, uint64_t seq);

/* Texture registry (server-issued opaque handles).                      */
int  scene_store_register_texture(scene_store *s, scene_texture_ref ref,
                                  uint32_t w, uint32_t h,
                                  uint16_t fmt, uint8_t opaque);
int  scene_store_release_texture(scene_store *s, scene_texture_ref ref);
/* 1 when `ref` is registered in s's texture table (per-session).       */
int  scene_store_texture_registered(const scene_store *s,
                                    scene_texture_ref ref);

/* Style/effect table sizes (server-owned tables).                       */
void scene_store_set_style_count(scene_store *s, uint32_t n);
void scene_store_set_effect_count(scene_store *s, uint32_t n);

/* Direct queries (used by compositor consumers / tests) ------------------ */
uint64_t scene_store_committed_seq(const scene_store *s);
uint64_t scene_store_view_seq(const scene_store *s);
uint32_t scene_store_node_count(const scene_store *s);
const scene_limits *scene_store_limits(const scene_store *s);
uint32_t scene_store_style_count(const scene_store *s);
uint32_t scene_store_effect_count(const scene_store *s);
uint32_t scene_store_texture_count(const scene_store *s);

/* Replay mode flag: true when the store is in REPLAY mode with an active
 * replay arena (cursor may be anywhere, including the head). The
 * compositor uses this to suppress transitions during seeks.           */
int scene_store_in_replay(const scene_store *s);

/* Region resolution: given an absolute point, returns the innermost
 * activatable semantic node id containing it, or SCENE_NO_PARENT.        */
scene_node_id scene_store_region_at(const scene_store *s,
                                    int32_t x, int32_t y);

/* Focus state. */
scene_node_id scene_store_focus(const scene_store *s);

/* Search committed text slots (case-insensitive ASCII substring match).   */
size_t scene_store_search(const scene_store *s, const char *term,
                          uint32_t term_len,
                          scene_node_id *out_nodes, size_t out_cap,
                          scene_text_id *out_texts, size_t *out_text_cap);

/* Cross-session automation: copy a recorded macro definition into `dst`
 * under a new macro id. Memory-level transfer; the wire format is not
 * touched. Returns 0 on success.                                          */
int  scene_store_import_macro(scene_store *dst, scene_store *src,
                              uint32_t from_id, uint32_t to_id);

/* Walk tree in depth-first order; visits every node once.
   cb returns nonzero to stop. `out` is passed through to cb. */
void scene_store_walk(const scene_store *s,
                      int (*cb)(scene_node_id id, void *out), void *out);

/* ---- per-node read view (compositor/effects/a11y consumers) ------------ */

typedef struct scene_node_text_vis {
    scene_text_id text_id;
    uint32_t      len;
    const char   *data;       /* into store memory (valid until next        */
} scene_node_text_vis;        /* mutation on that slot)                    */

typedef struct scene_node_vis {
    scene_node_id id, parent; /* parent NodeId or SCENE_NO_PARENT          */
    uint16_t      role;
    uint8_t       flags;
    uint8_t       stale;      /* ghost-crash marker                         */
    int32_t       rect[4];    /* absolute session space                     */
    int32_t       tex_src[4];
    uint32_t      style, effect, tex;  /* tex = SCENE_NO_TEXTURE when unset */
    uint8_t       blend, opacity;
    uint32_t      text_count; /* total slots (drain via node_texts)         */
} scene_node_vis;

/* 0 = ok, -1 = unknown node id. Views are snapshots; pointers into the
 * store's text storage must not be retained across mutations.           */
int scene_store_node_vis(const scene_store *s, scene_node_id id,
                         scene_node_vis *out);
/* Fill up to `cap` text slots for `id` into `out`. Returns the slot
 * count (may exceed cap), or -1 for an unknown node id.                 */
int scene_store_node_texts(const scene_store *s, scene_node_id id,
                           scene_node_text_vis *out, uint32_t cap);

/* Number of direct children of `id` (not counting grandchildren).
 * Returns 0 for unknown/stale ids or leaf nodes.                          */
uint32_t scene_store_node_child_count(const scene_store *s, scene_node_id id);

/* ---- host-side WM service (OS interventions, not wire) ---------------- */
/* These are direct library calls for OS services (the shell WM, cross-
 * app automation). They do NOT produce wire bytes: the peer's seq stream
 * (s->next_seq) is untouched. They commit through the engine's own path
 * (apply_op on the live arena + committed-seq advance), so the
 * compositor's per-layer diff fires on the next frame, and they mirror
 * the engine's internal semantics (focus assignment as on InputActivate,
 * flags/rect as on SetFlags/SetRect).
 *
 * Honest limitations (by design): (a) host mutations are OS interventions,
 * not client input — they are NOT appended to the op log, so replay/seek
 * reconstructs only the app's own committed input history; (b) a host
 * mutation advances the committed seq by one without consuming a wire
 * seq, so the next client mutation that lands on the same seq value is
 * coalesced into the already-painted frame (visually benign: the layer's
 * next seq change repaints it).                                     */
/* Set the engine focus to an existing live node (as InputActivate).
 * -1 for a dead store, replay mode, or an unknown/stale node id.          */
int scene_store_host_focus(scene_store *s, scene_node_id id);
/* Set or clear SCENE_FLAG_VISIBLE on a live node and commit. -1 as above. */
int scene_store_host_set_visible(scene_store *s, scene_node_id id, int on);
/* Set the absolute rect on a live node and commit. -1 as above.           */
int scene_store_host_set_rect(scene_store *s, scene_node_id id,
                              int32_t x, int32_t y, int32_t w, int32_t h);

/* ---- internal-consumer mode transitions (bypass op/seq pipeline) ------ */

/* Enter replay mode directly (no wire op, no seq advance).
 * Returns 0 on success, -1 if already in replay or dead.                 */
int scene_store_begin_replay(scene_store *s);

/* Exit replay mode, returning to live. Tears down replay arena.
 * Returns 0 on success, -1 if not in replay or dead.                     */
int scene_store_end_replay(scene_store *s);

/* Seek the replay cursor to `target` seq (1-based, clamped to
 * [1, committed_seq]). Builds the replay arena for that point.
 * Returns 0 on success, -1 if not in replay or target is invalid.        */
int scene_store_seek_to(scene_store *s, uint64_t target);

#endif /* SCENE_STORE_H */