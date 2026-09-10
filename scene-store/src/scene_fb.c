/* scene_fb.c — CPU framebuffer and primitive rasterizer (compositor
 * core). Pixels are premultiplied ARGB; every draw clips to the given
 * clip rect, then to the framebuffer bounds. All arithmetic is integer
 * and deterministic; blend math matches scene_fb.h exactly. */
#include "scene_fb.h"

#include <stdlib.h>

/* Intersect `r` with `clip` (may be NULL) and the fb bounds; emits the
 * clamped half-open range [x0,x1) x [y0,y1). Returns 0 if empty.      */
static int fb_clip_rect(const scene_rect *r, const scene_rect *clip,
                        uint32_t fbw, uint32_t fbh,
                        int64_t *x0, int64_t *y0, int64_t *x1, int64_t *y1)
{
    int64_t lo, hi;

    if (r->w <= 0 || r->h <= 0) return 0;
    if (clip && (clip->w <= 0 || clip->h <= 0)) return 0;

    lo = r->x;
    hi = (int64_t)r->x + r->w;
    if (lo < 0) lo = 0;
    if (hi > (int64_t)fbw) hi = fbw;
    if (clip) {
        if (lo < clip->x) lo = clip->x;
        if (hi > (int64_t)clip->x + clip->w) hi = (int64_t)clip->x + clip->w;
    }
    if (lo >= hi) return 0;
    *x0 = lo;
    *x1 = hi;

    lo = r->y;
    hi = (int64_t)r->y + r->h;
    if (lo < 0) lo = 0;
    if (hi > (int64_t)fbh) hi = fbh;
    if (clip) {
        if (lo < clip->y) lo = clip->y;
        if (hi > (int64_t)clip->y + clip->h) hi = (int64_t)clip->y + clip->h;
    }
    if (lo >= hi) return 0;
    *y0 = lo;
    *y1 = hi;
    return 1;
}

int scene_fb_init(scene_fb *fb, uint32_t w, uint32_t h)
{
    uint64_t n;

    if (!fb) return -1;
    fb->px = NULL;
    fb->w = 0;
    fb->h = 0;
    fb->pitch = 0;
    if (w == 0 || h == 0) return -1;
    n = (uint64_t)w * (uint64_t)h;
    if (n > SIZE_MAX / sizeof(uint32_t)) return -1;
    fb->px = calloc((size_t)n, sizeof(uint32_t));
    if (!fb->px) return -1;
    fb->w = w;
    fb->h = h;
    fb->pitch = w;
    return 0;
}

void scene_fb_free(scene_fb *fb)
{
    if (!fb) return;
    free(fb->px);
    fb->px = NULL;
    fb->w = 0;
    fb->h = 0;
    fb->pitch = 0;
}

uint32_t scene_fb_get(const scene_fb *fb, int32_t x, int32_t y)
{
    if (!fb || !fb->px || x < 0 || y < 0) return 0;
    if ((uint32_t)x >= fb->w || (uint32_t)y >= fb->h) return 0;
    return fb->px[(size_t)y * fb->pitch + (uint32_t)x];
}

void scene_fb_clear(scene_fb *fb, uint32_t color)
{
    uint32_t y;

    if (!fb || !fb->px) return;
    for (y = 0; y < fb->h; y++) {
        uint32_t *row = fb->px + (size_t)y * fb->pitch;
        uint32_t x;
        for (x = 0; x < fb->w; x++)
            row[x] = color;
    }
}

void scene_fb_fill(scene_fb *fb, const scene_rect *r, uint32_t color,
                   const scene_rect *clip)
{
    int64_t x0, y0, x1, y1;
    uint32_t y;

    if (!fb || !fb->px || !r) return;
    if (!fb_clip_rect(r, clip, fb->w, fb->h, &x0, &y0, &x1, &y1)) return;
    for (y = (uint32_t)y0; y < (uint32_t)y1; y++) {
        uint32_t *row = fb->px + (size_t)y * fb->pitch;
        uint32_t x;
        for (x = (uint32_t)x0; x < (uint32_t)x1; x++)
            row[x] = color;
    }
}

