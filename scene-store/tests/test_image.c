/*
 * test_image.c — tests for the image loader (BMP/TGA in-house, PNG/
 * JPEG/GIF via vendored stb_image at the OS seam).
 *
 * Creates minimal test images in-memory, writes them to temp files,
 * then loads and verifies pixel data. JPEG/GIF fixtures live in
 * tests/fixtures/ (generated with GDI+; JPEG asserts carry a small
 * lossy-codec tolerance).
 */
#include "scene_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* stb_image_write is used ONLY to synthesize PNG fixtures in this test
 * TU; the shipped loader ships stb_image (vendored, same origin).      */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb/stb_image_write.h"

static int checks, failures;

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

/* ---- BMP helpers ------------------------------------------------------ */

static void write_bmp_32(const char *path, int w, int h, const uint32_t *px)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;

    int row_size = w * 4;
    int img_size = row_size * h;
    int file_size = 14 + 40 + img_size;

    /* File header */
    uint8_t fh[14] = {0};
    fh[0] = 'B'; fh[1] = 'M';
    *(uint32_t *)&fh[2] = (uint32_t)file_size;
    *(uint32_t *)&fh[10] = 54; /* data offset */
    fwrite(fh, 1, 14, f);

    /* Info header */
    uint8_t ih[40] = {0};
    *(uint32_t *)&ih[0] = 40;
    *(int32_t *)&ih[4] = w;
    *(int32_t *)&ih[8] = -h; /* negative = top-down */
    *(uint16_t *)&ih[12] = 1; /* planes */
    *(uint16_t *)&ih[14] = 32;
    *(uint32_t *)&ih[20] = (uint32_t)img_size;
    fwrite(ih, 1, 40, f);

    /* Pixel data (top-down, BGRA in file) */
    int row, col;
    for (row = 0; row < h; row++) {
        for (col = 0; col < w; col++) {
            uint32_t c = px[row * w + col];
            uint8_t bgra[4] = {
                (uint8_t)(c & 0xFF),
                (uint8_t)((c >> 8) & 0xFF),
                (uint8_t)((c >> 16) & 0xFF),
                (uint8_t)((c >> 24) & 0xFF)
            };
            fwrite(bgra, 1, 4, f);
        }
    }

    fclose(f);
}

/* ---- TGA helpers (raw, 32-bit) ---------------------------------------- */

static void write_tga_32(const char *path, int w, int h, const uint32_t *px)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;

    /* Header */
    uint8_t th[18] = {0};
    th[2] = 2; /* raw RGB */
    *(uint16_t *)&th[12] = (uint16_t)w;
    *(uint16_t *)&th[14] = (uint16_t)h;
    th[16] = 32;
    th[17] = 0x28; /* top-left origin, 8 alpha bits */
    fwrite(th, 1, 18, f);

    /* Pixel data: BGRA in file */
    int i;
    for (i = 0; i < w * h; i++) {
        uint32_t c = px[i];
        uint8_t bgra[4] = {
            (uint8_t)(c & 0xFF),
            (uint8_t)((c >> 8) & 0xFF),
            (uint8_t)((c >> 16) & 0xFF),
            (uint8_t)((c >> 24) & 0xFF)
        };
        fwrite(bgra, 1, 4, f);
    }

    fclose(f);
}

/* ---- Tests ------------------------------------------------------------ */

static void test_bmp_32(void)
{
    printf("test_bmp_32:\n");
    uint32_t src[4] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF };
    write_bmp_32("_test.bmp", 2, 2, src);

    int w, h;
    uint32_t *px = NULL;
    CHECK(scene_image_load("_test.bmp", &w, &h, &px) == 0);
    CHECK_EQ((uint32_t)w, 2);
    CHECK_EQ((uint32_t)h, 2);
    if (px) {
        CHECK_EQ(px[0], 0xFFFF0000u);
        CHECK_EQ(px[1], 0xFF00FF00u);
        CHECK_EQ(px[2], 0xFF0000FFu);
        CHECK_EQ(px[3], 0xFFFFFFFFu);
        scene_image_free(px);
    }

    remove("_test.bmp");
}

static void test_tga_32(void)
{
    printf("test_tga_32:\n");
    uint32_t src[4] = { 0xAA112233, 0xBB445566, 0xCC778899, 0xDDAABBCC };
    write_tga_32("_test.tga", 2, 2, src);

    int w, h;
    uint32_t *px = NULL;
    CHECK(scene_image_load("_test.tga", &w, &h, &px) == 0);
    CHECK_EQ((uint32_t)w, 2);
    CHECK_EQ((uint32_t)h, 2);
    if (px) {
        CHECK_EQ(px[0], 0xAA112233u);
        CHECK_EQ(px[1], 0xBB445566u);
        CHECK_EQ(px[2], 0xCC778899u);
        CHECK_EQ(px[3], 0xDDAABBCCu);
        scene_image_free(px);
    }

    remove("_test.tga");
}

static void test_nonexistent(void)
{
    printf("test_nonexistent:\n");
    int w, h;
    uint32_t *px = NULL;
    CHECK(scene_image_load("no_such_file.bmp", &w, &h, &px) != 0);
    CHECK(scene_image_error() != NULL);
}

static void test_null_args(void)
{
    printf("test_null_args:\n");
    CHECK(scene_image_load(NULL, NULL, NULL, NULL) == -1);
}

/* ---- PNG (stb): round-trip + premultiply contract ---------------------- */

