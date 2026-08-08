/*
 * scene_a11y.h — accessibility consumer of the semantic scene store.
 *
 * Provides a complete accessibility view of the scene tree: per-node
 * role/name/state/geometry, tree traversal with full node data, focus
 * chain, and role-based search. All queries are read-only snapshots
 * over the store's live or replay arena.
 *
 * This is the OS-level a11y service API. External tools (screen readers,
 * automation frameworks, cross-app search) consume the scene through
 * this layer.
 */
#ifndef SCENE_A11Y_H
#define SCENE_A11Y_H

#include "scene_store.h"

/* ---- A11y node: complete accessibility view of a single node ---------- */

typedef struct scene_a11y_node {
    scene_node_id id;
    scene_node_id parent;
    uint16_t      role;
    uint8_t       flags;        /* SCENE_FLAG_ENABLED/FOCUSABLE/VISIBLE    */
    int32_t       rect[4];      /* x, y, w, h in absolute session space    */
    uint32_t      text_count;   /* number of text slots on this node       */
    scene_text_id primary_text_id; /* id of the first text slot, or 0      */
    uint32_t      primary_text_len; /* length of first text, or 0          */
    const char   *primary_text; /* pointer into store memory (transient)   */
    uint32_t      child_count;  /* direct children                         */
    uint32_t      depth;        /* 0 = root child, increases downward      */
} scene_a11y_node;

/* ---- Per-node query --------------------------------------------------- */

/* Fill `out` with the complete a11y view of node `id`.
 * Returns 0 on success, -1 if the node id is unknown/stale.              */
int scene_store_a11y_node(const scene_store *s, scene_node_id id,
                          scene_a11y_node *out);

/* ---- Tree traversal --------------------------------------------------- */

/* Callback for a11y tree walk. Return nonzero to stop early.             */
typedef int (*scene_a11y_walk_fn)(const scene_a11y_node *node, void *ud);

/* Walk the entire a11y tree in document order (pre-order DFS).
 * For each node, fills a scene_a11y_node and calls fn.
 * Skips stale nodes. Walks the active arena (live or replay).            */
void scene_store_a11y_walk(const scene_store *s,
                           scene_a11y_walk_fn fn, void *ud);

/* ---- Focus chain ------------------------------------------------------ */

/* Fill `out` with the a11y node of the currently focused element.
 * Returns 0 on success, -1 if nothing is focused.                        */
int scene_store_a11y_focused(const scene_store *s, scene_a11y_node *out);

/* Write the ids of all visible+focusable nodes in document order into
 * `out` (up to `cap`). Returns the total count (may exceed cap).         */
size_t scene_store_a11y_focus_chain(const scene_store *s,
                                    scene_node_id *out, size_t cap);

/* ---- Role-based search ------------------------------------------------ */

/* Write the ids of all visible nodes matching `role` into `out`
 * (up to `cap`). Returns the total count (may exceed cap).               */
size_t scene_store_a11y_find_by_role(const scene_store *s, uint16_t role,
                                     scene_node_id *out, size_t cap);

/* ---- Child enumeration ------------------------------------------------ */

/* Write the ids of direct children of `parent_id` into `out`
 * (up to `cap`). Returns the total count (may exceed cap).               */
size_t scene_store_a11y_children(const scene_store *s, scene_node_id parent_id,
                                 scene_node_id *out, size_t cap);

#endif /* SCENE_A11Y_H */