void scene_fb_stroke(scene_fb *fb, const scene_rect *r, uint32_t color,
                     uint8_t border_w, const scene_rect *clip)
{
    scene_rect s;

    if (!fb || !r || border_w == 0) return;
    s.x = r->x;
    s.y = r->y;
    s.w = r->w;
    s.h = (int32_t)border_w;
    scene_fb_fill(fb, &s, color, clip);
    s.y = (int32_t)((int64_t)r->y + r->h - border_w);
    scene_fb_fill(fb, &s, color, clip);
    s.y = r->y;
    s.w = (int32_t)border_w;
    s.h = r->h;
    scene_fb_fill(fb, &s, color, clip);
    s.x = (int32_t)((int64_t)r->x + r->w - border_w);
    scene_fb_fill(fb, &s, color, clip);
}

void scene_fb_fill_a(scene_fb *fb, const scene_rect *r, uint32_t color,
                     uint32_t alpha, const scene_rect *clip)
{
    int64_t x0, y0, x1, y1;
    uint32_t y;

    if (!fb || !fb->px || !r) return;
    if (alpha >= 255u) {
        scene_fb_fill(fb, r, color, clip);
        return;
    }
    if (alpha == 0) return;
    if (!fb_clip_rect(r, clip, fb->w, fb->h, &x0, &y0, &x1, &y1)) return;
    {
        uint32_t inv = 255u - alpha;
        uint32_t ca = (color >> 24) & 0xFFu;
        uint32_t cr = (color >> 16) & 0xFFu;
        uint32_t cg = (color >>  8) & 0xFFu;
        uint32_t cb = color & 0xFFu;

        for (y = (uint32_t)y0; y < (uint32_t)y1; y++) {
            uint32_t *row = fb->px + (size_t)y * fb->pitch;
            uint32_t x;
            for (x = (uint32_t)x0; x < (uint32_t)x1; x++) {
                uint32_t dc = row[x];
                uint32_t a = (ca * alpha + ((dc >> 24) & 0xFFu) * inv) / 255u;
                uint32_t rr = (cr * alpha + ((dc >> 16) & 0xFFu) * inv) / 255u;
                uint32_t gg = (cg * alpha + ((dc >>  8) & 0xFFu) * inv) / 255u;
                uint32_t bb = (cb * alpha + (dc & 0xFFu) * inv) / 255u;
                row[x] = (a << 24) | (rr << 16) | (gg << 8) | bb;
            }
        }
    }
}

void scene_fb_stroke_a(scene_fb *fb, const scene_rect *r, uint32_t color,
                       uint8_t border_w, uint32_t alpha,
                       const scene_rect *clip)
{
    scene_rect s;

    if (!fb || !r || border_w == 0) return;
    s.x = r->x;
    s.y = r->y;
    s.w = r->w;
    s.h = (int32_t)border_w;
    scene_fb_fill_a(fb, &s, color, alpha, clip);
    s.y = (int32_t)((int64_t)r->y + r->h - border_w);
    scene_fb_fill_a(fb, &s, color, alpha, clip);
    s.y = r->y;
    s.w = (int32_t)border_w;
    s.h = r->h;
    scene_fb_fill_a(fb, &s, color, alpha, clip);
    s.x = (int32_t)((int64_t)r->x + r->w - border_w);
    scene_fb_fill_a(fb, &s, color, alpha, clip);
}

/* Number of columns a corner circle of `radius` cuts from a rect at a
 * row `dy` pixels from the nearest horizontal edge, floored, 0 once
 * dy >= radius. The corner circle is a FILLET: centered at (x+r, y+r)
 * with the rect corner at distance r in both axes, so the cut at row dy
 * is radius - sqrt(2*r*dy - dy^2). (The naive radius - sqrt(r^2 - dy^2)
 * centers the circle at the rect corner itself and leaves the top edge
 * row uncut — an under-rounded corner. Fixed in Pass 7.)              */
