/* scene_compositor.c — compositor core: first consumer of the store.
 *
 * Composes the shell session (layer 0) plus any number of app sessions
 * (layers 1..n, scene_compositor_add_session). Each scene_compositor_frame():
 * every layer's view seq is diffed against its own render model (an
 * id-keyed map of last-painted visual state per node) and the deltas
 * become damage rects; each damaged rect is repainted by clearing it to
 * the desktop color and redrawing every visible node of every layer
 * intersecting it, in layer order (layer 0 first = desktop below apps),
 * then document order within a layer (children over parents). No change
 * -> no frame.
 *
 * Styles are server-owned: style 0 means "role default" from an internal
 * table; changing a style entry dirties every node referencing it in
 * every layer, which is the OS-side re-theme of a running app (spec §7).
 *
 * Effects (v1): scene_compositor_set_effects() enables deterministic,
 * tick-driven enter/exit transitions — new nodes fade in while sliding
 * up, deleted nodes fade out via a phantom snapshot drawn after the live
 * scene. Everything is driven by the internal tick counter (advanced on
 * every frame()), integer math only, never wall-clock, so the pipeline
 * stays deterministic. Replay seeks and ghost re-connects never animate.
 * When effects are off the paint is identity. Rounded corners are style-
 * driven (scene_style.radius). Transitions are per-layer (ids collide
 * across sessions).
 *
 * Textures are registered by the compositor (server side) and validated
 * by the store's registry. Input is forwarded to the hit-tested layer's
 * store, fully flow-controlled (§8).
 *
 * One compositor, many sessions, one thread. Deterministic: same op log
 * in, same framebuffer out.
 */
#include "scene_compositor.h"

#include <stdlib.h>
#include <string.h>

#define SCENE_COMPOSITOR_MAX_DAMAGE   32u
#define SCENE_COMPOSITOR_TEXT_CAP     16u
#define SCENE_COMPOSITOR_ANIM_TICKS   8u
#define SCENE_COMPOSITOR_ANIM_SLIDE   6
#define SCENE_COMPOSITOR_ANIM_CAP     64u
#define SCENE_COMPOSITOR_LAYER_CAP0   4u

#define SCENE_ANIM_ENTER 1u
#define SCENE_ANIM_EXIT  2u

typedef struct scene_tex_ent {
    scene_texture_ref ref;
    uint32_t          w, h;
    uint16_t          fmt;
    uint8_t           opaque;
    uint8_t           used;
    uint8_t           store_registered; /* ref known to the store registry */
    uint32_t         *px;
} scene_tex_ent;

/* One node's last-painted state (render model). The text blob (tx_*)
 * is a copy of the node's texts, kept so a deleted node can still fade
 * out (the store kills a destroyed node's texts with it). */
typedef struct scene_rnode {
    scene_node_id id;
    uint32_t      sig;      /* text-content signature                       */
    uint32_t      style, tex;
    uint16_t      role;
    uint8_t       flags;
    uint8_t       used, seen;
    int32_t       rect[4];
    uint8_t       blend, opacity;
    char         *tx_data;  /* owned text blob (exit-fade snapshot)         */
    uint32_t      tx_count;
    uint32_t      tx_len;
    uint32_t      tx_lens[SCENE_COMPOSITOR_TEXT_CAP];
} scene_rnode;

/* One transition (enter or exit) on the compositor's deterministic
 * tick clock. EXIT entries carry a phantom: the resolved colors and a
 * copy of the texts the store has already destroyed. */
typedef struct anim_ent {
    scene_node_id id;
    uint8_t       active;
    uint8_t       kind;     /* SCENE_ANIM_ENTER / SCENE_ANIM_EXIT         */
    uint8_t       t;        /* elapsed ticks 0..SCENE_COMPOSITOR_ANIM_TICKS */
    int32_t       base[4];  /* enter: final rect; exit: rect being faded   */
    uint32_t      fill, border, text;
    uint8_t       border_w, radius;
    uint8_t       opacity;
    uint32_t      tex;
    uint32_t      text_count;
    scene_node_text_vis t_snap[SCENE_COMPOSITOR_TEXT_CAP];
    char         *tbuf;     /* owned text blob (phantom)                   */
} anim_ent;

/* One session's contribution to the composition: its wire seam, its
 * store, its render model and its transition table. Node ids are
 * per-session (locked v0), so every session keeps its own map. */
typedef struct scene_layer {
    scene_server *sv;
    scene_store  *store;

    scene_rnode  *map;      /* open-addressing model, power-of-two cap      */
    uint32_t      map_cap;
    uint32_t      map_shift;
    uint32_t      map_used;

    anim_ent      anims[SCENE_COMPOSITOR_ANIM_CAP];
    uint32_t      anim_used;

    uint64_t      rendered_seq;
    int           dead;     /* session in fatal state: skip render/input   */
} scene_layer;

struct scene_compositor {
    scene_layer  *ly;       /* array; ly[0] = shell session (never empty)  */
    uint32_t      ly_count;
    uint32_t      ly_cap;
    uint32_t      focus_layer;  /* keyboard focus (0 = shell)              */

    scene_fb      fb;
    uint32_t      clear;
    int           force;
    uint64_t      tick;
    int           effects_on;

    scene_layer  *walk_ly;  /* scratch: the layer of the current walk      */

    scene_style  *styles;
    uint32_t      style_count;

    scene_tex_ent *tex_ents;
    uint32_t      tex_cap;

    scene_rect    pending[SCENE_COMPOSITOR_MAX_DAMAGE];
    uint32_t      pending_count;
    scene_rect    damage[SCENE_COMPOSITOR_MAX_DAMAGE];
    uint32_t      damage_count;

    scene_rect    paint_clip;    /* scratch for the paint walk             */
};

