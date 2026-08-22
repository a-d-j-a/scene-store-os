/*
 * scene_store.c — the semantic scene engine core.
 *
 * Implements the locked v0 wire semantics (scene-store-spec.md):
 * versioned scene tree + append-only op log + checkpoints + region
 * resolution + macros + replay/seek + ghost-crash + flow control.
 *
 * Binding decisions frozen with v0 (see scene_fmt.h):
 *  - Every client->server record's first payload field is its seq u64,
 *    strictly monotonic (starts at 1); violation = fatal SCENE_ERR_SEQ.
 *  - EXCEPTION: Ack's first field is the consumed input seq (per §5),
 *    not the stream counter; Acks do not advance the stream counter.
 *  - SNAPSHOT and CAPTURE share §6 payload: req_id u32 then
 *    {seq u64, node_count u32, texture_count u32, nodes..., textures...}.
 *  - "Committed scene" = current scene (all accepted ops); present_seq is
 *    the commit marker seq used by PresentDone, matching the §7
 *    client-side commit-vs-in-flight rule (server truth = current state).
 *  - Every accepted client record is appended to the op log (spec §2).
 *    Replay applies only scene-affecting records; requests are no-ops.
 *  - Replay (Seek) builds a separate replay scene; the live scene and its
 *    checkpoint table are preserved and reusable after re-live.
 *  - Macros record the mutation subsequence with per-op region anchors;
 *    ExecMacro re-resolves by id when present, else by recorded rect.
 *  - Search = ASCII case-insensitive substring over text+value slots,
 *    results in document order, one hit per (node, slot) match.
 *  - TextIndex is emitted per SetText/SetValue, count = 1.
 *  - All error codes are fatal: ERROR is emitted and the session closes.
 *  - Input enters engine-side (compositor feeder) and is flow-controlled:
 *    one un-acked pointer delivery at a time (§8).
 */
#define _POSIX_C_SOURCE 200809L
#include "scene_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

/* ==================================================================== */
/* Small containers                                                      */
/* ==================================================================== */

typedef struct dynbuf {
    uint8_t *data;
    uint32_t len, cap;
} dynbuf;

static int db_reserve(dynbuf *b, uint32_t need)
{
    if (b->cap >= need) return 0;
    uint32_t nc = b->cap ? b->cap : 256;
    while (nc < need) nc <<= 1;
    uint8_t *nd = (uint8_t *)realloc(b->data, nc);
    if (!nd) return -1;
    b->data = nd;
    b->cap = nc;
    return 0;
}

static int db_put(dynbuf *b, const void *p, uint32_t n)
{
    if (db_reserve(b, b->len + n) != 0) return -1;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 0;
}

static int db_put_u8(dynbuf *b, uint8_t v)  { return db_put(b, &v, 1); }
static int db_put_u16(dynbuf *b, uint16_t v){ uint8_t x[2]; scene_put_u16(x, v); return db_put(b, x, 2); }
static int db_put_u32(dynbuf *b, uint32_t v){ uint8_t x[4]; scene_put_u32(x, v); return db_put(b, x, 4); }
static int db_put_u64(dynbuf *b, uint64_t v){ uint8_t x[8]; scene_put_u64(x, v); return db_put(b, x, 8); }
static int db_put_i32(dynbuf *b, int32_t v) { return db_put_u32(b, (uint32_t)v); }

static void db_free(dynbuf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

static uint64_t default_clock(void *ud)
{
    (void)ud;
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000000ULL) / f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
#endif
}

/* ==================================================================== */
/* Text slot and node                                                    */
/* ==================================================================== */

typedef struct text_slot {
    uint32_t text_id;
    uint32_t len, cap;
    char    *data;
} text_slot;

typedef struct node {
    uint32_t id;
    uint32_t parent;        /* 1-based slot, 0 = root list              */
    uint32_t first_child;   /* slot of first child, 0 = none            */
    uint32_t next_sib;      /* slot of next sibling, 0 = none           */
    uint32_t depth;
    uint16_t role;
    uint8_t  flags;
    uint8_t  stale;         /* ghost-crash: retained, marked stale      */
    uint8_t  blend, opacity;
    int32_t  rect[4];
    int32_t  tex_src[4];
    uint32_t style, effect, tex;    /* tex = SCENE_NO_TEXTURE when unset */
    text_slot *texts;
    uint32_t text_count, text_cap;
} node;

typedef struct idmap {
    uint32_t *keys;         /* node id                                  */
    uint32_t *vals;         /* arena slot (1-based), 0 = empty          */
    uint32_t cap, count;
} idmap;

/* ==================================================================== */
/* Macro recording                                                       */
/* ==================================================================== */

typedef struct macro_op {
    uint16_t   opcode;
    uint32_t   payload_len;
    uint8_t   *payload;     /* full op payload (seq included)           */
    uint32_t   id;          /* target node at record time               */
    int32_t    rect[4];     /* target rect at record time               */
} macro_op;

typedef struct macro_rec {
    uint32_t  macro_id;
    macro_op *ops;
    uint32_t  count, cap;
} macro_rec;

typedef struct tex_ent {
    uint32_t ref, w, h;
    uint16_t fmt;
    uint8_t  opaque;
} tex_ent;

typedef struct ckpt_ent {
    uint64_t seq;
    dynbuf   dump;
} ckpt_ent;

/* ==================================================================== */
/* The store                                                             */
/* ==================================================================== */

struct scene_store {
    scene_limits lim;

    uint32_t  scene_id;
    uint64_t  next_seq;          /* next expected stream seq (1-based)   */
    uint64_t  scene_seq;         /* seq after last accepted mutating op  */
    uint64_t  present_seq;       /* commit marker from last Present      */
    uint64_t  last_token;
    uint32_t  input_pending;     /* un-acked input delivery pending      */
    uint64_t  last_input_seq;    /* scene seq stamped on InputPointer    */
    uint64_t  last_input_us;     /* clock stamp of last input delivery   */
    uint8_t   mode;              /* live/replay/record                   */
    uint8_t   dead;
    uint32_t  budget_violations;
    uint64_t  budget_max_us;

    /* live arena; slot 0 is the pseudo-root holding the root list      */
    node     *nodes;
    uint32_t  node_cap, node_count, node_free;
    idmap     by_id;
    scene_node_id focus;

    /* replay arena (built by Seek, torn down on live)                  */
    node     *rnodes;
    uint32_t  rcap, rcount, rfree;
    idmap     rid;
    uint64_t  replay_seq;
    scene_node_id replay_focus;
    tex_ent  *rtextures;
    uint32_t  rtex_count, rtex_cap;

    /* texture registry (server-issued, fed by compositor layer)        */
    tex_ent  *textures;
    uint32_t  tex_count, tex_cap;

    uint32_t  style_count, effect_count;

    /* op log: {opcode u16, len u32, payload}* ; every accepted record  */
    dynbuf    log;
    uint64_t  log_records;
    ckpt_ent *ckpts;               /* every 4096 log records            */
    uint32_t  ckpt_count, ckpt_cap;

    macro_rec *macros;
    uint32_t  macro_count, macro_cap;
    uint32_t  open_macro;

    /* outbound */
    dynbuf    out;
    uint32_t  out_off;

    scene_clock_fn clock;
    void *clock_ud;
};

#define SCENE_CKPT_PERIOD 4096u
#define SCENE_NO_MACRO    UINT32_MAX

/* ==================================================================== */
/* Internal helpers                                                      */
/* ==================================================================== */

static uint64_t now_us(const scene_store *s)
{
    return s->clock ? s->clock(s->clock_ud) : default_clock(NULL);
}

static int idmap_init(idmap *m)
{
    m->cap = 1024;
    m->count = 0;
    m->keys = (uint32_t *)calloc(m->cap, sizeof(uint32_t));
    m->vals = (uint32_t *)calloc(m->cap, sizeof(uint32_t));
    if (!m->keys || !m->vals) { free(m->keys); free(m->vals); return -1; }
    return 0;
}

static void idmap_free(idmap *m)
{
    free(m->keys); free(m->vals);
    m->keys = m->vals = NULL; m->cap = m->count = 0;
}

static void idmap_grow(idmap *m)
{
    uint32_t ncap = m->cap << 1;
    uint32_t *nk = (uint32_t *)calloc(ncap, sizeof(uint32_t));
    uint32_t *nv = (uint32_t *)calloc(ncap, sizeof(uint32_t));
    if (!nk || !nv) { free(nk); free(nv); return; }
    uint32_t i;
    for (i = 0; i < m->cap; i++) {
        if (m->vals[i]) {
            uint32_t h = m->keys[i] & (ncap - 1);
            while (nk[h]) h = (h + 1) & (ncap - 1);
            nk[h] = m->keys[i]; nv[h] = m->vals[i];
        }
    }
    free(m->keys); free(m->vals);
    m->keys = nk; m->vals = nv; m->cap = ncap;
}

static void idmap_put(idmap *m, uint32_t key, uint32_t slot)
{
    if ((m->count + 1) * 4 >= m->cap * 3) idmap_grow(m);
    uint32_t h = key & (m->cap - 1);
    while (m->vals[h]) h = (h + 1) & (m->cap - 1);
    m->keys[h] = key; m->vals[h] = slot; m->count++;
}

static uint32_t idmap_get(const idmap *m, uint32_t key)
{
    if (!m->keys || m->count == 0) return 0;
    uint32_t h = key & (m->cap - 1);
    while (m->vals[h]) {
        if (m->keys[h] == key) return m->vals[h];
        h = (h + 1) & (m->cap - 1);
    }
    return 0;
}

static uint32_t idmap_remove(idmap *m, uint32_t key)
{
    if (!m->keys || m->count == 0) return 0;
    uint32_t h = key & (m->cap - 1);
    uint32_t result = 0;
    while (m->vals[h]) {
        if (m->keys[h] == key) {
            result = m->vals[h];
            m->vals[h] = 0;
            m->count--;
            /* rehash the cluster to keep lookups correct                */
            uint32_t i = (h + 1) & (m->cap - 1);
            while (m->vals[i]) {
                uint32_t k = m->keys[i], v = m->vals[i];
                m->vals[i] = 0; m->count--;
                idmap_put(m, k, v);
                i = (i + 1) & (m->cap - 1);
            }
            return result;
        }
        h = (h + 1) & (m->cap - 1);
    }
    return result;
}

static void text_slot_free(text_slot *t)
{
    free(t->data);
    t->data = NULL; t->len = t->cap = 0;
}

/* ---- node arena ------------------------------------------------------ */

/* Allocate a slot (1-based) from an arena + free list.                  */
static uint32_t arena_alloc(node **arena, uint32_t *cap, uint32_t *count,
                            uint32_t *free_head)
{
    uint32_t slot;
    node *n;
    if (*free_head) {
        slot = *free_head;
        n = &(*arena)[slot];
        *free_head = n->next_sib; /* free list reuses next_sib          */
        (*count)++;               /* count = live nodes, not slots      */
        memset(n, 0, sizeof(*n));
        n->id = UINT32_MAX;
        n->tex = SCENE_NO_TEXTURE;
        return slot;
    }
    if (*count + 1 >= *cap) {
        uint32_t nc = *cap ? *cap * 2 : 256;
        node *na = (node *)realloc(*arena, sizeof(node) * nc);
        if (!na) return 0;
        *arena = na; *cap = nc;
    }
    slot = ++(*count);
    n = &(*arena)[slot];
    memset(n, 0, sizeof(*n));
    n->id = UINT32_MAX;
    n->tex = SCENE_NO_TEXTURE;
    return slot;
}