static int rcorner_inset(uint8_t radius, uint32_t dy)
{
    uint32_t r = (uint32_t)radius;
    int64_t inside, s;

    if (dy >= r) return 0;
    inside = 2 * (int64_t)r * (int64_t)dy - (int64_t)dy * (int64_t)dy;
    s = 0;
    while ((s + 1) * (s + 1) <= inside) s++;
    return (int)r - (int)s;
}

void scene_fb_fill_round(scene_fb *fb, const scene_rect *r, uint32_t color,
                         uint8_t radius, uint32_t alpha,
                         const scene_rect *clip)
{
    scene_rect run;
    uint32_t h, y, rr;

    if (!fb || !fb->px || !r) return;
    if (r->w <= 0 || r->h <= 0) return;
    rr = radius;
    if (rr > (uint32_t)r->w / 2u) rr = (uint32_t)r->w / 2u;
    if (rr > (uint32_t)r->h / 2u) rr = (uint32_t)r->h / 2u;
    if (rr == 0) {
        scene_fb_fill_a(fb, r, color, alpha, clip);
        return;
    }
    h = (uint32_t)r->h;
    for (y = 0; y < h; y++) {
        uint32_t d = y;
        uint32_t from_bottom = h - 1u - y;
        int32_t inset;

        if (from_bottom < d) d = from_bottom;
        inset = rcorner_inset((uint8_t)rr, d);
        run.x = r->x + inset;
        run.y = r->y + (int32_t)y;
        run.w = r->w - 2 * inset;
        run.h = 1;
        if (run.w > 0)
            scene_fb_fill_a(fb, &run, color, alpha, clip);
    }
}

/* One-pass rounded chrome: single blend per pixel, interior = fill,
 * ring = border, notch = clear (see scene_fb.h). */
