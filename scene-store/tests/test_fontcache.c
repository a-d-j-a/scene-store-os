/* test_fontcache.c — unit tests for the TrueType glyph cache. */
#include "scene_fontcache.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
    checks++; \
} while (0)

/* ---- Lifecycle tests --------------------------------------------- */

static int test_null_inputs(void)
{
    scene_fontcache *fc;

    fc = scene_fontcache_new("nonexistent.ttf", 12.0f);
    CHECK(fc == NULL);

    fc = scene_fontcache_new_from_mem(NULL, 0, 12.0f);
    CHECK(fc == NULL);

    fc = scene_fontcache_new_from_mem((const uint8_t *)"garbage", 7, 12.0f);
    CHECK(fc == NULL);

    scene_fontcache_free(NULL); /* must not crash */

    printf("  test_null_inputs: %d checks OK\n", checks);
    return 0;
}

/* This test requires fonts/DejaVuSansMono.ttf in the working dir. */
static int test_load_and_metrics(void)
{
    scene_fontcache *fc;

    fc = scene_fontcache_new("fonts/DejaVuSansMono.ttf", 13.0f);
    CHECK(fc != NULL);
    CHECK(scene_fontcache_valid(fc));

    /* Ascent should be positive, descent negative or zero. */
    CHECK(scene_fontcache_ascent(fc) > 0);
    CHECK(scene_fontcache_descent(fc) <= 0);
    CHECK(scene_fontcache_line_gap(fc) >= 0);
    CHECK(scene_fontcache_line_height(fc) > 0);

    /* Line height = ascent - descent + line_gap */
    CHECK(scene_fontcache_line_height(fc)
          == scene_fontcache_ascent(fc) - scene_fontcache_descent(fc)
             + scene_fontcache_line_gap(fc));

    scene_fontcache_free(fc);
    printf("  test_load_and_metrics: %d checks OK\n", checks);
    return 0;
}

static int test_glyph_ascii(void)
{
    scene_fontcache *fc;
    const scene_cached_glyph *g;

    fc = scene_fontcache_new("fonts/DejaVuSansMono.ttf", 13.0f);
    CHECK(fc != NULL);

    /* 'A' should produce a non-zero bitmap. */
    g = scene_fontcache_glyph(fc, 'A');
    CHECK(g != NULL);
    CHECK(g->w > 0);
    CHECK(g->h > 0);
    CHECK(g->advance > 0);
    CHECK(g->pixels != NULL);

    /* '0' should also work. */
    g = scene_fontcache_glyph(fc, '0');
    CHECK(g != NULL);
    CHECK(g->w > 0);
    CHECK(g->h > 0);

    /* Space should have advance but may have zero-size bitmap. */
    g = scene_fontcache_glyph(fc, ' ');
    CHECK(g != NULL);
    CHECK(g->advance > 0);

    scene_fontcache_free(fc);
    printf("  test_glyph_ascii: %d checks OK\n", checks);
    return 0;
}

static int test_glyph_extended(void)
{
    scene_fontcache *fc;
    const scene_cached_glyph *g;

    fc = scene_fontcache_new("fonts/DejaVuSansMono.ttf", 13.0f);
    CHECK(fc != NULL);

    /* Latin-1 extended: e-acute (U+00E9), n-tilde (U+00F1) */
    g = scene_fontcache_glyph(fc, 0x00E9);
    CHECK(g != NULL);
    CHECK(g->w > 0);
    CHECK(g->h > 0);
    CHECK(g->advance > 0);

    g = scene_fontcache_glyph(fc, 0x00F1);
    CHECK(g != NULL);
    CHECK(g->w > 0);

    /* Latin Extended: a-macron (U+0101), o-double-acute (U+0151) */
    g = scene_fontcache_glyph(fc, 0x0101);
    CHECK(g != NULL);
    CHECK(g->w > 0);

    /* CJK range: U+4E2D (中) — DejaVu may not have it, but the cache
     * should still return a valid entry (possibly the .notdef glyph). */
    g = scene_fontcache_glyph(fc, 0x4E2D);
    CHECK(g != NULL);  /* entry exists even if empty */

    scene_fontcache_free(fc);
    printf("  test_glyph_extended: %d checks OK\n", checks);
    return 0;
}

