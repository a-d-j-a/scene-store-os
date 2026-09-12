/*
 * scene_fontcache.h — TrueType glyph cache (stb_truetype backend).
 *
 * Loads a TTF font, rasterizes glyphs on demand, and caches the alpha
 * bitmaps in a hash table with LRU eviction. Designed as an additive
 * module: the existing 8x8 bitmap font stays as the fast path for ASCII;
 * this cache handles non-ASCII and scalable text.
 *
 * The cache is NOT thread-safe — same single-thread contract as the rest
 * of the compositor.
 */
#ifndef SCENE_FONTCACHE_H
#define SCENE_FONTCACHE_H

#include <stdint.h>

/* A rasterized glyph from the cache. `pixels` is a w*h alpha bitmap
 * (8-bit per pixel, 0 = transparent, 255 = opaque). Owned by the cache;
 * valid until the next call that may evict (scene_fontcache_glyph). */
typedef struct scene_cached_glyph {
    int32_t  x0, y0, x1, y1;  /* bitmap box relative to cursor origin  */
    int      w, h;             /* pixel dimensions of the alpha bitmap   */
    int      advance;          /* horizontal advance to next glyph (px)  */
    const uint8_t *pixels;     /* alpha bitmap (owned by cache)          */
} scene_cached_glyph;

typedef struct scene_fontcache scene_fontcache;

/* Load a TTF font from a file path at the given pixel height.
 * Returns NULL on failure (file not found, invalid font).             */
scene_fontcache *scene_fontcache_new(const char *font_path,
                                     float pixel_height);

/* Load a TTF font from an in-memory buffer. The caller must keep
 * `data` alive for the lifetime of the cache (stb_truetype references
 * it directly). Returns NULL on failure.                              */
scene_fontcache *scene_fontcache_new_from_mem(const uint8_t *data,
                                              uint32_t size,
                                              float pixel_height);

void scene_fontcache_free(scene_fontcache *fc);

/* Look up (or rasterize) a glyph by Unicode codepoint.
 * Returns a pointer to the cached entry, or NULL for a missing glyph.
 * The pointer is valid until the next call to this function (LRU
 * eviction may reclaim it). For the .notdef / missing glyph, the
 * caller should render the box glyph from the old 8x8 font.          */
const scene_cached_glyph *scene_fontcache_glyph(scene_fontcache *fc,
                                                uint32_t codepoint);

/* Font metrics (at the pixel height given at creation time).
 * All values are in integer pixels.                                  */
int scene_fontcache_ascent(const scene_fontcache *fc);
int scene_fontcache_descent(const scene_fontcache *fc);
int scene_fontcache_line_gap(const scene_fontcache *fc);
int scene_fontcache_line_height(const scene_fontcache *fc);

/* Horizontal advance of a codepoint (integer pixels, rounded).       */
int scene_fontcache_advance(scene_fontcache *fc, uint32_t codepoint);

/* 1 when the cache was loaded successfully and is usable.            */
int scene_fontcache_valid(const scene_fontcache *fc);

#endif /* SCENE_FONTCACHE_H */