static void arena_free_one(node *arena, uint32_t *free_head, uint32_t slot)
{
    node *n = &arena[slot];
    uint32_t i;
    for (i = 0; i < n->text_count; i++) text_slot_free(&n->texts[i]);
    free(n->texts);
    n->texts = NULL; n->text_count = n->text_cap = 0;
    n->next_sib = *free_head;
    *free_head = slot;
}

/* Append `slot` as last child of `parent_slot` (0 = root list).         */
static void attach(node *arena, uint32_t parent_slot, uint32_t slot,
                   uint32_t depth)
{
    node *n = &arena[slot];
    node *p = &arena[parent_slot];   /* slot 0 = pseudo-root            */
    if (!p->first_child) p->first_child = slot;
    else {
        uint32_t c = p->first_child;
        while (arena[c].next_sib) c = arena[c].next_sib;
        arena[c].next_sib = slot;
    }
    n->parent = parent_slot;
    n->depth = depth;
}

/* Remove `slot` from its parent's sibling list.                         */
static void detach(node *arena, uint32_t slot)
{
    node *n = &arena[slot];
    node *p = &arena[n->parent];
    if (!p->first_child) return;
    if (p->first_child == slot) p->first_child = n->next_sib;
    else {
        uint32_t c = p->first_child;
        while (arena[c].next_sib != slot) c = arena[c].next_sib;
        arena[c].next_sib = n->next_sib;
    }
    n->next_sib = 0;
    n->parent = 0;
}

/* ==================================================================== */
/* Outbound emission                                                     */
/* ==================================================================== */

static int emit(scene_store *s, uint16_t opcode,
                const uint8_t *payload, uint32_t plen)
{
    if (db_reserve(&s->out, SCENE_HEADER_SIZE + plen) != 0) return -1;
    uint32_t base = s->out.len;
    uint8_t hdr[SCENE_HEADER_SIZE];
    scene_put_u32(hdr + 0, SCENE_MAGIC);
    scene_put_u16(hdr + 4, SCENE_PROTOCOL_V0);
    scene_put_u16(hdr + 6, opcode);
    scene_put_u32(hdr + 8, plen);
    scene_put_u32(hdr + 12, 0); /* checksum placeholder */
    if (db_put(&s->out, hdr, SCENE_HEADER_SIZE) != 0) return -1;
    if (plen && db_put(&s->out, payload, plen) != 0) return -1;
    uint32_t ck = scene_fnv1a32(s->out.data + base, SCENE_HEADER_SIZE + plen);
    scene_put_u32(s->out.data + base + 12, ck);
    return 0;
}

static int emit_error(scene_store *s, uint16_t code, const char *msg)
{
    dynbuf b = {0};
    size_t mlen = msg ? strlen(msg) : 0;
    if (mlen > 0xFFFF) mlen = 0xFFFF;
    uint8_t h4[4];
    scene_put_u32(h4, (uint32_t)mlen);
    int r = -1;
    if (db_put_u16(&b, code) == 0 && db_put(&b, h4, 4) == 0 &&
        (mlen == 0 || db_put(&b, msg, (uint32_t)mlen) == 0))
        r = emit(s, SCENE_SRV_ERROR, b.data, b.len);
    db_free(&b);
    return r;
}

static int emit_text_index(scene_store *s, uint32_t text_id, uint32_t node_id,
                           const char *data, uint32_t len)
{
    dynbuf b = {0};
    uint8_t h4[4];
    scene_put_u32(h4, len);
    int r = -1;
    if (db_put_u32(&b, 1) == 0 &&                    /* count             */
        db_put_u32(&b, text_id) == 0 &&
        db_put_u32(&b, node_id) == 0 &&
        db_put(&b, h4, 4) == 0 &&
        (len == 0 || db_put(&b, data, len) == 0))
        r = emit(s, SCENE_SRV_TEXT_INDEX, b.data, b.len);
    db_free(&b);
    return r;
}

/* ==================================================================== */
/* DFS walk stack: fixed entries on the C stack; only pathological
 * tree depths fall back to the heap. The hot paths (region_at, search)
 * must not allocate per query.                                          */
/* ==================================================================== */

#define WALK_FIXED 64

typedef struct {
    uint32_t  fixed[WALK_FIXED];
    uint32_t *buf;
    uint32_t  cap;
    uint32_t  sp;
} walk_stack;

static void walk_init(walk_stack *ws)
{
    ws->buf = ws->fixed;
    ws->cap = WALK_FIXED;
    ws->sp = 0;
}

/* Returns 0 on success, -1 on heap failure (tree deeper than WALK_FIXED
 * and the grow allocation failed).                                      */
static int walk_push(walk_stack *ws, uint32_t v)
{
    if (ws->sp == ws->cap) {
        uint32_t nc = ws->cap * 2;
        uint32_t *nb = (uint32_t *)malloc(sizeof(uint32_t) * nc);
        if (!nb) return -1;
        memcpy(nb, ws->buf, sizeof(uint32_t) * ws->sp);
        if (ws->buf != ws->fixed) free(ws->buf);
        ws->buf = nb;
        ws->cap = nc;
    }
    ws->buf[ws->sp++] = v;
    return 0;
}

static void walk_free(walk_stack *ws)
{
    if (ws->buf != ws->fixed) free(ws->buf);
}

/* ==================================================================== */
/* Snapshot serialization (§6)                                           */
/* ==================================================================== */

static int dump_node(dynbuf *dump, const node *arena, const node *nd)
{
    uint8_t h4[4];
    uint32_t i;
    if (db_put_u32(dump, nd->id) != 0 ||
        db_put_u32(dump, nd->parent ? arena[nd->parent].id
                                    : SCENE_NO_PARENT) != 0 ||
        db_put_u16(dump, nd->role) != 0 ||
        db_put_u8(dump, nd->flags) != 0 ||
        db_put_i32(dump, nd->rect[0]) != 0 ||
        db_put_i32(dump, nd->rect[1]) != 0 ||
        db_put_i32(dump, nd->rect[2]) != 0 ||
        db_put_i32(dump, nd->rect[3]) != 0 ||
        db_put_u32(dump, nd->style) != 0 ||
        db_put_u32(dump, nd->effect) != 0 ||
        db_put_u32(dump, nd->tex) != 0 ||
        db_put_u32(dump, nd->text_count) != 0) return -1;
    for (i = 0; i < nd->text_count; i++) {
        const text_slot *t = &nd->texts[i];
        scene_put_u32(h4, t->len);
        if (db_put_u32(dump, t->text_id) != 0 || db_put(dump, h4, 4) != 0 ||
            (t->len && db_put(dump, t->data, t->len) != 0)) return -1;
    }
    return 0;
}

/* Serialize the scene in `arena` (document order) into `dump`.          */
static int snapshot_serialize(node *arena, uint32_t count, uint64_t seq,
                              const tex_ent *texes, uint32_t tex_count,
                              dynbuf *dump)
{
    if (db_put_u64(dump, seq) != 0 ||
        db_put_u32(dump, count) != 0 ||
        db_put_u32(dump, tex_count) != 0) return -1;
    walk_stack ws;
    walk_init(&ws);
    uint32_t seen = 0, c = arena[0].first_child;
    while (c || ws.sp) {
        while (c) {
            if (dump_node(dump, arena, &arena[c]) != 0) { walk_free(&ws); return -1; }
            seen++;
            if (walk_push(&ws, c) != 0) { walk_free(&ws); return -1; }
            c = arena[c].first_child;
        }
        c = ws.buf[--ws.sp];
        c = arena[c].next_sib;
    }
    walk_free(&ws);
    if (seen != count) return -1;
    uint32_t i;
    for (i = 0; i < tex_count; i++) {
        if (db_put_u32(dump, texes[i].ref) != 0 ||
            db_put_u32(dump, texes[i].w) != 0 ||
            db_put_u32(dump, texes[i].h) != 0 ||
            db_put_u16(dump, texes[i].fmt) != 0 ||
            db_put_u8(dump, texes[i].opaque) != 0) return -1;
    }
    return 0;
}

/* Rebuild an arena from a §6 dump. *free_out and links are rebuilt.     */
static int snapshot_restore(scene_store *s, const uint8_t *p, uint32_t len,
                            node **arena_out, uint32_t *count_out,
                            uint32_t *cap_out, uint32_t *free_out,
                            idmap *id_out, uint64_t *seq_out,
                            tex_ent **texes_out, uint32_t *tex_count_out)
{
    uint32_t off = 0;
    if (len < 16) return -1;
    uint64_t seq = scene_get_u64(p + off); off += 8;
    uint32_t count = scene_get_u32(p + off); off += 4;
    uint32_t texc = scene_get_u32(p + off); off += 4;
    node *arena = NULL; uint32_t cap = 0, free_head = 0, made = 0;
    idmap idm;
    if (idmap_init(&idm) != 0) return -1;
    uint32_t i;
    for (i = 0; i < count; i++) {
        uint32_t slot = arena_alloc(&arena, &cap, &made, &free_head);
        if (!slot) goto fail;
        node *nd = &arena[slot];
        if (len - off < 32) goto fail;
        nd->id = scene_get_u32(p + off); off += 4;
        nd->parent = scene_get_u32(p + off); off += 4;
        nd->role = scene_get_u16(p + off); off += 2;
        nd->flags = p[off]; off += 1;
        nd->rect[0] = scene_get_i32(p + off); off += 4;
        nd->rect[1] = scene_get_i32(p + off); off += 4;
        nd->rect[2] = scene_get_i32(p + off); off += 4;
        nd->rect[3] = scene_get_i32(p + off); off += 4;
        nd->style = scene_get_u32(p + off); off += 4;
        nd->effect = scene_get_u32(p + off); off += 4;
        nd->tex = scene_get_u32(p + off); off += 4;
        uint32_t tcount = scene_get_u32(p + off); off += 4;
        if (tcount > s->lim.max_text_slots_per_node) goto fail;
        if (tcount) {
            nd->text_cap = tcount;
            nd->texts = (text_slot *)calloc(tcount, sizeof(text_slot));
            if (!nd->texts) goto fail;
        }
        nd->text_count = tcount;
        uint32_t j;
        for (j = 0; j < tcount; j++) {
            text_slot *t = &nd->texts[j];
            if (len - off < 8) goto fail;
            t->text_id = scene_get_u32(p + off); off += 4;
            uint32_t tl = scene_get_u32(p + off); off += 4;
            if (tl > s->lim.max_text_bytes_per_slot || len - off < tl) goto fail;
            t->len = t->cap = tl;
            if (tl) {
                t->data = (char *)malloc(tl);
                if (!t->data) goto fail;
                memcpy(t->data, p + off, tl);
            }
            off += tl;
        }
        idmap_put(&idm, nd->id, slot);
    }
    /* rebuild parent/child links */
    for (i = 1; i <= made; i++) {
        node *nd = &arena[i];
        if (nd->id == UINT32_MAX) continue;
        uint32_t ps;
        if (nd->parent == SCENE_NO_PARENT) ps = 0;
        else {
            ps = idmap_get(&idm, nd->parent);
            if (!ps) goto fail;
        }
        attach(arena, ps, i, ps ? arena[ps].depth + 1 : 0);
    }
    /* texture table (§6) */
    if (texc) {
        if (len - off < texc * 15u) goto fail;
        tex_ent *tx = (tex_ent *)malloc(sizeof(tex_ent) * texc);
        if (!tx) goto fail;
        for (i = 0; i < texc; i++) {
            tx[i].ref = scene_get_u32(p + off); off += 4;
            tx[i].w = scene_get_u32(p + off); off += 4;
            tx[i].h = scene_get_u32(p + off); off += 4;
            tx[i].fmt = scene_get_u16(p + off); off += 2;
            tx[i].opaque = p[off]; off += 1;
        }
        *texes_out = tx;
    } else {
        *texes_out = NULL;
    }
    *tex_count_out = texc;
    *arena_out = arena; *count_out = made; *cap_out = cap; *free_out = free_head;
    *id_out = idm; *seq_out = seq;
    return 0;
fail:
    {
        uint32_t j;
        for (j = 1; j <= made; j++) {
            node *nd = &arena[j];
            if (nd->id != UINT32_MAX) {
                uint32_t k;
                for (k = 0; k < nd->text_count; k++) text_slot_free(&nd->texts[k]);
                free(nd->texts);
            }
        }
    }
    free(arena);
    idmap_free(&idm);
    return -1;
}

