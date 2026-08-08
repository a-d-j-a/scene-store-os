/*
 * test_wallpaper.c — tests for the wallpaper system.
 *
 * Tests cover: static, Ken Burns, slideshow, procedural effects,
 * auto-pause, pixel access, and determinism.
 */
#include "scene_wallpaper.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int checks, failures;

static inline uint32_t pack_argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { failures++; printf("FAIL %s:%d: %s\n", \
        __FILE__, __LINE__, #expr); } \
} while(0)
#define CHECK_EQ(a, b) do { \
    checks++; \
    if ((a) != (b)) { failures++; printf("FAIL %s:%d: %s (%lu) != %s (%lu)\n", \
        __FILE__, __LINE__, #a, (unsigned long)(a), #b, (unsigned long)(b)); } \
} while(0)

/* ---- Helpers ---------------------------------------------------------- */

static void write_bmp_32(const char *path, int w, int h, const uint32_t *px)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint8_t fh[14] = {0};
    fh[0] = 'B'; fh[1] = 'M';
    int row_size = w * 4;
    int img_size = row_size * h;
    *(uint32_t *)&fh[2] = (uint32_t)(14 + 40 + img_size);
    *(uint32_t *)&fh[10] = 54;
    fwrite(fh, 1, 14, f);
    uint8_t ih[40] = {0};
    *(uint32_t *)&ih[0] = 40;
    *(int32_t *)&ih[4] = w;
    *(int32_t *)&ih[8] = -h;
    *(uint16_t *)&ih[14] = 32;
    fwrite(ih, 1, 40, f);
    int i;
    for (i = 0; i < w * h; i++) {
        uint32_t c = px[i];
        uint8_t bgra[4] = {
            (uint8_t)(c & 0xFF), (uint8_t)((c >> 8) & 0xFF),
            (uint8_t)((c >> 16) & 0xFF), (uint8_t)((c >> 24) & 0xFF)
        };
        fwrite(bgra, 1, 4, f);
    }
    fclose(f);
}

/* ---- Tests ------------------------------------------------------------ */

static void test_lifecycle(void)
{
    printf("test_lifecycle:\n");
    scene_wallpaper *wp = scene_wallpaper_new(800, 600);
    CHECK(wp != NULL);
    CHECK(scene_wallpaper_type(wp) == SCENE_WP_STATIC);
    CHECK(scene_wallpaper_frame(wp) == 0);
    scene_wallpaper_free(wp);
}

static void test_render_size(void)
{
    printf("test_render_size:\n");
    /* 800x600 should stay at 800x600 (under 1920x1080) */
    scene_wallpaper *wp1 = scene_wallpaper_new(800, 600);
    uint32_t w, h;
    scene_wallpaper_render_size(wp1, &w, &h);
    CHECK_EQ(w, 800u);
    CHECK_EQ(h, 600u);
    scene_wallpaper_free(wp1);

    /* 3840x2160 (4K) should downscale to 1920x1080 */
    scene_wallpaper *wp2 = scene_wallpaper_new(3840, 2160);
    scene_wallpaper_render_size(wp2, &w, &h);
    CHECK_EQ(w, 1920u);
    CHECK_EQ(h, 1080u);
    scene_wallpaper_free(wp2);

    /* 2560x1440 should downscale proportionally */
    scene_wallpaper *wp3 = scene_wallpaper_new(2560, 1440);
    scene_wallpaper_render_size(wp3, &w, &h);
    CHECK(w <= 1920);
    CHECK(h <= 1080);
    CHECK(w > 0 && h > 0);
    scene_wallpaper_free(wp3);
}

static void test_static(void)
{
    printf("test_static:\n");
    uint32_t src[4] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF };
    write_bmp_32("_wp_test.bmp", 2, 2, src);

    scene_wallpaper *wp = scene_wallpaper_new(800, 600);
    CHECK(scene_wallpaper_set_static(wp, "_wp_test.bmp") == 0);
    CHECK(scene_wallpaper_type(wp) == SCENE_WP_STATIC);
    CHECK(scene_wallpaper_pixels(wp) != NULL);

    /* Static returns 1 on first tick (texture registration signal), 0 after */
    CHECK(scene_wallpaper_tick(wp) == 1);
    CHECK(scene_wallpaper_frame(wp) == 1);
    CHECK(scene_wallpaper_tick(wp) == 0);
    CHECK(scene_wallpaper_frame(wp) == 1);

    scene_wallpaper_free(wp);
    remove("_wp_test.bmp");
}