/* ---- Role default styles (the OS's dark look seed; server-owned) ----- */
static const scene_style role_defaults[32] = {
    /* GENERIC    */ {0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0, 0, 0, 0},
    /* WINDOW     */ {0xFF202020u, 0x00000000u, 0xFFFFFFFFu, 0, 0, 0, 8},
    /* PANEL      */ {0xFF2A2A2Au, 0x00000000u, 0xFFFFFFFFu, 0, 0, 0, 4},
    /* BUTTON     */ {0xFF3C3C3Cu, 0xFF555555u, 0xFFFFFFFFu, 1, 0, 0, 4},
    /* CHECKBOX   */ {0x00000000u, 0xFF666666u, 0xFFFFFFFFu, 1, 0, 0, 2},
    /* TEXTFIELD  */ {0xFF1E1E1Eu, 0xFF4A4A4Au, 0xFFFFFFFFu, 1, 0, 0, 4},
    /* LABEL      */ {0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0, 0, 0, 0},
    /* LIST       */ {0xFF2A2A2Au, 0xFF333333u, 0xFFFFFFFFu, 1, 0, 0, 2},
    /* TREE       */ {0xFF2A2A2Au, 0xFF333333u, 0xFFFFFFFFu, 1, 0, 0, 2},
    /* TABLE      */ {0xFF2A2A2Au, 0xFF333333u, 0xFFFFFFFFu, 1, 0, 0, 2},
    /* MENU       */ {0xFF2A2A2Au, 0xFF444444u, 0xFFFFFFFFu, 1, 0, 0, 2},
    /* DIALOG     */ {0xFF262626u, 0xFF444444u, 0xFFFFFFFFu, 1, 0, 0, 8},
    /* SCROLLBAR  */ {0xFF2E2E2Eu, 0xFF3F3F3Fu, 0xFFFFFFFFu, 1, 0, 0, 0},
    /* TABBAR     */ {0xFF222222u, 0x00000000u, 0xFFFFFFFFu, 0, 0, 0, 0},
    /* SLIDER     */ {0xFF2E2E2Eu, 0xFF3F3F3Fu, 0xFFFFFFFFu, 1, 0, 0, 0},
    /* IMAGE      */ {0xFF1E1E1Eu, 0x00000000u, 0xFFFFFFFFu, 0, 0, 0, 0},
    /* SPINNER    */ {0xFF2E2E2Eu, 0xFF3F3F3Fu, 0xFFFFFFFFu, 1, 0, 0, 0},
    /* TOOLBAR    */ {0xFF222222u, 0x00000000u, 0xFFFFFFFFu, 0, 0, 0, 0},
    /* STATUSBAR  */ {0xFF222222u, 0x00000000u, 0xFFFFFFFFu, 0, 0, 0, 0},
    /* TITLEBAR   */ {0xFF1A1A1Au, 0x00000000u, 0xFFFFFFFFu, 0, 0, 0, 0},
    /* TERMINAL   */ {0xFF141414u, 0x00000000u, 0xFFDDDDDDu, 0, 0, 0, 0},
    /* EDITOR     */ {0xFF141414u, 0x00000000u, 0xFFDDDDDDu, 0, 0, 0, 0},
    /* COMBO      */ {0xFF2A2A2Au, 0xFF4A4A4Au, 0xFFFFFFFFu, 1, 0, 0, 2},
    /* PROGRESS   */ {0xFF2E2E2Eu, 0xFF3F3F3Fu, 0xFFFFFFFFu, 1, 0, 0, 0},
    /* TOOLTIP    */ {0xFF3A3A2Au, 0xFF555540u, 0xFFFFFFFFu, 1, 0, 0, 4},
    /* POPUP      */ {0xFF2A2A2Au, 0xFF444444u, 0xFFFFFFFFu, 1, 0, 0, 2},
    /* GROUP      */ {0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0, 0, 0, 0},
    /* CANVAS     */ {0xFF101010u, 0x00000000u, 0xFFFFFFFFu, 0, 0, 0, 0},
    /* TEXTBLOCK  */ {0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0, 0, 0, 0},
    /* SELECTION  */ {0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0, 0, 0, 0},
    /* CURSOR     */ {0x00000000u, 0x00000000u, 0xFFFFFFFFu, 0, 0, 0, 0},
    /* LINK       */ {0x00000000u, 0x00000000u, 0xFF66B2FFu, 0, 0, 0, 0},
};

/* ---- Damage list ----------------------------------------------------- */

/* Append `r` to `list` (cap entries), merging into the last slot once
 * the cap is reached (a superset repaint is always correct).           */
static void rectlist_add(scene_rect *list, uint32_t *count, uint32_t cap,
                         const scene_rect *r)
{
    int32_t x0, y0, x1, y1;
    scene_rect *last;

    if (!r || r->w <= 0 || r->h <= 0) return;
    if (*count < cap) {
        list[*count] = *r;
        (*count)++;
        return;
    }
    last = &list[cap - 1u];
    x0 = last->x < r->x ? last->x : r->x;
    y0 = last->y < r->y ? last->y : r->y;
    x1 = last->x + last->w > r->x + r->w ? last->x + last->w : r->x + r->w;
    y1 = last->y + last->h > r->y + r->h ? last->y + last->h : r->y + r->h;
    last->x = x0;
    last->y = y0;
    last->w = x1 - x0;
    last->h = y1 - y0;
}

/* Frame output (reported to the backend, rebuilt every frame).         */
static void damage_add(scene_compositor *cp, const scene_rect *r)
{
    rectlist_add(cp->damage, &cp->damage_count, SCENE_COMPOSITOR_MAX_DAMAGE,
                 r);
}

/* Compositor-side offers (texture/style changes between frames): they
 * survive the per-frame report reset and are merged on the next frame. */
static void pending_add(scene_compositor *cp, const scene_rect *r)
{
    rectlist_add(cp->pending, &cp->pending_count,
                 SCENE_COMPOSITOR_MAX_DAMAGE, r);
}

static void damage_rect(scene_compositor *cp, const int32_t r[4])
{
    scene_rect rc;

    rc.x = r[0];
    rc.y = r[1];
    rc.w = r[2];
    rc.h = r[3];
    damage_add(cp, &rc);
}

static void pending_rect(scene_compositor *cp, const int32_t r[4])
{
    scene_rect rc;

    rc.x = r[0];
    rc.y = r[1];
    rc.w = r[2];
    rc.h = r[3];
    pending_add(cp, &rc);
}

/* ---- Render model (open addressing, power-of-two, multiplicative) ---- */

static uint32_t map_slot(const scene_layer *ly, scene_node_id id)
{
    return (uint32_t)((uint64_t)id * UINT32_C(2654435761))
           >> (32u - ly->map_shift);
}

static scene_rnode *map_find(const scene_layer *ly, scene_node_id id)
{
    uint32_t i, n;

    n = ly->map_cap;
    for (i = 0; i < n; i++) {
        scene_rnode *rn = &ly->map[(map_slot(ly, id) + i) & (n - 1u)];
        if (!rn->used) return NULL;
        if (rn->id == id) return rn;
    }
    return NULL;
}

static int map_grow(scene_layer *ly)
{
    uint32_t ncap = ly->map_cap * 2u;
    scene_rnode *nmap;
    uint32_t i;

    nmap = calloc(ncap, sizeof(*nmap));
    if (!nmap) return -1;
    for (i = 0; i < ly->map_cap; i++) {
        const scene_rnode *rn = &ly->map[i];
        uint32_t j;

        if (!rn->used) continue;
        for (j = 0; j < ncap; j++) {
            uint32_t slot = (uint32_t)((uint64_t)rn->id
                                       * UINT32_C(2654435761))
                            >> (32u - (ly->map_shift + 1u));
            scene_rnode *dst = &nmap[(slot + j) & (ncap - 1u)];
            if (!dst->used) {
                *dst = *rn;
                break;
            }
        }
    }
    free(ly->map);
    ly->map = nmap;
    ly->map_cap = ncap;
    ly->map_shift += 1u;
    return 0;
}

static scene_rnode *map_insert(scene_layer *ly, scene_node_id id)
{
    uint32_t i;

    if (ly->map_used * 10u >= ly->map_cap * 7u) {
        if (map_grow(ly) != 0) return NULL;
    }
    for (i = 0; i < ly->map_cap; i++) {
        scene_rnode *rn = &ly->map[(map_slot(ly, id) + i)
                                   & (ly->map_cap - 1u)];
        if (!rn->used) {
            memset(rn, 0, sizeof(*rn));
            rn->id = id;
            rn->used = 1;
            ly->map_used++;
            return rn;
        }
        if (rn->id == id) return rn;
    }
    return NULL;
}

