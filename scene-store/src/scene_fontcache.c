/* scene_fontcache.c — TrueType glyph cache (stb_truetype backend).
 *
 * Open-addressing hash table keyed by codepoint, FNV-1a hash, LRU
 * eviction at 1024 entries. Each entry owns an alpha bitmap allocated
 * by stb_truetype and freed on eviction.                            */
#include "scene_fontcache.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FC_CAP       1024u
#define FC_CAP_LOG   10u       /* log2(1024)                          */
#define FC_HASH_INIT UINT32_C(2166136261)  /* FNV-1a offset basis     */

typedef struct fc_entry {
    uint32_t     codepoint;
    uint32_t     lru;        /* monotonic timestamp; higher = newer   */
    uint8_t     *pixels;     /* stbtt-allocated alpha bitmap          */
    int          w, h;
    int          x0, y0;     /* bitmap box offsets                    */
    int          advance;
    uint8_t      used;
} fc_entry;

struct scene_fontcache {
    uint8_t     *font_data;  /* owned copy (stb_truetype needs it)    */
    uint32_t     font_size;
    stbtt_fontinfo info;
    float        scale;
    int          ascent;
    int          descent;
    int          line_gap;
    fc_entry     entries[FC_CAP];
    uint32_t     lru_clock;
    uint32_t     count;
};

static uint32_t fc_hash(uint32_t cp)
{
    uint32_t h = FC_HASH_INIT;
    h ^= cp;  h *= 16777619u;
    return h;
}

static fc_entry *fc_find(const scene_fontcache *fc, uint32_t cp)
{
    uint32_t slot = fc_hash(cp) >> (32u - FC_CAP_LOG);
    uint32_t i;

    for (i = 0; i < (1u << FC_CAP_LOG); i++) {
        fc_entry *e = &fc->entries[(slot + i) & ((1u << FC_CAP_LOG) - 1u)];
        if (!e->used) return NULL;
        if (e->codepoint == cp) return (fc_entry *)e;
    }
    return NULL;
}

static void fc_evict_lru(scene_fontcache *fc)
{
    uint32_t i, oldest_idx = 0;
    uint32_t oldest_lru = UINT32_MAX;

    for (i = 0; i < (1u << FC_CAP_LOG); i++) {
        fc_entry *e = &fc->entries[i];
        if (e->used && e->lru < oldest_lru) {
            oldest_lru = e->lru;
            oldest_idx = i;
        }
    }
    {
        fc_entry *e = &fc->entries[oldest_idx];
        stbtt_FreeBitmap(e->pixels, NULL);
        memset(e, 0, sizeof(*e));
        fc->count--;
    }
}

static fc_entry *fc_insert(scene_fontcache *fc, uint32_t cp)
{
    uint32_t slot = fc_hash(cp) >> (32u - FC_CAP_LOG);
    uint32_t i;

    for (i = 0; i < (1u << FC_CAP_LOG); i++) {
        fc_entry *e = &fc->entries[(slot + i) & ((1u << FC_CAP_LOG) - 1u)];
        if (!e->used) {
            memset(e, 0, sizeof(*e));
            e->codepoint = cp;
            e->used = 1;
            fc->count++;
            return e;
        }
    }
    /* Table completely full — shouldn't happen at 70% load, but be safe. */
    fc_evict_lru(fc);
    return fc_insert(fc, cp);
}

/* ---- Lifecycle ---------------------------------------------------- */