/* ==================================================================== */
/* Op log + checkpoints                                                  */
/* ==================================================================== */

static int log_append(scene_store *s, uint16_t opcode,
                      const uint8_t *payload, uint32_t plen)
{
    if (db_put_u16(&s->log, opcode) != 0) return -1;
    if (db_put_u32(&s->log, plen) != 0) return -1;
    if (plen && db_put(&s->log, payload, plen) != 0) return -1;
    s->log_records++;
    if (s->log_records % SCENE_CKPT_PERIOD == 0) {
        if (s->ckpt_count == s->ckpt_cap) {
            uint32_t nc = s->ckpt_cap ? s->ckpt_cap * 2 : 4;
            ckpt_ent *nc2 =
                realloc(s->ckpts, sizeof(*s->ckpts) * nc);
            if (!nc2) return -1;
            s->ckpts = nc2; s->ckpt_cap = nc;
        }
        dynbuf dump = {0};
        if (snapshot_serialize(s->nodes, s->node_count, s->scene_seq,
                               s->textures, s->tex_count, &dump) != 0) return -1;
        s->ckpts[s->ckpt_count].seq = s->scene_seq;
        s->ckpts[s->ckpt_count].dump = dump;
        s->ckpt_count++;
    }
    return 0;
}

/* ==================================================================== */
/* Mutation primitives (shared by ingest, replay, macro exec)            */
/* ==================================================================== */

typedef struct apply_ctx {
    scene_store    *s;
    node    *arena;
    node   **arena_back;  /* store field to sync when the arena reallocs */
    idmap   *idm;
    uint32_t *count, *cap, *free_head;
    scene_node_id *focus;    /* focus state for this arena              */
    tex_ent *texes;          /* texture table for this arena            */
    uint32_t tex_count;
    int      events;
    int      replay;         /* replay arena: re-creates are resurrections */
} apply_ctx;

/* Document-order DFS from a given arena. `cb(id, slot, out)`.           */
typedef int (*walk_cb)(scene_node_id id, uint32_t slot, void *out);

static int walk_arena(node *arena, uint32_t count, walk_cb cb, void *out)
{
    uint32_t *stack = (uint32_t *)malloc(sizeof(uint32_t) * (count ? count : 1));
    if (!stack) return -1;
    uint32_t sp = 0, c = arena[0].first_child;
    while (c || sp) {
        while (c) {
            int r = cb(arena[c].id, c, out);
            if (r) { free(stack); return r; }
            stack[sp++] = c;
            c = arena[c].first_child;
        }
        c = stack[--sp];
        c = arena[c].next_sib;
    }
    free(stack);
    return 0;
}

static int op_create(apply_ctx *a, const uint8_t *p, uint32_t plen,
                     uint16_t *fatal_err)
{
    scene_store *s = a->s;
    uint32_t parent_id, id, slot;
    uint16_t role;
    int32_t rect[4];
    uint8_t flags;
    if (plen != 35) { *fatal_err = SCENE_ERR_PROTOCOL; return 1; }
    parent_id = scene_get_u32(p + 8);
    id = scene_get_u32(p + 12);
    role = scene_get_u16(p + 16);
    rect[0] = scene_get_i32(p + 18);
    rect[1] = scene_get_i32(p + 22);
    rect[2] = scene_get_i32(p + 26);
    rect[3] = scene_get_i32(p + 30);
    flags = p[34];
    if (role > SCENE_ROLE_MAX) { *fatal_err = SCENE_ERR_BAD_ROLE; return 1; }
    if (id == SCENE_NO_PARENT) { *fatal_err = SCENE_ERR_PROTOCOL; return 1; }
    slot = idmap_get(a->idm, id);
    if (slot) {
        node *e = &a->arena[slot];
        if (!e->stale && !a->replay) { *fatal_err = SCENE_ERR_BAD_NODE; return 1; }
        /* ghost resurrection: apply delta, retain text                  */
        uint32_t ps = 0;
        if (parent_id != SCENE_NO_PARENT) {
            ps = idmap_get(a->idm, parent_id);
            if (!ps || a->arena[ps].stale) {
                *fatal_err = SCENE_ERR_BAD_PARENT; return 1;
            }
        }
        detach(a->arena, slot);
        e->role = role;
        e->flags = flags;
        e->blend = 0;
        e->opacity = 255;
        memcpy(e->rect, rect, sizeof(rect));
        e->stale = 0;
        attach(a->arena, ps, slot, ps ? a->arena[ps].depth + 1 : 0);
        return 0;
    }
    if (a->count[0] >= s->lim.max_nodes_per_session) {
        *fatal_err = SCENE_ERR_LIMIT; return 1;
    }
    uint32_t ps = 0;
    if (parent_id != SCENE_NO_PARENT) {
        ps = idmap_get(a->idm, parent_id);
        if (!ps) { *fatal_err = SCENE_ERR_BAD_PARENT; return 1; }
        if (a->arena[ps].stale) { *fatal_err = SCENE_ERR_BAD_PARENT; return 1; }
    }
    slot = arena_alloc(&a->arena, a->cap, a->count, a->free_head);
    if (!slot) return -1;
    *a->arena_back = a->arena; /* realloc may have moved the base        */
    node *n = &a->arena[slot];
    n->id = id;
    n->role = role;
    n->flags = flags;
    memcpy(n->rect, rect, sizeof(rect));
    n->tex = SCENE_NO_TEXTURE;
    n->blend = 0;        /* per-node blend/opacity only matter once      */
    n->opacity = 255;    /* a texture is set; untextured nodes are opaque */
    attach(a->arena, ps, slot, ps ? a->arena[ps].depth + 1 : 0);
    idmap_put(a->idm, id, slot);
    return 0;
}

static int op_destroy(apply_ctx *a, const uint8_t *p, uint32_t plen,
                      uint16_t *fatal_err)
{
    if (plen != 12) { *fatal_err = SCENE_ERR_PROTOCOL; return 1; }
    uint32_t id = scene_get_u32(p + 8);
    uint32_t slot = idmap_get(a->idm, id);
    if (!slot) { *fatal_err = SCENE_ERR_BAD_NODE; return 1; }
    /* Subtree destroy: children first (post-order), then the node.      */
    {
        uint32_t *stack = (uint32_t *)malloc(sizeof(uint32_t) *
                                             (a->count[0] ? a->count[0] : 1));
        if (!stack) return -1;
        uint32_t sp = 0, c = a->arena[slot].first_child;
        while (c || sp) {
            while (c) { stack[sp++] = c; c = a->arena[c].first_child; }
            c = stack[--sp];
            uint32_t ns = a->arena[c].next_sib;
            idmap_remove(a->idm, a->arena[c].id);
            arena_free_one(a->arena, a->free_head, c);
            a->count[0]--;
            c = ns;
        }
        free(stack);
    }
    detach(a->arena, slot);
    idmap_remove(a->idm, id);
    arena_free_one(a->arena, a->free_head, slot);
    a->count[0]--;
    if (*a->focus == id) *a->focus = SCENE_NO_PARENT;
    return 0;
}

static int text_set(apply_ctx *a, uint32_t id, uint32_t text_id,
                    const char *data, uint32_t len, uint16_t *fatal_err)
{
    scene_store *s = a->s;
    uint32_t slot = idmap_get(a->idm, id);
    if (!slot) { *fatal_err = SCENE_ERR_BAD_NODE; return 1; }
    node *n = &a->arena[slot];
    if (n->stale) { *fatal_err = SCENE_ERR_BAD_NODE; return 1; }
    uint32_t i;
    for (i = 0; i < n->text_count; i++)
        if (n->texts[i].text_id == text_id) break;
    if (i == n->text_count) {
        if (n->text_count >= s->lim.max_text_slots_per_node) {
            *fatal_err = SCENE_ERR_LIMIT; return 1;
        }
        if (n->text_count >= n->text_cap) {
            uint32_t nc = n->text_cap ? n->text_cap * 2 : 4;
            text_slot *nt = (text_slot *)realloc(n->texts, sizeof(text_slot) * nc);
            if (!nt) return -1;
            n->texts = nt; n->text_cap = nc;
        }
        text_slot *t = &n->texts[n->text_count];
        t->text_id = text_id;
        t->data = NULL; t->len = t->cap = 0;
        n->text_count++;
    }
    text_slot *t = &n->texts[i];
    if (len > s->lim.max_text_bytes_per_slot) {
        *fatal_err = SCENE_ERR_LIMIT; return 1;
    }
    if (t->cap < len) {
        uint32_t nc = len ? len : 1;
        char *nd = (char *)realloc(t->data, nc);
        if (!nd) return -1;
        t->data = nd; t->cap = nc;
    }
    memcpy(t->data, data, len);
    t->len = len;
    if (a->events)
        return emit_text_index(s, text_id, id, t->data, t->len);
    return 0;
}

static int op_set_text(apply_ctx *a, const uint8_t *p, uint32_t plen,
                       uint16_t *fatal_err, int is_value)
{
    (void)is_value; /* same storage; semantic distinction is client-side */
    if (plen < 20) { *fatal_err = SCENE_ERR_PROTOCOL; return 1; }
    uint32_t id = scene_get_u32(p + 8);
    uint32_t text_id = scene_get_u32(p + 12);
    uint32_t slen = scene_get_u32(p + 16);
    if (plen != 20 + slen) { *fatal_err = SCENE_ERR_PROTOCOL; return 1; }
    return text_set(a, id, text_id, (const char *)p + 20, slen, fatal_err);
}

static int op_set_rect(apply_ctx *a, const uint8_t *p, uint32_t plen,
                       uint16_t *fatal_err)
{
    if (plen != 28) { *fatal_err = SCENE_ERR_PROTOCOL; return 1; }
    uint32_t id = scene_get_u32(p + 8);
    uint32_t slot = idmap_get(a->idm, id);
    if (!slot) { *fatal_err = SCENE_ERR_BAD_NODE; return 1; }
    node *n = &a->arena[slot];
    if (n->stale) { *fatal_err = SCENE_ERR_BAD_NODE; return 1; }
    n->rect[0] = scene_get_i32(p + 12);
    n->rect[1] = scene_get_i32(p + 16);
    n->rect[2] = scene_get_i32(p + 20);
    n->rect[3] = scene_get_i32(p + 24);
    return 0;
}

static int op_set_flags(apply_ctx *a, const uint8_t *p, uint32_t plen,
                        uint16_t *fatal_err)
{
    if (plen != 13) { *fatal_err = SCENE_ERR_PROTOCOL; return 1; }
    uint32_t id = scene_get_u32(p + 8);
    uint32_t slot = idmap_get(a->idm, id);
    if (!slot) { *fatal_err = SCENE_ERR_BAD_NODE; return 1; }
    node *n = &a->arena[slot];
    if (n->stale) { *fatal_err = SCENE_ERR_BAD_NODE; return 1; }
    n->flags = p[12];
    return 0;
}

