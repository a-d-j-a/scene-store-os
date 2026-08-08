/* bench.c — throughput benchmark for the semantic scene engine core.
 *
 * Builds a 10,000-node app scene through the real wire path (framed
 * records + checksums, seq monotonicity) and measures the store's
 * money paths: ingest, outbound drain, region resolution, search,
 * snapshot serialization, replay rebuild.
 */
#include "scene_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static double t_us(void)
{
    static LARGE_INTEGER f;
    static int once;
    LARGE_INTEGER c;
    if (!once) { QueryPerformanceFrequency(&f); once = 1; }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000000.0 / (double)f.QuadPart;
}
#else
#include <time.h>
static double t_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000000.0 + (double)ts.tv_nsec / 1000.0;
}
#endif

#define NODE_BUFFER 65536
static uint8_t pl[NODE_BUFFER];

static void put_u64_at(size_t off, uint64_t v)
{
    scene_put_u64(pl + off, v);
}
static void put_u32_at(size_t off, uint32_t v)
{
    scene_put_u32(pl + off, v);
}
static void put_i32_at(size_t off, int32_t v)
{
    scene_put_i32(pl + off, v);
}
static void put_u16_at(size_t off, uint16_t v)
{
    scene_put_u16(pl + off, v);
}

static uint32_t seq;
static uint8_t *mk_frame(uint16_t opcode, uint32_t plen, uint32_t *out_len)
{
    uint32_t total = SCENE_HEADER_SIZE + plen;
    uint8_t *f = (uint8_t *)malloc(total);
    if (!f) exit(2);
    scene_put_u32(f + 0, SCENE_MAGIC);
    scene_put_u16(f + 4, SCENE_PROTOCOL_V0);
    scene_put_u16(f + 6, opcode);
    scene_put_u32(f + 8, plen);
    memset(f + 12, 0, 4);
    if (plen) memcpy(f + SCENE_HEADER_SIZE, pl, plen);
    scene_put_u32(f + 12, scene_fnv1a32(f, SCENE_HEADER_SIZE + plen));
    *out_len = total;
    return f;
}

static int ingest(scene_store *s, uint16_t opcode, const uint8_t *payload,
                  uint32_t plen)
{
    uint32_t flen;
    uint8_t *f = mk_frame(opcode, plen, &flen);
    int r = scene_store_ingest(s, opcode, payload, plen);
    free(f);
    return r;
}

static int create(scene_store *s, uint32_t parent, uint32_t id, uint16_t role,
                  int32_t x, int32_t y, int32_t w, int32_t h, uint8_t flags)
{
    seq++;
    put_u64_at(0, seq);
    put_u32_at(8, parent);
    put_u32_at(12, id);
    put_u16_at(16, role);
    put_i32_at(18, x);
    put_i32_at(22, y);
    put_i32_at(26, w);
    put_i32_at(30, h);
    pl[34] = flags;
    return ingest(s, SCENE_OP_CREATE_NODE, pl, 35);
}

static int set_text(scene_store *s, uint32_t id, uint32_t tid, const char *txt)
{
    uint32_t n = (uint32_t)strlen(txt);
    seq++;
    put_u64_at(0, seq);
    put_u32_at(8, id);
    put_u32_at(12, tid);
    put_u32_at(16, n);
    memcpy(pl + 20, txt, n);
    return ingest(s, SCENE_OP_SET_TEXT, pl, 20 + n);
}

static void drain(scene_store *s, uint64_t *records)
{
    uint16_t op;
    const uint8_t *p;
    uint32_t n;
    *records = 0;
    while (scene_store_out_next(s, &op, &p, &n)) (*records)++;
}

