/*
 * scene_image.h — image file loader (BMP, TGA).
 *
 * Loads image files and returns ARGB pixel data suitable for compositor
 * texture registration. Used for wallpaper and custom textures.
 *
 * Usage:
 *   uint32_t *pixels;
 *   int w, h;
 *   if (scene_image_load("wallpaper.bmp", &w, &h, &pixels) == 0) {
 *       scene_compositor_register_texture(cp, ref, w, h, SCENE_TEX_ARGB, 0, pixels);
 *       scene_image_free(pixels);
 *   }
 */
#ifndef SCENE_IMAGE_H
#define SCENE_IMAGE_H

#include <stdint.h>

/* Load an image file (BMP or TGA). Returns 0 on success.
 * pixels = ARGB uint32_t array, w*h, top-left origin, 4 bytes per pixel.
 * Caller must free with scene_image_free().                                */
int scene_image_load(const char *path, int *w, int *h, uint32_t **pixels);

/* Free pixel data returned by scene_image_load.                           */
void scene_image_free(uint32_t *pixels);

/* Get the last error message (static buffer, overwritten on next call).   */
const char *scene_image_error(void);

#endif /* SCENE_IMAGE_H */
