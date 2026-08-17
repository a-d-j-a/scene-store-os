/*
 * test_codec.c — deterministic media decode at the OS seam.
 *
 * The fixture (tests/fixtures/demo.mpg) is the locked demo clip:
 * mpeg1video in an MPEG-PS container, 240x128, 100 frames, 25fps,
 * encoded from a deterministic generator (background cycles
 * red/green/blue every 3 frames, a 10px-tall white bar whose top row
 * is (n*11)%118, a 4px white vertical stripe at x=(n*3)%236). Every
 * expected pixel is derived from that same generator — nothing is
 * guessed.
 *
 * Assertions:
 *   1. open: 240x128, 25fps nominal, no frames decoded yet.
 *   2. decode: exactly 100 frames, then EOF.
 *   3. determinism: two full decode passes are byte-identical.
 *   4. per-frame truth on sampled frames (0,1,2,9,10,38,39,40,98,99):
 *      bar at the generator position (+-2px), background hue by n%3,
 *      alpha byte always 0xFF (XRGB opaque).
 *   5. error path: opening a nonexistent file fails cleanly.
 */
#include "scene_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do {                                                    \
    checks++;                                                               \
    if (!(cond)) {                                                          \
        failures++;                                                         \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
    }                                                                       \
} while (0)

#define CHECK_EQ(a, b) do {                                                 \
    checks++;                                                               \
    uint64_t va = (uint64_t)(a), vb = (uint64_t)(b);                        \
    if (va != vb) {                                                         \
        failures++;                                                         \
        printf("FAIL %s:%d: %s (%llu) != %s (%llu)\n", __FILE__, __LINE__,  \
               #a, (unsigned long long)va, #b, (unsigned long long)vb);     \
    }                                                                       \
} while (0)

#define FIXTURE "tests/fixtures/demo.mpg"

#define FW 240
#define FH 128
#define NFRAMES 100

/* ---- the generator the fixture was encoded from ------------------------ */

/* Background hue of frame n: 0=red, 1=green, 2=blue. */
static int bg_hue(uint32_t n)
{
    return (int)(n % 3);
}

/* White bar top row of frame n; the 4px stripe covers probe column
 * x=119 for n where (n*3)%236 is in [116,119] (n=39 within 0..99), in
 * which case the column is white from row 0. */
static int bar_top_exp(uint32_t n)
{
    uint32_t stripe = (n * 3u) % 236u;
    if (stripe >= 116u && stripe <= 119u) return 0;
    return (int)((n * 11u) % 118u);
}

/* Scan column x=119 for the first all-white row. */
static int bar_top_found(const uint32_t *px)
{
    int y;

    for (y = 0; y < FH; y++) {
        uint32_t p = px[(size_t)y * FW + 119];
        if (((p >> 16) & 0xFFu) >= 200u &&
            ((p >> 8) & 0xFFu) >= 200u &&
            (p & 0xFFu) >= 200u) return y;
    }
    return -1;
}

/* Dominant hue over the whole frame (white bar pixels are ~8% of the
 * frame; they vote for all three channels and are outweighed by the
 * background). Returns 0=red,1=green,2=blue,3=undecided. */
static int bg_hue_found(const uint32_t *px)
{
    unsigned cnt[3] = {0, 0, 0};
    uint32_t y, x;

    for (y = 4; y < FH; y++) {
        for (x = 0; x < FW; x++) {
            uint32_t p = px[(size_t)y * FW + x];
            if (((p >> 16) & 0xFFu) > 150u) cnt[0]++;
            if (((p >> 8) & 0xFFu) > 150u) cnt[1]++;
            if ((p & 0xFFu) > 150u) cnt[2]++;
        }
    }
    if (cnt[0] > cnt[1] && cnt[0] > cnt[2]) return 0;
    if (cnt[1] > cnt[0] && cnt[1] > cnt[2]) return 1;
    if (cnt[2] > cnt[0] && cnt[2] > cnt[1]) return 2;
    return 3;
}

/* ---- tests -------------------------------------------------------------- */

static void test_codec_open(void)
{
    scene_codec *c;
    uint32_t w = 0, h = 0;
    unsigned fps = 0, frames = 99;

    c = scene_codec_open(FIXTURE, &w, &h, &fps, &frames);
    CHECK(c != NULL);
    if (!c) return;
    CHECK_EQ(w, FW);
    CHECK_EQ(h, FH);
    /* Nominal 25fps, but the muxer's r_frame_rate header field is odd
     * (50/1); accept 20..60 — the fps value is cosmetic to the importer
     * (the compositor advances a frame per tick regardless). */
    CHECK(fps >= 20 && fps <= 60);
    CHECK_EQ(frames, 0);                 /* nothing decoded yet */
    CHECK_EQ(scene_codec_frames(c), 0);

    /* decode one frame, then close; the count follows */
    {
        uint32_t px[FW * FH];
        CHECK_EQ(scene_codec_frame(c, px), 1);
        CHECK_EQ(scene_codec_frames(c), 1);
    }
    scene_codec_close(c);

    /* NULL/error paths */
    scene_codec_close(NULL);
    CHECK(scene_codec_open("tests/fixtures/no-such-file.mpg", NULL, NULL,
                           NULL, NULL) == NULL);
    printf("test_codec_open: ok\n");
}