scene_fontcache *scene_fontcache_new(const char *font_path,
                                     float pixel_height)
{
    FILE *f;
    long sz;
    uint8_t *buf;
    scene_fontcache *fc;

    if (!font_path || pixel_height <= 0) return NULL;
    f = fopen(font_path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    if (sz <= 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);

    fc = scene_fontcache_new_from_mem(buf, (uint32_t)sz, pixel_height);
    if (!fc) { free(buf); return NULL; }
    /* Transfer ownership of buf to the cache (it keeps it for stbtt). */
    free(fc->font_data);
    fc->font_data = buf;
    fc->font_size = (uint32_t)sz;
    return fc;
}

scene_fontcache *scene_fontcache_new_from_mem(const uint8_t *data,
                                              uint32_t size,
                                              float pixel_height)
{
    scene_fontcache *fc;
    int ascent, descent, line_gap;

    if (!data || size == 0 || pixel_height <= 0) return NULL;

    fc = (scene_fontcache *)calloc(1, sizeof(*fc));
    if (!fc) return NULL;

    /* stb_truetype needs the data to persist — make our own copy. */
    fc->font_data = (uint8_t *)malloc(size);
    if (!fc->font_data) { free(fc); return NULL; }
    memcpy(fc->font_data, data, size);
    fc->font_size = size;

    if (!stbtt_InitFont(&fc->info, fc->font_data,
                         stbtt_GetFontOffsetForIndex(fc->font_data, 0))) {
        free(fc->font_data);
        free(fc);
        return NULL;
    }

    fc->scale = stbtt_ScaleForPixelHeight(&fc->info, pixel_height);
    stbtt_GetFontVMetrics(&fc->info, &ascent, &descent, &line_gap);
    fc->ascent    = (int)(ascent  * fc->scale + 0.5f);
    fc->descent   = (int)(descent * fc->scale + 0.5f);
    fc->line_gap  = (int)(line_gap * fc->scale + 0.5f);
    fc->lru_clock = 1u;
    return fc;
}

void scene_fontcache_free(scene_fontcache *fc)
{
    uint32_t i;

    if (!fc) return;
    for (i = 0; i < (1u << FC_CAP_LOG); i++) {
        if (fc->entries[i].used)
            stbtt_FreeBitmap(fc->entries[i].pixels, NULL);
    }
    free(fc->font_data);
    free(fc);
}

/* ---- Glyph lookup ----------------------------------------------- */

const scene_cached_glyph *scene_fontcache_glyph(scene_fontcache *fc,
                                                uint32_t codepoint)
{
    fc_entry *e;
    static scene_cached_glyph result;

    if (!fc) return NULL;

    e = fc_find(fc, codepoint);
    if (e) {
        e->lru = ++fc->lru_clock;
        result.x0      = e->x0;
        result.y0      = e->y0;
        result.x1      = e->x0 + e->w;
        result.y1      = e->y0 + e->h;
        result.w       = e->w;
        result.h       = e->h;
        result.advance = e->advance;
        result.pixels  = e->pixels;
        return &result;
    }

    /* Evict if at capacity. */
    if (fc->count >= FC_CAP)
        fc_evict_lru(fc);

    e = fc_insert(fc, codepoint);
    {
        int w, h, x0, y0;
        int advance, lsb;
        unsigned char *bmp;

        bmp = stbtt_GetCodepointBitmap(&fc->info, fc->scale, fc->scale,
                                       (int)codepoint, &w, &h, &x0, &y0);
        stbtt_GetCodepointHMetrics(&fc->info, (int)codepoint, &advance,
                                   &lsb);

        e->pixels  = bmp;  /* may be NULL for space / invisible glyphs */
        e->w       = w;
        e->h       = h;
        e->x0      = x0;
        e->y0      = y0;
        e->advance = (int)(advance * fc->scale + 0.5f);
        e->lru     = ++fc->lru_clock;
    }

    result.x0      = e->x0;
    result.y0      = e->y0;
    result.x1      = e->x0 + e->w;
    result.y1      = e->y0 + e->h;
    result.w       = e->w;
    result.h       = e->h;
    result.advance = e->advance;
    result.pixels  = e->pixels;
    return &result;
}

/* ---- Metrics ----------------------------------------------------- */

int scene_fontcache_ascent(const scene_fontcache *fc)
{
    return fc ? fc->ascent : 0;
}

int scene_fontcache_descent(const scene_fontcache *fc)
{
    return fc ? fc->descent : 0;
}

int scene_fontcache_line_gap(const scene_fontcache *fc)
{
    return fc ? fc->line_gap : 0;
}

int scene_fontcache_line_height(const scene_fontcache *fc)
{
    if (!fc) return 0;
    return fc->ascent - fc->descent + fc->line_gap;
}

int scene_fontcache_advance(scene_fontcache *fc, uint32_t codepoint)
{
    int advance, lsb;

    if (!fc) return 0;
    stbtt_GetCodepointHMetrics(&fc->info, (int)codepoint, &advance, &lsb);
    return (int)(advance * fc->scale + 0.5f);
}

int scene_fontcache_valid(const scene_fontcache *fc)
{
    return fc != NULL;
}