void scene_fb_chrome_round(scene_fb *fb, const scene_rect *r, uint8_t radius,
                           uint8_t bw, uint32_t fill, uint32_t border,
                           uint32_t alpha, const scene_rect *clip)
{
    int64_t x0, y0, x1, y1;
    uint32_t rr, h, y;

    if (!fb || !fb->px || !r) return;
    if (r->w <= 0 || r->h <= 0) return;
    if (radius == 0) {
        scene_fb_fill_a(fb, r, fill, alpha, clip);
        if (bw) scene_fb_stroke_a(fb, r, border, bw, alpha, clip);
        return;
    }
    rr = radius;
    if (rr > (uint32_t)r->w / 2u) rr = (uint32_t)r->w / 2u;
    if (rr > (uint32_t)r->h / 2u) rr = (uint32_t)r->h / 2u;
    if (rr == 0 || bw == 0) {
        scene_fb_fill_round(fb, r, fill, (uint8_t)rr, alpha, clip);
        return;
    }
    if (!fb_clip_rect(r, clip, fb->w, fb->h, &x0, &y0, &x1, &y1)) return;
    h = (uint32_t)r->h;
    {
        int32_t ix0 = r->x + (int32_t)bw;
        int32_t iy0 = r->y + (int32_t)bw;
        int32_t iw = r->w - 2 * (int32_t)bw;
        int32_t ih = r->h - 2 * (int32_t)bw;
        uint32_t ir = rr > (uint32_t)bw ? rr - (uint32_t)bw : 0;
        uint32_t inv = 255u - alpha;

        for (y = (uint32_t)y0; y < (uint32_t)y1; y++) {
            uint32_t d = y - (uint32_t)r->y;
            uint32_t from_bottom = h - 1u - d;
            int32_t ocut, icut = 0;
            int32_t ixl = INT32_MAX, ixh = INT32_MIN;
            uint32_t *row = fb->px + (size_t)y * fb->pitch;
            uint32_t x;

            if (from_bottom < d) d = from_bottom;
            ocut = rcorner_inset((uint8_t)rr, d);
            if (fill && iw > 0 && ih > 0
                && y >= (uint32_t)iy0 && y < (uint32_t)iy0 + (uint32_t)ih) {
                uint32_t id = y - (uint32_t)iy0;
                uint32_t ifb = (uint32_t)ih - 1u - id;
                if (ifb < id) id = ifb;
                icut = rcorner_inset((uint8_t)ir, id);
                ixl = ix0 + icut;
                ixh = ix0 + iw - icut;
            }
            for (x = (uint32_t)x0; x < (uint32_t)x1; x++) {
                int32_t xx = (int32_t)x;
                uint32_t dc, col;
                int use_fill, use_border;

                use_fill = fill && xx >= ixl && xx < ixh;
                use_border = xx >= r->x + ocut
                             && xx < r->x + r->w - ocut;
                if (!use_fill && !use_border) continue;
                col = use_fill ? fill : border;
                dc = row[x];
                if (alpha >= 255u) {
                    row[x] = col;
                } else {
                    uint32_t ca2 = (col >> 24) & 0xFFu;
                    uint32_t cr2 = (col >> 16) & 0xFFu;
                    uint32_t cg2 = (col >>  8) & 0xFFu;
                    uint32_t cb2 = col & 0xFFu;
                    uint32_t a = (ca2 * alpha + ((dc >> 24) & 0xFFu) * inv)
                                 / 255u;
                    uint32_t rr2 = (cr2 * alpha
                                    + ((dc >> 16) & 0xFFu) * inv) / 255u;
                    uint32_t gg = (cg2 * alpha
                                   + ((dc >>  8) & 0xFFu) * inv) / 255u;
                    uint32_t bb2 = (cb2 * alpha
                                    + (dc & 0xFFu) * inv) / 255u;
                    row[x] = (a << 24) | (rr2 << 16) | (gg << 8) | bb2;
                }
            }
        }
    }
}

