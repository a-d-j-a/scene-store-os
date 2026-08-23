/*
 * scene_client.c — reference wire client for the locked v0 protocol.
 *
 * Implements the client half of scene-store-spec.md v0 exactly:
 * seq-managed op stream (every client->server record starts with a
 * monotonic seq; Ack is the exception), framed outbound records with
 * the amended checksum coverage [0, 16+length), inbound frame
 * validation, typed server-record dispatch, and the ghost-crash
 * reconnect op log (scene-affecting ops only; see scene_client.h).
 */
#define _POSIX_C_SOURCE 200809L
#include "scene_client.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- growable buffers -------------------------------------------------- */

typedef struct cl_buf {
    uint8_t *data;
    uint32_t len, cap;
} cl_buf;

static int cb_need(cl_buf *b, uint32_t need)
{
    if (b->cap >= need) return 0;
    uint32_t nc = b->cap ? b->cap : 512;
    while (nc < need) nc <<= 1;
    uint8_t *nd = (uint8_t *)realloc(b->data, nc);
    if (!nd) return -1;
    b->data = nd;
    b->cap = nc;
    return 0;
}

static void cb_free(cl_buf *b)
{
    free(b->data);
    b->data = NULL; b->len = b->cap = 0;
}

/* ---- reconnect op log -------------------------------------------------- */

typedef struct clog_ent {
    uint16_t opcode;
    uint32_t blen;              /* body length (payload minus seq)        */
    uint8_t *body;
} clog_ent;

static int  clog_append(scene_client *c, uint16_t opcode,
                        const uint8_t *body, uint32_t blen);
static void clog_free(scene_client *c);

/* ---- the client ---------------------------------------------------------- */

struct scene_client {
    scene_transport *t;
    const scene_client_cbs *cbs;
    void *ud;
    char *target;

    int conn_open;              /* transport open                          */
    int welcomed;               /* WELCOME seen on this connection         */
    int fatal;                  /* server ERROR or corrupt record: closed  */
    int had_session;            /* a previous connection had a session     */
    uint32_t prev_scene_id;

    uint32_t scene_id;
    scene_limits lim;
    uint64_t next_seq;          /* next stream seq to assign               */
    uint64_t sent_seq;          /* last seq sent (reconnect continuation)  */
    uint64_t last_token;
    uint64_t last_present_seq;

    cl_buf out;                 /* pending outbound frames                 */
    uint32_t out_off;
    cl_buf in;                  /* inbound reassembly                      */
    uint32_t in_off;

    clog_ent *log;
    uint32_t log_count, log_cap;
};