/* ---- Transitions (enter/exit) ─────────────────────────────────────── */

static int anim_replaying(const scene_layer *ly)
{
    return scene_store_in_replay(ly->store);
}

static anim_ent *anim_find(const scene_layer *ly, scene_node_id id)
{
    uint32_t i;

    for (i = 0; i < SCENE_COMPOSITOR_ANIM_CAP; i++)
        if (ly->anims[i].active && ly->anims[i].id == id)
            return (anim_ent *)&ly->anims[i];
    return NULL;
}

static void anim_free(scene_layer *ly, anim_ent *an)
{
    if (an->tbuf) {
        free(an->tbuf);
        an->tbuf = NULL;
    }
    if (an->active) ly->anim_used--;
    memset(an, 0, sizeof(*an));
}

static anim_ent *anim_alloc(scene_compositor *cp, scene_layer *ly,
                            scene_node_id id, int kind)
{
    anim_ent *an = anim_find(ly, id);
    uint32_t i;

    if (an) {
        /* If a live enter is being replaced (destroy/revive), clear its
         * slide tail so stale pixels don't linger below the base rect. */
        if (an->kind == SCENE_ANIM_ENTER
            && an->t < SCENE_COMPOSITOR_ANIM_TICKS) {
            int32_t r[4] = { an->base[0], an->base[1], an->base[2],
                             an->base[3] + SCENE_COMPOSITOR_ANIM_SLIDE };
            damage_rect(cp, r);
        }
        anim_free(ly, an);
    }
    for (i = 0; i < SCENE_COMPOSITOR_ANIM_CAP; i++)
        if (!ly->anims[i].active) break;
    if (i == SCENE_COMPOSITOR_ANIM_CAP) return NULL;  /* full: no effect */
    an = &ly->anims[i];
    memset(an, 0, sizeof(*an));
    an->id = id;
    an->kind = (uint8_t)kind;
    an->active = 1;
    ly->anim_used++;
    return an;
}

/* Clear all active transitions of a layer and damage their footprints so
 * the next repaint re-establishes the correct post-fade state (phantom →
 * live scene or desktop). Used when effects are turned off and when
 * replay mode begins.                                                 */
static void anim_clear_all(scene_compositor *cp, scene_layer *ly)
{
    uint32_t i;

    for (i = 0; i < SCENE_COMPOSITOR_ANIM_CAP; i++) {
        if (ly->anims[i].active) {
            damage_rect(cp, ly->anims[i].base);
            anim_free(ly, &ly->anims[i]);
        }
    }
}

/* Copy the node's committed texts into the render entry. The store lets
 * a destroyed node's texts die with it, so the model keeps its own copy
 * to feed the exit fade. Copy only when the signature actually moved.  */
static void rn_text_capture(scene_layer *ly, scene_rnode *rn,
                            scene_node_id id)
{
    scene_node_text_vis t[SCENE_COMPOSITOR_TEXT_CAP];
    char *p;
    uint32_t i, total = 0;
    int n = scene_store_node_texts(ly->store, id, t,
                                   SCENE_COMPOSITOR_TEXT_CAP);

    if (n < 0) n = 0;
    if ((uint32_t)n > SCENE_COMPOSITOR_TEXT_CAP) n = SCENE_COMPOSITOR_TEXT_CAP;
    for (i = 0; i < (uint32_t)n; i++) total += t[i].len;
    if (rn->tx_data && (rn->tx_count != (uint32_t)n || rn->tx_len != total)) {
        free(rn->tx_data);
        rn->tx_data = NULL;
    }
    if (total && !rn->tx_data) {
        rn->tx_data = malloc(total);
        if (!rn->tx_data) {           /* keep old strings if alloc fails */
            rn->tx_count = 0;
            rn->tx_len = 0;
            return;
        }
    }
    p = rn->tx_data;
    for (i = 0; i < (uint32_t)n; i++) {
        rn->tx_lens[i] = t[i].len;
        if (t[i].len) {
            memcpy(p, t[i].data, t[i].len);
            p += t[i].len;
        }
    }
    rn->tx_count = (uint32_t)n;
    rn->tx_len = total;
}

/* The EXIT phantom owns its own copy of the model's last text blob.    */
static void anim_snapshot_from_rn(anim_ent *an, const scene_rnode *rn)
{
    uint32_t i, off = 0;

    an->text_count = 0;
    if (!rn->tx_data || rn->tx_count == 0) return;
    an->tbuf = malloc(rn->tx_len + rn->tx_count);   /* NULs between slots */
    if (!an->tbuf) return;
    an->text_count = rn->tx_count;
    for (i = 0; i < rn->tx_count; i++) {
        memcpy(an->tbuf + off, rn->tx_data + (size_t)off, rn->tx_lens[i]);
        an->tbuf[off + rn->tx_lens[i]] = 0;
        an->t_snap[i].len = rn->tx_lens[i];
        an->t_snap[i].data = an->tbuf + off;
        off += rn->tx_lens[i] + 1u;
    }
}

/* Advance all transitions of a layer one tick and collect their damage.
 *
 * ENTER: frames 1..6 (off>0) damage base + sweep (2 rects); frame 7
 * (off==0) damages base only; the free frame (t==TICKS) damages the
 * full slide band [base.y, base.y+h+SLIDE] — one rect that covers
 * both the settled node and the tail rows it occupied while below its
 * final spot during the slide.  Without the band, those tail rows
 * hold stale enter-fade pixels after the transition ends.
 *
 * EXIT: every frame damages base; the free frame's base damage
 * repaints the cleared area (phantom gone).                             */
static void anim_advance(scene_compositor *cp, scene_layer *ly)
{
    uint32_t i;

    for (i = 0; i < SCENE_COMPOSITOR_ANIM_CAP; i++) {
        anim_ent *an = &ly->anims[i];
        int32_t off;

        if (!an->active) continue;
        an->t++;
        if (an->kind == SCENE_ANIM_ENTER) {
            if (an->t >= SCENE_COMPOSITOR_ANIM_TICKS) {
                int32_t r[4] = { an->base[0], an->base[1], an->base[2],
                                 an->base[3] + SCENE_COMPOSITOR_ANIM_SLIDE };
                damage_rect(cp, r);
            } else {
                off = ((int32_t)SCENE_COMPOSITOR_ANIM_TICKS - (int32_t)an->t)
                      * (int32_t)SCENE_COMPOSITOR_ANIM_SLIDE
                      / (int32_t)SCENE_COMPOSITOR_ANIM_TICKS;
                if (off == 0) {
                    damage_rect(cp, an->base);
                } else {
                    int32_t r[4] = { an->base[0], an->base[1] + off,
                                     an->base[2], an->base[3] };
                    damage_rect(cp, an->base);
                    damage_rect(cp, r);
                }
            }
        } else {
            damage_rect(cp, an->base);
        }
        if (an->t >= SCENE_COMPOSITOR_ANIM_TICKS)
            anim_free(ly, an);
    }
}

/* ---- Text content signature (repaint on SetText without geometry) ---- */