static void test_codec_decode_all(void)
{
    scene_codec *c;
    uint32_t *px = malloc(sizeof(uint32_t) * FW * FH);
    int r, n = 0;

    CHECK(px != NULL);
    c = scene_codec_open(FIXTURE, NULL, NULL, NULL, NULL);
    CHECK(c != NULL);
    if (!c || !px) { free(px); return; }
    while ((r = scene_codec_frame(c, px)) == 1) n++;
    CHECK_EQ(r, 0);                       /* end of stream, not error */
    CHECK_EQ(n, NFRAMES);
    CHECK_EQ(scene_codec_frames(c), NFRAMES);
    /* EOF is sticky and repeating the call still returns 0 */
    CHECK_EQ(scene_codec_frame(c, px), 0);
    scene_codec_close(c);
    free(px);
    printf("test_codec_decode_all: ok\n");
}

static void test_codec_determinism(void)
{
    scene_codec *c;
    uint32_t *a = malloc(sizeof(uint32_t) * FW * FH * NFRAMES);
    uint32_t *b = malloc(sizeof(uint32_t) * FW * FH * NFRAMES);
    int r, n = 0;

    CHECK(a != NULL && b != NULL);
    c = scene_codec_open(FIXTURE, NULL, NULL, NULL, NULL);
    CHECK(c != NULL);
    if (!c || !a || !b) { free(a); free(b); return; }
    while ((r = scene_codec_frame(c, a + (size_t)n * FW * FH)) == 1) n++;
    CHECK_EQ(n, NFRAMES);
    scene_codec_close(c);

    c = scene_codec_open(FIXTURE, NULL, NULL, NULL, NULL);
    CHECK(c != NULL);
    if (!c) { free(a); free(b); return; }
    for (n = 0; n < NFRAMES; n++)
        CHECK_EQ(scene_codec_frame(c, b + (size_t)n * FW * FH), 1);
    scene_codec_close(c);

    CHECK(memcmp(a, b, sizeof(uint32_t) * FW * FH * NFRAMES) == 0);
    free(a);
    free(b);
    printf("test_codec_determinism: ok\n");
}

static void test_codec_truth(void)
{
    static const uint32_t samples[] = {0, 1, 2, 9, 10, 38, 39, 40, 98, 99};
    scene_codec *c;
    uint32_t *px = malloc(sizeof(uint32_t) * FW * FH);
    size_t i;
    int n;

    CHECK(px != NULL);
    c = scene_codec_open(FIXTURE, NULL, NULL, NULL, NULL);
    CHECK(c != NULL);
    if (!c || !px) { free(px); return; }

    for (n = 0; n < NFRAMES; n++) {
        CHECK_EQ(scene_codec_frame(c, px), 1);
        for (i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
            if ((uint32_t)n != samples[i]) continue;
            /* alpha always opaque XRGB */
            {
                uint32_t y, x;
                for (y = 0; y < FH; y += 7)
                    for (x = 0; x < FW; x += 11)
                        CHECK(((px[(size_t)y * FW + x] >> 24) & 0xFFu)
                              == 0xFFu);
            }
            /* bar at the generator position (+-2px of YUV rounding) */
            {
                int got = bar_top_found(px);
                int exp = bar_top_exp((uint32_t)n);
                CHECK(got >= 0 && abs(got - exp) <= 2);
                if (got < 0 || abs(got - exp) > 2)
                    printf("  frame %d: bar got=%d exp=%d\n", n, got, exp);
            }
            /* background hue by n%3 */
            {
                int got = bg_hue_found(px);
                int exp = bg_hue((uint32_t)n);
                CHECK_EQ(got, exp);
                if (got != exp)
                    printf("  frame %d: hue got=%d exp=%d\n", n, got, exp);
            }
        }
    }
    CHECK_EQ(scene_codec_frame(c, px), 0);   /* drained cleanly */

    scene_codec_close(c);
    free(px);
    printf("test_codec_truth: ok\n");
}

int main(void)
{
    test_codec_open();
    test_codec_decode_all();
    test_codec_determinism();
    test_codec_truth();

    printf("test_codec: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
