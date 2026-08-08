/*
 * scene_rewind.c — deterministic replay service.
 */
#include "scene_rewind.h"
#include "scene_a11y.h"
#include <stdlib.h>
#include <string.h>

struct scene_rewind {
    scene_store *store;
    uint64_t     pos;       /* current cursor position (replay seq) */
    int          in_replay;
};

/* ---- lifecycle ------------------------------------------------------- */

scene_rewind *scene_rewind_new(scene_store *s)
{
    scene_rewind *rw = (scene_rewind *)calloc(1, sizeof(scene_rewind));
    if (rw) { rw->store = s; rw->in_replay = 0; }
    return rw;
}

void scene_rewind_free(scene_rewind *rw)
{
    if (!rw) return;
    if (rw->in_replay) scene_rewind_exit_replay(rw);
    free(rw);
}

/* ---- mode transitions ------------------------------------------------ */

int scene_rewind_enter_replay(scene_rewind *rw)
{
    if (!rw || rw->in_replay) return -1;
    if (scene_store_begin_replay(rw->store) != 0)
        return -1;
    rw->in_replay = 1;
    rw->pos = 0;
    return 0;
}

int scene_rewind_exit_replay(scene_rewind *rw)
{
    if (!rw || !rw->in_replay) return -1;
    if (scene_store_end_replay(rw->store) != 0)
        return -1;
    rw->in_replay = 0;
    return 0;
}

/* ---- navigation ------------------------------------------------------ */

int scene_rewind_seek(scene_rewind *rw, uint64_t target_seq)
{
    if (!rw || !rw->in_replay) return -1;
    if (scene_store_seek_to(rw->store, target_seq) != 0)
        return -1;
    rw->pos = target_seq;
    return 0;
}

int scene_rewind_step_forward(scene_rewind *rw, uint32_t n)
{
    if (!rw || !rw->in_replay) return -1;
    uint64_t head = scene_store_committed_seq(rw->store);
    uint64_t target = rw->pos + n;
    if (target > head) target = head;
    return scene_rewind_seek(rw, target);
}

int scene_rewind_step_backward(scene_rewind *rw, uint32_t n)
{
    if (!rw || !rw->in_replay) return -1;
    uint64_t target = rw->pos > n ? rw->pos - n : 1;
    return scene_rewind_seek(rw, target);
}

uint64_t scene_rewind_tell(const scene_rewind *rw)
{
    return rw ? rw->pos : 0;
}

uint64_t scene_rewind_head(const scene_rewind *rw)
{
    return rw ? scene_store_committed_seq(rw->store) : 0;
}

uint64_t scene_rewind_tail(const scene_rewind *rw)
{
    (void)rw;
    return 0; /* the earliest accessible seq is always 0 (empty scene) */
}

/* ---- node snapshot for diff ------------------------------------------ */

typedef struct node_snap {
    scene_node_id id;
    uint16_t      role;
    uint8_t       flags;
    int32_t       rect[4];
    uint32_t      text_hash;
    int           valid;
} node_snap;