static uint32_t text_sig(const scene_layer *ly, scene_node_id id)
{
    scene_node_text_vis t[SCENE_COMPOSITOR_TEXT_CAP];
    uint32_t h = UINT32_C(0x811C9DC5);
    int n, i;

    n = scene_store_node_texts(ly->store, id, t, SCENE_COMPOSITOR_TEXT_CAP);
    if (n < 0) return h;
    for (i = 0; i < n; i++) {
        const scene_node_text_vis *tv = &t[i];
        uint8_t buf[40];
        uint32_t take = tv->len < 32u ? tv->len : 32u;
        uint32_t j;

        scene_put_u32(buf, tv->text_id);
        scene_put_u32(buf + 4, tv->len);
        for (j = 0; j < take; j++) buf[8 + j] = (uint8_t)tv->data[j];
        h ^= scene_fnv1a32(buf, 8u + take);
        h = h * UINT32_C(16777619);
    }
    h ^= (uint32_t)n;
    return h;
}

/* ---- Style resolution ------------------------------------------------ */

static const scene_style *resolve_style(const scene_compositor *cp,
                                        const scene_node_vis *v)
{
    if (v->style >= 1u && v->style < cp->style_count)
        return &cp->styles[v->style];
    if (v->role <= SCENE_ROLE_MAX)
        return &role_defaults[v->role];
    return &role_defaults[SCENE_ROLE_GENERIC];
}

/* ---- Painting -------------------------------------------------------- */

static int rects_intersect(const scene_rect *a, const scene_rect *b)
{
    int64_t x0, y0, x1, y1;

    if (a->w <= 0 || a->h <= 0) return 0;
    if (b && (b->w <= 0 || b->h <= 0)) return 0;
    x0 = a->x > b->x ? a->x : b->x;
    y0 = a->y > b->y ? a->y : b->y;
    x1 = a->x + a->w < b->x + b->w ? a->x + a->w : b->x + b->w;
    y1 = a->y + a->h < b->y + b->h ? a->y + a->h : b->y + b->h;
    return x0 < x1 && y0 < y1;
}

static void clip_intersect(scene_rect *out, const scene_rect *a,
                           const scene_rect *b)
{
    int64_t x0, y0, x1, y1;

    x0 = a->x;
    y0 = a->y;
    x1 = (int64_t)a->x + a->w;
    y1 = (int64_t)a->y + a->h;
    if (b) {
        if (x0 < b->x) x0 = b->x;
        if (y0 < b->y) y0 = b->y;
        if (x1 > (int64_t)b->x + b->w) x1 = (int64_t)b->x + b->w;
        if (y1 > (int64_t)b->y + b->h) y1 = (int64_t)b->y + b->h;
    }
    out->x = (int32_t)x0;
    out->y = (int32_t)y0;
    out->w = (int32_t)(x1 - x0);
    out->h = (int32_t)(y1 - y0);
}

static scene_tex_ent *tex_find(scene_compositor *cp, scene_texture_ref ref)
{
    uint32_t i;

    for (i = 0; i < cp->tex_cap; i++)
        if (cp->tex_ents[i].used && cp->tex_ents[i].ref == ref)
            return &cp->tex_ents[i];
    return NULL;
}

/* Apply the entering transition to a live node: fills `out` with the
 * paint rect and `eff` with the effect alpha factor (255 = settled).
 * The node's own opacity only ever scales the texture (engine v1 rule);
 * fills/strokes/text render at the effect factor only.                 */
static void anim_live_geom(const scene_compositor *cp, const scene_layer *ly,
                           scene_node_id id,
                           const int32_t base[4], int32_t out[4],
                           uint32_t *eff)
{
    const anim_ent *an;
    int32_t off;

    memcpy(out, base, sizeof(int32_t[4]));
    *eff = 255u;
    if (!cp->effects_on) return;
    an = anim_find(ly, id);
    if (!an || an->kind != SCENE_ANIM_ENTER) return;
    off = ((int32_t)SCENE_COMPOSITOR_ANIM_TICKS - (int32_t)an->t)
          * (int32_t)SCENE_COMPOSITOR_ANIM_SLIDE
          / (int32_t)SCENE_COMPOSITOR_ANIM_TICKS;
    out[0] = an->base[0];
    out[1] = an->base[1] + off;
    out[2] = an->base[2];
    out[3] = an->base[3];
    *eff = (uint32_t)an->t * 255u / SCENE_COMPOSITOR_ANIM_TICKS;
    if (*eff > 255u) *eff = 255u;
}

/* Paint a node's chrome (fill + border) honoring radius and alpha.
 * Rounded corners: the STROKE FOLLOWS THE ROUNDED CORNER. The ring is
 * painted as one single-blend-per-pixel pass (scene_fb_chrome_round):
 * interior pixels blend the fill once, ring pixels blend the border
 * once, corner notches stay clear. Single pass matters for fades — a
 * border-then-fill two-pass would double-blend the interior. Radius 0
 * keeps the classic fill-over-stroke (those shapes don't overlap).    */
static void style_chrome(scene_fb *fb, const scene_rect *rc,
                         const scene_style *st, uint32_t alpha,
                         const scene_rect *clip)
{
    int32_t bw = (st->border_w && st->border) ? st->border_w : 0;

    if (st->radius > 0) {
        scene_fb_chrome_round(fb, rc, st->radius, (uint8_t)bw, st->fill,
                              bw ? st->border : st->fill, alpha, clip);
    } else {
        if (st->fill)
            scene_fb_fill_a(fb, rc, st->fill, alpha, clip);
        if (bw)
            scene_fb_stroke_a(fb, rc, st->border, (uint8_t)bw, alpha, clip);
    }
}

static void paint_node(scene_compositor *cp, const scene_layer *ly,
                       const scene_node_vis *v, const scene_rect *clip)
{
    scene_rect rc, c;
    const scene_style *st;
    scene_node_text_vis t[SCENE_COMPOSITOR_TEXT_CAP];
    int32_t r[4];
    uint32_t eff;
    int n, i;

    if (!(v->flags & SCENE_FLAG_VISIBLE) || v->opacity == 0) return;
    if (v->rect[2] <= 0 || v->rect[3] <= 0) return;

    anim_live_geom(cp, ly, v->id, v->rect, r, &eff);
    if (eff == 0) return;

    rc.x = r[0];
    rc.y = r[1];
    rc.w = r[2];
    rc.h = r[3];
    clip_intersect(&c, &rc, clip);
    if (c.w <= 0 || c.h <= 0) return;

    st = resolve_style(cp, v);
    if (st->fill || (st->border_w && st->border))
        style_chrome(&cp->fb, &rc, st, eff, &c);
    if (v->tex != SCENE_NO_TEXTURE) {
        scene_tex_ent *te = tex_find(cp, v->tex);
        if (te) {
            scene_rect src;
            src.x = v->tex_src[0];
            src.y = v->tex_src[1];
            src.w = v->tex_src[2];
            src.h = v->tex_src[3];
            scene_fb_blit(&cp->fb, r[0], r[1], te->px, te->w, te->h,
                          &src, (uint8_t)(v->opacity * eff / 255u), te->fmt,
                          &c);
        }
    }
    n = scene_store_node_texts(ly->store, v->id, t,
                               SCENE_COMPOSITOR_TEXT_CAP);
    if (n < 0) n = 0;
    if ((uint32_t)n > SCENE_COMPOSITOR_TEXT_CAP) n = (int)SCENE_COMPOSITOR_TEXT_CAP;
    for (i = 0; i < n; i++) {
        if (t[i].len == 0) continue;
        scene_font_draw_a(&cp->fb, r[0] + st->pad_x,
                          r[1] + st->pad_y + (int32_t)i * SCENE_FONT_GLYPH_H,
                          t[i].data, t[i].len, st->text, eff, &c);
    }
}

