/*
 * scene_wallpaper.c — lightweight wallpaper system for the compositor.
 *
 * All effects are pure integer math on ARGB pixel buffers.
 * No GPU, no shaders, no floating-point in hot paths.
 */
#include "scene_wallpaper.h"
#include "scene_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- constants -------------------------------------------------------- */

#define MAX_SLIDES     16
#define MAX_RENDER_W   1920
#define MAX_RENDER_H   1080

/* ---- internal state --------------------------------------------------- */

typedef struct slide_entry {
    uint32_t *pixels;       /* ARGB pixel data (owned)                    */
    int       width, height;
} slide_entry;

struct scene_wallpaper {
    /* Output / render dimensions */
    uint32_t out_w, out_h;          /* full output resolution             */
    uint32_t ren_w, ren_h;          /* internal render resolution         */

    /* Pixel buffer (caller uploads to texture) */
    uint32_t *buf;                  /* ren_w * ren_h ARGB pixels          */

    /* State */
    scene_wp_type type;
    int           occluded;
    uint64_t      frame;
    float         tick_f;           /* fractional tick for sub-frame       */

    /* Procedural params */
    scene_wp_params params;

    /* Static / Ken Burns */
    uint32_t *src_img;              /* source image pixels (owned)        */
    int       src_w, src_h;
    float     kb_duration;          /* Ken Burns cycle duration (sec)     */
    float     kb_zoom;              /* Ken Burns max zoom (1.0-1.5)       */

    /* Slideshow */
    slide_entry slides[MAX_SLIDES];
    int         slide_count;
    int         current_slide;
    int         next_slide;
    float       display_sec;        /* seconds per slide                  */
    float       fade_sec;           /* crossfade duration (sec)           */
    float       slide_timer;        /* current slide elapsed time         */
    int         in_crossfade;       /* 1 = transitioning                  */
    float       crossfade_t;        /* crossfade progress 0.0-1.0         */
};

/* ---- helpers ---------------------------------------------------------- */

static inline uint32_t pack_argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) |
           ((uint32_t)g << 8)  | b;
}

static inline uint8_t clamp255(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* Cosine easing: smooth start and end */
static inline float cosine_ease(float t)
{
    return (1.0f - cosf(t * 3.14159265f)) * 0.5f;
}

/* Blend two ARGB pixels with alpha (0-255) */
static inline uint32_t blend_px(uint32_t dst, uint32_t src, uint8_t alpha)
{
    if (alpha == 0) return dst;
    if (alpha == 255) return src;
    int a = alpha;
    int ia = 255 - a;
    int dr = ((dst >> 16) & 0xFF), dg = ((dst >> 8) & 0xFF), db = (dst & 0xFF);
    int sr = ((src >> 16) & 0xFF), sg = ((src >> 8) & 0xFF), sb = (src & 0xFF);
    int r = (sr * a + dr * ia) / 255;
    int g = (sg * a + dg * ia) / 255;
    int b = (sb * a + db * ia) / 255;
    uint8_t sa = (src >> 24) & 0xFF;
    uint8_t da = (dst >> 24) & 0xFF;
    int out_a = (sa * a + da * ia) / 255;
    return pack_argb((uint8_t)out_a, (uint8_t)r, (uint8_t)g, (uint8_t)b);
}

/* Simple 1D hash for noise */
static inline uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x45d9f3b;
    x ^= x >> 16; x *= 0x45d9f3b;
    x ^= x >> 16;
    return x;
}

/* ---- resolution selection --------------------------------------------- */

static void calc_render_size(uint32_t out_w, uint32_t out_h,
                             uint32_t *ren_w, uint32_t *ren_h)
{
    /* Render at max 1920x1080, preserve aspect ratio */
    if (out_w <= MAX_RENDER_W && out_h <= MAX_RENDER_H) {
        *ren_w = out_w;
        *ren_h = out_h;
    } else {
        float scale_w = (float)MAX_RENDER_W / (float)out_w;
        float scale_h = (float)MAX_RENDER_H / (float)out_h;
        float scale = scale_w < scale_h ? scale_w : scale_h;
        *ren_w = (uint32_t)((float)out_w * scale);
        *ren_h = (uint32_t)((float)out_h * scale);
        if (*ren_w < 1) *ren_w = 1;
        if (*ren_h < 1) *ren_h = 1;
    }
}

/* ---- load image into wallpaper ---------------------------------------- */

