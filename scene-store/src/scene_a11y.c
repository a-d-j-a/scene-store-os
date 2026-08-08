/*
 * scene_a11y.c — accessibility consumer of the semantic scene store.
 *
 * Built entirely on the public store API: scene_store_walk for traversal,
 * scene_store_node_vis for per-node data, scene_store_node_child_count
 * for child enumeration, scene_store_focus for focus state.
 */
#include "scene_a11y.h"
#include <string.h>

/* ---- per-node query -------------------------------------------------- */

int scene_store_a11y_node(const scene_store *s, scene_node_id id,
                          scene_a11y_node *out)
{
    scene_node_vis nv;
    if (scene_store_node_vis(s, id, &nv) != 0) return -1;
    if (nv.stale) return -1;
    out->id = nv.id;
    out->parent = nv.parent;
    out->role = nv.role;
    out->flags = nv.flags;
    out->rect[0] = nv.rect[0];
    out->rect[1] = nv.rect[1];
    out->rect[2] = nv.rect[2];
    out->rect[3] = nv.rect[3];
    out->depth = 0;
    out->child_count = scene_store_node_child_count(s, id);
    out->text_count = nv.text_count;
    out->primary_text_id = 0;
    out->primary_text_len = 0;
    out->primary_text = NULL;
    if (nv.text_count > 0) {
        scene_node_text_vis tv;
        if (scene_store_node_texts(s, id, &tv, 1) >= 1) {
            out->primary_text_id = tv.text_id;
            out->primary_text_len = tv.len;
            out->primary_text = tv.data;
        }
    }
    return 0;
}

/* ---- tree traversal -------------------------------------------------- */

struct walk_ctx {
    scene_a11y_walk_fn fn;
    void *ud;
    const scene_store *s;
};

static int walk_cb(scene_node_id id, void *ud)
{
    struct walk_ctx *ctx = ud;
    scene_a11y_node an;
    if (scene_store_a11y_node(ctx->s, id, &an) != 0) return 0; /* skip stale */
    if (!(an.flags & SCENE_FLAG_VISIBLE)) return 0; /* skip invisible */
    return ctx->fn(&an, ctx->ud);
}

void scene_store_a11y_walk(const scene_store *s,
                           scene_a11y_walk_fn fn, void *ud)
{
    struct walk_ctx ctx = { fn, ud, s };
    scene_store_walk(s, walk_cb, &ctx);
}

/* ---- focus chain ----------------------------------------------------- */

int scene_store_a11y_focused(const scene_store *s, scene_a11y_node *out)
{
    scene_node_id fid = scene_store_focus(s);
    if (fid == SCENE_NO_PARENT) return -1;
    return scene_store_a11y_node(s, fid, out);
}

struct fc_ctx {
    scene_node_id *out;
    size_t cap;
    size_t count;
};

static int fc_cb(const scene_a11y_node *node, void *ud)
{
    struct fc_ctx *c = ud;
    if ((node->flags & SCENE_FLAG_VISIBLE) &&
        (node->flags & SCENE_FLAG_FOCUSABLE)) {
        if (c->count < c->cap) c->out[c->count] = node->id;
        c->count++;
    }
    return 0;
}

size_t scene_store_a11y_focus_chain(const scene_store *s,
                                    scene_node_id *out, size_t cap)
{
    struct fc_ctx ctx = { out, cap, 0 };
    scene_store_a11y_walk(s, fc_cb, &ctx);
    return ctx.count;
}

/* ---- role-based search ----------------------------------------------- */

struct rb_ctx {
    uint16_t role;
    scene_node_id *out;
    size_t cap;
    size_t count;
};

static int rb_cb(const scene_a11y_node *node, void *ud)
{
    struct rb_ctx *c = ud;
    if ((node->flags & SCENE_FLAG_VISIBLE) && node->role == c->role) {
        if (c->count < c->cap) c->out[c->count] = node->id;
        c->count++;
    }
    return 0;
}

size_t scene_store_a11y_find_by_role(const scene_store *s, uint16_t role,
                                     scene_node_id *out, size_t cap)
{
    struct rb_ctx ctx = { role, out, cap, 0 };
    scene_store_a11y_walk(s, rb_cb, &ctx);
    return ctx.count;
}

/* ---- child enumeration ----------------------------------------------- */

struct child_ctx {
    scene_node_id pid;
    scene_node_id *out;
    size_t cap;
    size_t count;
};

static int child_cb(const scene_a11y_node *node, void *ud)
{
    struct child_ctx *c = ud;
    if (node->parent == c->pid && (node->flags & SCENE_FLAG_VISIBLE)) {
        if (c->count < c->cap) c->out[c->count] = node->id;
        c->count++;
    }
    return 0;
}

size_t scene_store_a11y_children(const scene_store *s, scene_node_id parent_id,
                                 scene_node_id *out, size_t cap)
{
    struct child_ctx ctx = { parent_id, out, cap, 0 };
    scene_store_a11y_walk(s, child_cb, &ctx);
    return ctx.count;
}