static int paint_cb(scene_node_id id, void *out)
{
    scene_compositor *cp = out;
    scene_layer *ly = cp->walk_ly;
    scene_node_vis v;
    int32_t r[4];
    uint32_t a;

    if (scene_store_node_vis(ly->store, id, &v) != 0) return 0;
    if (!(v.flags & SCENE_FLAG_VISIBLE) || v.opacity == 0) return 0;
    if (v.rect[2] <= 0 || v.rect[3] <= 0) return 0;
    anim_live_geom(cp, ly, id, v.rect, r, &a);
    if (a == 0) return 0;
    {
        scene_rect rc;
        rc.x = r[0];
        rc.y = r[1];
        rc.w = r[2];
        rc.h = r[3];
        if (!rects_intersect(&rc, &cp->paint_clip)) return 0;
    }
    paint_node(cp, ly, &v, &cp->paint_clip);
    return 0;
}

/* Draw the fading visual of a deleted node (its texts and colors were
 * snapshotted at destroy time; the store no longer knows it).          */
static void paint_phantom(scene_compositor *cp, const anim_ent *an,
                          const scene_rect *clip)
{
    scene_rect rc, c;
    uint32_t alpha;
    int i;

    rc.x = an->base[0];
    rc.y = an->base[1];
    rc.w = an->base[2];
    rc.h = an->base[3];
    if (rc.w <= 0 || rc.h <= 0) return;
    clip_intersect(&c, &rc, clip);
    if (c.w <= 0 || c.h <= 0) return;

    alpha = ((uint32_t)SCENE_COMPOSITOR_ANIM_TICKS - an->t) * 255u
            / SCENE_COMPOSITOR_ANIM_TICKS;
    if (alpha > 255u) alpha = 255u;
    if (alpha == 0) return;

    if (an->fill || (an->border_w && an->border)) {
        scene_style st;

        memset(&st, 0, sizeof(st));
        st.fill = an->fill;
        st.border = an->border;
        st.text = an->text;
        st.border_w = an->border_w;
        st.radius = an->radius;
        style_chrome(&cp->fb, &rc, &st, alpha, &c);
    }
    if (an->tex != SCENE_NO_TEXTURE) {
        scene_tex_ent *te = tex_find(cp, an->tex);
        if (te) {
            scene_rect src;
            src.x = 0;
            src.y = 0;
            src.w = te->w;
            src.h = te->h;
            scene_fb_blit(&cp->fb, rc.x, rc.y, te->px, te->w, te->h,
                          &src, (uint8_t)(an->opacity * alpha / 255u),
                          te->fmt, &c);
        }
    }
    for (i = 0; i < (int)an->text_count; i++) {
        if (an->t_snap[i].len == 0) continue;
        scene_font_draw_a(&cp->fb, rc.x,
                          rc.y + (int32_t)i * SCENE_FONT_GLYPH_H,
                          an->t_snap[i].data, an->t_snap[i].len,
                          an->text, alpha, &c);
    }
}

/* Fade the last visible state of deleted nodes of a layer over the
 * live scene.                                                          */
static void anim_paint_exits(scene_compositor *cp, const scene_layer *ly,
                             const scene_rect *clip)
{
    uint32_t i;

    if (!cp->effects_on) return;
    for (i = 0; i < SCENE_COMPOSITOR_ANIM_CAP; i++)
        if (ly->anims[i].active && ly->anims[i].kind == SCENE_ANIM_EXIT)
            paint_phantom(cp, &ly->anims[i], clip);
}

static void repaint_rect(scene_compositor *cp, const scene_rect *r)
{
    uint32_t i;

    scene_fb_fill(&cp->fb, r, cp->clear, NULL);
    cp->paint_clip = *r;
    for (i = 0; i < cp->ly_count; i++) {
        scene_layer *ly = &cp->ly[i];

        if (ly->dead) continue;
        cp->walk_ly = ly;
        scene_store_walk(ly->store, paint_cb, cp);
        anim_paint_exits(cp, ly, r);
    }
}

static void repaint_all(scene_compositor *cp)
{
    scene_rect full;
    uint32_t i;

    scene_fb_clear(&cp->fb, cp->clear);
    full.x = 0;
    full.y = 0;
    full.w = (int32_t)cp->fb.w;
    full.h = (int32_t)cp->fb.h;
    cp->paint_clip = full;
    for (i = 0; i < cp->ly_count; i++) {
        scene_layer *ly = &cp->ly[i];

        if (ly->dead) continue;
        cp->walk_ly = ly;
        scene_store_walk(ly->store, paint_cb, cp);
        anim_paint_exits(cp, ly, &full);
    }
}

/* ---- Diff walk (per layer) ------------------------------------------- */

static int diff_cb(scene_node_id id, void *out)
{
    scene_compositor *cp = out;
    scene_layer *ly = cp->walk_ly;
    scene_node_vis v;
    scene_rnode *rn;
    anim_ent *an;
    uint32_t sig;
    int changed;

    if (scene_store_node_vis(ly->store, id, &v) != 0) return 0;
    rn = map_find(ly, id);
    if (!rn) {
        rn = map_insert(ly, id);
        if (!rn) return 0;
        rn->seen = 1;
        rn->role = v.role;
        rn->flags = v.flags;
        rn->style = v.style;
        rn->tex = v.tex;
        rn->blend = v.blend;
        rn->opacity = v.opacity;
        rn->sig = text_sig(ly, id);
        memcpy(rn->rect, v.rect, sizeof(rn->rect));
        rn_text_capture(ly, rn, id);
        if (cp->effects_on && !anim_replaying(ly)
            && (v.flags & SCENE_FLAG_VISIBLE)
            && v.rect[2] > 0 && v.rect[3] > 0) {
            /* fade+slide in; anim_advance adds the swept rects */
            an = anim_alloc(cp, ly, id, SCENE_ANIM_ENTER);
            if (an) memcpy(an->base, v.rect, sizeof(an->base));
        } else if (v.flags & SCENE_FLAG_VISIBLE) {
            damage_rect(cp, v.rect);
        }
        return 0;
    }
    rn->seen = 1;
    sig = text_sig(ly, id);
    changed = (rn->role != v.role || rn->style != v.style || rn->tex != v.tex
               || rn->flags != v.flags || rn->sig != sig
               || memcmp(rn->rect, v.rect, sizeof(rn->rect)) != 0);
    rn->blend = v.blend;
    rn->opacity = v.opacity;
    if (!changed) return 0;

    /* If a node resurfaces during its exit fade, revive it as an enter. */
    if (cp->effects_on) {
        an = anim_find(ly, id);
        if (an) {
            if (an->kind == SCENE_ANIM_EXIT) {
                anim_free(ly, an);
                if (!anim_replaying(ly) && (v.flags & SCENE_FLAG_VISIBLE)
                    && v.rect[2] > 0 && v.rect[3] > 0) {
                    an = anim_alloc(cp, ly, id, SCENE_ANIM_ENTER);
                    if (an) memcpy(an->base, v.rect, sizeof(an->base));
                }
            } else {
                memcpy(an->base, v.rect, sizeof(an->base));
            }
        }
    }
    if (rn->sig != sig)
        rn_text_capture(ly, rn, id);

    /* Damage the old and/or new rect exactly as needed: a content-only
     * change (same rect) damages once; a move damages old+new.        */
    {
        int had_old = (rn->flags & SCENE_FLAG_VISIBLE)
                      && rn->rect[2] > 0 && rn->rect[3] > 0;
        int has_new = (v.flags & SCENE_FLAG_VISIBLE)
                      && v.rect[2] > 0 && v.rect[3] > 0;
        if (had_old) {
            if (has_new && memcmp(rn->rect, v.rect, sizeof(rn->rect)) == 0)
                damage_rect(cp, v.rect);
            else {
                damage_rect(cp, rn->rect);
                if (has_new) damage_rect(cp, v.rect);
            }
        } else if (has_new) {
            damage_rect(cp, v.rect);
        }
    }

    rn->role = v.role;
    rn->flags = v.flags;
    rn->style = v.style;
    rn->tex = v.tex;
    rn->sig = sig;
    memcpy(rn->rect, v.rect, sizeof(rn->rect));
    return 0;
}