static int op_set_style(apply_ctx *a, const uint8_t *p, uint32_t plen,
                        uint16_t *fatal_err)
{
    if (plen != 16) { *fatal_err = SCENE_ERR_PROTOCOL; return 1; }
    uint32_t id = scene_get_u32(p + 8);
    uint32_t slot = idmap_get(a->idm, id);
    if (!slot) { *fatal_err = SCENE_ERR_BAD_NODE; return 1; }
    node *n = &a->arena[slot];
    if (n->stale) { *fatal_err = SCENE_ERR_BAD_NODE; return 1; }
    uint32_t st = scene_get_u32(p + 12);
    if (st >= a->s->style_count) { *fatal_err = SCENE_ERR_BAD_NODE; return 1; }
    n->style = st;
    return 0;
}

static int op_set_effect(apply_ctx *a, const uint8_t *p, uint32_t plen,
                         uint16_t *fatal_err)
{
    if (plen != 16) { *fatal_err = SCENE_ERR_PROTOCOL; return 1; }
    uint32_t id = scene_get_u32(p + 8);
    uint32_t slot = idmap_get(a->idm, id);
    if (!slot) { *fatal_err = SCENE_ERR_BAD_NODE; return 1; }
    node *n = &a->arena[slot];
    if (n->stale) { *fatal_err = SCENE_ERR_BAD_NODE; return 1; }
    uint32_t ef = scene_get_u32(p + 12);
    if (ef >= a->s->effect_count) { *fatal_err = SCENE_ERR_BAD_NODE; return 1; }
    n->effect = ef;
    return 0;
}

static int op_set_texture(apply_ctx *a, const uint8_t *p, uint32_t plen,
                          uint16_t *fatal_err)
{
    if (plen != 34) { *fatal_err = SCENE_ERR_PROTOCOL; return 1; }
    uint32_t id = scene_get_u32(p + 8);
    uint32_t slot = idmap_get(a->idm, id);
    if (!slot) { *fatal_err = SCENE_ERR_BAD_NODE; return 1; }
    node *n = &a->arena[slot];
    if (n->stale) { *fatal_err = SCENE_ERR_BAD_NODE; return 1; }
    uint32_t tex = scene_get_u32(p + 12);
    uint32_t i, found = 0;
    for (i = 0; i < a->tex_count; i++)
        if (a->texes[i].ref == tex) { found = 1; break; }
    if (!found) { *fatal_err = SCENE_ERR_PROTOCOL; return 1; }
    n->tex = tex;
    n->tex_src[0] = scene_get_i32(p + 16);
    n->tex_src[1] = scene_get_i32(p + 20);
    n->tex_src[2] = scene_get_i32(p + 24);
    n->tex_src[3] = scene_get_i32(p + 28);
    n->blend = p[32];
    n->opacity = p[33];
    return 0;
}

static int emit_focus_evt(scene_store *s, uint32_t id, uint8_t state)
{
    dynbuf b = {0};
    int r = -1;
    if (db_put_u64(&b, s->scene_seq) == 0 &&
        db_put_u32(&b, id) == 0 && db_put_u8(&b, state) == 0)
        r = emit(s, SCENE_SRV_INPUT_FOCUS, b.data, b.len);
    db_free(&b);
    return r;
}

static int op_focus(apply_ctx *a, const uint8_t *p, uint32_t plen,
                    uint16_t *fatal_err)
{
    scene_store *s = a->s;
    if (plen != 12) { *fatal_err = SCENE_ERR_PROTOCOL; return 1; }
    uint32_t id = scene_get_u32(p + 8);
    uint32_t slot = idmap_get(a->idm, id);
    if (!slot) { *fatal_err = SCENE_ERR_BAD_NODE; return 1; }
    node *n = &a->arena[slot];
    if (n->stale) { *fatal_err = SCENE_ERR_BAD_NODE; return 1; }
    if (*a->focus == id) return 0;
    if (a->events && *a->focus != SCENE_NO_PARENT)
        if (emit_focus_evt(s, *a->focus, 0) != 0) return -1;
    *a->focus = id;
    if (a->events)
        if (emit_focus_evt(s, id, 1) != 0) return -1;
    return 0;
}

/* Document-order list of visible+focusable node ids (live arena).       */
struct fnext_ctx {
    node     *arena;
    uint32_t *ids;
    uint32_t n, cap;
};

static int fnext_cb(scene_node_id id, uint32_t slot, void *out)
{
    struct fnext_ctx *f = (struct fnext_ctx *)out;
    node *nd = &f->arena[slot];
    (void)id;
    if ((nd->flags & SCENE_FLAG_VISIBLE) &&
        (nd->flags & SCENE_FLAG_FOCUSABLE) && !nd->stale) {
        if (f->n < f->cap) f->ids[f->n++] = nd->id;
    }
    return 0;
}

/* ==================================================================== */
/* Op application (shared by live ingest, macro exec, replay)            */
/* ==================================================================== */

/* Scene-affecting ops: applied during replay; everything else is a
 * no-op in replay (requests) or session state (mode/macro records).     */
static int op_is_scene_affecting(uint16_t op)
{
    switch (op) {
    case SCENE_OP_CREATE_NODE: case SCENE_OP_DESTROY_NODE:
    case SCENE_OP_SET_TEXT: case SCENE_OP_SET_VALUE:
    case SCENE_OP_SET_RECT: case SCENE_OP_SET_FLAGS:
    case SCENE_OP_SET_STYLE: case SCENE_OP_SET_TEXTURE:
    case SCENE_OP_SET_EFFECT: case SCENE_OP_FOCUS:
    case SCENE_OP_FOCUS_NEXT:
        return 1;
    default:
        return 0;
    }
}

static int op_focus_next(apply_ctx *a, const uint8_t *p, uint32_t plen,
                         uint16_t *fatal_err)
{
    if (plen != 9) { *fatal_err = SCENE_ERR_PROTOCOL; return 1; }
    int8_t step = (int8_t)p[8];
    if (step == 0) { *fatal_err = SCENE_ERR_PROTOCOL; return 1; }
    uint32_t cap = *a->count ? *a->count : 1;
    uint32_t *ids = (uint32_t *)malloc(sizeof(uint32_t) * cap);
    if (!ids) return -1;
    struct fnext_ctx fc = { a->arena, ids, 0, cap };
    int w = walk_arena(a->arena, *a->count, fnext_cb, &fc);
    if (w != 0) { free(ids); return -1; }
    if (fc.n == 0) { free(ids); return 0; }
    uint32_t cur = *a->focus;
    uint32_t idx = 0, found = 0, i;
    for (i = 0; i < fc.n; i++) if (ids[i] == cur) { idx = i; found = 1; break; }
    uint32_t ni;
    if (step > 0) ni = found ? (idx + 1) % fc.n : 0;
    else          ni = found ? (idx + fc.n - 1) % fc.n : fc.n - 1;
    uint32_t target = ids[ni];
    free(ids);
    if (target == cur) return 0;
    if (a->events && *a->focus != SCENE_NO_PARENT)
        if (emit_focus_evt(a->s, *a->focus, 0) != 0) return -1;
    *a->focus = target;
    if (a->events)
        if (emit_focus_evt(a->s, target, 1) != 0) return -1;
    return 0;
}

/* Apply one client op to `a`'s arena. Returns 0 ok, 1 user error
 * (fatal_err set), -1 internal failure.                                 */
static int apply_op(apply_ctx *a, uint16_t opcode,
                    const uint8_t *p, uint32_t plen, uint16_t *fatal_err)
{
    switch (opcode) {
    case SCENE_OP_CREATE_NODE:  return op_create(a, p, plen, fatal_err);
    case SCENE_OP_DESTROY_NODE: return op_destroy(a, p, plen, fatal_err);
    case SCENE_OP_SET_TEXT:     return op_set_text(a, p, plen, fatal_err, 0);
    case SCENE_OP_SET_VALUE:    return op_set_text(a, p, plen, fatal_err, 1);
    case SCENE_OP_SET_RECT:     return op_set_rect(a, p, plen, fatal_err);
    case SCENE_OP_SET_FLAGS:    return op_set_flags(a, p, plen, fatal_err);
    case SCENE_OP_SET_STYLE:    return op_set_style(a, p, plen, fatal_err);
    case SCENE_OP_SET_TEXTURE:  return op_set_texture(a, p, plen, fatal_err);
    case SCENE_OP_SET_EFFECT:   return op_set_effect(a, p, plen, fatal_err);
    case SCENE_OP_FOCUS:        return op_focus(a, p, plen, fatal_err);
    case SCENE_OP_FOCUS_NEXT:   return op_focus_next(a, p, plen, fatal_err);
    default:
        *fatal_err = SCENE_ERR_PROTOCOL;
        return 1;
    }
}

/* ==================================================================== */
/* Fatal error handling                                                  */
/* ==================================================================== */

static int fatal_error(scene_store *s, uint16_t code, const char *msg)
{
    emit_error(s, code, msg);
    s->dead = 1;
    return -(int)code;
}

/* ==================================================================== */
/* Replay: build the replay arena at a historical seq                    */
/* ==================================================================== */

static void replay_teardown(scene_store *s)
{
    uint32_t i;
    if (s->rnodes) {
        for (i = 1; i <= s->rcount; i++) {
            node *n = &s->rnodes[i];
            uint32_t j;
            for (j = 0; j < n->text_count; j++) text_slot_free(&n->texts[j]);
            free(n->texts);
        }
        free(s->rnodes);
    }
    s->rnodes = NULL; s->rcap = s->rcount = s->rfree = 0;
    free(s->rtextures);
    s->rtextures = NULL; s->rtex_count = s->rtex_cap = 0;
    idmap_free(&s->rid);
}

/* Apply log records with base < seq <= target onto the replay arena.    */
static int replay_apply_log(scene_store *s, uint64_t base, uint64_t target)
{
    const uint8_t *log = s->log.data;
    uint32_t off = 0;
    while (off + 6 <= s->log.len) {
        uint16_t op = scene_get_u16(log + off);
        uint32_t plen = scene_get_u32(log + off + 2);
        if (off + 6 + plen > s->log.len) return -1;
        const uint8_t *pay = log + off + 6;
        off += 6 + plen;
        if (plen < 8 || !op_is_scene_affecting(op)) continue;
        uint64_t seq = scene_get_u64(pay);
        if (seq <= base || seq > target) continue;
        apply_ctx a;
        a.s = s;
        a.arena = s->rnodes;
        a.arena_back = &s->rnodes;
        a.idm = &s->rid;
        a.count = &s->rcount;
        a.cap = &s->rcap;
        a.free_head = &s->rfree;
        a.focus = &s->replay_focus;
        a.texes = s->rtextures;
        a.tex_count = s->rtex_count;
        a.events = 0;
        a.replay = 1;
        uint16_t fe = 0;
        int r = apply_op(&a, op, pay, plen, &fe);
        if (r == 1) return -1;
        if (r < 0) return -1;
        s->replay_seq = seq;
    }
    return 0;
}

static int replay_build(scene_store *s, uint64_t target)
{
    replay_teardown(s);
    if (idmap_init(&s->rid) != 0) return -1;
    uint64_t base = 0;
    uint32_t cki = s->ckpt_count;
    while (cki > 0 && s->ckpts[cki - 1].seq > target) cki--;
    if (cki > 0) {
        const dynbuf *ck = &s->ckpts[cki - 1].dump;
        uint64_t rseq = 0;
        int r = snapshot_restore(s, ck->data, ck->len,
                                 &s->rnodes, &s->rcount, &s->rcap, &s->rfree,
                                 &s->rid, &rseq,
                                 &s->rtextures, &s->rtex_count);
        if (r != 0) return -1;
        s->rtex_cap = s->rtex_count;
        base = rseq;
        s->replay_seq = base;
    } else {
        s->rnodes = (node *)calloc(1, sizeof(node));
        if (!s->rnodes) return -1;
        s->rcap = 1;
        s->rcount = 0;
        s->rfree = 0;
        s->replay_seq = 0;
    }
    if (replay_apply_log(s, base, target) != 0) return -1;
    return 0;
}