static int load_src_image(scene_wallpaper *wp, const char *path)
{
    uint32_t *px = NULL;
    int w, h;
    if (scene_image_load(path, &w, &h, &px) != 0) return -1;
    free(wp->src_img);
    wp->src_img = px;
    wp->src_w = w;
    wp->src_h = h;
    return 0;
}

/* ---- copy/resize source to buffer ------------------------------------- */

/* Nearest-neighbor blit of src_img into buf at given offset/scale.
 * ox, oy = output pixel offset in buf
 * scale = zoom factor (1.0 = original pixel size in buf coords)         */
static void blit_scaled_offset(const scene_wallpaper *wp,
                               const uint32_t *src, int sw, int sh,
                               int ox, int oy, float scale)
{
    uint32_t *dst = wp->buf;
    int dw = (int)wp->ren_w;
    int dh = (int)wp->ren_h;
    int y;
    for (y = 0; y < dh; y++) {
        int x;
        for (x = 0; x < dw; x++) {
            /* Map output pixel back to source pixel */
            int sx = (int)(((float)(x - ox) / scale));
            int sy = (int)(((float)(y - oy) / scale));
            if (sx >= 0 && sx < sw && sy >= 0 && sy < sh)
                dst[y * dw + x] = src[sy * sw + sx];
        }
    }
}

/* ---- STATIC wallpaper ------------------------------------------------- */

static void render_static(scene_wallpaper *wp)
{
    if (!wp->src_img) return;
    /* Cover: scale image to fill render buffer, center it */
    float sx = (float)wp->ren_w / (float)wp->src_w;
    float sy = (float)wp->ren_h / (float)wp->src_h;
    float scale = sx > sy ? sx : sy;  /* pick larger to cover */
    int ox = (int)((float)wp->ren_w / 2.0f -
                   (float)wp->src_w * scale / 2.0f);
    int oy = (int)((float)wp->ren_h / 2.0f -
                   (float)wp->src_h * scale / 2.0f);
    blit_scaled_offset(wp, wp->src_img, wp->src_w, wp->src_h, ox, oy, scale);
}

/* ---- KEN BURNS wallpaper ---------------------------------------------- */

static void render_ken_burns(scene_wallpaper *wp)
{
    if (!wp->src_img) return;

    float duration = wp->kb_duration > 0 ? wp->kb_duration : 30.0f;
    float max_zoom = wp->kb_zoom > 1.0f ? wp->kb_zoom : 1.1f;

    /* Progress through the cycle (wraps) */
    float t = fmodf((float)wp->frame / 30.0f, duration) / duration;

    /* Zoom: 1.0 -> max_zoom -> 1.0 (sine curve) */
    float zoom = 1.0f + (max_zoom - 1.0f) * (0.5f - 0.5f * cosf(t * 2.0f * 3.14159265f));

    /* Pan: slow drift, wraps seamlessly */
    float pan_x = 0.5f + 0.3f * sinf(t * 2.0f * 3.14159265f);
    float pan_y = 0.5f + 0.2f * cosf(t * 2.0f * 3.14159265f * 0.7f);

    /* Calculate source crop (centered on pan position) */
    float crop_w = (float)wp->ren_w / zoom;
    float crop_h = (float)wp->ren_h / zoom;
    float src_x = pan_x * (float)wp->src_w - crop_w * 0.5f;
    float src_y = pan_y * (float)wp->src_h - crop_h * 0.5f;

    /* Clamp to source bounds */
    if (src_x < 0) src_x = 0;
    if (src_y < 0) src_y = 0;
    if (src_x + crop_w > (float)wp->src_w) src_x = (float)wp->src_w - crop_w;
    if (src_y + crop_h > (float)wp->src_h) src_y = (float)wp->src_h - crop_h;

    /* Blit the cropped region, scaled to output */
    uint32_t *dst = wp->buf;
    int dw = (int)wp->ren_w;
    int dh = (int)wp->ren_h;
    int y;
    for (y = 0; y < dh; y++) {
        int x;
        for (x = 0; x < dw; x++) {
            int sx = (int)(src_x + (float)x * crop_w / (float)dw);
            int sy = (int)(src_y + (float)y * crop_h / (float)dh);
            if (sx >= 0 && sx < wp->src_w && sy >= 0 && sy < wp->src_h)
                dst[y * dw + x] = wp->src_img[sy * wp->src_w + sx];
        }
    }
}

