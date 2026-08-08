/*
 * scene_font.h — compositor text rendering (in-house bitmap font).
 *
 * The compositor's own 8x8 bitmap font: ASCII 32..126 plus a replacement
 * box for everything else. Glyphs are 8 rows of one byte each, bitmap
 * MSB-first (bit 7 = leftmost pixel), top-down. Drawing is clipped and
 * writes premultiplied color pixels directly into the framebuffer.
 */
#ifndef SCENE_FONT_H
#define SCENE_FONT_H

#include "scene_fb.h"

#define SCENE_FONT_W       8
#define SCENE_FONT_GLYPH_H 8
#define SCENE_FONT_ADVANCE 8

/* Glyph index 0..95 = ASCII 32..126; 96 = replacement box. Never NULL.  */
const uint8_t *scene_font_glyph(unsigned char c);

/* Horizontal advance of `len` characters (len * SCENE_FONT_ADVANCE).    */
int scene_font_advance(const char *s, uint32_t len);

/* Draw `s` at (x, y) top-left using `color` (premultiplied), clipped.   */
void scene_font_draw(scene_fb *fb, int32_t x, int32_t y,
                     const char *s, uint32_t len, uint32_t color,
                     const scene_rect *clip);

/* Alpha-blended variant: glyph pixels blend by `alpha` per channel
 * (out = (color*alpha + dst*(255-alpha)) / 255). alpha 255 == draw.
 * Used by the compositor's enter/exit fade transitions.                */
void scene_font_draw_a(scene_fb *fb, int32_t x, int32_t y,
                       const char *s, uint32_t len, uint32_t color,
                       uint32_t alpha, const scene_rect *clip);

#endif /* SCENE_FONT_H */