/* ==================================================================== */
/* Request replies (Snapshot, Search, Capture, Pong)                     */
/* ==================================================================== */

/* Pick the arena + texture table a request serves from.                 */
static void req_arena(scene_store *s, node **arena, uint32_t *count,
                      uint64_t *seq, tex_ent **texes, uint32_t *tex_count)
{
    if (s->mode == SCENE_MODE_REPLAY && s->rnodes) {
        *arena = s->rnodes;
        *count = s->rcount;
        *seq = s->replay_seq;
        *texes = s->rtextures;
        *tex_count = s->rtex_count;
    } else {
        *arena = s->nodes;
        *count = s->node_count;
        *seq = s->scene_seq;
        *texes = s->textures;
        *tex_count = s->tex_count;
    }
}

static int emit_snapshot_reply(scene_store *s, uint32_t req_id)
{
    node *arena; uint32_t count; uint64_t seq;
    tex_ent *texes; uint32_t tex_count;
    req_arena(s, &arena, &count, &seq, &texes, &tex_count);
    dynbuf d = {0};
    dynbuf body = {0};
    int r = -1;
    if (db_put_u32(&body, req_id) == 0 &&
        snapshot_serialize(arena, count, seq, texes, tex_count, &d) == 0 &&
        db_put(&body, d.data, d.len) == 0)
        r = emit(s, SCENE_SRV_SNAPSHOT, body.data, body.len);
    db_free(&d);
    db_free(&body);
    return r;
}

static int emit_capture_reply(scene_store *s, uint32_t req_id)
{
    node *arena; uint32_t count; uint64_t seq;
    tex_ent *texes; uint32_t tex_count;
    req_arena(s, &arena, &count, &seq, &texes, &tex_count);
    dynbuf d = {0};
    dynbuf body = {0};
    int r = -1;
    if (db_put_u32(&body, req_id) == 0 &&
        db_put_u64(&body, seq) == 0 &&
        snapshot_serialize(arena, count, seq, texes, tex_count, &d) == 0 &&
        db_put(&body, d.data, d.len) == 0)
        r = emit(s, SCENE_SRV_CAPTURE, body.data, body.len);
    db_free(&d);
    db_free(&body);
    return r;
}

static int emit_pong(scene_store *s, uint64_t nonce)
{
    uint8_t x[8];
    scene_put_u64(x, nonce);
    return emit(s, SCENE_SRV_PONG, x, 8);
}

/* ASCII case-insensitive substring search (single text slot).           */
static int ascii_ci_find(const char *h, uint32_t hlen,
                         const char *n, uint32_t nlen)
{
    if (nlen == 0) return 1;
    if (nlen > hlen) return 0;
    uint32_t i, j;
    for (i = 0; i + nlen <= hlen; i++) {
        for (j = 0; j < nlen; j++) {
            unsigned char a = (unsigned char)h[i + j];
            unsigned char b = (unsigned char)n[j];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32);
            if (a != b) break;
        }
        if (j == nlen) return 1;
    }
    return 0;
}

/* Emit SEARCH_RESULT: req_id u32, count u32, then per hit:
 * NodeId u32, rect Rect(16), role u16, TextId u32.                      */
static int emit_search_reply(scene_store *s, uint32_t req_id,
                             const char *term, uint32_t term_len)
{
    node *arena; uint32_t count; uint64_t seq;
    tex_ent *texes; uint32_t tex_count;
    req_arena(s, &arena, &count, &seq, &texes, &tex_count);
    (void)texes; (void)tex_count;
    dynbuf body = {0};
    uint8_t h4[4];
    scene_put_u32(h4, 0);
    int r = -1;
    if (db_put_u32(&body, req_id) != 0 || db_put(&body, h4, 4) != 0) goto done;
    uint32_t hits = 0;
    walk_stack ws;
    walk_init(&ws);
    uint32_t c = arena[0].first_child;
    while (c || ws.sp) {
        while (c) {
            if (walk_push(&ws, c) != 0) { walk_free(&ws); goto done; }
            node *nd = &arena[c];
            if (!(nd->flags & SCENE_FLAG_VISIBLE)) {
                c = arena[c].first_child;
                continue;
            }
            uint32_t i;
            for (i = 0; i < nd->text_count; i++) {
                text_slot *t = &nd->texts[i];
                if (t->len < term_len) continue;
                if (!ascii_ci_find(t->data, t->len, term, term_len)) continue;
                if (db_put_u32(&body, nd->id) != 0 ||
                    db_put_i32(&body, nd->rect[0]) != 0 ||
                    db_put_i32(&body, nd->rect[1]) != 0 ||
                    db_put_i32(&body, nd->rect[2]) != 0 ||
                    db_put_i32(&body, nd->rect[3]) != 0 ||
                    db_put_u16(&body, nd->role) != 0 ||
                    db_put_u32(&body, t->text_id) != 0) { walk_free(&ws); goto done; }
                hits++;
            }
            c = arena[c].first_child;
        }
        c = ws.buf[--ws.sp];
        c = arena[c].next_sib;
    }
    walk_free(&ws);
    scene_put_u32(h4, hits);
    memcpy(body.data + 4, h4, 4);
    r = emit(s, SCENE_SRV_SEARCH_RESULT, body.data, body.len);
done:
    db_free(&body);
    return r;
}

/* ==================================================================== */
/* Macros                                                                */
/* ==================================================================== */

static macro_rec *macro_find(scene_store *s, uint32_t macro_id)
{
    uint32_t i;
    for (i = 0; i < s->macro_count; i++)
        if (s->macros[i].macro_id == macro_id) return &s->macros[i];
    return NULL;
}

static void macro_op_free(macro_op *mo)
{
    free(mo->payload);
    mo->payload = NULL;
    mo->payload_len = 0;
}

static void macro_rec_free(macro_rec *mr)
{
    uint32_t i;
    for (i = 0; i < mr->count; i++) macro_op_free(&mr->ops[i]);
    free(mr->ops);
    mr->ops = NULL; mr->count = mr->cap = 0;
}

/* Record one captured mutation into the open macro (live ingest).       */
static int macro_capture(scene_store *s, uint16_t opcode,
                         const uint8_t *payload, uint32_t plen)
{
    macro_rec *mr = &s->macros[s->open_macro];
    uint32_t id;
    int32_t rect[4];
    if (opcode == SCENE_OP_CREATE_NODE) {
        if (plen < 18) return 0;
        id = scene_get_u32(payload + 12);
        rect[0] = scene_get_i32(payload + 18);
        rect[1] = scene_get_i32(payload + 22);
        rect[2] = scene_get_i32(payload + 26);
        rect[3] = scene_get_i32(payload + 30);
    } else {
        if (plen < 12) return 0;
        id = scene_get_u32(payload + 8);
        uint32_t slot = idmap_get(&s->by_id, id);
        if (!slot) return 0;
        node *n = &s->nodes[slot];
        memcpy(rect, n->rect, sizeof(rect));
    }
    if (mr->count == mr->cap) {
        uint32_t nc = mr->cap ? mr->cap * 2 : 8;
        macro_op *no = (macro_op *)realloc(mr->ops, sizeof(macro_op) * nc);
        if (!no) return -1;
        mr->ops = no;
        mr->cap = nc;
    }
    macro_op *mo = &mr->ops[mr->count];
    mo->opcode = opcode;
    mo->payload_len = plen;
    mo->payload = (uint8_t *)malloc(plen ? plen : 1);
    if (!mo->payload) return -1;
    if (plen) memcpy(mo->payload, payload, plen);
    mo->id = id;
    memcpy(mo->rect, rect, sizeof(rect));
    mr->count++;
    return 0;
}

/* Resolve a macro op's target for the current live scene: keep the
 * recorded id if it still exists, else region-resolve via recorded
 * rect, else skip the op. Returns 1 to skip.                            */
static int macro_resolve(apply_ctx *a, macro_op *mo, uint8_t *p)
{
    if (mo->opcode == SCENE_OP_CREATE_NODE) {
        uint32_t id = scene_get_u32(p + 12);
        if (idmap_get(a->idm, id)) return 1;   /* id already exists      */
        return 0;
    }
    uint32_t id = scene_get_u32(p + 8);
    if (idmap_get(a->idm, id)) return 0;
    int32_t cx = mo->rect[0] + (mo->rect[2] / 2);
    int32_t cy = mo->rect[1] + (mo->rect[3] / 2);
    scene_node_id rid = scene_store_region_at(a->s, cx, cy);
    if (rid == SCENE_NO_PARENT) return 1;
    scene_put_u32(p + 8, rid);
    return 0;
}

static int macro_exec(scene_store *s, uint32_t macro_id)
{
    macro_rec *mr = macro_find(s, macro_id);
    if (!mr) return 1;   /* SCENE_ERR_MACRO */
    apply_ctx a;
    a.s = s;
    a.arena = s->nodes;
    a.arena_back = &s->nodes;
    a.idm = &s->by_id;
    a.count = &s->node_count;
    a.cap = &s->node_cap;
    a.free_head = &s->node_free;
    a.focus = &s->focus;
    a.texes = s->textures;
    a.tex_count = s->tex_count;
    a.events = 1;
    a.replay = 0;
    uint32_t i;
    for (i = 0; i < mr->count; i++) {
        macro_op *mo = &mr->ops[i];
        uint8_t *p = (uint8_t *)malloc(mo->payload_len ? mo->payload_len : 1);
        if (!p) return -1;
        if (mo->payload_len) memcpy(p, mo->payload, mo->payload_len);
        int skip = macro_resolve(&a, mo, p);
        if (!skip) {
            uint16_t fe = 0;
            int r = apply_op(&a, mo->opcode, p, mo->payload_len, &fe);
            if (r == 1) {
                free(p);
                return (int)fe;   /* resolution failed mid-macro        */
            }
            if (r < 0) { free(p); return -1; }
        }
        free(p);
    }
    return 0;
}

/* Transfer a recorded macro definition from one session to another
 * (OS-service cross-app automation). The definition is copied at the
 * memory level; the wire format is unchanged.                          */
int scene_store_import_macro(scene_store *dst, scene_store *src,
                             uint32_t from_id, uint32_t to_id)
{
    if (!dst || !src || dst == src) return -1;
    if (dst->open_macro != SCENE_NO_MACRO) return -1;
    macro_rec *sr = macro_find(src, from_id);
    if (!sr) return -1;
    if (macro_find(dst, to_id)) return -1;
    if (dst->macro_count == dst->macro_cap) {
        uint32_t nc = dst->macro_cap ? dst->macro_cap * 2 : 8;
        macro_rec *nm = (macro_rec *)realloc(dst->macros, sizeof(macro_rec) * nc);
        if (!nm) return -1;
        dst->macros = nm;
        dst->macro_cap = nc;
    }
    macro_rec *dr = &dst->macros[dst->macro_count];
    memset(dr, 0, sizeof(*dr));
    dr->macro_id = to_id;
    if (sr->count) {
        dr->ops = (macro_op *)malloc(sizeof(macro_op) * sr->count);
        if (!dr->ops) return -1;
        dr->cap = sr->count;
    }
    uint32_t i;
    for (i = 0; i < sr->count; i++) {
        macro_op *so = &sr->ops[i];
        macro_op *do_ = &dr->ops[i];
        do_->opcode = so->opcode;
        do_->id = so->id;
        memcpy(do_->rect, so->rect, sizeof(do_->rect));
        do_->payload_len = so->payload_len;
        do_->payload = (uint8_t *)malloc(so->payload_len ? so->payload_len : 1);
        if (!do_->payload) { dr->count = i; macro_rec_free(dr); return -1; }
        if (so->payload_len) memcpy(do_->payload, so->payload, so->payload_len);
        dr->count++;
    }
    dst->macro_count++;
    return 0;
}