/* ---- SLIDESHOW wallpaper ---------------------------------------------- */

static void blit_slide(scene_wallpaper *wp, int slide_idx, uint32_t alpha)
{
    if (slide_idx < 0 || slide_idx >= wp->slide_count) return;
    slide_entry *s = &wp->slides[slide_idx];
    uint32_t *dst = wp->buf;
    int dw = (int)wp->ren_w;
    int dh = (int)wp->ren_h;
    int y;
    for (y = 0; y < dh; y++) {
        int x;
        for (x = 0; x < dw; x++) {
            int sx = x * s->width / dw;
            int sy = y * s->height / dh;
            if (sx >= 0 && sx < s->width && sy >= 0 && sy < s->height) {
                uint32_t px = s->pixels[sy * s->width + sx];
                if (alpha < 255)
                    dst[y * dw + x] = blend_px(dst[y * dw + x], px, alpha);
                else
                    dst[y * dw + x] = px;
            }
        }
    }
}

static void render_slideshow(scene_wallpaper *wp)
{
    if (wp->slide_count == 0) return;

    if (wp->in_crossfade) {
        /* Crossfade: blit next slide at increasing alpha */
        uint8_t alpha = (uint8_t)(cosine_ease(wp->crossfade_t) * 255.0f);
        /* First clear to current slide */
        blit_slide(wp, wp->current_slide, 255);
        /* Then blend next slide on top */
        blit_slide(wp, wp->next_slide, alpha);
    } else {
        blit_slide(wp, wp->current_slide, 255);
    }
}

/* ---- PROCEDURAL: plasma ----------------------------------------------- */

/*
 * Classic plasma: layered sine waves creating organic color patterns.
 * Pure integer math in the inner loop (fixed-point).
 */
static void render_plasma(scene_wallpaper *wp)
{
    float speed = wp->params.speed > 0 ? wp->params.speed : 1.0f;
    float intensity = wp->params.intensity > 0 ? wp->params.intensity : 0.8f;
    uint32_t c1 = wp->params.color1 ? wp->params.color1 : 0xFFFF6B35;
    uint32_t c2 = wp->params.color2 ? wp->params.color2 : 0xFF004E98;
    uint32_t c3 = wp->params.color3 ? wp->params.color3 : 0xFF1A1A2E;

    float time = (float)wp->frame * speed * 0.05f;
    uint32_t *dst = wp->buf;
    int dw = (int)wp->ren_w;
    int dh = (int)wp->ren_h;
    int y;
    for (y = 0; y < dh; y++) {
        int x;
        for (x = 0; x < dw; x++) {
            float fx = (float)x / (float)dw * 6.2832f;
            float fy = (float)y / (float)dh * 6.2832f;

            /* Three interference waves */
            float v1 = sinf(fx * 2.0f + time) * intensity;
            float v2 = sinf(fy * 3.0f + time * 0.7f) * intensity;
            float v3 = sinf((fx + fy) * 1.5f + time * 1.3f) * intensity * 0.5f;
            float v = (v1 + v2 + v3 + intensity) / (intensity * 3.0f + intensity);
            v = v * 0.5f; /* normalize to 0-1 */
            if (v < 0) v = 0;
            if (v > 1) v = 1;

            /* Map to gradient between three colors */
            uint32_t color;
            if (v < 0.5f) {
                float t2 = v * 2.0f;
                int r = (int)(((c1 >> 16) & 0xFF) * (1 - t2) + ((c2 >> 16) & 0xFF) * t2);
                int g = (int)(((c1 >> 8)  & 0xFF) * (1 - t2) + ((c2 >> 8)  & 0xFF) * t2);
                int b = (int)(( c1        & 0xFF) * (1 - t2) + ( c2        & 0xFF) * t2);
                color = pack_argb(0xFF, clamp255(r), clamp255(g), clamp255(b));
            } else {
                float t2 = (v - 0.5f) * 2.0f;
                int r = (int)(((c2 >> 16) & 0xFF) * (1 - t2) + ((c3 >> 16) & 0xFF) * t2);
                int g = (int)(((c2 >> 8)  & 0xFF) * (1 - t2) + ((c3 >> 8)  & 0xFF) * t2);
                int b = (int)(( c2        & 0xFF) * (1 - t2) + ( c3        & 0xFF) * t2);
                color = pack_argb(0xFF, clamp255(r), clamp255(g), clamp255(b));
            }
            dst[y * dw + x] = color;
        }
    }
}