static int test_advance(void)
{
    scene_fontcache *fc;
    int adv_a, adv_b;

    fc = scene_fontcache_new("fonts/DejaVuSansMono.ttf", 13.0f);
    CHECK(fc != NULL);

    /* Monospace font: all visible characters should have the same advance. */
    adv_a = scene_fontcache_advance(fc, 'A');
    adv_b = scene_fontcache_advance(fc, 'B');
    CHECK(adv_a > 0);
    CHECK(adv_a == adv_b);

    /* Same advance from the glyph struct. */
    {
        const scene_cached_glyph *ga = scene_fontcache_glyph(fc, 'A');
        CHECK(ga->advance == adv_a);
    }

    scene_fontcache_free(fc);
    printf("  test_advance: %d checks OK\n", checks);
    return 0;
}

static int test_cache_eviction(void)
{
    scene_fontcache *fc;
    const scene_cached_glyph *g;
    uint32_t i;

    fc = scene_fontcache_new("fonts/DejaVuSansMono.ttf", 13.0f);
    CHECK(fc != NULL);

    /* The cache holds 1024 entries. Rasterize 1100 distinct codepoints
     * (U+0020..U+046F) to trigger evictions. Verify that the cache
     * still returns valid entries. */
    for (i = 0x0020; i <= 0x046F; i++) {
        g = scene_fontcache_glyph(fc, i);
        CHECK(g != NULL);
        /* After eviction, the glyph should still be retrievable. */
    }

    /* The first codepoint (space) should have been evicted and re-created.
     * Verify it still returns a valid entry. */
    g = scene_fontcache_glyph(fc, ' ');
    CHECK(g != NULL);
    CHECK(g->advance > 0);

    scene_fontcache_free(fc);
    printf("  test_cache_eviction: %d checks OK\n", checks);
    return 0;
}

static int test_same_glyph_stable(void)
{
    scene_fontcache *fc;
    const scene_cached_glyph *g1, *g2;

    fc = scene_fontcache_new("fonts/DejaVuSansMono.ttf", 13.0f);
    CHECK(fc != NULL);

    /* Two lookups of the same glyph should return the same entry
     * (cache hit, same pointer). */
    g1 = scene_fontcache_glyph(fc, 'Z');
    CHECK(g1 != NULL);
    g2 = scene_fontcache_glyph(fc, 'Z');
    CHECK(g2 != NULL);
    CHECK(g1 == g2);  /* same pointer = cache hit */

    scene_fontcache_free(fc);
    printf("  test_same_glyph_stable: %d checks OK\n", checks);
    return 0;
}

static int test_from_mem(void)
{
    FILE *f;
    long sz;
    uint8_t *buf;
    scene_fontcache *fc;
    const scene_cached_glyph *g;

    f = fopen("fonts/DejaVuSansMono.ttf", "rb");
    if (!f) {
        fprintf(stderr, "  test_from_mem: SKIP (font file not found)\n");
        return 0;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)sz);
    assert(buf);
    fread(buf, 1, (size_t)sz, f);
    fclose(f);

    fc = scene_fontcache_new_from_mem(buf, (uint32_t)sz, 16.0f);
    CHECK(fc != NULL);

    g = scene_fontcache_glyph(fc, 'H');
    CHECK(g != NULL);
    CHECK(g->w > 0);
    CHECK(g->h > 0);

    /* Free the caller's buffer — cache has its own copy. */
    free(buf);

    /* The glyph should still be valid (cache owns its own copy). */
    g = scene_fontcache_glyph(fc, 'H');
    CHECK(g != NULL);

    scene_fontcache_free(fc);
    printf("  test_from_mem: %d checks OK\n", checks);
    return 0;
}

int main(void)
{
    int fail = 0;

    printf("test_fontcache:\n");
    fail += test_null_inputs();
    fail += test_load_and_metrics();
    fail += test_glyph_ascii();
    fail += test_glyph_extended();
    fail += test_advance();
    fail += test_cache_eviction();
    fail += test_same_glyph_stable();
    fail += test_from_mem();

    if (fail) {
        printf("test_fontcache: FAIL (%d test failures)\n", fail);
        return 1;
    }
    printf("test_fontcache: ALL OK (%d checks)\n", checks);
    return 0;
}