/* ==================================================================== */
/* Public API                                                            */
/* ==================================================================== */

scene_store *scene_store_new(const scene_limits *limits)
{
    scene_store *s = (scene_store *)calloc(1, sizeof(scene_store));
    if (!s) return NULL;
    if (limits) s->lim = *limits;
    else {
        s->lim.max_nodes_per_session   = SCENE_DEFAULT_NODES;
        s->lim.max_text_bytes_per_slot = SCENE_DEFAULT_TEXT_BYTES;
        s->lim.max_text_slots_per_node = SCENE_DEFAULT_TEXT_SLOTS;
        s->lim.max_record_length       = SCENE_DEFAULT_RECORD_LENGTH;
        s->lim.input_latency_budget_us = SCENE_DEFAULT_LATENCY_US;
    }
    if (idmap_init(&s->by_id) != 0) { free(s); return NULL; }
    s->nodes = (node *)calloc(1, sizeof(node));
    if (!s->nodes) { idmap_free(&s->by_id); free(s); return NULL; }
    s->node_cap = 1;
    s->node_count = 0;
    s->node_free = 0;
    s->scene_id = (uint32_t)(now_us(s) & 0xFFFFFFFFu);
    s->next_seq = 1;
    s->scene_seq = 0;
    s->present_seq = 0;
    s->focus = SCENE_NO_PARENT;
    s->mode = SCENE_MODE_LIVE;
    s->open_macro = SCENE_NO_MACRO;
    s->clock = NULL;
    s->clock_ud = NULL;
    return s;
}

void scene_store_free(scene_store *s)
{
    if (!s) return;
    uint32_t i;
    for (i = 1; i <= s->node_count; i++) {
        node *n = &s->nodes[i];
        uint32_t j;
        for (j = 0; j < n->text_count; j++) text_slot_free(&n->texts[j]);
        free(n->texts);
    }
    free(s->nodes);
    idmap_free(&s->by_id);
    replay_teardown(s);
    free(s->textures);
    free(s->log.data);
    for (i = 0; i < s->ckpt_count; i++) db_free(&s->ckpts[i].dump);
    free(s->ckpts);
    for (i = 0; i < s->macro_count; i++) macro_rec_free(&s->macros[i]);
    free(s->macros);
    free(s->out.data);
    free(s);
}

void scene_store_set_clock(scene_store *s, scene_clock_fn fn, void *ud)
{
    s->clock = fn;
    s->clock_ud = ud;
}

int scene_store_welcome(scene_store *s)
{
    dynbuf b = {0};
    int r = -1;
    if (db_put_u32(&b, s->scene_id) == 0 &&
        db_put_u16(&b, SCENE_PROTOCOL_V0) == 0 &&
        db_put_u32(&b, s->lim.max_nodes_per_session) == 0 &&
        db_put_u32(&b, s->lim.max_text_bytes_per_slot) == 0 &&
        db_put_u32(&b, s->lim.max_text_slots_per_node) == 0 &&
        db_put_u32(&b, s->lim.max_record_length) == 0 &&
        db_put_u64(&b, s->lim.input_latency_budget_us) == 0)
        r = emit(s, SCENE_SRV_WELCOME, b.data, b.len);
    db_free(&b);
    return r;
}

int scene_store_ghost_mark(scene_store *s)
{
    uint32_t i;
    /* Mark the whole arena, not just 1..node_count: a destroyed node in
     * the middle leaves a gap, so live nodes can sit at slots beyond the
     * live count (their stale bit must be set or a ghost re-create of a
     * later node hits BAD_NODE). Free slots carry id == UINT32_MAX.      */
    for (i = 1; i < s->node_cap; i++)
        if (s->nodes[i].id != UINT32_MAX) s->nodes[i].stale = 1;
    return 0;
}

int scene_store_rejoin(scene_store *s, uint64_t seq)
{
    if (!s || s->dead) return -1;
    if (s->mode != SCENE_MODE_LIVE) return -1;
    if (seq < s->next_seq) return -1;
    s->next_seq = seq;
    return 0;
}

/* Fatal session failure: emit ERROR and close the session. Used by the
 * server adapter on frame-level protocol violations the engine cannot
 * see (bad magic/version/length/checksum in scene_frame_check).         */
int scene_store_fail(scene_store *s, uint16_t code, const char *msg)
{
    if (!s) return -1;
    if (emit_error(s, code, msg) != 0) return -1;
    s->dead = 1;
    return 0;
}

int scene_store_emit_record(scene_store *s, uint16_t opcode,
                            const uint8_t *payload, uint32_t plen)
{
    if (!s) return -1;
    if (opcode < 0x8000 || opcode > 0x8FFF) return -1;
    if (plen > s->lim.max_record_length) return -1;
    return emit(s, opcode, payload, plen);
}

int scene_store_register_texture(scene_store *s, scene_texture_ref ref,
                                 uint32_t w, uint32_t h,
                                 uint16_t fmt, uint8_t opaque)
{
    uint32_t i;
    for (i = 0; i < s->tex_count; i++)
        if (s->textures[i].ref == ref) return -1;
    if (s->tex_count == s->tex_cap) {
        uint32_t nc = s->tex_cap ? s->tex_cap * 2 : 16;
        tex_ent *nt = (tex_ent *)realloc(s->textures, sizeof(tex_ent) * nc);
        if (!nt) return -1;
        s->textures = nt;
        s->tex_cap = nc;
    }
    s->textures[s->tex_count].ref = ref;
    s->textures[s->tex_count].w = w;
    s->textures[s->tex_count].h = h;
    s->textures[s->tex_count].fmt = fmt;
    s->textures[s->tex_count].opaque = opaque;
    s->tex_count++;
    return 0;
}

int scene_store_release_texture(scene_store *s, scene_texture_ref ref)
{
    uint32_t i;
    for (i = 0; i < s->tex_count; i++) {
        if (s->textures[i].ref != ref) continue;
        s->textures[i] = s->textures[s->tex_count - 1];
        s->tex_count--;
        return 0;
    }
    return -1;
}

int scene_store_texture_registered(const scene_store *s, scene_texture_ref ref)
{
    uint32_t i;
    if (!s) return 0;
    for (i = 0; i < s->tex_count; i++)
        if (s->textures[i].ref == ref) return 1;
    return 0;
}

void scene_store_set_style_count(scene_store *s, uint32_t n) { s->style_count = n; }
void scene_store_set_effect_count(scene_store *s, uint32_t n) { s->effect_count = n; }

int scene_store_input_pointer(scene_store *s, uint8_t device,
                              int32_t x, int32_t y, uint8_t buttons)
{
    if (s->dead) return -SCENE_ERR_STATE;
    if (s->mode != SCENE_MODE_LIVE) return -SCENE_ERR_INPUT_MODE;
    if (s->input_pending) return 0;   /* flow control: drop until acked */
    dynbuf b = {0};
    int r = -1;
    if (db_put_u64(&b, s->scene_seq) == 0 &&
        db_put_u8(&b, device) == 0 &&
        db_put_i32(&b, x) == 0 &&
        db_put_i32(&b, y) == 0 &&
        db_put_u8(&b, buttons) == 0)
        r = emit(s, SCENE_SRV_INPUT_POINTER, b.data, b.len);
    db_free(&b);
    if (r != 0) return r;
    s->input_pending = 1;
    s->last_input_seq = s->scene_seq;
    s->last_input_us = now_us(s);
    if (buttons & 0x01) {
        scene_node_id id = scene_store_region_at(s, x, y);
        if (id != SCENE_NO_PARENT) {
            dynbuf act = {0};
            int r2 = -1;
            if (db_put_u64(&act, s->scene_seq) == 0 &&
                db_put_u32(&act, id) == 0)
                r2 = emit(s, SCENE_SRV_INPUT_ACTIVATE, act.data, act.len);
            db_free(&act);
            if (r2 != 0) return r2;
        }
    }
    return 0;
}

int scene_store_input_key(scene_store *s, uint32_t key_code,
                           uint8_t state, uint8_t modifiers)
{
    if (s->dead) return -SCENE_ERR_STATE;
    if (s->mode != SCENE_MODE_LIVE) return -SCENE_ERR_INPUT_MODE;
    if (s->input_pending) return 0;   /* flow control: drop until acked */
    dynbuf b = {0};
    int r = -1;
    if (db_put_u64(&b, s->scene_seq) == 0 &&
        db_put_u32(&b, key_code) == 0 &&
        db_put_u8(&b, state) == 0 &&
        db_put_u8(&b, modifiers) == 0)
        r = emit(s, SCENE_SRV_INPUT_KEY, b.data, b.len);
    db_free(&b);
    if (r != 0) return r;
    s->input_pending = 1;
    s->last_input_seq = s->scene_seq;
    s->last_input_us = now_us(s);
    return 0;
}

int scene_store_input_text(scene_store *s, const char *text, uint32_t len)
{
    if (s->dead) return -SCENE_ERR_STATE;
    if (s->mode != SCENE_MODE_LIVE) return -SCENE_ERR_INPUT_MODE;
    if (s->input_pending) return 0;   /* flow control: drop until acked */
    if (len && !text) return -SCENE_ERR_PROTOCOL;
    if (len > s->lim.max_record_length - 12u) return -SCENE_ERR_LIMIT;
    dynbuf b = {0};
    int r = -1;
    if (db_put_u64(&b, s->scene_seq) == 0 &&
        db_put_u32(&b, len) == 0 &&
        (len == 0 || db_put(&b, text, len) == 0))
        r = emit(s, SCENE_SRV_INPUT_TEXT, b.data, b.len);
    db_free(&b);
    if (r != 0) return r;
    s->input_pending = 1;
    s->last_input_seq = s->scene_seq;
    s->last_input_us = now_us(s);
    return 0;
}

int scene_store_out_next(scene_store *s, uint16_t *opcode,
                         const uint8_t **payload, uint32_t *payload_len)
{
    if (s->out_off == s->out.len) {
        s->out_off = 0;
        s->out.len = 0;
        return 0;
    }
    const uint8_t *p = s->out.data + s->out_off;
    uint32_t plen = scene_get_u32(p + 8);
    if (s->out_off + SCENE_HEADER_SIZE + plen > s->out.len) return 0;
    *opcode = scene_get_u16(p + 6);
    *payload = p + SCENE_HEADER_SIZE;
    *payload_len = plen;
    s->out_off += SCENE_HEADER_SIZE + plen;
    return 1;
}

/* Like scene_store_out_next but yields the complete framed record
 * (header + payload, checksum included) — the raw bytes that go on the
 * wire. Drains the same outbound buffer as scene_store_out_next; use one
 * drain style per poll cycle, not the two mixed.                        */
int scene_store_out_next_frame(scene_store *s,
                               const uint8_t **frame, uint32_t *frame_len)
{
    if (s->out_off == s->out.len) {
        s->out_off = 0;
        s->out.len = 0;
        return 0;
    }
    const uint8_t *p = s->out.data + s->out_off;
    uint32_t plen = scene_get_u32(p + 8);
    uint32_t total = SCENE_HEADER_SIZE + plen;
    if (s->out_off + total > s->out.len) return 0;
    *frame = p;
    *frame_len = total;
    s->out_off += total;
    return 1;
}