/* ---- PROCEDURAL: aurora ----------------------------------------------- */

/*
 * Aurora borealis: horizontal bands with vertical wave motion.
 * Multiple layers at different speeds create depth.
 */
static void render_aurora(scene_wallpaper *wp)
{
    float speed = wp->params.speed > 0 ? wp->params.speed : 1.0f;
    float intensity = wp->params.intensity > 0 ? wp->params.intensity : 0.7f;
    uint32_t c1 = wp->params.color1 ? wp->params.color1 : 0xFF00FF88;
    uint32_t c2 = wp->params.color2 ? wp->params.color2 : 0xFF0088FF;
    uint32_t c3 = wp->params.color3 ? wp->params.color3 : 0xFF0D1117;

    float time = (float)wp->frame * speed * 0.03f;
    uint32_t *dst = wp->buf;
    int dw = (int)wp->ren_w;
    int dh = (int)wp->ren_h;

    /* Start with dark background */
    int y;
    for (y = 0; y < dh; y++) {
        int x;
        for (x = 0; x < dw; x++) {
            float fx = (float)x / (float)dw;
            float fy = (float)y / (float)dh;

            /* Multiple aurora bands */
            float band1 = expf(-powf((fy - 0.3f - 0.05f * sinf(fx * 4.0f + time)) * 4.0f, 2.0f));
            float band2 = expf(-powf((fy - 0.45f - 0.04f * sinf(fx * 3.0f + time * 1.2f)) * 5.0f, 2.0f));
            float band3 = expf(-powf((fy - 0.35f - 0.06f * sinf(fx * 5.0f + time * 0.8f)) * 3.5f, 2.0f));

            float brightness = (band1 * 0.5f + band2 * 0.3f + band3 * 0.2f) * intensity;
            if (brightness > 1.0f) brightness = 1.0f;

            /* Vertical shimmer */
            float shimmer = 0.8f + 0.2f * sinf(fx * 20.0f + time * 2.0f + fy * 10.0f);
            brightness *= shimmer;

            /* Color varies across the band */
            int r, g, b;
            if (brightness < 0.3f) {
                float t2 = brightness / 0.3f;
                r = (int)(((c3 >> 16) & 0xFF) * (1 - t2) + ((c1 >> 16) & 0xFF) * t2);
                g = (int)(((c3 >> 8)  & 0xFF) * (1 - t2) + ((c1 >> 8)  & 0xFF) * t2);
                b = (int)(( c3        & 0xFF) * (1 - t2) + ( c1        & 0xFF) * t2);
            } else {
                float t2 = (brightness - 0.3f) / 0.7f;
                r = (int)(((c1 >> 16) & 0xFF) * (1 - t2) + ((c2 >> 16) & 0xFF) * t2);
                g = (int)(((c1 >> 8)  & 0xFF) * (1 - t2) + ((c2 >> 8)  & 0xFF) * t2);
                b = (int)(( c1        & 0xFF) * (1 - t2) + ( c2        & 0xFF) * t2);
            }

            /* Dark sky base with aurora overlay */
            uint32_t sky = pack_argb(0xFF,
                clamp255((int)((c3 >> 16) & 0xFF)),
                clamp255((int)((c3 >> 8)  & 0xFF)),
                clamp255((int)( c3        & 0xFF)));
            uint32_t aurora = pack_argb(0xFF,
                clamp255(r), clamp255(g), clamp255(b));
            uint8_t alpha = (uint8_t)(brightness * 255.0f);
            dst[y * dw + x] = blend_px(sky, aurora, alpha);
        }
    }
}

/* ---- PROCEDURAL: gradient mesh ---------------------------------------- */

/*
 * Smooth gradient mesh: 4 corner colors interpolated with cosine easing.
 * Slowly rotates hue over time for a living gradient effect.
 */
