/*
 * scene_rewind.h — deterministic replay service.
 *
 * Wraps the store's op log + replay arena into a consumer API for
 * scrubbing through desktop history. The rewind service does not own
 * the store — it borrows it. The store handles op log, checkpoints,
 * and arena rebuild; the rewind service adds navigation and diff.
 *
 * Usage:
 *   scene_rewind *rw = scene_rewind_new(store);
 *   scene_rewind_enter_replay(rw);           // switch store to replay mode
 *   scene_rewind_seek(rw, target_seq);       // jump to any historical seq
 *   scene_rewind_step_backward(rw, 5);       // step back 5 ops
 *   scene_rewind_diff(rw, other_seq, &diff); // what changed?
 *   scene_rewind_exit_replay(rw);            // return to live mode
 *   scene_rewind_free(rw);
 */
#ifndef SCENE_REWIND_H
#define SCENE_REWIND_H

#include "scene_store.h"

typedef struct scene_rewind scene_rewind;

/* ---- lifecycle ------------------------------------------------------- */

scene_rewind *scene_rewind_new(scene_store *s);
void          scene_rewind_free(scene_rewind *rw);

/* ---- mode transitions ------------------------------------------------ */

/* Switch the underlying store to REPLAY mode. Returns 0 on success.      */
int scene_rewind_enter_replay(scene_rewind *rw);

/* Return the underlying store to LIVE mode. The replay arena is freed.   */
int scene_rewind_exit_replay(scene_rewind *rw);

/* ---- navigation ------------------------------------------------------ */

/* Seek to an absolute seq. The seq must be ≤ the store's committed seq.
 * Returns 0 on success, -1 if not in replay mode or seq out of range.   */
int scene_rewind_seek(scene_rewind *rw, uint64_t target_seq);

/* Step forward by `n` ops (seq increments). Clamped at head.
 * Returns 0 on success, -1 if not in replay mode.                       */
int scene_rewind_step_forward(scene_rewind *rw, uint32_t n);

/* Step backward by `n` ops (seq decrements). Clamped at 1.
 * Returns 0 on success, -1 if not in replay mode.                       */
int scene_rewind_step_backward(scene_rewind *rw, uint32_t n);

/* Current cursor position (seq of the last applied op).                 */
uint64_t scene_rewind_tell(const scene_rewind *rw);

/* Latest seq in the op log (the live scene's head).                      */
uint64_t scene_rewind_head(const scene_rewind *rw);

/* Earliest accessible seq (0 = empty scene).                             */
uint64_t scene_rewind_tail(const scene_rewind *rw);

/* ---- diff ------------------------------------------------------------- */

typedef struct scene_rewind_delta {
    scene_node_id id;
    uint16_t      role;
    uint8_t       flags;
    int32_t       rect[4];
    uint32_t      text_hash;  /* FNV-1a of primary text, 0 if no text   */
} scene_rewind_delta;

/* Diff result: nodes created, destroyed, or modified between two seqs.  */
typedef struct scene_rewind_diff_result {
    scene_rewind_delta *created;
    uint32_t                 created_count;
    scene_rewind_delta *destroyed;
    uint32_t                 destroyed_count;
    scene_rewind_delta *modified;
    uint32_t                 modified_count;
} scene_rewind_diff_result;

/* Compute the diff between the scene at `rw`'s current position and
 * the scene at `other_seq`. Both positions are rebuilt from the op log.
 * Returns 0 on success. Call scene_rewind_diff_free() to release.        */
int scene_rewind_diff(scene_rewind *rw, uint64_t other_seq,
                      scene_rewind_diff_result *out);

/* Release memory owned by a diff result.                                 */
void scene_rewind_diff_free(scene_rewind_diff_result *out);

#endif /* SCENE_REWIND_H */
