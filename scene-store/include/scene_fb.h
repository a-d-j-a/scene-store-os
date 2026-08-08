/*
 * scene_fb.h — framebuffer + primitive rasterizer (compositor core).
 *
 * The compositor renders into a CPU framebuffer of premultiplied ARGB
 * pixels (spec §3 Color). Backends (DRM/KMS, GDI, validation targets)
 * copy the damage rects the compositor reports; this module knows no
 * device. All drawing is clipped (clip rect, then fb bounds), all
 * arithmetic is integer and deterministic.
 */
#ifndef SCENE_FB_H
#define SCENE_FB_H

#include "scene_fmt.h"

typedef struct scene_fb {
    uint32_t w, h, pitch;        /* pitch in pixels                       */
    uint32_t *px;                /* premultiplied ARGB                    */
} scene_fb;

int  scene_fb_init(scene_fb *fb, uint32_t w, uint32_t h);   /* 0 = ok    */
void scene_fb_free(scene_fb *fb);
uint32_t scene_fb_get(const scene_fb *fb, int32_t x, int32_t y); /* 0 oob */
void scene_fb_clear(scene_fb *fb, uint32_t color);

/* Texture pixel formats (scene_fb_blit fmt).                            */
#define SCENE_TEX_FMT_XRGB 0u   /* opaque, straight (alpha ignored)      */
#define SCENE_TEX_FMT_ARGB 1u   /* premultiplied ARGB                     */

/* Fill `r` with `color`, clipped. clip NULL = no extra clip.            */
void scene_fb_fill(scene_fb *fb, const scene_rect *r, uint32_t color,
                   const scene_rect *clip);

/* border_w px stroke inside `r` (top/bottom/left/right), clipped.       */
void scene_fb_stroke(scene_fb *fb, const scene_rect *r, uint32_t color,
                     uint8_t border_w, const scene_rect *clip);

/* Alpha-blended fill: premul per-channel blend
 * out = (color*alpha + dst*(255-alpha)) / 255. alpha 255 == fill,
 * alpha 0 == no-op. Preparations used by the compositor effects layer.  */
void scene_fb_fill_a(scene_fb *fb, const scene_rect *r, uint32_t color,
                     uint32_t alpha, const scene_rect *clip);

/* `border_w` px stroke inside `r`, alpha-blended (see scene_fb_fill_a). */
void scene_fb_stroke_a(scene_fb *fb, const scene_rect *r, uint32_t color,
                       uint8_t border_w, uint32_t alpha,
                       const scene_rect *clip);

/* Rounded-rectangle fill: the four corners are clipped to a circle of
 * `radius` (capped at min(w,h)/2). radius 0 == scene_fb_fill_a. Rows
 * are drawn as per-row chords, so the cost scales with height only.     */
void scene_fb_fill_round(scene_fb *fb, const scene_rect *r, uint32_t color,
                         uint8_t radius, uint32_t alpha,
                         const scene_rect *clip);

/* One-pass rounded chrome (fill + border) for the effects layer: every
 * pixel of the chrome gets exactly ONE alpha blend, so fading the fill
 * over the border can never double-blend the interior. Interior pixels
 * (inside the concentric inner rounded rect of radius radius-border_w)
 * blend `fill`, ring pixels blend `border`, corner notches stay clear.
 * Radius 0 falls back to fill-over-stroke (no overlap there); bw 0 is
 * just scene_fb_fill_round; fill 0 draws the ring only.                */
void scene_fb_chrome_round(scene_fb *fb, const scene_rect *r, uint8_t radius,
                           uint8_t bw, uint32_t fill, uint32_t border,
                           uint32_t alpha, const scene_rect *clip);

/* 1:1 blit of src_rc from src (stride src_w px) to (dx, dy). No scaling.
 * fmt SCENE_TEX_FMT_XRGB: src is opaque; opacity 0..255 blends toward
 * dst. fmt SCENE_TEX_FMT_ARGB: premultiplied src-over with opacity.
 * Integer math only: out = (src*op + dst*(255 - src_a*op/255))/255 in
 * premul space for ARGB; (src*op + dst*(255-op))/255 for XRGB.          */
void scene_fb_blit(scene_fb *fb, int32_t dx, int32_t dy,
                   const uint32_t *src, uint32_t src_w, uint32_t src_h,
                   const scene_rect *src_rc, uint8_t opacity, uint8_t fmt,
                   const scene_rect *clip);

#endif /* SCENE_FB_H */