/* Nodes gone from a layer's store: damage their last rect. With effects
 * on and not replaying, a visible gone node becomes an exit fade (its
 * visuals were snapshotted in the model).                              */
static void map_sweep(scene_compositor *cp, scene_layer *ly)
{
    uint32_t i;
    int replaying = anim_replaying(ly);

    for (i = 0; i < ly->map_cap; i++) {
        scene_rnode *rn = &ly->map[i];
        if (!rn->used) continue;
        if (!rn->seen) {
            int was_vis = (rn->flags & SCENE_FLAG_VISIBLE)
                          && rn->rect[2] > 0 && rn->rect[3] > 0;
            if (was_vis && cp->effects_on && !replaying) {
                scene_node_vis v;
                const scene_style *st;
                anim_ent *an = anim_alloc(cp, ly, rn->id, SCENE_ANIM_EXIT);

                if (an) {
                    memset(&v, 0, sizeof(v));
                    v.role = rn->role;
                    v.style = rn->style;
                    v.tex = rn->tex;
                    v.opacity = rn->opacity;
                    st = resolve_style(cp, &v);
                    memcpy(an->base, rn->rect, sizeof(an->base));
                    an->fill = st->fill;
                    an->border = st->border;
                    an->text = st->text;
                    an->border_w = st->border_w;
                    an->radius = st->radius;
                    an->opacity = rn->opacity;
                    an->tex = rn->tex;
                    anim_snapshot_from_rn(an, rn);
                    /* anim_advance adds the fade rect to this frame */
                } else {
                    damage_rect(cp, rn->rect);
                }
            } else if (was_vis) {
                damage_rect(cp, rn->rect);
            }
            free(rn->tx_data);
            rn->tx_data = NULL;
            rn->tx_count = 0;
            rn->tx_len = 0;
            rn->used = 0;
            ly->map_used--;
        } else {
            rn->seen = 0;
        }
    }
}

/* ---- Layer lifecycle -------------------------------------------------- */

static void layer_free(scene_layer *ly)
{
    uint32_t i;

    for (i = 0; i < ly->map_cap; i++)
        free(ly->map[i].tx_data);
    free(ly->map);
    ly->map = NULL;
    ly->map_cap = 0;
    for (i = 0; i < SCENE_COMPOSITOR_ANIM_CAP; i++)
        free(ly->anims[i].tbuf);
    if (ly->sv) scene_server_free(ly->sv);
    ly->sv = NULL;
    ly->store = NULL;
}

static int layer_init(scene_layer *ly, scene_server *sv)
{
    memset(ly, 0, sizeof(*ly));
    ly->sv = sv;
    ly->store = sv ? scene_server_store(sv) : NULL;
    ly->map_cap = 64u;
    ly->map_shift = 6u;
    ly->map = calloc(ly->map_cap, sizeof(*ly->map));
    if (!ly->map) {
        ly->map_cap = 0;
        return -1;
    }
    return 0;
}

/* ---- Public API ------------------------------------------------------ */

scene_compositor *scene_compositor_new(const scene_limits *limits,
                                       uint32_t fb_w, uint32_t fb_h)
{
    scene_compositor *cp;

    cp = calloc(1, sizeof(*cp));
    if (!cp) return NULL;
    cp->ly_cap = SCENE_COMPOSITOR_LAYER_CAP0;
    cp->ly = calloc(cp->ly_cap, sizeof(*cp->ly));
    if (!cp->ly) {
        free(cp);
        return NULL;
    }
    cp->ly[0].sv = scene_server_new(limits);
    if (!cp->ly[0].sv) {
        free(cp->ly);
        free(cp);
        return NULL;
    }
    if (layer_init(&cp->ly[0], cp->ly[0].sv) != 0) {
        scene_server_free(cp->ly[0].sv);
        free(cp->ly);
        free(cp);
        return NULL;
    }
    cp->ly_count = 1;
    cp->clear = UINT32_C(0xFF101010);
    if (scene_fb_init(&cp->fb, fb_w, fb_h) != 0) {
        layer_free(&cp->ly[0]);
        free(cp->ly);
        free(cp);
        return NULL;
    }
    return cp;
}

void scene_compositor_free(scene_compositor *cp)
{
    uint32_t i;

    if (!cp) return;
    for (i = 0; i < cp->tex_cap; i++)
        free(cp->tex_ents[i].px);
    free(cp->tex_ents);
    free(cp->styles);
    for (i = 0; i < cp->ly_count; i++)
        layer_free(&cp->ly[i]);
    free(cp->ly);
    scene_fb_free(&cp->fb);
    free(cp);
}

scene_store *scene_compositor_store(scene_compositor *cp)
{
    return cp ? cp->ly[0].store : NULL;
}

scene_server *scene_compositor_server(scene_compositor *cp)
{
    return cp ? cp->ly[0].sv : NULL;
}

int scene_compositor_add_session(scene_compositor *cp, scene_server *sv)
{
    scene_layer *nl;

    if (!cp || !sv) return 0;
    if (cp->ly_count == cp->ly_cap) {
        uint32_t ncap = cp->ly_cap * 2u;
        nl = realloc(cp->ly, ncap * sizeof(*cp->ly));
        if (!nl) return 0;
        memset(&nl[cp->ly_cap], 0, (ncap - cp->ly_cap) * sizeof(*cp->ly));
        cp->ly = nl;
        cp->ly_cap = ncap;
    }
    if (layer_init(&cp->ly[cp->ly_count], sv) != 0) return 0;
    cp->ly_count++;
    return (int)cp->ly_count - 1;
}

