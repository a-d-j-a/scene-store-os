/*
 * test_image.c — tests for BMP/TGA image loader.
 *
 * Creates minimal test images in-memory, writes them to temp files,
 * then loads and verifies pixel data.
 */
#include "scene_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ---- main ------------------------------------------------------------ */

int main(void)
{
    test_bmp_32();
    test_tga_32();
    test_nonexistent();
    test_null_args();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