scene_client *scene_client_new(void)
{
    scene_client *c = (scene_client *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->lim.max_record_length = SCENE_DEFAULT_RECORD_LENGTH;
    return c;
}

void scene_client_free(scene_client *c)
{
    if (!c) return;
    scene_transport_close(c->t);
    free(c->target);
    cb_free(&c->out);
    cb_free(&c->in);
    clog_free(c);
    free(c);
}

int scene_client_dead(const scene_client *c)
{
    return !c->conn_open || c->fatal;
}

int scene_client_connect(scene_client *c, scene_transport *t,
                         const char *target,
                         const scene_client_cbs *cbs, void *ud)
{
    if (!c || !t || !target) return -1;
    free(c->target);
    size_t tl = strlen(target);
    c->target = (char *)malloc(tl + 1);
    if (!c->target) return -1;
    memcpy(c->target, target, tl + 1);
    c->t = t;
    c->cbs = cbs;
    c->ud = ud;
    c->conn_open = 0;
    c->welcomed = 0;
    c->fatal = 0;
    c->next_seq = 1;
    c->sent_seq = 0;
    c->out.len = c->out_off = 0;
    c->in.len = c->in_off = 0;
    if (scene_transport_open(t, target) != 0) {
        c->conn_open = 0;
        if (cbs && cbs->closed) cbs->closed(ud);
        return -1;
    }
    c->conn_open = 1;
    return 0;
}

int scene_client_reconnect(scene_client *c, scene_transport *t)
{
    if (!c || !t) return -1;
    if (c->fatal) return -1;    /* server ERROR closed the session: dead   */
    scene_transport_close(c->t);
    c->t = t;
    c->conn_open = 0;
    c->welcomed = 0;
    c->out.len = c->out_off = 0;
    c->in.len = c->in_off = 0;
    if (scene_transport_open(t, c->target) != 0) return -1;
    c->conn_open = 1;
    return 0;
}

/* ---- outbound ------------------------------------------------------------ */

static int clog_append(scene_client *c, uint16_t opcode,
                       const uint8_t *body, uint32_t blen)
{
    if (c->log_count == c->log_cap) {
        uint32_t nc = c->log_cap ? c->log_cap * 2 : 32;
        clog_ent *nl = (clog_ent *)realloc(c->log, sizeof(clog_ent) * nc);
        if (!nl) return -1;
        c->log = nl;
        c->log_cap = nc;
    }
    clog_ent *e = &c->log[c->log_count];
    e->opcode = opcode;
    e->blen = blen;
    e->body = (uint8_t *)malloc(blen ? blen : 1);
    if (!e->body) return -1;
    if (blen) memcpy(e->body, body, blen);
    c->log_count++;
    return 0;
}

static void clog_free(scene_client *c)
{
    uint32_t i;
    for (i = 0; i < c->log_count; i++) free(c->log[i].body);
    free(c->log);
    c->log = NULL; c->log_count = c->log_cap = 0;
}

/* Append one framed record. `body` is the payload WITHOUT the leading
 * seq, except for ack ops where it is the full 16-byte payload (the
 * ack's first field is the consumed input seq, not the stream counter).
 * `log_it` copies the body into the reconnect log (scene-affecting ops
 * only). Returns 0 on success. */
static int cli_emit(scene_client *c, uint16_t opcode,
                    const uint8_t *body, uint32_t blen,
                    int is_ack, int log_it)
{
    if (!c->conn_open || !c->welcomed || c->fatal) {
        fprintf(stderr, "cli_emit blocked: conn_open %d welcomed %d fatal %d opcode 0x%04x\n",
                c->conn_open, c->welcomed, c->fatal, opcode);
        fflush(stderr);
        return -1;
    }
    uint64_t seq = 0;
    uint32_t plen;
    if (is_ack) {
        plen = blen;
    } else {
        seq = c->next_seq;
        c->next_seq++;
        c->sent_seq = seq;
        plen = 8u + blen;
    }
    uint32_t total = SCENE_HEADER_SIZE + plen;
    if (cb_need(&c->out, c->out.len + total) != 0) return -1;
    uint8_t *f = c->out.data + c->out.len;
    scene_put_u32(f + 0, SCENE_MAGIC);
    scene_put_u16(f + 4, SCENE_PROTOCOL_V0);
    scene_put_u16(f + 6, opcode);
    scene_put_u32(f + 8, plen);
    scene_put_u32(f + 12, 0);   /* checksum placeholder (zeroed at hash)  */
    if (is_ack) {
        memcpy(f + SCENE_HEADER_SIZE, body, blen);
    } else {
        scene_put_u64(f + SCENE_HEADER_SIZE, seq);
        if (blen) memcpy(f + SCENE_HEADER_SIZE + 8, body, blen);
    }
    uint32_t ck = scene_fnv1a32(f, total);
    scene_put_u32(f + 12, ck);
    c->out.len += total;
    if (log_it) {
        if (clog_append(c, opcode, body, blen) != 0) return -1;
    }
    return 0;
}

int scene_client_flush(scene_client *c)
{
    fprintf(stderr, "scene_client_flush: conn_open %d out_len %u out_off %u next_seq %llu t %p\n",
            c->conn_open, c->out.len, c->out_off, (unsigned long long)c->next_seq, (void*)c->t);
    fflush(stderr);
    if (!c->conn_open) return -1;
    if (c->out_off < c->out.len) {
        if (scene_transport_send(c->t, c->out.data + c->out_off,
                                 c->out.len - c->out_off) != 0) {
            c->conn_open = 0;
            if (c->cbs && c->cbs->closed) c->cbs->closed(c->ud);
            return -1;
        }
    }
    c->out.len = c->out_off = 0;
    return 0;
}

/* ---- op builders (body layouts: payload minus the leading seq) ----------- */

int scene_client_create_node(scene_client *c, scene_node_id parent,
                             scene_node_id id, scene_role role,
                             const scene_rect *rect, uint8_t flags)
{
    if (role > SCENE_ROLE_MAX || id == SCENE_NO_PARENT || !rect) return -1;
    uint8_t b[27];
    scene_put_u32(b + 0, parent);
    scene_put_u32(b + 4, id);
    scene_put_u16(b + 8, (uint16_t)role);
    scene_put_i32(b + 10, rect->x);
    scene_put_i32(b + 14, rect->y);
    scene_put_i32(b + 18, rect->w);
    scene_put_i32(b + 22, rect->h);
    b[26] = flags;
    return cli_emit(c, SCENE_OP_CREATE_NODE, b, sizeof(b), 0, 1);
}

int scene_client_destroy_node(scene_client *c, scene_node_id id)
{
    uint8_t b[4];
    scene_put_u32(b, id);
    return cli_emit(c, SCENE_OP_DESTROY_NODE, b, sizeof(b), 0, 1);
}

static int cli_set_text_like(scene_client *c, uint16_t opcode,
                             scene_node_id id, scene_text_id slot,
                             const char *utf8, uint32_t len)
{
    if (!utf8 && len) return -1;
    if (len > c->lim.max_text_bytes_per_slot) return -1;
    uint8_t *b = (uint8_t *)malloc(12u + len);
    if (!b) return -1;
    scene_put_u32(b + 0, id);
    scene_put_u32(b + 4, slot);
    scene_put_u32(b + 8, len);
    if (len) memcpy(b + 12, utf8, len);
    int r = cli_emit(c, opcode, b, 12u + len, 0, 1);
    free(b);
    return r;
}

int scene_client_set_text(scene_client *c, scene_node_id id,
                          scene_text_id slot, const char *utf8, uint32_t len)
{ return cli_set_text_like(c, SCENE_OP_SET_TEXT, id, slot, utf8, len); }

int scene_client_set_value(scene_client *c, scene_node_id id,
                           scene_text_id slot, const char *utf8, uint32_t len)
{ return cli_set_text_like(c, SCENE_OP_SET_VALUE, id, slot, utf8, len); }

int scene_client_set_rect(scene_client *c, scene_node_id id,
                          const scene_rect *rect)
{
    if (!rect) return -1;
    uint8_t b[20];
    scene_put_u32(b + 0, id);
    scene_put_i32(b + 4, rect->x);
    scene_put_i32(b + 8, rect->y);
    scene_put_i32(b + 12, rect->w);
    scene_put_i32(b + 16, rect->h);
    return cli_emit(c, SCENE_OP_SET_RECT, b, sizeof(b), 0, 1);
}

int scene_client_set_flags(scene_client *c, scene_node_id id, uint8_t flags)
{
    uint8_t b[5];
    scene_put_u32(b + 0, id);
    b[4] = flags;
    return cli_emit(c, SCENE_OP_SET_FLAGS, b, sizeof(b), 0, 1);
}

int scene_client_set_style(scene_client *c, scene_node_id id,
                           scene_style_ref style)
{
    uint8_t b[8];
    scene_put_u32(b + 0, id);
    scene_put_u32(b + 4, style);
    return cli_emit(c, SCENE_OP_SET_STYLE, b, sizeof(b), 0, 1);
}

int scene_client_set_texture(scene_client *c, scene_node_id id,
                             scene_texture_ref tex, const scene_rect *src,
                             uint8_t blend, uint8_t opacity)
{
    if (tex == SCENE_NO_TEXTURE || !src) return -1;
    uint8_t b[26];
    scene_put_u32(b + 0, id);
    scene_put_u32(b + 4, tex);
    scene_put_i32(b + 8, src->x);
    scene_put_i32(b + 12, src->y);
    scene_put_i32(b + 16, src->w);
    scene_put_i32(b + 20, src->h);
    b[24] = blend;
    b[25] = opacity;
    return cli_emit(c, SCENE_OP_SET_TEXTURE, b, sizeof(b), 0, 1);
}

int scene_client_set_effect(scene_client *c, scene_node_id id,
                            scene_effect_ref effect)
{
    uint8_t b[8];
    scene_put_u32(b + 0, id);
    scene_put_u32(b + 4, effect);
    return cli_emit(c, SCENE_OP_SET_EFFECT, b, sizeof(b), 0, 1);
}

int scene_client_focus(scene_client *c, scene_node_id id)
{
    uint8_t b[4];
    scene_put_u32(b, id);
    return cli_emit(c, SCENE_OP_FOCUS, b, sizeof(b), 0, 1);
}

int scene_client_focus_next(scene_client *c, int8_t step)
{
    if (step == 0) return -1;
    uint8_t b[1];
    b[0] = (uint8_t)step;
    return cli_emit(c, SCENE_OP_FOCUS_NEXT, b, sizeof(b), 0, 1);
}

int scene_client_present(scene_client *c, uint64_t token)
{
    uint8_t b[8];
    scene_put_u64(b, token);
    return cli_emit(c, SCENE_OP_PRESENT, b, sizeof(b), 0, 0);
}

int scene_client_snapshot(scene_client *c, uint32_t req_id)
{
    uint8_t b[4];
    scene_put_u32(b, req_id);
    return cli_emit(c, SCENE_OP_SNAPSHOT, b, sizeof(b), 0, 0);
}

int scene_client_search(scene_client *c, uint32_t req_id,
                        const char *term, uint32_t len)
{
    if (!term && len) return -1;
    uint8_t *b = (uint8_t *)malloc(8u + len);
    if (!b) return -1;
    scene_put_u32(b + 0, req_id);
    scene_put_u32(b + 4, len);
    if (len) memcpy(b + 8, term, len);
    int r = cli_emit(c, SCENE_OP_SEARCH, b, 8u + len, 0, 0);
    free(b);
    return r;
}

int scene_client_macro_begin(scene_client *c, uint32_t macro_id)
{
    uint8_t b[4];
    scene_put_u32(b, macro_id);
    return cli_emit(c, SCENE_OP_MACRO_BEGIN, b, sizeof(b), 0, 0);
}

int scene_client_macro_end(scene_client *c, uint32_t macro_id)
{
    uint8_t b[4];
    scene_put_u32(b, macro_id);
    return cli_emit(c, SCENE_OP_MACRO_END, b, sizeof(b), 0, 0);
}

int scene_client_exec_macro(scene_client *c, uint32_t macro_id)
{
    uint8_t b[4];
    scene_put_u32(b, macro_id);
    return cli_emit(c, SCENE_OP_EXEC_MACRO, b, sizeof(b), 0, 0);
}

int scene_client_capture(scene_client *c, uint32_t req_id)
{
    uint8_t b[4];
    scene_put_u32(b, req_id);
    return cli_emit(c, SCENE_OP_CAPTURE, b, sizeof(b), 0, 0);
}

int scene_client_ack(scene_client *c, uint64_t consumed_seq)
{
    uint8_t b[16];
    scene_put_u64(b + 0, consumed_seq);
    scene_put_u64(b + 8, c->last_token);
    return cli_emit(c, SCENE_OP_ACK, b, sizeof(b), 1, 0);
}

int scene_client_ping(scene_client *c, uint64_t nonce)
{
    uint8_t b[8];
    scene_put_u64(b, nonce);
    return cli_emit(c, SCENE_OP_PING, b, sizeof(b), 0, 0);
}

int scene_client_set_input_mode(scene_client *c, uint8_t mode)
{
    if (mode > SCENE_MODE_RECORD) return -1;
    uint8_t b[1];
    b[0] = mode;
    return cli_emit(c, SCENE_OP_SET_INPUT_MODE, b, sizeof(b), 0, 0);
}

int scene_client_seek(scene_client *c, uint64_t target_seq)
{
    uint8_t b[8];
    scene_put_u64(b, target_seq);
    return cli_emit(c, SCENE_OP_SEEK, b, sizeof(b), 0, 0);
}

int scene_client_import_texture(scene_client *c, scene_texture_ref ref,
                                const char *path)
{
    if (!path) return -1;
    uint32_t len = (uint32_t)strlen(path);
    if (len > c->lim.max_record_length - 16u) return -1;
    uint8_t *b = (uint8_t *)malloc(12u + len);
    if (!b) return -1;
    scene_put_u32(b + 0, ref);
    scene_put_u32(b + 4, len);
    if (len) memcpy(b + 8, path, len);
    int r = cli_emit(c, SCENE_OP_IMPORT_TEXTURE, b, 8u + len, 0, 0);
    free(b);
    return r;
}

/* ---- inbound -------------------------------------------------------------- */

/* Protocol violation from the server side: report and close the session. */
static void cli_violation(scene_client *c, const char *what)
{
    c->fatal = 1;
    if (c->cbs && c->cbs->error)
        c->cbs->error(c->ud, SCENE_ERR_PROTOCOL, what, (uint32_t)strlen(what));
}

static void dispatch(scene_client *c, uint16_t opcode,
                     const uint8_t *p, uint32_t plen)
{
    const scene_client_cbs *cb = c->cbs;

    switch (opcode) {

    case SCENE_SRV_WELCOME: {
        if (plen != 30) { cli_violation(c, "welcome len"); return; }
        uint32_t sid = scene_get_u32(p + 0);
        uint16_t ver = scene_get_u16(p + 4);
        if (ver != SCENE_PROTOCOL_V0) { cli_violation(c, "version"); return; }
        scene_limits lim;
        lim.max_nodes_per_session   = scene_get_u32(p + 6);
        lim.max_text_bytes_per_slot = scene_get_u32(p + 10);
        lim.max_text_slots_per_node = scene_get_u32(p + 14);
        lim.max_record_length       = scene_get_u32(p + 18);
        lim.input_latency_budget_us = scene_get_u64(p + 22);
        if (!c->welcomed) {
            if (c->had_session && sid == c->prev_scene_id)
                c->next_seq = c->sent_seq + 1;   /* retained session: continue */
            else
                c->next_seq = 1;                 /* fresh session: rebuild     */
            c->prev_scene_id = sid;
            c->had_session = 1;
            c->welcomed = 1;
            c->fatal = 0;
            c->scene_id = sid;
            c->lim = lim;
            uint32_t i;
            for (i = 0; i < c->log_count; i++) {
                if (cli_emit(c, c->log[i].opcode, c->log[i].body,
                             c->log[i].blen, 0, 0) != 0)
                    break;
            }
            if (c->log_count) scene_client_flush(c);
        }
        if (cb && cb->welcome) cb->welcome(c->ud, sid, ver, &lim);
        return;
    }

    case SCENE_SRV_ERROR: {
        if (plen < 6) { cli_violation(c, "error len"); return; }
        uint16_t code = scene_get_u16(p + 0);
        uint32_t mlen = scene_get_u32(p + 2);
        if (mlen != plen - 6) { cli_violation(c, "error msg"); return; }
        c->fatal = 1;
        if (cb && cb->error) cb->error(c->ud, code, (const char *)p + 6, mlen);
        return;
    }

    case SCENE_SRV_SNAPSHOT: {
        if (plen < 4) { cli_violation(c, "snapshot len"); return; }
        uint32_t req = scene_get_u32(p + 0);
        if (cb && cb->snapshot) cb->snapshot(c->ud, req, p + 4, plen - 4);
        return;
    }

    case SCENE_SRV_CAPTURE: {
        if (plen < 12) { cli_violation(c, "capture len"); return; }
        uint32_t req = scene_get_u32(p + 0);
        uint64_t seq = scene_get_u64(p + 4);
        if (cb && cb->capture) cb->capture(c->ud, req, seq, p + 12, plen - 12);
        return;
    }

    case SCENE_SRV_SEARCH_RESULT: {
        if (plen < 8) { cli_violation(c, "search len"); return; }
        uint32_t req = scene_get_u32(p + 0);
        uint32_t count = scene_get_u32(p + 4);
        if ((uint64_t)plen - 8 < (uint64_t)count * 26u) {
            cli_violation(c, "search count");
            return;
        }
        scene_search_hit *hits =
            (scene_search_hit *)malloc(count ? sizeof(*hits) * count : 1);
        if (!hits) { c->fatal = 1; return; }
        uint32_t i;
        for (i = 0; i < count; i++) {
            const uint8_t *h = p + 8 + i * 26u;
            hits[i].id = scene_get_u32(h + 0);
            hits[i].rect.x = scene_get_i32(h + 4);
            hits[i].rect.y = scene_get_i32(h + 8);
            hits[i].rect.w = scene_get_i32(h + 12);
            hits[i].rect.h = scene_get_i32(h + 16);
            hits[i].role = scene_get_u16(h + 20);
            hits[i].text_id = scene_get_u32(h + 22);
        }
        if (cb && cb->search_result) cb->search_result(c->ud, req, count, hits);
        free(hits);
        return;
    }

    case SCENE_SRV_PONG: {
        if (plen != 8) { cli_violation(c, "pong len"); return; }
        if (cb && cb->pong) cb->pong(c->ud, scene_get_u64(p + 0));
        return;
    }

    case SCENE_SRV_INPUT_POINTER: {
        if (plen != 18) { cli_violation(c, "pointer len"); return; }
        uint64_t seq = scene_get_u64(p + 0);
        uint8_t dev = p[8];
        int32_t x = scene_get_i32(p + 9);
        int32_t y = scene_get_i32(p + 13);
        uint8_t btns = p[17];
        if (cb && cb->input_pointer)
            cb->input_pointer(c->ud, seq, dev, x, y, btns);
        return;
    }

    case SCENE_SRV_INPUT_ACTIVATE: {
        if (plen != 12) { cli_violation(c, "activate len"); return; }
        if (cb && cb->input_activate)
            cb->input_activate(c->ud, scene_get_u64(p + 0),
                               scene_get_u32(p + 8));
        return;
    }

    case SCENE_SRV_INPUT_FOCUS: {
        if (plen != 13) { cli_violation(c, "focus len"); return; }
        if (cb && cb->input_focus)
            cb->input_focus(c->ud, scene_get_u64(p + 0),
                            scene_get_u32(p + 8), p[12]);
        return;
    }

    case SCENE_SRV_INPUT_KEY: {
        if (plen != 14) { cli_violation(c, "key len"); return; }
        uint64_t seq = scene_get_u64(p + 0);
        uint32_t key = scene_get_u32(p + 8);
        uint8_t st = p[12];
        uint8_t mod = p[13];
        if (cb && cb->input_key)
            cb->input_key(c->ud, seq, key, st, mod);
        return;
    }

    case SCENE_SRV_INPUT_TEXT: {
        if (plen < 12) { cli_violation(c, "text len"); return; }
        uint64_t seq = scene_get_u64(p + 0);
        uint32_t len = scene_get_u32(p + 8);
        if ((uint64_t)plen - 12 < len) { cli_violation(c, "text bytes"); return; }
        if (cb && cb->input_text)
            cb->input_text(c->ud, seq, (const char *)p + 12, len);
        return;
    }

    case SCENE_SRV_PRESENT_DONE: {
        if (plen != 24) { cli_violation(c, "present len"); return; }
        uint64_t seq = scene_get_u64(p + 0);
        uint64_t token = scene_get_u64(p + 8);
        uint64_t lat = scene_get_u64(p + 16);
        c->last_token = token;
        c->last_present_seq = seq;
        if (cb && cb->present_done) cb->present_done(c->ud, seq, token, lat);
        return;
    }

    case SCENE_SRV_IMPORT_RESULT: {
        if (plen != 5) { cli_violation(c, "import len"); return; }
        if (cb && cb->import_result)
            cb->import_result(c->ud, scene_get_u32(p + 0), p[4]);
        return;
    }

    case SCENE_SRV_TEXT_INDEX: {
        if (plen < 4) { cli_violation(c, "textindex len"); return; }
        uint32_t count = scene_get_u32(p + 0);
        uint32_t off = 4;
        uint32_t i;
        for (i = 0; i < count; i++) {
            if ((uint64_t)plen - off < 12u) { cli_violation(c, "textindex"); return; }
            off += 12u + scene_get_u32(p + off + 8);
        }
        if (off != plen) { cli_violation(c, "textindex end"); return; }
        scene_text_hit *hits =
            (scene_text_hit *)malloc(count ? sizeof(*hits) * count : 1);
        if (!hits) { c->fatal = 1; return; }
        off = 4;
        for (i = 0; i < count; i++) {
            hits[i].text_id = scene_get_u32(p + off + 0);
            hits[i].node_id = scene_get_u32(p + off + 4);
            hits[i].len = scene_get_u32(p + off + 8);
            hits[i].data = (const char *)p + off + 12;
            off += 12u + hits[i].len;
        }
        if (cb && cb->text_index) cb->text_index(c->ud, hits, count);
        free(hits);
        return;
    }

    default:
        cli_violation(c, "unknown record");
        return;
    }
}

int scene_client_pump(scene_client *c)
{
    if (!c->conn_open) return -1;
    uint8_t tmp[4096];
    for (;;) {
        uint32_t got = 0;
        int r = scene_transport_recv(c->t, tmp, sizeof(tmp), &got);
        if (r == 1) break;
        if (r == -1) {
            c->conn_open = 0;
            if (c->cbs && c->cbs->closed) c->cbs->closed(c->ud);
            return -1;
        }
        if (got == 0) break;
        if (cb_need(&c->in, c->in.len + got) != 0) return -1;
        memcpy(c->in.data + c->in.len, tmp, got);
        c->in.len += got;
        /* One batch per pump: on a blocking transport (TCP) an extra recv
         * would stall the caller until more data or close. Callers that
         * need to poll drain repeatedly; the frame loop below fully
         * consumes whatever this batch delivered.                          */
        break;
    }
    for (;;) {
        uint32_t avail = c->in.len - c->in_off;
        if (avail < SCENE_HEADER_SIZE) break;
        const uint8_t *f = c->in.data + c->in_off;
        uint32_t plen = scene_get_u32(f + 8);
        uint32_t maxr = c->welcomed ? c->lim.max_record_length
                                    : SCENE_DEFAULT_RECORD_LENGTH;
        if (plen > maxr) {
            cli_violation(c, "oversized record");
            return 0;
        }
        uint32_t total = SCENE_HEADER_SIZE + plen;
        if (avail < total) break;
        scene_frame_header h;
        h.magic = scene_get_u32(f + 0);
        h.version = scene_get_u16(f + 4);
        h.opcode = scene_get_u16(f + 6);
        h.length = plen;
        h.checksum = scene_get_u32(f + 12);
        if (scene_frame_check(&h, f, total) != 0) {
            cli_violation(c, "bad frame");
            return 0;
        }
        dispatch(c, h.opcode, f + SCENE_HEADER_SIZE, plen);
        c->in_off += total;
        if (c->in_off == c->in.len) { c->in.len = 0; c->in_off = 0; }
    }
    if (c->in_off && c->in_off > 4096) {
        memmove(c->in.data, c->in.data + c->in_off, c->in.len - c->in_off);
        c->in.len -= c->in_off;
        c->in_off = 0;
    }
    return 0;
}

/* ---- session state -------------------------------------------------------- */

uint32_t scene_client_scene_id(const scene_client *c) { return c->scene_id; }
uint64_t scene_client_next_seq(const scene_client *c) { return c->next_seq; }
uint64_t scene_client_last_present_seq(const scene_client *c)
{ return c->last_present_seq; }
int scene_client_welcomed(const scene_client *c)
{ return c && c->conn_open && c->welcomed && !c->fatal; }

/* ---- §6 snapshot/capture decode --------------------------------------------- */

int scene_snapshot_parse(const uint8_t *p, uint32_t plen,
                         uint64_t *seq_out, uint32_t *tex_count_out,
                         uint32_t *texts_used_out,
                         scene_snapshot_node *nodes, uint32_t cap,
                         scene_snapshot_text *texts_buf, uint32_t texts_cap,
                         scene_texture_ref *texes, uint32_t tex_cap)
{
    uint32_t off = 0;
    if (plen < 16) return -1;
    uint64_t seq = scene_get_u64(p + off); off += 8;
    uint32_t count = scene_get_u32(p + off); off += 4;
    uint32_t texc = scene_get_u32(p + off); off += 4;
    uint32_t filled = 0;
    uint32_t i;
    for (i = 0; i < count; i++) {
        if (plen - off < 43u) return -1;   /* fixed part of one node */
        scene_snapshot_node *n = (i < cap) ? &nodes[i] : NULL;
        if (n) {
            n->id = scene_get_u32(p + off + 0);
            n->parent = scene_get_u32(p + off + 4);
            n->role = scene_get_u16(p + off + 8);
            n->flags = p[off + 10];
            n->rect.x = scene_get_i32(p + off + 11);
            n->rect.y = scene_get_i32(p + off + 15);
            n->rect.w = scene_get_i32(p + off + 19);
            n->rect.h = scene_get_i32(p + off + 23);
            n->style = scene_get_u32(p + off + 27);
            n->effect = scene_get_u32(p + off + 31);
            n->tex = scene_get_u32(p + off + 35);
        }
        uint32_t tcount = scene_get_u32(p + off + 39);
        uint32_t fill_start = filled;
        off += 43u;
        uint32_t j;
        for (j = 0; j < tcount; j++) {
            if (plen - off < 8u) return -1;
            uint32_t tl = scene_get_u32(p + off + 4);
            if (plen - off < 8u + tl) return -1;
            if (filled < texts_cap) {
                texts_buf[filled].id = scene_get_u32(p + off + 0);
                texts_buf[filled].len = tl;
                texts_buf[filled].data = (const char *)p + off + 8;
            }
            filled++;
            off += 8u + tl;
        }
        if (n) {
            n->text_count = tcount;
            n->texts = tcount ? texts_buf + fill_start : NULL;
        }
    }
    if (texc) {
        if (plen - off < texc * 15u) return -1;
        uint32_t j;
        for (j = 0; j < texc; j++)
            if (j < tex_cap) texes[j] = scene_get_u32(p + off + j * 15u);
        off += texc * 15u;
    }
    if (off != plen) return -1;
    if (seq_out) *seq_out = seq;
    if (tex_count_out) *tex_count_out = texc;
    if (texts_used_out) *texts_used_out = filled;
    return (int)count;
}