uint64_t scene_store_committed_seq(const scene_store *s) { return s->scene_seq; }
uint64_t scene_store_view_seq(const scene_store *s)
{
    if (!s) return 0;
    if (s->mode == SCENE_MODE_REPLAY && s->rnodes)
        return s->replay_seq;
    return s->scene_seq;
}

int scene_store_in_replay(const scene_store *s)
{
    return s && s->mode == SCENE_MODE_REPLAY && s->rnodes != NULL;
}
uint32_t scene_store_node_count(const scene_store *s) { return s->node_count; }
const scene_limits *scene_store_limits(const scene_store *s) { return &s->lim; }
uint32_t scene_store_style_count(const scene_store *s) { return s->style_count; }
uint32_t scene_store_effect_count(const scene_store *s) { return s->effect_count; }
uint32_t scene_store_texture_count(const scene_store *s) { return s->tex_count; }
scene_node_id scene_store_focus(const scene_store *s) { return s ? s->focus : SCENE_NO_PARENT; }

scene_node_id scene_store_region_at(const scene_store *s,
                                    int32_t x, int32_t y)
{
    const node *arena = s->nodes;
    uint32_t count = s->node_count;
    if (!arena || count == 0) return SCENE_NO_PARENT;
    scene_node_id best = SCENE_NO_PARENT;
    uint32_t best_depth = 0;
    walk_stack ws;
    walk_init(&ws);
    uint32_t c = arena[0].first_child;
    while (c || ws.sp) {
        while (c) {
            if (walk_push(&ws, c) != 0) { walk_free(&ws); return best; }
            c = arena[c].first_child;
        }
        c = ws.buf[--ws.sp];
        const node *nd = &arena[c];
        if (!nd->stale && (nd->flags & SCENE_FLAG_VISIBLE) &&
            x >= nd->rect[0] && y >= nd->rect[1] &&
            x < nd->rect[0] + nd->rect[2] &&
            y < nd->rect[1] + nd->rect[3]) {
            if (nd->depth >= best_depth) {
                best = nd->id;
                best_depth = nd->depth;
            }
        }
        c = arena[c].next_sib;
    }
    walk_free(&ws);
    return best;
}

size_t scene_store_search(const scene_store *s, const char *term,
                          uint32_t term_len,
                          scene_node_id *out_nodes, size_t out_cap,
                          scene_text_id *out_texts, size_t *out_text_cap)
{
    size_t hits = 0;
    walk_stack ws;
    walk_init(&ws);
    uint32_t c = s->nodes[0].first_child;
    while (c || ws.sp) {
        while (c) {
            if (walk_push(&ws, c) != 0) { walk_free(&ws); return hits; }
            const node *nd = &s->nodes[c];
            if (!(nd->flags & SCENE_FLAG_VISIBLE)) {
                c = s->nodes[c].first_child;
                continue;
            }
            uint32_t i;
            for (i = 0; i < nd->text_count; i++) {
                const text_slot *t = &nd->texts[i];
                if (t->len < term_len) continue;
                if (!ascii_ci_find(t->data, t->len, term, term_len)) continue;
                if (hits < out_cap) out_nodes[hits] = nd->id;
                if (out_texts && out_text_cap && hits < *out_text_cap)
                    out_texts[hits] = t->text_id;
                hits++;
            }
            c = s->nodes[c].first_child;
        }
        c = ws.buf[--ws.sp];
        c = s->nodes[c].next_sib;
    }
    walk_free(&ws);
    if (out_text_cap && hits < *out_text_cap) *out_text_cap = hits;
    return hits;
}

/* Pick the arena + id map the read views serve from. During replay the
 * replay arena is built from the log (snapshot restores + deltas), so
 * walk/vis/texts follow the same rule as req_arena: REPLAY mode serves
 * the rebuilt scene, LIVE serves the live scene. node_count stays the
 * live count (locked by test_replay_determinism).                      */
static void view_arena(const scene_store *s, const node **arena,
                       const idmap **idm)
{
    if (s->mode == SCENE_MODE_REPLAY && s->rnodes) {
        *arena = s->rnodes;
        *idm = &s->rid;
    } else {
        *arena = s->nodes;
        *idm = &s->by_id;
    }
}

void scene_store_walk(const scene_store *s,
                      int (*cb)(scene_node_id id, void *out), void *out)
{
    const node *ar;
    const idmap *im;
    walk_stack ws;
    uint32_t c;

    if (!s || !cb) return;
    view_arena(s, &ar, &im);
    walk_init(&ws);
    c = ar[0].first_child;
    while (c || ws.sp) {
        while (c) {
            if (walk_push(&ws, c) != 0) { walk_free(&ws); return; }
            if (cb(ar[c].id, out)) { walk_free(&ws); return; }
            c = ar[c].first_child;
        }
        c = ws.buf[--ws.sp];
        c = ar[c].next_sib;
    }
    walk_free(&ws);
}

/* ---- per-node read view -------------------------------------------------- */

int scene_store_node_vis(const scene_store *s, scene_node_id id,
                         scene_node_vis *out)
{
    const node *ar;
    const idmap *im;
    uint32_t slot;

    if (!s || !out) return -1;
    view_arena(s, &ar, &im);
    slot = idmap_get(im, id);
    if (!slot) return -1;
    const node *n = &ar[slot];
    out->id = id;
    out->parent = n->parent ? ar[n->parent].id : SCENE_NO_PARENT;
    out->role = n->role;
    out->flags = n->flags;
    out->stale = n->stale;
    memcpy(out->rect, n->rect, sizeof(out->rect));
    memcpy(out->tex_src, n->tex_src, sizeof(out->tex_src));
    out->style = n->style;
    out->effect = n->effect;
    out->tex = n->tex;
    out->blend = n->blend;
    out->opacity = n->opacity;
    out->text_count = n->text_count;
    return 0;
}

int scene_store_node_texts(const scene_store *s, scene_node_id id,
                           scene_node_text_vis *out, uint32_t cap)
{
    const node *ar;
    const idmap *im;
    uint32_t slot;

    if (!s || !out) return -1;
    view_arena(s, &ar, &im);
    slot = idmap_get(im, id);
    if (!slot) return -1;
    const node *n = &ar[slot];
    uint32_t i, fill = n->text_count < cap ? n->text_count : cap;
    for (i = 0; i < fill; i++) {
        out[i].text_id = n->texts[i].text_id;
        out[i].len = n->texts[i].len;
        out[i].data = n->texts[i].data;
    }
    return (int)n->text_count;
}

uint32_t scene_store_node_child_count(const scene_store *s, scene_node_id id)
{
    const node *ar;
    const idmap *im;
    uint32_t slot;
    if (!s) return 0;
    view_arena(s, &ar, &im);
    slot = idmap_get(im, id);
    if (!slot) return 0;
    const node *n = &ar[slot];
    uint32_t cnt = 0;
    uint32_t c = n->first_child;
    while (c) { cnt++; c = ar[c].next_sib; }
    return cnt;
}

/* ==================================================================== */
/* Ingest dispatcher                                                     */
/* ==================================================================== */