static void test_png_roundtrip(void)
{
    printf("test_png_roundtrip:\n");
    enum { W = 6, H = 4 };
    /* Straight RGBA source (PNG's native form): opaque white, semi red,
     * semi green, opaque black, semi blue, opaque gray — the semi ones
     * exercise the premultiply contract (compositor ARGB is
     * premultiplied).                                                    */
    static const uint8_t rgba[W * H * 4] = {
        255,255,255,255,  255,  0,  0,128,    0,255,  0, 64,
          0,  0,  0,255,    0,  0,255,200,  128,128,128,255,
        255,255,255,255,  255,  0,  0,128,    0,255,  0, 64,
          0,  0,  0,255,    0,  0,255,200,  128,128,128,255,
        255,255,255,255,  255,  0,  0,128,    0,255,  0, 64,
          0,  0,  0,255,    0,  0,255,200,  128,128,128,255,
        255,255,255,255,  255,  0,  0,128,    0,255,  0, 64,
          0,  0,  0,255,    0,  0,255,200,  128,128,128,255,
    };
    /* Expected premultiplied ARGB per column. */
    static const uint32_t expected[W] = {
        0xFFFFFFFFu,                                  /* a255 255/255  */
        0x80800000u,                                  /* a128 128/255  */
        0x40004000u,                                  /* a64  64/255   */
        0xFF000000u,
        0xC80000C8u,                                  /* a200 200/255  */
        0xFF808080u,
    };
    int rc = stbi_write_png("_test.png", W, H, 4, rgba, W * 4);
    CHECK(rc != 0);
    int w = 0, h = 0;
    uint32_t *px = NULL;
    CHECK(scene_image_load("_test.png", &w, &h, &px) == 0);
    CHECK_EQ((uint32_t)w, W);
    CHECK_EQ((uint32_t)h, H);
    if (px) {
        int row, col;
        for (row = 0; row < H; row++)
            for (col = 0; col < W; col++)
                CHECK_EQ(px[row * W + col], expected[col]);
        scene_image_free(px);
    }
    remove("_test.png");
}

/* ---- JPEG (stb): fixture, lossy tolerance ------------------------------ */

static void test_jpeg_fixture(void)
{
    printf("test_jpeg_fixture:\n");
    int w = 0, h = 0;
    uint32_t *px = NULL;
    CHECK(scene_image_load("tests/fixtures/jpeg_fix.jpg", &w, &h, &px) == 0);
    CHECK_EQ((uint32_t)w, 16);
    CHECK_EQ((uint32_t)h, 16);
    if (px) {
        /* Center probes away from the block seam; JPEG is lossy, so
         * each channel carries a small tolerance.                        */
        uint32_t a = px[8 * 16 + 4];   /* left half (228,74,43)        */
        uint32_t b = px[8 * 16 + 12];  /* right half (43,106,228)      */
        CHECK_EQ(a >> 24, 0xFFu);
        CHECK_EQ(b >> 24, 0xFFu);
        CHECK((a >> 16 & 0xFFu) > 220u && (a >> 16 & 0xFFu) < 236u);
        CHECK((a >> 8  & 0xFFu) >  70u && (a >> 8  & 0xFFu) <  78u);
        CHECK((a       & 0xFFu) >  39u && (a       & 0xFFu) <  47u);
        CHECK((b >> 16 & 0xFFu) >  39u && (b >> 16 & 0xFFu) <  47u);
        CHECK((b >> 8  & 0xFFu) > 102u && (b >> 8  & 0xFFu) < 110u);
        CHECK((b       & 0xFFu) > 224u && (b       & 0xFFu) < 232u);
        scene_image_free(px);
    }
}

/* ---- GIF (stb): fixture, lossless exact probes ------------------------ */

static void test_gif_fixture(void)
{
    printf("test_gif_fixture:\n");
    int w = 0, h = 0;
    uint32_t *px = NULL;
    CHECK(scene_image_load("tests/fixtures/gif_fix.gif", &w, &h, &px) == 0);
    CHECK_EQ((uint32_t)w, 8);
    CHECK_EQ((uint32_t)h, 8);
    if (px) {
        CHECK_EQ(px[0],          0xFFFF0000u);  /* (0,0) red    */
        CHECK_EQ(px[4],          0xFF00FF00u);  /* (4,0) green  */
        CHECK_EQ(px[4 * 8 + 0],  0xFF0000FFu);  /* (0,4) blue   */
        CHECK_EQ(px[4 * 8 + 4],  0xFFFFFF00u);  /* (4,4) yellow */
        scene_image_free(px);
    }
}

/* ---- Magic sniffing (extension-less files) ----------------------------- */

static void test_magic_sniff(void)
{
    printf("test_magic_sniff:\n");
    /* Copy the JPEG fixture without an extension: the loader must fall
     * back to magic detection (BMP in-house, everything else stb).      */
    FILE *src = fopen("tests/fixtures/jpeg_fix.jpg", "rb");
    CHECK(src != NULL);
    if (src) {
        FILE *dst = fopen("_test_noext", "wb");
        CHECK(dst != NULL);
        if (dst) {
            uint8_t buf[1024];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
                fwrite(buf, 1, n, dst);
            fclose(dst);
        }
        fclose(src);
    }
    int w = 0, h = 0;
    uint32_t *px = NULL;
    CHECK(scene_image_load("_test_noext", &w, &h, &px) == 0);
    CHECK_EQ((uint32_t)w, 16);
    CHECK_EQ((uint32_t)h, 16);
    if (px) scene_image_free(px);
    remove("_test_noext");
}

/* ---- main ------------------------------------------------------------ */

int main(void)
{
    test_bmp_32();
    test_tga_32();
    test_nonexistent();
    test_null_args();
    test_png_roundtrip();
    test_jpeg_fixture();
    test_gif_fixture();
    test_magic_sniff();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