int scene_compositor_remove_session(scene_compositor *cp, scene_server *sv)
{
    uint32_t i;
    uint32_t layer;

    if (!cp || !sv) return -1;
    for (i = 1; i < cp->ly_count; i++)
        if (cp->ly[i].sv == sv) break;
    if (i == cp->ly_count) return -1;
    layer = i;
    layer_free(&cp->ly[layer]);
    for (i = (uint32_t)layer + 1u; i < cp->ly_count; i++)
        cp->ly[i - 1u] = cp->ly[i];
    cp->ly_count--;
    memset(&cp->ly[cp->ly_count], 0, sizeof(*cp->ly));
    if (cp->focus_layer >= cp->ly_count) cp->focus_layer = 0;
    cp->force = 1;   /* the removed layer's area repaints as the desktop */
    return (int)layer;
}

int scene_compositor_focus_is_shell(scene_compositor *cp)
{
    return cp ? (cp->focus_layer == 0) : 1;
}

void scene_compositor_resize(scene_compositor *cp, uint32_t w, uint32_t h)
{
    if (!cp) return;
    if (cp->fb.w == w && cp->fb.h == h) return;
    scene_fb_free(&cp->fb);
    if (scene_fb_init(&cp->fb, w, h) != 0) return;
    cp->force = 1;
}

void scene_compositor_set_clear(scene_compositor *cp, uint32_t color)
{
    if (!cp) return;
    if (cp->clear == color) return;
    cp->clear = color;
    cp->force = 1;
}

int scene_compositor_register_texture(scene_compositor *cp,
                                      scene_texture_ref ref,
                                      uint32_t w, uint32_t h, uint16_t fmt,
                                      uint8_t opaque, const uint32_t *pixels)
{
    scene_tex_ent *te;
    uint32_t n, i, li;

    if (!cp || !pixels || w == 0 || h == 0) return -1;
    if (fmt != SCENE_TEX_FMT_XRGB && fmt != SCENE_TEX_FMT_ARGB) return -1;
    n = (uint64_t)w * h > SIZE_MAX / sizeof(uint32_t) ? 0 : w * h;
    if (n == 0) return -1;

    te = tex_find(cp, ref);
    if (!te) {
        if (cp->tex_cap == 0) {
            cp->tex_cap = 8u;
            cp->tex_ents = calloc(cp->tex_cap, sizeof(*cp->tex_ents));
            if (!cp->tex_ents) return -1;
            te = &cp->tex_ents[0];
        } else {
            uint32_t cap = cp->tex_cap;
            scene_tex_ent *ne;

            for (i = 0; i < cp->tex_cap; i++)
                if (!cp->tex_ents[i].used) break;
            if (i == cp->tex_cap) {
                ne = realloc(cp->tex_ents, cap * 2u * sizeof(*cp->tex_ents));
                if (!ne) return -1;
                cp->tex_ents = ne;
                memset(&cp->tex_ents[cap], 0, cap * sizeof(*cp->tex_ents));
                cp->tex_cap = cap * 2u;
                te = &cp->tex_ents[cp->tex_cap - 1u];
            } else {
                te = &cp->tex_ents[i];
            }
        }
    }
    /* Ref change or size change: reallocate the pixel buffer.           */
    if (te->px && (te->w != w || te->h != h)) {
        free(te->px);
        te->px = NULL;
    }
    if (!te->px) {
        te->px = malloc(n * sizeof(uint32_t));
        if (!te->px) return -1;
    }
    memcpy(te->px, pixels, n * sizeof(uint32_t));
    te->ref = ref;
    te->w = w;
    te->h = h;
    te->fmt = fmt;
    te->opaque = opaque;
    te->used = 1;
    if (!te->store_registered) {
        if (scene_store_register_texture(cp->ly[0].store, ref, w, h, fmt,
                                         opaque) != 0)
            return -1;
        te->store_registered = 1;
    }

    /* New pixels may change what the scene shows: dirty referencing
     * model entries in every layer (server-owned texture update).       */
    for (li = 0; li < cp->ly_count; li++) {
        scene_layer *ly = &cp->ly[li];
        for (i = 0; i < ly->map_cap; i++) {
            scene_rnode *rn = &ly->map[i];
            if (rn->used && rn->tex == ref
                && (rn->flags & SCENE_FLAG_VISIBLE)
                && rn->rect[2] > 0 && rn->rect[3] > 0)
                pending_rect(cp, rn->rect);
        }
    }
    return 0;
}

int scene_compositor_release_texture(scene_compositor *cp,
                                     scene_texture_ref ref)
{
    scene_tex_ent *te;
    uint32_t i, li;

    if (!cp) return -1;
    te = tex_find(cp, ref);
    if (!te) return -1;
    for (li = 0; li < cp->ly_count; li++) {
        scene_layer *ly = &cp->ly[li];
        for (i = 0; i < ly->map_cap; i++) {
            scene_rnode *rn = &ly->map[i];
            if (rn->used && rn->tex == ref
                && (rn->flags & SCENE_FLAG_VISIBLE)
                && rn->rect[2] > 0 && rn->rect[3] > 0)
                pending_rect(cp, rn->rect);
        }
    }
    if (te->store_registered)
        scene_store_release_texture(cp->ly[0].store, ref);
    free(te->px);
    memset(te, 0, sizeof(*te));
    return 0;
}

void scene_compositor_set_style_count(scene_compositor *cp, uint32_t n)
{
    scene_style *ns;

    if (!cp) return;
    scene_store_set_style_count(cp->ly[0].store, n);
    if (n == cp->style_count) return;
    if (n == 0) {
        free(cp->styles);
        cp->styles = NULL;
        cp->style_count = 0;
        return;
    }
    ns = realloc(cp->styles, n * sizeof(*ns));
    if (!ns) return;
    if (n > cp->style_count)
        memset(&ns[cp->style_count], 0, (n - cp->style_count) * sizeof(*ns));
    cp->styles = ns;
    cp->style_count = n;
}

int scene_compositor_set_style(scene_compositor *cp, scene_style_ref ref,
                               const scene_style *st)
{
    scene_style *old;
    uint32_t i, li;

    if (!cp || !st) return -1;
    if (ref == 0 || ref >= cp->style_count) return -1;
    old = &cp->styles[ref];
    if (memcmp(old, st, sizeof(*st)) == 0) return 0;
    *old = *st;
    for (li = 0; li < cp->ly_count; li++) {
        scene_layer *ly = &cp->ly[li];
        for (i = 0; i < ly->map_cap; i++) {
            scene_rnode *rn = &ly->map[i];
            if (rn->used && rn->style == ref
                && (rn->flags & SCENE_FLAG_VISIBLE)
                && rn->rect[2] > 0 && rn->rect[3] > 0)
                pending_rect(cp, rn->rect);
        }
    }
    return 0;
}