int scene_store_ingest(scene_store *s, uint16_t opcode,
                       const uint8_t *payload, uint32_t plen)
{
    if (s->dead) return -SCENE_ERR_STATE;
    if (plen > s->lim.max_record_length)
        return fatal_error(s, SCENE_ERR_LIMIT, "record exceeds max_record_length");

    /* Ack: first field is the consumed input seq, not the stream
     * counter; acks do not advance the stream counter.                  */
    if (opcode == SCENE_OP_ACK) {
        if (plen != 16) return fatal_error(s, SCENE_ERR_PROTOCOL, "ack len");
        uint64_t consumed = scene_get_u64(payload);
        if (consumed >= s->last_input_seq) s->input_pending = 0;
        if (log_append(s, opcode, payload, plen) != 0)
            return fatal_error(s, SCENE_ERR_LIMIT, "log");
        return 0;
    }

    if (plen < 8) return fatal_error(s, SCENE_ERR_PROTOCOL, "no seq");
    uint64_t seq = scene_get_u64(payload);
    if (seq != s->next_seq) {
        fprintf(stderr, "scene_store: SEQ MISMATCH opcode 0x%04x got %llu expected %llu\n",
                opcode, (unsigned long long)seq, (unsigned long long)s->next_seq);
        return fatal_error(s, SCENE_ERR_SEQ, "non-monotonic seq");
    }
    s->next_seq = seq + 1;

    switch (opcode) {

    case SCENE_OP_CREATE_NODE: case SCENE_OP_DESTROY_NODE:
    case SCENE_OP_SET_TEXT: case SCENE_OP_SET_VALUE:
    case SCENE_OP_SET_RECT: case SCENE_OP_SET_FLAGS:
    case SCENE_OP_SET_STYLE: case SCENE_OP_SET_TEXTURE:
    case SCENE_OP_SET_EFFECT: case SCENE_OP_FOCUS:
    case SCENE_OP_FOCUS_NEXT: {
        if (s->mode == SCENE_MODE_REPLAY)
            return fatal_error(s, SCENE_ERR_STATE, "mutation in replay");
        apply_ctx a;
        a.s = s;
        a.arena = s->nodes;
        a.arena_back = &s->nodes;
        a.idm = &s->by_id;
        a.count = &s->node_count;
        a.cap = &s->node_cap;
        a.free_head = &s->node_free;
        a.focus = &s->focus;
        a.texes = s->textures;
        a.tex_count = s->tex_count;
        a.events = 1;
        a.replay = 0;
        uint16_t fe = 0;
        int r = apply_op(&a, opcode, payload, plen, &fe);
        if (r == 1) {
            static const char *msg = "op rejected";
            return fatal_error(s, fe, msg);
        }
        if (r < 0) return fatal_error(s, SCENE_ERR_LIMIT, "oom");
        if (s->open_macro != SCENE_NO_MACRO) {
            if (macro_capture(s, opcode, payload, plen) != 0)
                return fatal_error(s, SCENE_ERR_LIMIT, "macro cap");
        }
        s->scene_seq = seq;
        if (log_append(s, opcode, payload, plen) != 0)
            return fatal_error(s, SCENE_ERR_LIMIT, "log");
        return 0;
    }

    case SCENE_OP_PRESENT: {
        if (s->mode == SCENE_MODE_REPLAY)
            return fatal_error(s, SCENE_ERR_STATE, "present in replay");
        if (plen != 16) return fatal_error(s, SCENE_ERR_PROTOCOL, "present len");
        uint64_t token = scene_get_u64(payload + 8);
        s->present_seq = s->scene_seq;
        s->last_token = token;
        uint64_t lat = s->last_input_us ? now_us(s) - s->last_input_us : 0;
        if (lat > s->lim.input_latency_budget_us) s->budget_violations++;
        if (lat > s->budget_max_us) s->budget_max_us = lat;
        dynbuf b = {0};
        int r = -1;
        if (db_put_u64(&b, s->present_seq) == 0 &&
            db_put_u64(&b, token) == 0 &&
            db_put_u64(&b, lat) == 0)
            r = emit(s, SCENE_SRV_PRESENT_DONE, b.data, b.len);
        db_free(&b);
        if (r != 0) return fatal_error(s, SCENE_ERR_LIMIT, "present emit");
        if (log_append(s, opcode, payload, plen) != 0)
            return fatal_error(s, SCENE_ERR_LIMIT, "log");
        return 0;
    }

    case SCENE_OP_IMPORT_TEXTURE: {
        /* Host-side request (OS importer decodes the file; the engine's
         * only duty is strict length validation + seq advancement). The
         * ref becomes valid for SET_TEXTURE only after the host calls
         * scene_store_register_texture on this session's store.       */
        if (plen < 16) return fatal_error(s, SCENE_ERR_PROTOCOL, "imp len");
        uint32_t tlen = scene_get_u32(payload + 12);
        if (plen != 16 + tlen)
            return fatal_error(s, SCENE_ERR_PROTOCOL, "imp path");
        return 0;
    }

    case SCENE_OP_SNAPSHOT: {
        if (plen != 12) return fatal_error(s, SCENE_ERR_PROTOCOL, "snap len");
        uint32_t req_id = scene_get_u32(payload + 8);
        if (log_append(s, opcode, payload, plen) != 0)
            return fatal_error(s, SCENE_ERR_LIMIT, "log");
        if (emit_snapshot_reply(s, req_id) != 0)
            return fatal_error(s, SCENE_ERR_LIMIT, "snap emit");
        return 0;
    }

    case SCENE_OP_CAPTURE: {
        if (plen != 12) return fatal_error(s, SCENE_ERR_PROTOCOL, "cap len");
        uint32_t req_id = scene_get_u32(payload + 8);
        if (log_append(s, opcode, payload, plen) != 0)
            return fatal_error(s, SCENE_ERR_LIMIT, "log");
        if (emit_capture_reply(s, req_id) != 0)
            return fatal_error(s, SCENE_ERR_LIMIT, "cap emit");
        return 0;
    }

    case SCENE_OP_SEARCH: {
        if (plen < 12) return fatal_error(s, SCENE_ERR_PROTOCOL, "search len");
        uint32_t req_id = scene_get_u32(payload + 8);
        uint32_t tlen = scene_get_u32(payload + 12);
        if (plen != 16 + tlen)
            return fatal_error(s, SCENE_ERR_PROTOCOL, "search term");
        if (log_append(s, opcode, payload, plen) != 0)
            return fatal_error(s, SCENE_ERR_LIMIT, "log");
        if (emit_search_reply(s, req_id, (const char *)payload + 16, tlen) != 0)
            return fatal_error(s, SCENE_ERR_LIMIT, "search emit");
        return 0;
    }

    case SCENE_OP_PING: {
        if (plen != 16) return fatal_error(s, SCENE_ERR_PROTOCOL, "ping len");
        uint64_t nonce = scene_get_u64(payload + 8);
        if (log_append(s, opcode, payload, plen) != 0)
            return fatal_error(s, SCENE_ERR_LIMIT, "log");
        if (emit_pong(s, nonce) != 0)
            return fatal_error(s, SCENE_ERR_LIMIT, "pong");
        return 0;
    }

    case SCENE_OP_MACRO_BEGIN: {
        if (plen != 12) return fatal_error(s, SCENE_ERR_PROTOCOL, "mb len");
        if (s->open_macro != SCENE_NO_MACRO)
            return fatal_error(s, SCENE_ERR_MACRO, "macro already open");
        uint32_t mid = scene_get_u32(payload + 8);
        if (macro_find(s, mid))
            return fatal_error(s, SCENE_ERR_MACRO, "macro id exists");
        if (s->macro_count == s->macro_cap) {
            uint32_t nc = s->macro_cap ? s->macro_cap * 2 : 8;
            macro_rec *nm = (macro_rec *)realloc(s->macros, sizeof(macro_rec) * nc);
            if (!nm) return fatal_error(s, SCENE_ERR_LIMIT, "oom");
            s->macros = nm;
            s->macro_cap = nc;
        }
        macro_rec *mr = &s->macros[s->macro_count];
        memset(mr, 0, sizeof(*mr));
        mr->macro_id = mid;
        s->macro_count++;
        s->open_macro = s->macro_count - 1;
        if (log_append(s, opcode, payload, plen) != 0)
            return fatal_error(s, SCENE_ERR_LIMIT, "log");
        return 0;
    }

    case SCENE_OP_MACRO_END: {
        if (plen != 12) return fatal_error(s, SCENE_ERR_PROTOCOL, "me len");
        if (s->open_macro == SCENE_NO_MACRO)
            return fatal_error(s, SCENE_ERR_MACRO, "no open macro");
        uint32_t mid = scene_get_u32(payload + 8);
        if (s->macros[s->open_macro].macro_id != mid)
            return fatal_error(s, SCENE_ERR_MACRO, "macro id mismatch");
        s->open_macro = SCENE_NO_MACRO;
        if (log_append(s, opcode, payload, plen) != 0)
            return fatal_error(s, SCENE_ERR_LIMIT, "log");
        return 0;
    }

    case SCENE_OP_EXEC_MACRO: {
        if (plen != 12) return fatal_error(s, SCENE_ERR_PROTOCOL, "ex len");
        if (s->mode == SCENE_MODE_REPLAY)
            return fatal_error(s, SCENE_ERR_STATE, "exec in replay");
        uint32_t mid = scene_get_u32(payload + 8);
        if (log_append(s, opcode, payload, plen) != 0)
            return fatal_error(s, SCENE_ERR_LIMIT, "log");
        int r = macro_exec(s, mid);
        if (r == 1) return fatal_error(s, SCENE_ERR_MACRO, "macro not found");
        if (r < 0) return fatal_error(s, SCENE_ERR_LIMIT, "macro oom");
        if (r > 1) return fatal_error(s, (uint16_t)r, "macro op rejected");
        s->scene_seq = seq;
        return 0;
    }

    case SCENE_OP_SET_INPUT_MODE: {
        if (plen != 9) return fatal_error(s, SCENE_ERR_PROTOCOL, "mode len");
        uint8_t mode = payload[8];
        if (mode > SCENE_MODE_RECORD)
            return fatal_error(s, SCENE_ERR_PROTOCOL, "mode value");
        if (log_append(s, opcode, payload, plen) != 0)
            return fatal_error(s, SCENE_ERR_LIMIT, "log");
        if (mode == SCENE_MODE_LIVE) replay_teardown(s);
        s->mode = mode;
        return 0;
    }

    case SCENE_OP_SEEK: {
        if (plen != 16) return fatal_error(s, SCENE_ERR_PROTOCOL, "seek len");
        if (s->mode != SCENE_MODE_REPLAY && s->mode != SCENE_MODE_RECORD)
            return fatal_error(s, SCENE_ERR_STATE, "seek in live mode");
        uint64_t target = scene_get_u64(payload + 8);
        if (target > s->next_seq - 1)
            return fatal_error(s, SCENE_ERR_SEQ, "seek past end");
        if (log_append(s, opcode, payload, plen) != 0)
            return fatal_error(s, SCENE_ERR_LIMIT, "log");
        if (replay_build(s, target) != 0)
            return fatal_error(s, SCENE_ERR_STATE, "replay build failed");
        return 0;
    }

    default:
        return fatal_error(s, SCENE_ERR_PROTOCOL, "unknown opcode");
    }
}

/* ---- host-side WM service ---------------------------------------------- */

/* Apply one host mutation through the engine's own commit path: the
 * mutation hits the live arena via apply_op (same op handlers as wire
 * records), then the committed seq advances so the compositor's per-layer
 * diff fires. The peer's stream counter is NOT consumed (no wire bytes;
 * the fabricated seq in the payload is an audit tag only, never compared
 * against s->next_seq). The op log is intentionally left untouched:
 * replay/seek reconstructs the client's own committed input history, and
 * host WM interventions are OS actions outside that history. */
static int host_mutate(scene_store *s, uint16_t opcode,
                       const uint8_t *payload, uint32_t plen)
{
    if (!s || s->dead) return -1;
    if (s->mode == SCENE_MODE_REPLAY) return -1;
    apply_ctx a;
    a.s = s;
    a.arena = s->nodes;
    a.arena_back = &s->nodes;
    a.idm = &s->by_id;
    a.count = &s->node_count;
    a.cap = &s->node_cap;
    a.free_head = &s->node_free;
    a.focus = &s->focus;
    a.texes = s->textures;
    a.tex_count = s->tex_count;
    a.events = 0;      /* host interventions: no wire bytes to the peer */
    a.replay = 0;
    uint16_t fe = 0;
    int r = apply_op(&a, opcode, payload, plen, &fe);
    if (r == 1) return -1;   /* user error (unknown/stale node, etc.) */
    if (r < 0) return -1;    /* internal failure                       */
    s->scene_seq++;          /* one committed seq, no wire seq consumed */
    return 0;
}

int scene_store_host_focus(scene_store *s, scene_node_id id)
{
    if (!s || s->dead) return -1;
    if (s->mode == SCENE_MODE_REPLAY) return -1;
    uint32_t slot = idmap_get(&s->by_id, id);
    if (!slot) return -1;
    node *n = &s->nodes[slot];
    if (n->stale) return -1;      /* ghost-crashed node: no-op would skip */
    if (scene_store_focus(s) == id) return 0;   /* already the focus */
    uint8_t p[12];
    scene_put_u64(p, s->scene_seq);   /* audit tag; not a wire seq */
    scene_put_u32(p + 8, id);
    return host_mutate(s, SCENE_OP_FOCUS, p, sizeof(p));
}

int scene_store_host_set_visible(scene_store *s, scene_node_id id, int on)
{
    if (!s || s->dead) return -1;
    if (s->mode == SCENE_MODE_REPLAY) return -1;
    uint32_t slot = idmap_get(&s->by_id, id);
    if (!slot) return -1;
    node *n = &s->nodes[slot];
    if (n->stale) return -1;
    uint8_t want = on ? 1 : 0;
    if (((n->flags & SCENE_FLAG_VISIBLE) != 0) == want) return 0;
    uint8_t p[13];
    scene_put_u64(p, s->scene_seq);   /* audit tag; not a wire seq */
    scene_put_u32(p + 8, id);
    p[12] = (uint8_t)(want ? (n->flags | SCENE_FLAG_VISIBLE)
                           : (n->flags & ~SCENE_FLAG_VISIBLE));
    return host_mutate(s, SCENE_OP_SET_FLAGS, p, sizeof(p));
}

int scene_store_host_set_rect(scene_store *s, scene_node_id id,
                              int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!s || s->dead) return -1;
    if (s->mode == SCENE_MODE_REPLAY) return -1;
    uint32_t slot = idmap_get(&s->by_id, id);
    if (!slot) return -1;
    node *n = &s->nodes[slot];
    if (n->stale) return -1;
    if (n->rect[0] == x && n->rect[1] == y &&
        n->rect[2] == w && n->rect[3] == h) return 0;   /* no change */
    uint8_t p[28];
    scene_put_u64(p, s->scene_seq);   /* audit tag; not a wire seq */
    scene_put_u32(p + 8, id);
    scene_put_i32(p + 12, x);
    scene_put_i32(p + 16, y);
    scene_put_i32(p + 20, w);
    scene_put_i32(p + 24, h);
    return host_mutate(s, SCENE_OP_SET_RECT, p, sizeof(p));
}

/* ---- internal-consumer mode transitions -------------------------------- */

int scene_store_begin_replay(scene_store *s)
{
    if (!s || s->dead) return -1;
    if (s->mode == SCENE_MODE_REPLAY) return -1;
    s->mode = SCENE_MODE_REPLAY;
    if (replay_build(s, 0) != 0) {
        s->mode = SCENE_MODE_LIVE;
        return -1;
    }
    return 0;
}

int scene_store_end_replay(scene_store *s)
{
    if (!s || s->dead) return -1;
    if (s->mode != SCENE_MODE_REPLAY) return -1;
    replay_teardown(s);
    s->mode = SCENE_MODE_LIVE;
    return 0;
}

int scene_store_seek_to(scene_store *s, uint64_t target)
{
    if (!s || s->dead) return -1;
    if (s->mode != SCENE_MODE_REPLAY) return -1;
    if (target < 1) target = 1;
    if (target > s->next_seq - 1) target = s->next_seq - 1;
    if (replay_build(s, target) != 0) return -1;
    return 0;
}