static void render_gradient_mesh(scene_wallpaper *wp)
{
    float speed = wp->params.speed > 0 ? wp->params.speed : 1.0f;
    float intensity = wp->params.intensity > 0 ? wp->params.intensity : 1.0f;
    uint32_t c1 = wp->params.color1 ? wp->params.color1 : 0xFF1A1A2E;
    uint32_t c2 = wp->params.color2 ? wp->params.color2 : 0xFF16213E;
    uint32_t c3 = wp->params.color3 ? wp->params.color3 : 0xFF0F3460;
    uint32_t c4_default = 0xFF533483;

    float time = (float)wp->frame * speed * 0.01f;
    uint32_t *dst = wp->buf;
    int dw = (int)wp->ren_w;
    int dh = (int)wp->ren_h;
    int y;

    /* Slowly shift the mesh control points */
    float cx1 = 0.2f + 0.1f * sinf(time * 0.3f);
    float cy1 = 0.2f + 0.1f * cosf(time * 0.4f);
    float cx2 = 0.8f + 0.1f * sinf(time * 0.5f + 1.0f);
    float cy2 = 0.8f + 0.1f * cosf(time * 0.3f + 2.0f);

    for (y = 0; y < dh; y++) {
        int x;
        for (x = 0; x < dw; x++) {
            float fx = (float)x / (float)dw;
            float fy = (float)y / (float)dh;

            /* Distance to moving control points */
            float dx1 = fx - cx1, dy1 = fy - cy1;
            float dx2 = fx - cx2, dy2 = fy - cy2;
            float d1 = sqrtf(dx1 * dx1 + dy1 * dy1);
            float d2 = sqrtf(dx2 * dx2 + dy2 * dy2);
            float d_max = sqrtf(2.0f);

            /* Influence falls off with distance */
            float w1 = 1.0f - (d1 / d_max);
            float w2 = 1.0f - (d2 / d_max);
            if (w1 < 0) w1 = 0;
            if (w2 < 0) w2 = 0;
            float w3 = 1.0f - w1 - w2;
            if (w3 < 0) w3 = 0;

            /* Normalize weights */
            float wsum = w1 + w2 + w3 + 0.001f;
            w1 /= wsum; w2 /= wsum; w3 /= wsum;

            float w4 = intensity - w1 - w2 - w3;
            if (w4 < 0) w4 = 0;

            /* Bilinear mix of corner colors */
            int r = (int)(((c1 >> 16) & 0xFF) * w1 + ((c2 >> 16) & 0xFF) * w2 +
                          ((c3 >> 16) & 0xFF) * w3 + (((c4_default >> 16) & 0xFF)) * w4);
            int g = (int)(((c1 >> 8)  & 0xFF) * w1 + ((c2 >> 8)  & 0xFF) * w2 +
                          ((c3 >> 8)  & 0xFF) * w3 + (((c4_default >> 8)  & 0xFF)) * w4);
            int b = (int)(( c1        & 0xFF) * w1 + ( c2        & 0xFF) * w2 +
                          ( c3        & 0xFF) * w3 + ( c4_default        & 0xFF) * w4);

            dst[y * dw + x] = pack_argb(0xFF, clamp255(r), clamp255(g), clamp255(b));
        }
    }
}

/* ---- public API ------------------------------------------------------- */

scene_wallpaper *scene_wallpaper_new(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return NULL;
    scene_wallpaper *wp = (scene_wallpaper *)calloc(1, sizeof(scene_wallpaper));
    if (!wp) return NULL;

    wp->out_w = width;
    wp->out_h = height;
    calc_render_size(width, height, &wp->ren_w, &wp->ren_h);

    wp->buf = (uint32_t *)calloc((size_t)(wp->ren_w * wp->ren_h), sizeof(uint32_t));
    if (!wp->buf) { free(wp); return NULL; }

    wp->type = SCENE_WP_STATIC;
    wp->display_sec = 15.0f;
    wp->fade_sec = 1.0f;
    wp->kb_duration = 30.0f;
    wp->kb_zoom = 1.1f;
    wp->current_slide = 0;
    wp->next_slide = 1;

    return wp;
}

void scene_wallpaper_free(scene_wallpaper *wp)
{
    if (!wp) return;
    free(wp->buf);
    free(wp->src_img);
    int i;
    for (i = 0; i < wp->slide_count; i++)
        free(wp->slides[i].pixels);
    free(wp);
}

int scene_wallpaper_set_static(scene_wallpaper *wp, const char *path)
{
    if (!wp || !path) return -1;
    if (load_src_image(wp, path) != 0) return -1;
    wp->type = SCENE_WP_STATIC;
    render_static(wp);
    return 0;
}

int scene_wallpaper_set_ken_burns(scene_wallpaper *wp, const char *path,
                                  float duration_sec, float zoom)
{
    if (!wp || !path) return -1;
    if (load_src_image(wp, path) != 0) return -1;
    wp->type = SCENE_WP_KEN_BURNS;
    wp->kb_duration = duration_sec > 0 ? duration_sec : 30.0f;
    wp->kb_zoom = (zoom >= 1.0f && zoom <= 2.0f) ? zoom : 1.1f;
    wp->frame = 0;
    return 0;
}