static void test_plasma(void)
{
    printf("test_plasma:\n");
    scene_wallpaper *wp = scene_wallpaper_new(800, 600);
    scene_wp_params p = { .speed = 1.0f, .intensity = 0.8f };
    CHECK(scene_wallpaper_set_procedural(wp, SCENE_WP_PLASMA, &p) == 0);
    CHECK(scene_wallpaper_type(wp) == SCENE_WP_PLASMA);

    /* First tick should produce pixels */
    CHECK(scene_wallpaper_tick(wp) == 1);
    CHECK(scene_wallpaper_frame(wp) == 1);
    const uint32_t *px = scene_wallpaper_pixels(wp);
    CHECK(px != NULL);
    CHECK(px[0] != 0); /* not all black */

    /* Second tick should differ (animation) */
    scene_wallpaper_tick(wp);
    /* Plasma changes every frame, but center pixel might be same by
     * coincidence; just check frame advanced */
    CHECK(scene_wallpaper_frame(wp) == 2);

    scene_wallpaper_free(wp);
}

static void test_aurora(void)
{
    printf("test_aurora:\n");
    scene_wallpaper *wp = scene_wallpaper_new(800, 600);
    CHECK(scene_wallpaper_set_procedural(wp, SCENE_WP_AURORA, NULL) == 0);
    CHECK(scene_wallpaper_tick(wp) == 1);
    CHECK(scene_wallpaper_frame(wp) == 1);
    const uint32_t *px = scene_wallpaper_pixels(wp);
    CHECK(px != NULL);
    CHECK(px[0] != 0);
    scene_wallpaper_free(wp);
}

static void test_gradient_mesh(void)
{
    printf("test_gradient_mesh:\n");
    scene_wallpaper *wp = scene_wallpaper_new(800, 600);
    CHECK(scene_wallpaper_set_procedural(wp, SCENE_WP_GRADIENT_MESH, NULL) == 0);
    CHECK(scene_wallpaper_tick(wp) == 1);
    CHECK(scene_wallpaper_frame(wp) == 1);
    const uint32_t *px = scene_wallpaper_pixels(wp);
    CHECK(px != NULL);
    CHECK(px[0] != 0);
    scene_wallpaper_free(wp);
}

static void test_occluded(void)
{
    printf("test_occluded:\n");
    scene_wallpaper *wp = scene_wallpaper_new(800, 600);
    scene_wallpaper_set_procedural(wp, SCENE_WP_PLASMA, NULL);
    scene_wallpaper_tick(wp);
    CHECK(scene_wallpaper_frame(wp) == 1);

    /* When occluded, tick does nothing */
    scene_wallpaper_set_occluded(wp, 1);
    CHECK(scene_wallpaper_tick(wp) == 0);
    CHECK(scene_wallpaper_frame(wp) == 1);

    /* Un-occlude: tick resumes */
    scene_wallpaper_set_occluded(wp, 0);
    CHECK(scene_wallpaper_tick(wp) == 1);
    CHECK(scene_wallpaper_frame(wp) == 2);

    scene_wallpaper_free(wp);
}

static void test_slideshow(void)
{
    printf("test_slideshow:\n");
    uint32_t red[4]   = { 0xFFFF0000, 0xFFFF0000, 0xFFFF0000, 0xFFFF0000 };
    uint32_t green[4] = { 0xFF00FF00, 0xFF00FF00, 0xFF00FF00, 0xFF00FF00 };
    write_bmp_32("_wp_s1.bmp", 2, 2, red);
    write_bmp_32("_wp_s2.bmp", 2, 2, green);

    scene_wallpaper *wp = scene_wallpaper_new(800, 600);
    CHECK(scene_wallpaper_add_slide(wp, "_wp_s1.bmp") == 0);
    CHECK(scene_wallpaper_add_slide(wp, "_wp_s2.bmp") == 0);
    scene_wallpaper_set_slideshow_timing(wp, 0.1f, 0.1f);
    CHECK(scene_wallpaper_start_slideshow(wp) == 0);
    CHECK(scene_wallpaper_type(wp) == SCENE_WP_SLIDESHOW);

    /* Tick advances timer, no crossfade yet */
    int i;
    for (i = 0; i < 3; i++) scene_wallpaper_tick(wp);
    CHECK(scene_wallpaper_frame(wp) >= 1);

    scene_wallpaper_free(wp);
    remove("_wp_s1.bmp");
    remove("_wp_s2.bmp");
}

