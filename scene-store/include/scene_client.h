/*
 * scene_client.h — reference wire client for the locked v0 protocol.
 *
 * The client is what every native app links: it owns the app's op
 * stream (monotonic seqs), the reconnect/ghost-recovery op log, and the
 * inbound server-record dispatch. Typed op builders append framed
 * records to an outbound buffer; scene_client_flush() puts them on the
 * transport. The server answers with typed records delivered through
 * scene_client_cbs.
 *
 * Reconnect semantics (ghost-crash recovery, spec §7):
 *   - same scene_id in WELCOME: the server retained the session (its
 *     nodes were marked stale by ghost_mark). The client re-issues its
 *     full op log with continuing seqs; the engine diffs against the
 *     retained scene and applies deltas only. State survives.
 *   - different scene_id: the server restarted. The client restarts its
 *     stream at seq 1 and rebuilds the scene from the op log.
 *   - The op log stores scene-affecting ops only (CreateNode..FocusNext,
 *     the engine's op_is_scene_affecting set); request/session records
 *     (Present, Snapshot, Macro*, ...) are not replayed. Macro-executed
 *     mutations therefore do not survive a server restart — the same
 *     limitation the engine's own replay has (macro effects are not in
 *     the op log). Honest and documented.
 *
 * Commit-vs-in-flight (§7): the client does not ack automatically. On
 * InputPointer the app consumes the event and calls
 * scene_client_ack(c, seq) once it is safe to surface server state for
 * that input; the server delivers the next pointer only after the ack.
 *
 * The client validates every inbound frame (magic/version/length/
 * checksum via scene_frame_check). Server ERROR records are fatal: the
 * session is closed and the client is dead; a server-dead client cannot
 * reconnect (the app creates a new client to a fresh session).
 */
#ifndef SCENE_CLIENT_H
#define SCENE_CLIENT_H

#include "scene_fmt.h"
#include "scene_transport.h"

typedef struct scene_client scene_client;

/* Create a zeroed client; connect it with scene_client_connect(). */
scene_client *scene_client_new(void);

typedef struct scene_search_hit {
    scene_node_id id;
    scene_rect    rect;
    uint16_t      role;
    scene_text_id text_id;
} scene_search_hit;

typedef struct scene_text_hit {
    scene_text_id text_id;
    scene_node_id node_id;
    const char   *data;        /* points INTO the record payload        */
    uint32_t      len;
} scene_text_hit;

typedef struct scene_client_cbs {
    void (*welcome)(void *ud, uint32_t scene_id, uint16_t version,
                    const scene_limits *lim);
    void (*error)(void *ud, uint16_t code, const char *msg, uint32_t len);
    void (*snapshot)(void *ud, uint32_t req_id,
                     const uint8_t *payload, uint32_t plen);
    void (*search_result)(void *ud, uint32_t req_id, uint32_t count,
                          const scene_search_hit *hits);
    void (*capture)(void *ud, uint32_t req_id, uint64_t seq,
                    const uint8_t *payload, uint32_t plen);
    void (*pong)(void *ud, uint64_t nonce);
    void (*input_pointer)(void *ud, uint64_t seq, uint8_t device,
                          int32_t x, int32_t y, uint8_t buttons);
    void (*input_activate)(void *ud, uint64_t seq, scene_node_id id);
    void (*input_focus)(void *ud, uint64_t seq, scene_node_id id,
                        uint8_t state);
    void (*input_key)(void *ud, uint64_t seq, uint32_t key_code,
                      uint8_t state, uint8_t modifiers);
    void (*present_done)(void *ud, uint64_t seq, uint64_t token,
                         uint64_t latency_us);
    void (*text_index)(void *ud, const scene_text_hit *entries,
                       uint32_t count);
    /* Transport closed or open failed: the connection is gone, the
     * client is dead until reconnect(). */
    void (*closed)(void *ud);
} scene_client_cbs;

/* Connect: takes ownership of `t`, opens it on `target`, and waits for
 * the server's WELCOME (delivered through cbs->welcome on the next
 * pump). Returns 0 on transport open success, -1 on failure. */
int  scene_client_connect(scene_client *c, scene_transport *t,
                          const char *target,
                          const scene_client_cbs *cbs, void *ud);

/* Ghost-crash recovery: close the old transport, take ownership of `t`,
 * re-open on the stored target. The WELCOME handling on the next pump
 * re-issues the op log (continuing or fresh seqs, see header notes).
 * Returns 0 on transport open success, -1 on failure or when the
 * session died from a server ERROR (not revivable). */
int  scene_client_reconnect(scene_client *c, scene_transport *t);

