/*
 * scene_wallpaper.h — lightweight wallpaper system for the compositor.
 *
 * Supports static images, Ken Burns pan/zoom, image slideshows with
 * crossfade, and procedural effects (plasma, aurora, gradient mesh).
 * All rendering is pure math on ARGB framebuffers — no GPU required.
 *
 * Key design decisions (from research):
 *   - 30fps is enough for ambient motion (not interactive content)
 *   - Cosine easing for crossfade (linear looks mechanical)
 *   - Pause when occluded (saves 90%+ when maximized windows cover bg)
 *   - Render at 1080p, upscale to 4K (indistinguishable for backgrounds)
 *   - Procedural = resolution-independent, zero disk memory
 *
 * Usage:
 *   wp = scene_wallpaper_new(width, height);
 *   scene_wallpaper_set_static(wp, "wallpaper.bmp");
 *   loop {
 *     scene_wallpaper_tick(wp);
 *     // copy wp->pixels to bg texture if tick returned 1
 *   }
 *   scene_wallpaper_free(wp);
 */
#ifndef SCENE_WALLPAPER_H
#define SCENE_WALLPAPER_H

#include <stdint.h>

typedef struct scene_wallpaper scene_wallpaper;

/* ---- types ------------------------------------------------------------ */

typedef enum scene_wp_type {
    SCENE_WP_STATIC,        /* single image, no animation                */
    SCENE_WP_KEN_BURNS,     /* slow pan/zoom on static image             */
    SCENE_WP_SLIDESHOW,     /* multiple images with crossfade            */
    SCENE_WP_PLASMA,        /* procedural sine-wave plasma               */
    SCENE_WP_AURORA,        /* procedural northern lights effect         */
    SCENE_WP_GRADIENT_MESH, /* procedural smooth gradient                */
    SCENE_WP_COUNT
} scene_wp_type;

/* Procedural effect parameters (NULL = use defaults) */
typedef struct scene_wp_params {
    float    speed;         /* animation speed multiplier (1.0 = normal) */
    float    intensity;     /* effect intensity (0.0-1.0, 1.0 = full)    */
    uint32_t color1;        /* primary color ARGB (0 = use default)       */
    uint32_t color2;        /* secondary color ARGB (0 = use default)     */
    uint32_t color3;        /* tertiary color ARGB (0 = use default)      */
} scene_wp_params;

/* ---- lifecycle -------------------------------------------------------- */

/* Create a wallpaper manager for the given output dimensions.
 * Internally renders at min(width, 1920) x min(height, 1080) for
 * performance, then the caller scales up to full resolution.            */
scene_wallpaper *scene_wallpaper_new(uint32_t width, uint32_t height);

/* Free the wallpaper manager and all internal buffers.                   */
void scene_wallpaper_free(scene_wallpaper *wp);

/* ---- static wallpaper ------------------------------------------------- */

/* Set a single static image as wallpaper. Loads BMP/TGA via scene_image.
 * Returns 0 on success, -1 if image load fails.                         */
int scene_wallpaper_set_static(scene_wallpaper *wp, const char *path);

/* ---- Ken Burns (pan/zoom on static image) ----------------------------- */

/* Set Ken Burns mode with the given image.
 * duration_sec = seconds for one full pan cycle (default 30).
 * zoom = max zoom factor (1.0-1.5, default 1.1 = 10% zoom).            */
int scene_wallpaper_set_ken_burns(scene_wallpaper *wp, const char *path,
                                  float duration_sec, float zoom);

/* ---- slideshow -------------------------------------------------------- */

/* Add an image to the slideshow playlist.
 * Returns 0 on success, -1 if image load fails or playlist full.        */
int scene_wallpaper_add_slide(scene_wallpaper *wp, const char *path);

/* Configure slideshow timing.
 * display_sec = seconds each slide is shown (default 15).
 * fade_sec = seconds for crossfade transition (default 1).              */
void scene_wallpaper_set_slideshow_timing(scene_wallpaper *wp,
                                          float display_sec, float fade_sec);

/* Start the slideshow (must add slides first).                          */
int scene_wallpaper_start_slideshow(scene_wallpaper *wp);

/* ---- procedural effects ----------------------------------------------- */

/* Set a procedural wallpaper effect.
 * params = optional parameters (NULL = defaults).                        */
int scene_wallpaper_set_procedural(scene_wallpaper *wp, scene_wp_type type,
                                   const scene_wp_params *params);

/* ---- per-frame update ------------------------------------------------- */

/* Advance the wallpaper animation by one tick. Call once per compositor
 * frame (at 30fps or display refresh rate).
 * Returns 1 if the wallpaper pixels changed (caller should re-upload
 * texture and damage the background rect), 0 if no change.              */
int scene_wallpaper_tick(scene_wallpaper *wp);

/* ---- auto-pause ------------------------------------------------------- */

/* Notify the wallpaper whether it is fully occluded by windows.
 * When occluded, animation stops (zero CPU cost).
 * occluded = 1 means fully hidden, 0 means visible.                     */
void scene_wallpaper_set_occluded(scene_wallpaper *wp, int occluded);

/* ---- pixel access ----------------------------------------------------- */

/* Get the current wallpaper pixel buffer (ARGB, top-left origin).
 * Size is render_width * render_height * 4 bytes.
 * The caller should upload this to a compositor texture.                */
const uint32_t *scene_wallpaper_pixels(const scene_wallpaper *wp);

/* Get the render dimensions (may be smaller than output for perf).       */
void scene_wallpaper_render_size(const scene_wallpaper *wp,
                                 uint32_t *w, uint32_t *h);

/* ---- info ------------------------------------------------------------- */

/* Get the current wallpaper type.                                        */
scene_wp_type scene_wallpaper_type(const scene_wallpaper *wp);

/* Get the current frame number (increments per tick).                    */
uint64_t scene_wallpaper_frame(const scene_wallpaper *wp);

#endif /* SCENE_WALLPAPER_H */