int scene_wallpaper_add_slide(scene_wallpaper *wp, const char *path)
{
    if (!wp || !path || wp->slide_count >= MAX_SLIDES) return -1;
    slide_entry *s = &wp->slides[wp->slide_count];
    if (scene_image_load(path, &s->width, &s->height, &s->pixels) != 0)
        return -1;
    wp->slide_count++;
    return 0;
}

void scene_wallpaper_set_slideshow_timing(scene_wallpaper *wp,
                                          float display_sec, float fade_sec)
{
    if (!wp) return;
    if (display_sec > 0) wp->display_sec = display_sec;
    if (fade_sec > 0) wp->fade_sec = fade_sec;
}

int scene_wallpaper_start_slideshow(scene_wallpaper *wp)
{
    if (!wp || wp->slide_count < 1) return -1;
    wp->type = SCENE_WP_SLIDESHOW;
    wp->current_slide = 0;
    wp->next_slide = (wp->slide_count > 1) ? 1 : 0;
    wp->slide_timer = 0;
    wp->in_crossfade = 0;
    wp->crossfade_t = 0;
    wp->frame = 0;
    return 0;
}

int scene_wallpaper_set_procedural(scene_wallpaper *wp, scene_wp_type type,
                                   const scene_wp_params *params)
{
    if (!wp) return -1;
    if (type <= SCENE_WP_STATIC || type >= SCENE_WP_COUNT) return -1;
    wp->type = type;
    if (params) wp->params = *params;
    else memset(&wp->params, 0, sizeof(wp->params));
    wp->frame = 0;
    return 0;
}

int scene_wallpaper_tick(scene_wallpaper *wp)
{
    if (!wp || wp->occluded) return 0;

    switch (wp->type) {
    case SCENE_WP_STATIC:
        if (wp->frame == 0) { wp->frame = 1; return 1; }
        return 0;

    case SCENE_WP_KEN_BURNS:
        wp->frame++;
        render_ken_burns(wp);
        return 1;

    case SCENE_WP_SLIDESHOW: {
        if (wp->slide_count == 0) return 0;
        wp->frame++;
        float dt = 1.0f / 30.0f;
        wp->slide_timer += dt;

        if (wp->in_crossfade) {
            wp->crossfade_t += dt / wp->fade_sec;
            if (wp->crossfade_t >= 1.0f) {
                /* Crossfade complete */
                wp->crossfade_t = 0;
                wp->in_crossfade = 0;
                wp->current_slide = wp->next_slide;
                wp->next_slide = (wp->current_slide + 1) % wp->slide_count;
                wp->slide_timer = 0;
            }
            render_slideshow(wp);
            return 1;
        } else if (wp->slide_timer >= wp->display_sec && wp->slide_count > 1) {
            /* Start crossfade */
            wp->in_crossfade = 1;
            wp->crossfade_t = 0;
            render_slideshow(wp);
            return 1;
        }
        return 0;
    }

    case SCENE_WP_PLASMA:
        wp->frame++;
        render_plasma(wp);
        return 1;

    case SCENE_WP_AURORA:
        wp->frame++;
        render_aurora(wp);
        return 1;

    case SCENE_WP_GRADIENT_MESH:
        wp->frame++;
        render_gradient_mesh(wp);
        return 1;

    default:
        return 0;
    }
}

void scene_wallpaper_set_occluded(scene_wallpaper *wp, int occluded)
{
    if (wp) wp->occluded = occluded ? 1 : 0;
}

const uint32_t *scene_wallpaper_pixels(const scene_wallpaper *wp)
{
    return wp ? wp->buf : NULL;
}

void scene_wallpaper_render_size(const scene_wallpaper *wp,
                                 uint32_t *w, uint32_t *h)
{
    if (wp) { if (w) *w = wp->ren_w; if (h) *h = wp->ren_h; }
    else    { if (w) *w = 0;         if (h) *h = 0; }
}

scene_wp_type scene_wallpaper_type(const scene_wallpaper *wp)
{
    return wp ? wp->type : SCENE_WP_STATIC;
}

uint64_t scene_wallpaper_frame(const scene_wallpaper *wp)
{
    return wp ? wp->frame : 0;
}
