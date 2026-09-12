/* scene_font.c — in-house 8x8 bitmap text rendering + optional TrueType
 * fallback via callback-based glyph lookup (no hard fontcache dependency).
 * Glyph rows are MSB-first (bit 7 = leftmost pixel); drawing clips to
 * the given clip rect, then to the fb bounds, and writes premultiplied
 * color pixels directly. */
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

/* --- UTF-8 decode helper --- */
static uint32_t utf8_decode(const char **pp, const char *end)
{
    const unsigned char *p = (const unsigned char *)*pp;
    uint32_t cp;
    int len;

    if (p >= (const unsigned char *)end) { *pp = end; return 0xFFFDu; }
    if (*p < 0x80u) { cp = *p++; *pp = (const char *)p; return cp; }
    if ((*p & 0xE0u) == 0xC0u) { len = 2; cp = *p++ & 0x1Fu; }
    else if ((*p & 0xF0u) == 0xE0u) { len = 3; cp = *p++ & 0x0Fu; }
    else if ((*p & 0xF8u) == 0xF0u) { len = 4; cp = *p++ & 0x07u; }
    else { *pp = (const char *)p + 1; return 0xFFFDu; }
    while (--len > 0 && p < (const unsigned char *)end && (*p & 0xC0u) == 0x80u)
        cp = (cp << 6) | (*p++ & 0x3Fu);
    *pp = (const char *)p;
    return cp;
}

/* Blit an alpha bitmap onto the framebuffer at (dx, dy), blended by `alpha`. */
static inline void blit_alpha(scene_fb *fb, int32_t dx, int32_t dy,
                              int gw, int gh, const uint8_t *bmp,
                              uint32_t cr, uint32_t cg, uint32_t cb,
                              uint32_t alpha, const scene_rect *clip)
{
    int gy;
    for (gy = 0; gy < gh; gy++) {
        int gx;
        int32_t yy = dy + gy;
        if (yy < 0 || yy >= (int32_t)fb->h) continue;
        if (clip && (yy < clip->y || yy >= clip->y + clip->h)) continue;
        for (gx = 0; gx < gw; gx++) {
            int32_t xx = dx + gx;
            uint32_t a_src, a;
            if (xx < 0 || xx >= (int32_t)fb->w) continue;
            if (clip && (xx < clip->x || xx >= clip->x + clip->w)) continue;
            a_src = (uint32_t)bmp[gy * gw + gx];
            if (a_src == 0) continue;
            a = a_src * alpha / 255u;
            if (a > 0) {
                uint32_t inv = 255u - a;
                uint32_t dc = fb->px[(size_t)yy * fb->pitch + (uint32_t)xx];
                uint32_t dr = (dc >> 16) & 0xFFu;
                uint32_t dg = (dc >>  8) & 0xFFu;
                uint32_t db = dc & 0xFFu;
                uint32_t r = (cr * a + dr * inv) / 255u;
                uint32_t g = (cg * a + dg * inv) / 255u;
                uint32_t b = (cb * a + db * inv) / 255u;
                fb->px[(size_t)yy * fb->pitch + (uint32_t)xx] =
                    (r << 16) | (g << 8) | b;
            }
        }
    }
}

int scene_font_draw_utf8(scene_fb *fb, int32_t x, int32_t y,
                         const char *s, uint32_t len, uint32_t color,
                         uint32_t alpha, scene_utf8_lookup_fn lookup,
                         void *ud, const scene_rect *clip)
{
    const char *p = s, *end = s + len;
    int32_t cx = x;
    uint32_t cr = (color >> 16) & 0xFFu;
    uint32_t cg = (color >>  8) & 0xFFu;
    uint32_t cb = color & 0xFFu;

    if (!fb || !fb->px || !s || len == 0 || alpha == 0) return 0;

    while (p < end) {
        uint32_t cp_code = utf8_decode(&p, end);

        if (cp_code < 128u) {
            /* ASCII: fast bitmap path */
            const uint8_t *g = scene_font_glyph((unsigned char)cp_code);
            int gy;
            for (gy = 0; gy < SCENE_FONT_GLYPH_H; gy++) {
                uint8_t bits = g[gy];
                int32_t yy = y + gy;
                uint8_t bit;
                if (bits == 0 || yy < 0 || yy >= (int32_t)fb->h) continue;
                if (clip && (yy < clip->y || yy >= clip->y + clip->h)) continue;
                for (bit = 0; bit < 8; bit++) {
                    int32_t px = cx + (int32_t)bit;
                    if (px < 0 || px >= (int32_t)fb->w) continue;
                    if (clip && (px < clip->x || px >= clip->x + clip->w)) continue;
                    if (bits & (0x80u >> bit)) {
                        if (alpha >= 255u) {
                            fb->px[(size_t)yy * fb->pitch + (uint32_t)px] =
                                (cr << 16) | (cg << 8) | cb;
                        } else {
                            uint32_t inv = 255u - alpha;
                            uint32_t dc = fb->px[(size_t)yy * fb->pitch + (uint32_t)px];
                            uint32_t dr = (dc >> 16) & 0xFFu;
                            uint32_t dg = (dc >>  8) & 0xFFu;
                            uint32_t db = dc & 0xFFu;
                            fb->px[(size_t)yy * fb->pitch + (uint32_t)px] =
                                (((cr * alpha + dr * inv) / 255u) << 16) |
                                (((cg * alpha + dg * inv) / 255u) <<  8) |
                                 ((cb * alpha + db * inv) / 255u);
                        }
                    }
                }
            }
            cx += SCENE_FONT_ADVANCE;
        } else if (lookup) {
            /* TrueType path: look up glyph via callback */
            scene_utf8_glyph g = lookup(ud, cp_code);
            if (g.pixels) {
                int32_t dx = cx + g.x0;
                int32_t dy = y + g.y0;
                blit_alpha(fb, dx, dy, g.w, g.h, g.pixels,
                           cr, cg, cb, alpha, clip);
            }
            cx += g.advance ? g.advance : SCENE_FONT_ADVANCE;
        } else {
            /* No cache: draw box glyph (index 96) for non-ASCII */
            const uint8_t *g = scene_font_glyph(96);
            int gy;
            for (gy = 0; gy < SCENE_FONT_GLYPH_H; gy++) {
                uint8_t bits = g[gy];
                int32_t yy = y + gy;
                uint8_t bit;
                if (bits == 0 || yy < 0 || yy >= (int32_t)fb->h) continue;
                for (bit = 0; bit < 8; bit++) {
                    int32_t px = cx + (int32_t)bit;
                    if (px < 0 || px >= (int32_t)fb->w) continue;
                    if (bits & (0x80u >> bit))
                        fb->px[(size_t)yy * fb->pitch + (uint32_t)px] =
                            (cr << 16) | (cg << 8) | cb;
                }
            }
            cx += SCENE_FONT_ADVANCE;
        }
    }
    return cx - x;
}