int scene_compositor_input_pointer(scene_compositor *cp, uint8_t device,
                                   int32_t x, int32_t y, uint8_t buttons)
{
    uint32_t i;

    if (!cp) return -1;
    /* Hit-test app layers topmost first; the first session owning the
     * point receives the event and becomes the keyboard focus.       */
    for (i = cp->ly_count; i > 1; i--) {
        scene_layer *ly = &cp->ly[i - 1u];

        if (ly->dead || scene_server_dead(ly->sv)) continue;
        if (scene_store_region_at(ly->store, x, y) != SCENE_NO_PARENT) {
            cp->focus_layer = i - 1u;
            return scene_server_input_pointer(ly->sv, device, x, y, buttons);
        }
    }
    cp->focus_layer = 0;
    return scene_server_input_pointer(cp->ly[0].sv, device, x, y, buttons);
}

int scene_compositor_input_key(scene_compositor *cp, uint32_t key_code,
                               uint8_t state, uint8_t modifiers)
{
    scene_layer *ly;

    if (!cp) return -1;
    ly = &cp->ly[0];
    if (cp->focus_layer < cp->ly_count && !cp->ly[cp->focus_layer].dead
        && !scene_server_dead(cp->ly[cp->focus_layer].sv))
        ly = &cp->ly[cp->focus_layer];
    return scene_server_input_key(ly->sv, key_code, state, modifiers);
}

int scene_compositor_frame(scene_compositor *cp)
{
    uint64_t seq;
    uint32_t d, p, i;
    int anim_active = 0, any_change = 0;

    if (!cp) return -1;
    if (scene_server_dead(cp->ly[0].sv)) return -1;
    cp->tick++;

    /* Session death: a dead app layer freezes nothing — the next frame
     * repaints its area as the desktop (its layer is skipped). The
     * shell session's death is fatal to the whole desktop.            */
    for (i = 1; i < cp->ly_count; i++) {
        scene_layer *ly = &cp->ly[i];

        if (!ly->dead && scene_server_dead(ly->sv)) {
            ly->dead = 1;
            cp->force = 1;
        }
    }

    if (cp->force) {
        cp->force = 0;
        cp->damage_count = 0;
        cp->pending_count = 0;
        repaint_all(cp);
        cp->damage[0].x = 0;
        cp->damage[0].y = 0;
        cp->damage[0].w = (int32_t)cp->fb.w;
        cp->damage[0].h = (int32_t)cp->fb.h;
        cp->damage_count = 1;
        for (i = 0; i < cp->ly_count; i++)
            cp->ly[i].rendered_seq =
                scene_store_view_seq(cp->ly[i].store);
        return 0;
    }

    for (i = 0; i < cp->ly_count; i++) {
        scene_layer *ly = &cp->ly[i];

        if (ly->dead) continue;
        if (scene_store_view_seq(ly->store) != ly->rendered_seq)
            any_change = 1;
        if (cp->effects_on && ly->anim_used > 0)
            anim_active = 1;
    }
    if (!any_change && cp->pending_count == 0 && !anim_active) {
        cp->damage_count = 0;   /* nothing new this frame */
        return 0;
    }
    cp->damage_count = 0;       /* fresh report list for this frame */
    for (i = 0; i < cp->ly_count; i++) {
        scene_layer *ly = &cp->ly[i];

        if (ly->dead) continue;
        seq = scene_store_view_seq(ly->store);
        if (seq != ly->rendered_seq) {
            cp->walk_ly = ly;
            scene_store_walk(ly->store, diff_cb, cp);
            map_sweep(cp, ly);
        }
    }
    if (cp->effects_on) {
        for (i = 0; i < cp->ly_count; i++) {
            scene_layer *ly = &cp->ly[i];

            if (ly->dead) continue;
            if (anim_replaying(ly))
                anim_clear_all(cp, ly);   /* playback: no transients */
            else
                anim_advance(cp, ly);
        }
    }
    for (p = 0; p < cp->pending_count; p++)
        damage_add(cp, &cp->pending[p]);
    cp->pending_count = 0;
    /* First content frame of the shell session: establish the desktop
     * background once. App layers damage in over it.                   */
    if (scene_store_view_seq(cp->ly[0].store) > 0
        && cp->ly[0].rendered_seq == 0)
        scene_fb_clear(&cp->fb, cp->clear);
    for (d = 0; d < cp->damage_count; d++)
        repaint_rect(cp, &cp->damage[d]);
    for (i = 0; i < cp->ly_count; i++)
        cp->ly[i].rendered_seq = scene_store_view_seq(cp->ly[i].store);
    return 0;
}

void scene_compositor_set_effects(scene_compositor *cp, int on)
{
    uint32_t i;

    if (!cp) return;
    on = on ? 1 : 0;
    if (cp->effects_on && !on) {
        for (i = 0; i < cp->ly_count; i++)
            anim_clear_all(cp, &cp->ly[i]);  /* returning to identity */
    }
    cp->effects_on = on;
}

uint64_t scene_compositor_tick(scene_compositor *cp)
{
    return cp ? cp->tick : 0;
}

uint32_t scene_compositor_anim_count(scene_compositor *cp)
{
    uint32_t i, n = 0;

    if (!cp) return 0;
    for (i = 0; i < cp->ly_count; i++)
        n += cp->ly[i].anim_used;
    return n;
}

const scene_fb *scene_compositor_fb(scene_compositor *cp)
{
    return cp ? &cp->fb : NULL;
}

uint32_t scene_compositor_damage(scene_compositor *cp, scene_rect *out,
                                 uint32_t cap)
{
    uint32_t n, i;

    if (!cp || !out || cap == 0) return 0;
    n = cp->damage_count < cap ? cp->damage_count : cap;
    for (i = 0; i < n; i++) out[i] = cp->damage[i];
    return cp->damage_count;
}

uint64_t scene_compositor_rendered_seq(scene_compositor *cp)
{
    return cp ? cp->ly[0].rendered_seq : 0;
}

uint64_t scene_compositor_layer_seq(scene_compositor *cp, uint32_t layer)
{
    if (!cp || layer >= cp->ly_count) return 0;
    return scene_store_view_seq(cp->ly[layer].store);
}

void scene_compositor_force_repaint(scene_compositor *cp)
{
    if (cp) cp->force = 1;
}

scene_style_ref scene_compositor_setup_hover_style(scene_compositor *cp,
                                                   uint32_t fill,
                                                   uint32_t text)
{
    if (!cp) return 0;
    /* Ensure at least 2 style slots (0 = role default, 1 = hover) */
    if (cp->style_count < 2)
        scene_compositor_set_style_count(cp, 2);
    scene_style hs;
    memset(&hs, 0, sizeof(hs));
    hs.fill  = fill;
    hs.text  = text;
    hs.border = 0;
    hs.border_w = 0;
    hs.radius = 0;
    if (scene_compositor_set_style(cp, 1, &hs) != 0) return 0;
    return 1;
}

scene_style_ref scene_compositor_setup_active_style(scene_compositor *cp,
                                                    uint32_t fill,
                                                    uint32_t text)
{
    if (!cp) return 0;
    /* Ensure at least 3 style slots (0=default, 1=hover, 2=active) */
    if (cp->style_count < 3)
        scene_compositor_set_style_count(cp, 3);
    scene_style as;
    memset(&as, 0, sizeof(as));
    as.fill  = fill;
    as.text  = text;
    as.border = 0;
    as.border_w = 0;
    as.radius = 0;
    if (scene_compositor_set_style(cp, 2, &as) != 0) return 0;
    return 2;
}