static void test_slideshow_no_slides(void)
{
    printf("test_slideshow_no_slides:\n");
    scene_wallpaper *wp = scene_wallpaper_new(800, 600);
    CHECK(scene_wallpaper_start_slideshow(wp) == -1);
    scene_wallpaper_free(wp);
}

static void test_invalid_args(void)
{
    printf("test_invalid_args:\n");
    CHECK(scene_wallpaper_new(0, 0) == NULL);
    scene_wallpaper *wp = scene_wallpaper_new(800, 600);
    CHECK(scene_wallpaper_set_static(wp, "_nonexistent.bmp") == -1);
    CHECK(scene_wallpaper_set_procedural(wp, SCENE_WP_STATIC, NULL) == -1);
    CHECK(scene_wallpaper_set_procedural(wp, SCENE_WP_COUNT, NULL) == -1);
    CHECK(scene_wallpaper_add_slide(wp, "_nonexistent.bmp") == -1);
    scene_wallpaper_free(wp);
}

static void test_determinism(void)
{
    printf("test_determinism:\n");
    scene_wp_params p = { .speed = 2.0f, .intensity = 0.5f };

    scene_wallpaper *w1 = scene_wallpaper_new(800, 600);
    scene_wallpaper *w2 = scene_wallpaper_new(800, 600);
    scene_wallpaper_set_procedural(w1, SCENE_WP_PLASMA, &p);
    scene_wallpaper_set_procedural(w2, SCENE_WP_PLASMA, &p);

    /* Same frame count → identical pixels */
    int i;
    for (i = 0; i < 10; i++) {
        scene_wallpaper_tick(w1);
        scene_wallpaper_tick(w2);
    }
    CHECK(scene_wallpaper_frame(w1) == scene_wallpaper_frame(w2));
    const uint32_t *px1 = scene_wallpaper_pixels(w1);
    const uint32_t *px2 = scene_wallpaper_pixels(w2);
    int match = (memcmp(px1, px2, 800 * 600 * 4) == 0);
    CHECK(match);

    scene_wallpaper_free(w1);
    scene_wallpaper_free(w2);
}

static void test_ken_burns(void)
{
    printf("test_ken_burns:\n");
    /* Create a larger source image for Ken Burns to crop from */
    uint32_t *src = (uint32_t *)calloc(256 * 256, sizeof(uint32_t));
    int y, x;
    for (y = 0; y < 256; y++)
        for (x = 0; x < 256; x++)
            src[y * 256 + x] = pack_argb(0xFF,
                (uint8_t)(x), (uint8_t)(y), (uint8_t)(128));
    write_bmp_32("_wp_kb.bmp", 256, 256, src);
    free(src);

    scene_wallpaper *wp = scene_wallpaper_new(800, 600);
    CHECK(scene_wallpaper_set_ken_burns(wp, "_wp_kb.bmp", 5.0f, 1.1f) == 0);
    CHECK(scene_wallpaper_type(wp) == SCENE_WP_KEN_BURNS);

    /* Should produce different frames over time */
    scene_wallpaper_tick(wp);
    scene_wallpaper_tick(wp);
    scene_wallpaper_tick(wp);
    CHECK(scene_wallpaper_frame(wp) == 3);

    scene_wallpaper_free(wp);
    remove("_wp_kb.bmp");
}

static void test_pixels_null(void)
{
    printf("test_pixels_null:\n");
    CHECK(scene_wallpaper_pixels(NULL) == NULL);
    uint32_t w, h;
    scene_wallpaper_render_size(NULL, &w, &h);
    CHECK_EQ(w, 0u);
    CHECK_EQ(h, 0u);
    CHECK(scene_wallpaper_type(NULL) == SCENE_WP_STATIC);
    CHECK(scene_wallpaper_frame(NULL) == 0);
}

/* ---- main ------------------------------------------------------------ */

int main(void)
{
    test_lifecycle();
    test_render_size();
    test_static();
    test_plasma();
    test_aurora();
    test_gradient_mesh();
    test_ken_burns();
    test_slideshow();
    test_slideshow_no_slides();
    test_occluded();
    test_invalid_args();
    test_determinism();
    test_pixels_null();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