void scene_fb_blit(scene_fb *fb, int32_t dx, int32_t dy,
                   const uint32_t *src, uint32_t src_w, uint32_t src_h,
                   const scene_rect *src_rc, uint8_t opacity, uint8_t fmt,
                   const scene_rect *clip)
{
    int64_t sx0, sx1, sy0, sy1;
    int64_t ddx0, ddx1, ddy0, ddy1;
    int64_t dox, doy;
    uint32_t y;

    if (!fb || !fb->px || !src || !src_rc) return;
    if (src_w == 0 || src_h == 0 || src_rc->w <= 0 || src_rc->h <= 0) return;
    if (opacity == 0) return;

    /* Clamp the source copy to what actually exists in src.            */
    sx0 = src_rc->x;
    sx1 = (int64_t)src_rc->x + src_rc->w;
    if (sx0 < 0) sx0 = 0;
    if (sx1 > (int64_t)src_w) sx1 = src_w;
    sy0 = src_rc->y;
    sy1 = (int64_t)src_rc->y + src_rc->h;
    if (sy0 < 0) sy0 = 0;
    if (sy1 > (int64_t)src_h) sy1 = src_h;
    if (sx0 >= sx1 || sy0 >= sy1) return;

    /* Destination rect is the clamped source region, translated.       */
    dox = (int64_t)dx + sx0 - src_rc->x;   /* TRUE dest origin: the      */
    doy = (int64_t)dy + sy0 - src_rc->y;   /* source mapping anchors     */
    ddx0 = dox;                            /* here, NOT at the clip edge */
    ddx1 = (int64_t)dx + sx1 - src_rc->x;
    ddy0 = doy;
    ddy1 = (int64_t)dy + sy1 - src_rc->y;

    /* Clip destination to fb bounds, then to the caller's clip.        */
    if (ddx0 < 0) ddx0 = 0;
    if (ddx1 > (int64_t)fb->w) ddx1 = fb->w;
    if (ddy0 < 0) ddy0 = 0;
    if (ddy1 > (int64_t)fb->h) ddy1 = fb->h;
    if (clip) {
        if (ddx0 < clip->x) ddx0 = clip->x;
        if (ddx1 > (int64_t)clip->x + clip->w) ddx1 = (int64_t)clip->x + clip->w;
        if (ddy0 < clip->y) ddy0 = clip->y;
        if (ddy1 > (int64_t)clip->y + clip->h) ddy1 = (int64_t)clip->y + clip->h;
    }
    if (ddx0 >= ddx1 || ddy0 >= ddy1) return;

    if (fmt == SCENE_TEX_FMT_XRGB) {
        /* Opaque src: out = (src_c*op + dst_c*(255-op)) / 255, A = 255. */
        for (y = (uint32_t)ddy0; y < (uint32_t)ddy1; y++) {
            const uint32_t *srow =
                src + (size_t)((int64_t)sy0 + y - doy) * src_w;
            uint32_t *drow = fb->px + (size_t)y * fb->pitch;
            uint32_t x;
            if (opacity == 255) {
                for (x = (uint32_t)ddx0; x < (uint32_t)ddx1; x++)
                    drow[x] = (srow[(size_t)((int64_t)sx0 + x - dox)]
                               & UINT32_C(0x00FFFFFF)) | UINT32_C(0xFF000000);
            } else {
                uint32_t inv = 255u - opacity;
                for (x = (uint32_t)ddx0; x < (uint32_t)ddx1; x++) {
                    uint32_t sc = srow[(size_t)((int64_t)sx0 + x - dox)];
                    uint32_t dc = drow[x];
                    uint32_t a, r, g, b;
                    a = UINT32_C(0xFF000000);
                    r = (((sc >> 16) & 0xFFu) * opacity
                         + ((dc >> 16) & 0xFFu) * inv) / 255u;
                    g = (((sc >> 8) & 0xFFu) * opacity
                         + ((dc >> 8) & 0xFFu) * inv) / 255u;
                    b = ((sc & 0xFFu) * opacity
                         + (dc & 0xFFu) * inv) / 255u;
                    drow[x] = a | (r << 16) | (g << 8) | b;
                }
            }
        }
    } else if (fmt == SCENE_TEX_FMT_ARGB) {
        /* Premul src-over: t = (src_a*op)/255;
         * out_c = (src_c*op)/255 + (dst_c*(255-t))/255, per channel.    */
        for (y = (uint32_t)ddy0; y < (uint32_t)ddy1; y++) {
            const uint32_t *srow =
                src + (size_t)((int64_t)sy0 + y - doy) * src_w;
            uint32_t *drow = fb->px + (size_t)y * fb->pitch;
            uint32_t x;
            for (x = (uint32_t)ddx0; x < (uint32_t)ddx1; x++) {
                uint32_t sc = srow[(size_t)((int64_t)sx0 + x - dox)];
                uint32_t dc = drow[x];
                uint32_t t = ((sc >> 24) & 0xFFu) * opacity / 255u;
                uint32_t invt = 255u - t;
                uint32_t a, r, g, b;
                a = ((sc >> 24) & 0xFFu) * opacity / 255u
                    + ((dc >> 24) & 0xFFu) * invt / 255u;
                r = ((sc >> 16) & 0xFFu) * opacity / 255u
                    + ((dc >> 16) & 0xFFu) * invt / 255u;
                g = ((sc >> 8) & 0xFFu) * opacity / 255u
                    + ((dc >> 8) & 0xFFu) * invt / 255u;
                b = (sc & 0xFFu) * opacity / 255u
                    + (dc & 0xFFu) * invt / 255u;
                drow[x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }
}
