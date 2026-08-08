/* scene_font.c — in-house 8x8 bitmap text rendering. Glyph rows are
 * MSB-first (bit 7 = leftmost pixel); drawing clips to the given clip
 * rect, then to the fb bounds, and writes premultiplied color pixels
 * directly (text is opaque; no blending). */
#include "scene_font.h"

#include <limits.h>

int scene_font_advance(const char *s, uint32_t len)
{
    (void)s;
    if (len > (uint32_t)(INT_MAX / SCENE_FONT_ADVANCE))
        return INT_MAX;
    return (int)len * SCENE_FONT_ADVANCE;
}

void scene_font_draw(scene_fb *fb, int32_t x, int32_t y,
                     const char *s, uint32_t len, uint32_t color,
                     const scene_rect *clip)
{
    scene_font_draw_a(fb, x, y, s, len, color, 255u, clip);
}

void scene_font_draw_a(scene_fb *fb, int32_t x, int32_t y,
                       const char *s, uint32_t len, uint32_t color,
                       uint32_t alpha, const scene_rect *clip)
{
    int64_t x0, x1, cy0, cy1;
    uint32_t i;

    if (!fb || !fb->px || !s || len == 0) return;
    if (alpha == 0) return;

    x0 = x;
    x1 = (int64_t)x + (int64_t)len * SCENE_FONT_ADVANCE;
    cy0 = y;
    cy1 = (int64_t)y + SCENE_FONT_GLYPH_H;
    if (x0 < 0) x0 = 0;
    if (x1 > (int64_t)fb->w) x1 = fb->w;
    if (cy0 < 0) cy0 = 0;
    if (cy1 > (int64_t)fb->h) cy1 = fb->h;
    if (clip) {
        if (x0 < clip->x) x0 = clip->x;
        if (x1 > (int64_t)clip->x + clip->w) x1 = (int64_t)clip->x + clip->w;
        if (cy0 < clip->y) cy0 = clip->y;
        if (cy1 > (int64_t)clip->y + clip->h) cy1 = (int64_t)clip->y + clip->h;
    }
    if (x0 >= x1 || cy0 >= cy1) return;

    if (alpha >= 255u) {
        for (i = 0; i < len; i++) {
            const uint8_t *g = scene_font_glyph((unsigned char)s[i]);
            int32_t cx = (int32_t)((int64_t)x + (int64_t)i * SCENE_FONT_ADVANCE);
            int32_t row;

            if (cx >= x1 || cx + SCENE_FONT_W <= x0) continue;
            for (row = 0; row < SCENE_FONT_GLYPH_H; row++) {
                uint8_t bits = g[row];
                int32_t yy = y + row;
                uint8_t bit;

                if (bits == 0 || yy < cy0 || yy >= cy1) continue;
                for (bit = 0; bit < 8; bit++) {
                    int32_t px = cx + bit;
                    if (px < x0 || px >= x1) continue;
                    if (bits & (0x80u >> bit))
                        fb->px[(size_t)yy * fb->pitch + (uint32_t)px] = color;
                }
            }
        }
        return;
    }

    {
        uint32_t inv = 255u - alpha;
        uint32_t ca = (color >> 24) & 0xFFu;
        uint32_t cr = (color >> 16) & 0xFFu;
        uint32_t cg = (color >>  8) & 0xFFu;
        uint32_t cb = color & 0xFFu;

        for (i = 0; i < len; i++) {
            const uint8_t *g = scene_font_glyph((unsigned char)s[i]);
            int32_t cx = (int32_t)((int64_t)x + (int64_t)i * SCENE_FONT_ADVANCE);
            int32_t row;

            if (cx >= x1 || cx + SCENE_FONT_W <= x0) continue;
            for (row = 0; row < SCENE_FONT_GLYPH_H; row++) {
                uint8_t bits = g[row];
                int32_t yy = y + row;
                uint8_t bit;

                if (bits == 0 || yy < cy0 || yy >= cy1) continue;
                for (bit = 0; bit < 8; bit++) {
                    int32_t px = cx + bit;
                    uint32_t dc;

                    if (px < x0 || px >= x1) continue;
                    if (!(bits & (0x80u >> bit))) continue;
                    dc = fb->px[(size_t)yy * fb->pitch + (uint32_t)px];
                    {
                        uint32_t a = (ca * alpha + ((dc >> 24) & 0xFFu) * inv)
                                     / 255u;
                        uint32_t rr = (cr * alpha
                                       + ((dc >> 16) & 0xFFu) * inv) / 255u;
                        uint32_t gg = (cg * alpha
                                       + ((dc >>  8) & 0xFFu) * inv) / 255u;
                        uint32_t bb = (cb * alpha + (dc & 0xFFu) * inv) / 255u;
                        fb->px[(size_t)yy * fb->pitch + (uint32_t)px] =
                            (a << 24) | (rr << 16) | (gg << 8) | bb;
                    }
                }
            }
        }
    }
}