int main(void)
{
    const uint32_t N_PANELS = 100;
    const uint32_t N_BUTTONS = 99;          /* 1 + 100 + 9900 = 10001 nodes */
    const uint32_t N = 1 + N_PANELS * (1 + N_BUTTONS);

    scene_store *s = scene_store_new(NULL);
    if (!s) { fprintf(stderr, "store alloc failed\n"); return 2; }

    double t0 = t_us();
    create(s, SCENE_NO_PARENT, 1, SCENE_ROLE_WINDOW, 0, 0, 1920, 1080,
           SCENE_FLAG_VISIBLE);
    uint32_t nid = 1;
    uint32_t i, j;
    for (i = 0; i < N_PANELS; i++) {
        nid++;
        create(s, 1, nid, SCENE_ROLE_PANEL, (int32_t)((i % 10) * 192),
               (int32_t)((i / 10) * 108), 192, 108, SCENE_FLAG_VISIBLE);
        for (j = 0; j < N_BUTTONS; j++) {
            nid++;
            create(s, nid - 1, nid, SCENE_ROLE_BUTTON,
                   (int32_t)((j % 9) * 20 + 4), (int32_t)((j / 9) * 20 + 4),
                   16, 16, SCENE_FLAG_VISIBLE | SCENE_FLAG_FOCUSABLE);
        }
    }
    double t_build = t_us() - t0;
    uint64_t build_records = seq;
    printf("nodes             : %u\n", N);
    printf("build records     : %llu\n", (unsigned long long)build_records);
    printf("build time        : %.2f ms  (%.0f rec/s)\n",
           t_build / 1000.0, build_records * 1000000.0 / t_build);

    t0 = t_us();
    uint32_t di = 0;
    for (i = 0; i < N_PANELS; i++) {
        uint32_t base = 3 + i * 100;   /* first button id of panel i */
        for (j = 0; j < N_BUTTONS; j++) {
            char buf[16];
            snprintf(buf, sizeof(buf), "Item %u", di++);
            set_text(s, base + j, 1, buf);
        }
    }
    double t_text = t_us() - t0;
    printf("text records      : %llu\n",
           (unsigned long long)(seq - build_records));
    printf("text time         : %.2f ms\n", t_text / 1000.0);

    t0 = t_us();
    uint64_t drained;
    drain(s, &drained);
    double t_drain = t_us() - t0;
    printf("outbound drain    : %llu records in %.2f ms\n",
           (unsigned long long)drained, t_drain / 1000.0);

    t0 = t_us();
    for (i = 0; i < 100000; i++)
        scene_store_region_at(s, 100, 100);
    double t_region = t_us() - t0;
    printf("region_at         : %.2f ns/op (100k)\n", t_region / 100.0);

    scene_node_id nodes[64];
    scene_text_id texts[64];
    size_t tcap = 64;
    t0 = t_us();
    for (i = 0; i < 100000; i++) {
        size_t tc = tcap;
        scene_store_search(s, "Item 999", 8, nodes, 64, texts, &tc);
    }
    double t_search = t_us() - t0;
    printf("search            : %.2f ns/op (100k)\n", t_search / 100.0);

    /* snapshot: request + drain the reply; timing covers serialization */
    seq++;
    put_u64_at(0, seq);
    put_u32_at(8, 1);
    t0 = t_us();
    ingest(s, SCENE_OP_SNAPSHOT, pl, 12);
    uint64_t sz = 0;
    uint16_t op;
    const uint8_t *p;
    uint32_t n;
    while (scene_store_out_next(s, &op, &p, &n)) sz += SCENE_HEADER_SIZE + n;
    double t_snap = t_us() - t0;
    printf("snapshot          : %llu bytes in %.2f ms (%.0f MB/s)\n",
           (unsigned long long)sz, t_snap / 1000.0, (double)sz / t_snap);

    /* replay: rebuild the full scene from the op log */
    seq++;
    put_u64_at(0, seq);
    pl[8] = SCENE_MODE_REPLAY;
    ingest(s, SCENE_OP_SET_INPUT_MODE, pl, 9);
    seq++;
    put_u64_at(0, seq);
    put_u64_at(8, seq - 1);
    t0 = t_us();
    ingest(s, SCENE_OP_SEEK, pl, 16);
    double t_replay = t_us() - t0;
    printf("replay rebuild    : %llu records in %.2f ms (%.0f rec/s)\n",
           (unsigned long long)(seq - 3), t_replay / 1000.0,
           (double)(seq - 3) * 1000000.0 / t_replay);

    scene_store_free(s);
    printf("done\n");
    return 0;
}