void scene_client_free(scene_client *c);

int  scene_client_dead(const scene_client *c);   /* 1 = session closed   */

/* Frame pump: read available transport bytes, validate, dispatch.
 * Returns 0 on success, -1 on transport error (client dead). */
int  scene_client_pump(scene_client *c);
/* Flush pending outbound frames to the transport. */
int  scene_client_flush(scene_client *c);

/* ---- typed op builders -------------------------------------------------- */
/* Append a framed record to the outbound buffer. Return 0 on success,
 * -1 when not connected / session dead / invalid argument / oom.
 * Seq is assigned from the client's stream counter (Ack excluded).     */

int scene_client_create_node(scene_client *c, scene_node_id parent,
                             scene_node_id id, scene_role role,
                             const scene_rect *rect, uint8_t flags);
int scene_client_destroy_node(scene_client *c, scene_node_id id);
int scene_client_set_text(scene_client *c, scene_node_id id,
                          scene_text_id slot, const char *utf8, uint32_t len);
int scene_client_set_value(scene_client *c, scene_node_id id,
                           scene_text_id slot, const char *utf8, uint32_t len);
int scene_client_set_rect(scene_client *c, scene_node_id id,
                          const scene_rect *rect);
int scene_client_set_flags(scene_client *c, scene_node_id id, uint8_t flags);
int scene_client_set_style(scene_client *c, scene_node_id id,
                           scene_style_ref style);
int scene_client_set_texture(scene_client *c, scene_node_id id,
                             scene_texture_ref tex, const scene_rect *src,
                             uint8_t blend, uint8_t opacity);
int scene_client_set_effect(scene_client *c, scene_node_id id,
                            scene_effect_ref effect);
int scene_client_focus(scene_client *c, scene_node_id id);
int scene_client_focus_next(scene_client *c, int8_t step);
int scene_client_present(scene_client *c, uint64_t token);
int scene_client_snapshot(scene_client *c, uint32_t req_id);
int scene_client_search(scene_client *c, uint32_t req_id,
                        const char *term, uint32_t len);
int scene_client_macro_begin(scene_client *c, uint32_t macro_id);
int scene_client_macro_end(scene_client *c, uint32_t macro_id);
int scene_client_exec_macro(scene_client *c, uint32_t macro_id);
int scene_client_capture(scene_client *c, uint32_t req_id);
int scene_client_ack(scene_client *c, uint64_t consumed_seq);
int scene_client_ping(scene_client *c, uint64_t nonce);
int scene_client_set_input_mode(scene_client *c, uint8_t mode);
int scene_client_seek(scene_client *c, uint64_t target_seq);

/* ---- session state ------------------------------------------------------ */

uint32_t scene_client_scene_id(const scene_client *c);
uint64_t scene_client_next_seq(const scene_client *c);
uint64_t scene_client_last_present_seq(const scene_client *c);

/* ---- §6 snapshot/capture decode ------------------------------------------ */

typedef struct scene_snapshot_text {
    scene_text_id id;
    const char   *data;        /* points INTO the payload                */
    uint32_t      len;
} scene_snapshot_text;

typedef struct scene_snapshot_node {
    scene_node_id id;
    scene_node_id parent;      /* NodeId, or SCENE_NO_PARENT for roots   */
    uint16_t      role;
    uint8_t       flags;
    scene_rect    rect;
    uint32_t      style, effect, tex;
    const scene_snapshot_text *texts;  /* points INTO the payload        */
    uint32_t      text_count;
} scene_snapshot_node;

/* Parse a SNAPSHOT/CAPTURE payload (§6). Returns the node count, or -1
 * on a malformed payload. Fills min(cap, count) nodes, min(texts_cap,
 * total) text entries, and min(tex_cap, texc) texture refs; *seq_out,
 * *tex_count_out and *texts_used_out receive the header values and the
 * total number of text entries. Text entry data pointers reference the
 * payload (valid while it is alive); the entries themselves are copied
 * into texts_buf (NULL with texts_cap 0 = only validate/count). If the
 * text buffer runs out mid-node, that node's text_count still reports
 * the true count and its texts pointer covers only the filled entries. */
int scene_snapshot_parse(const uint8_t *payload, uint32_t plen,
                         uint64_t *seq_out, uint32_t *tex_count_out,
                         uint32_t *texts_used_out,
                         scene_snapshot_node *nodes, uint32_t cap,
                         scene_snapshot_text *texts_buf, uint32_t texts_cap,
                         scene_texture_ref *texes, uint32_t tex_cap);

#endif /* SCENE_CLIENT_H */
