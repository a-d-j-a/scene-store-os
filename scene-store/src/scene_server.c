/*
 * scene_server.c — server adapter: the compositor seam.
 *
 * Implements the server-facing half of the locked v0 protocol on top of
 * the engine: raw-byte feed with frame reassembly, scene_frame_check
 * validation (magic/version/length/checksum), engine ingest, and the
 * outbound frame drain. Fatal violations are reported with an ERROR
 * record exactly as the spec §4 mandates (engine ingest emits its own
 * ERRORs for op-level failures; the adapter emits for frame-level ones
 * via scene_store_fail).
 */
#define _POSIX_C_SOURCE 200809L
#include "scene_server.h"

#include <stdlib.h>
#include <string.h>

typedef struct sv_buf {
    uint8_t *data;
    uint32_t len, cap, off;
} sv_buf;

static int sv_need(sv_buf *b, uint32_t need)
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

static void sv_buf_free(sv_buf *b)
{
    free(b->data);
    b->data = NULL; b->len = b->cap = b->off = 0;
}

struct scene_server {
    scene_store *s;
    sv_buf in;
    int attached;
    int ghosted;                /* detached: next frame rebases the stream */
    int dead;
    scene_import_fn import_cb;
    void *import_ud;
};

scene_server *scene_server_new(const scene_limits *limits)
{
    scene_server *sv = (scene_server *)calloc(1, sizeof(*sv));
    if (!sv) return NULL;
    sv->s = scene_store_new(limits);
    if (!sv->s) { free(sv); return NULL; }
    return sv;
}

void scene_server_free(scene_server *sv)
{
    if (!sv) return;
    scene_store_free(sv->s);
    sv_buf_free(&sv->in);
    free(sv);
}

int scene_server_attach(scene_server *sv)
{
    if (sv->attached) return 0;
    if (scene_store_welcome(sv->s) != 0) return -1;
    sv->attached = 1;
    return 0;
}

void scene_server_detach(scene_server *sv)
{
    scene_store_ghost_mark(sv->s);
    sv->attached = 0;
    sv->ghosted = 1;
}

int scene_server_feed(scene_server *sv, const uint8_t *bytes, uint32_t len)
{
    if (sv->dead) return -1;
    if (len == 0) return 0;
    if (sv_need(&sv->in, sv->in.len + len) != 0) return -1;
    memcpy(sv->in.data + sv->in.len, bytes, len);
    sv->in.len += len;

    for (;;) {
        uint32_t avail = sv->in.len - sv->in.off;
        if (avail < SCENE_HEADER_SIZE) break;
        const uint8_t *f = sv->in.data + sv->in.off;
        uint32_t plen = scene_get_u32(f + 8);
        const scene_limits *lim = scene_store_limits(sv->s);
        if (plen > lim->max_record_length) {
            scene_store_fail(sv->s, SCENE_ERR_LIMIT, "record exceeds limit");
            sv->dead = 1;
            return -1;
        }
        uint32_t total = SCENE_HEADER_SIZE + plen;
        if (avail < total) break;          /* partial frame: wait          */
        scene_frame_header h;
        h.magic = scene_get_u32(f + 0);
        h.version = scene_get_u16(f + 4);
        h.opcode = scene_get_u16(f + 6);
        h.length = plen;
        h.checksum = scene_get_u32(f + 12);
        if (scene_frame_check(&h, f, total) != 0) {
            scene_store_fail(sv->s, SCENE_ERR_CKSUM, "bad frame");
            sv->dead = 1;
            return -1;
        }
        if (sv->ghosted && h.opcode != SCENE_OP_ACK) {
            uint64_t seq = scene_get_u64(f + SCENE_HEADER_SIZE);
            if (scene_store_rejoin(sv->s, seq) != 0) {
                scene_store_fail(sv->s, SCENE_ERR_SEQ, "ghost rejoin seq");
                sv->dead = 1;
                return -1;
            }
            sv->ghosted = 0;
        }
        if (h.opcode == SCENE_OP_IMPORT_TEXTURE) {
            /* Host-side request: strict payload check here (the engine
             * no-ops it), then hand {ref,path} to the importer. Failure
             * is silent by design — the client observes it when its
             * SET_TEXTURE of the ref is rejected.                      */
            uint32_t pl = plen;
            if (pl < 16) {
                scene_store_fail(sv->s, SCENE_ERR_PROTOCOL, "imp len");
                sv->dead = 1;
                return -1;
            }
            uint32_t tlen = scene_get_u32(f + SCENE_HEADER_SIZE + 12);
            if (pl != 16 + tlen) {
                scene_store_fail(sv->s, SCENE_ERR_PROTOCOL, "imp path");
                sv->dead = 1;
                return -1;
            }
            if (sv->import_cb) {
                uint32_t ref = scene_get_u32(f + SCENE_HEADER_SIZE + 8);
                char *path = (char *)malloc((size_t)tlen + 1);
                if (!path) { sv->dead = 1; return -1; }
                memcpy(path, f + SCENE_HEADER_SIZE + 16, tlen);
                path[tlen] = '\0';
                int rc = sv->import_cb(sv->import_ud, sv, ref, path);
                free(path);
                if (rc != 0) {
                    /* Decode failed: the importer could not register
                     * the ref. The client observes IMPORT_RESULT ok=0
                     * and must not SET_TEXTURE the ref.             */
                    if (scene_server_import_result(sv, ref, 0) != 0) {
                        sv->dead = 1;
                        return -1;
                    }
                }
            }
        }
        if (scene_store_ingest(sv->s, h.opcode, f + SCENE_HEADER_SIZE, plen)
            != 0) {
            sv->dead = 1;                  /* engine emitted ERROR itself   */
            return -1;
        }
        sv->in.off += total;
        if (sv->in.off == sv->in.len) { sv->in.len = 0; sv->in.off = 0; }
    }
    if (sv->in.off && sv->in.off > 4096) {
        memmove(sv->in.data, sv->in.data + sv->in.off, sv->in.len - sv->in.off);
        sv->in.len -= sv->in.off;
        sv->in.off = 0;
    }
    return 0;
}

int scene_server_out_next_frame(scene_server *sv,
                                const uint8_t **frame, uint32_t *frame_len)
{
    return scene_store_out_next_frame(sv->s, frame, frame_len);
}

int scene_server_input_pointer(scene_server *sv, uint8_t device,
                               int32_t x, int32_t y, uint8_t buttons)
{
    return scene_store_input_pointer(sv->s, device, x, y, buttons);
}

int scene_server_input_key(scene_server *sv, uint32_t key_code,
                           uint8_t state, uint8_t modifiers)
{
    return scene_store_input_key(sv->s, key_code, state, modifiers);
}

int scene_server_input_text(scene_server *sv, const char *text,
                            uint32_t len)
{
    return scene_store_input_text(sv->s, text, len);
}

int scene_server_dead(const scene_server *sv)
{
    return sv->dead;
}

scene_store *scene_server_store(scene_server *sv)
{
    return sv->s;
}

void scene_server_set_import_cb(scene_server *sv, scene_import_fn fn,
                                void *ud)
{
    if (!sv) return;
    sv->import_cb = fn;
    sv->import_ud = ud;
}

int scene_server_import_result(scene_server *sv, scene_texture_ref ref,
                               uint8_t ok)
{
    uint8_t b[5];
    if (!sv) return -1;
    scene_put_u32(b + 0, ref);
    b[4] = ok;
    return scene_store_emit_record(sv->s, SCENE_SRV_IMPORT_RESULT, b,
                                   sizeof(b));
}