static uint32_t fnv1a32_data(const void *data, uint32_t len)
{
    const uint8_t *p = data;
    uint32_t h = 2166136261u;
    uint32_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

struct snap_ctx {
    node_snap *snaps;
    uint32_t   count;
    uint32_t   cap;
};

static int snap_cb(const scene_a11y_node *node, void *ud)
{
    struct snap_ctx *c = ud;
    if (c->count >= c->cap) return 0;
    node_snap *ns = &c->snaps[c->count];
    ns->id = node->id;
    ns->role = node->role;
    ns->flags = node->flags;
    memcpy(ns->rect, node->rect, sizeof(ns->rect));
    ns->text_hash = 0;
    if (node->primary_text && node->primary_text_len > 0)
        ns->text_hash = fnv1a32_data(node->primary_text, node->primary_text_len);
    ns->valid = 1;
    c->count++;
    return 0;
}

static int collect_arena_snaps(scene_store *s, node_snap **out, uint32_t *count)
{
    /* Estimate capacity from node count. */
    uint32_t est = scene_store_node_count(s) + 16;
    node_snap *snaps = (node_snap *)calloc(est, sizeof(node_snap));
    if (!snaps) return -1;
    struct snap_ctx ctx = { snaps, 0, est };
    scene_store_a11y_walk(s, snap_cb, &ctx);
    *out = snaps;
    *count = ctx.count;
    return 0;
}

static int snap_id_cmp(const void *a, const void *b)
{
    uint32_t ai = ((const node_snap *)a)->id;
    uint32_t bi = ((const node_snap *)b)->id;
    return (ai > bi) - (ai < bi);
}

/* ---- diff ------------------------------------------------------------ */

int scene_rewind_diff(scene_rewind *rw, uint64_t other_seq,
                      scene_rewind_diff_result *out)
{
    if (!rw || !rw->in_replay || !out) return -1;
    memset(out, 0, sizeof(*out));
    uint64_t committed = scene_store_committed_seq(rw->store);
    if (other_seq > committed) return -1;

    /* Collect nodes at current position. */
    node_snap *cur_snaps = NULL;
    uint32_t cur_count = 0;
    if (collect_arena_snaps(rw->store, &cur_snaps, &cur_count) != 0)
        return -1;

    /* Save current position, seek to other_seq. */
    uint64_t saved_pos = rw->pos;
    if (scene_rewind_seek(rw, other_seq) != 0) {
        free(cur_snaps);
        return -1;
    }

    /* Collect nodes at other position. */
    node_snap *other_snaps = NULL;
    uint32_t other_count = 0;
    if (collect_arena_snaps(rw->store, &other_snaps, &other_count) != 0) {
        free(cur_snaps);
        scene_rewind_seek(rw, saved_pos);
        return -1;
    }

    /* Seek back. */
    scene_rewind_seek(rw, saved_pos);

    /* Sort both arrays by id for merge. */
    qsort(cur_snaps, cur_count, sizeof(node_snap), snap_id_cmp);
    qsort(other_snaps, other_count, sizeof(node_snap), snap_id_cmp);

    /* Estimate max diff size. */
    uint32_t max_delta = cur_count + other_count;
    scene_rewind_delta *created = (scene_rewind_delta *)
        calloc(max_delta, sizeof(scene_rewind_delta));
    scene_rewind_delta *destroyed = (scene_rewind_delta *)
        calloc(max_delta, sizeof(scene_rewind_delta));
    scene_rewind_delta *modified = (scene_rewind_delta *)
        calloc(max_delta, sizeof(scene_rewind_delta));
    if (!created || !destroyed || !modified) {
        free(created); free(destroyed); free(modified);
        free(cur_snaps); free(other_snaps);
        return -1;
    }

    /* Merge: walk both sorted arrays. */
    uint32_t ci = 0, oi = 0;
    uint32_t nc = 0, nd = 0, nm = 0;
    while (ci < cur_count || oi < other_count) {
        if (ci >= cur_count) {
            /* Remaining in other but not in current = created. */
            while (oi < other_count) {
                scene_rewind_delta *d = &created[nc++];
                d->id = other_snaps[oi].id;
                d->role = other_snaps[oi].role;
                d->flags = other_snaps[oi].flags;
                memcpy(d->rect, other_snaps[oi].rect, sizeof(d->rect));
                d->text_hash = other_snaps[oi].text_hash;
                oi++;
            }
            break;
        }
        if (oi >= other_count) {
            /* Remaining in current but not in other = destroyed. */
            while (ci < cur_count) {
                scene_rewind_delta *d = &destroyed[nd++];
                d->id = cur_snaps[ci].id;
                d->role = cur_snaps[ci].role;
                d->flags = cur_snaps[ci].flags;
                memcpy(d->rect, cur_snaps[ci].rect, sizeof(d->rect));
                d->text_hash = cur_snaps[ci].text_hash;
                ci++;
            }
            break;
        }
        if (cur_snaps[ci].id == other_snaps[oi].id) {
            /* Same id: check if modified. */
            node_snap *a = &cur_snaps[ci];
            node_snap *b = &other_snaps[oi];
            if (a->role != b->role || a->flags != b->flags ||
                a->text_hash != b->text_hash ||
                memcmp(a->rect, b->rect, sizeof(a->rect)) != 0) {
                scene_rewind_delta *d = &modified[nm++];
                d->id = a->id;
                d->role = b->role;
                d->flags = b->flags;
                memcpy(d->rect, b->rect, sizeof(d->rect));
                d->text_hash = b->text_hash;
            }
            ci++; oi++;
        } else if (cur_snaps[ci].id < other_snaps[oi].id) {
            /* In current but not other = destroyed from current's view. */
            scene_rewind_delta *d = &destroyed[nd++];
            d->id = cur_snaps[ci].id;
            d->role = cur_snaps[ci].role;
            d->flags = cur_snaps[ci].flags;
            memcpy(d->rect, cur_snaps[ci].rect, sizeof(d->rect));
            d->text_hash = cur_snaps[ci].text_hash;
            ci++;
        } else {
            /* In other but not current = created in other. */
            scene_rewind_delta *d = &created[nc++];
            d->id = other_snaps[oi].id;
            d->role = other_snaps[oi].role;
            d->flags = other_snaps[oi].flags;
            memcpy(d->rect, other_snaps[oi].rect, sizeof(d->rect));
            d->text_hash = other_snaps[oi].text_hash;
            oi++;
        }
    }

    /* Trim allocations to actual size. */
    out->created = (scene_rewind_delta *)realloc(created,
        nc * sizeof(scene_rewind_delta));
    out->created_count = nc;
    out->destroyed = (scene_rewind_delta *)realloc(destroyed,
        nd * sizeof(scene_rewind_delta));
    out->destroyed_count = nd;
    out->modified = (scene_rewind_delta *)realloc(modified,
        nm * sizeof(scene_rewind_delta));
    out->modified_count = nm;

    free(cur_snaps);
    free(other_snaps);
    return 0;
}

void scene_rewind_diff_free(scene_rewind_diff_result *out)
{
    if (!out) return;
    free(out->created);  out->created = NULL;
    free(out->destroyed); out->destroyed = NULL;
    free(out->modified); out->modified = NULL;
    out->created_count = out->destroyed_count = out->modified_count = 0;
}
