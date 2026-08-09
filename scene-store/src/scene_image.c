/*
 * scene_image.c — image file loader (BMP, TGA).
 *
 * BMP: 24-bit or 32-bit uncompressed, bottom-up or top-down.
 * TGA: 24-bit or 32-bit, uncompressed or RLE, top-left or bottom-left.
 */
#include "scene_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char err_buf[256];

const char *scene_image_error(void)
{
    return err_buf;
}

void scene_image_free(uint32_t *pixels)
{
    free(pixels);
}

/* ---- BMP loader ------------------------------------------------------- */

static int load_bmp(const char *path, int *out_w, int *out_h, uint32_t **out_px)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err_buf, sizeof(err_buf), "BMP: cannot open %s", path); return -1; }

    /* Read file header (14 bytes) */
    uint8_t fh[14];
    if (fread(fh, 1, 14, f) != 14) { fclose(f); snprintf(err_buf, sizeof(err_buf), "BMP: truncated file header"); return -1; }
    if (fh[0] != 'B' || fh[1] != 'M') { fclose(f); snprintf(err_buf, sizeof(err_buf), "BMP: not a BMP file"); return -1; }

    uint32_t data_offset = *(uint32_t *)&fh[10];

    /* Read info header (40 bytes minimum) */
    uint8_t ih[40];
    if (fread(ih, 1, 40, f) != 40) { fclose(f); snprintf(err_buf, sizeof(err_buf), "BMP: truncated info header"); return -1; }

    int32_t w = *(int32_t *)&ih[4];
    int32_t h_raw = *(int32_t *)&ih[8];
    int bottom_up = (h_raw > 0);
    int32_t abs_h = h_raw < 0 ? -h_raw : h_raw;
    uint16_t bpp = *(uint16_t *)&ih[14];
    uint32_t compression = *(uint32_t *)&ih[16];

    if (compression != 0) { fclose(f); snprintf(err_buf, sizeof(err_buf), "BMP: compressed BMP not supported"); return -1; }
    if (bpp != 24 && bpp != 32) { fclose(f); snprintf(err_buf, sizeof(err_buf), "BMP: only 24/32 bpp supported"); return -1; }
    if (w <= 0 || abs_h <= 0) { fclose(f); snprintf(err_buf, sizeof(err_buf), "BMP: invalid dimensions"); return -1; }

    /* Seek to pixel data */
    fseek(f, (long)data_offset, SEEK_SET);

    int bytes_per_pixel = bpp / 8;
    int row_stride = (w * bytes_per_pixel + 3) & ~3; /* rows are padded to 4 bytes */

    uint32_t *pixels = (uint32_t *)calloc((size_t)(w * abs_h), sizeof(uint32_t));
    if (!pixels) { fclose(f); snprintf(err_buf, sizeof(err_buf), "BMP: out of memory"); return -1; }

    int32_t row;
    for (row = 0; row < abs_h; row++) {
        uint8_t row_buf[4096];
        if ((int)(sizeof(row_buf)) < w * bytes_per_pixel) {
            /* Shouldn't happen for reasonable images, but just in case */
            uint8_t *tmp = (uint8_t *)malloc((size_t)row_stride);
            if (tmp) {
                if (fread(tmp, 1, (size_t)row_stride, f) != (size_t)row_stride) {
                    free(tmp); free(pixels); fclose(f);
                    snprintf(err_buf, sizeof(err_buf), "BMP: read error"); return -1;
                }
                int32_t col;
                for (col = 0; col < w; col++) {
            int dst_row = bottom_up ? (abs_h - 1 - row) : row;
                    uint8_t b = tmp[col * bytes_per_pixel + 0];
                    uint8_t g = tmp[col * bytes_per_pixel + 1];
                    uint8_t r = tmp[col * bytes_per_pixel + 2];
                    uint8_t a = (bytes_per_pixel == 4) ? tmp[col * bytes_per_pixel + 3] : 0xFF;
                    pixels[dst_row * w + col] = (uint32_t)((a << 24) | (r << 16) | (g << 8) | b);
                }
                free(tmp);
                continue;
            }
        }
        if (fread(row_buf, 1, (size_t)(w * bytes_per_pixel), f) != (size_t)(w * bytes_per_pixel)) {
            free(pixels); fclose(f);
            snprintf(err_buf, sizeof(err_buf), "BMP: read error"); return -1;
        }
        /* Skip padding */
        fseek(f, (long)(row_stride - w * bytes_per_pixel), SEEK_CUR);

        int32_t col;
        for (col = 0; col < w; col++) {
            int dst_row = bottom_up ? (abs_h - 1 - row) : row;
            uint8_t b = row_buf[col * bytes_per_pixel + 0];
            uint8_t g = row_buf[col * bytes_per_pixel + 1];
            uint8_t r = row_buf[col * bytes_per_pixel + 2];
            uint8_t a = (bytes_per_pixel == 4) ? row_buf[col * bytes_per_pixel + 3] : 0xFF;
            pixels[dst_row * w + col] = (uint32_t)((a << 24) | (r << 16) | (g << 8) | b);
        }
    }

    fclose(f);
    *out_w = w;
    *out_h = abs_h;
    *out_px = pixels;
    return 0;
}

/* ---- TGA loader ------------------------------------------------------- */

static int load_tga(const char *path, int *out_w, int *out_h, uint32_t **out_px)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err_buf, sizeof(err_buf), "TGA: cannot open %s", path); return -1; }

    /* TGA header: 18 bytes */
    uint8_t th[18];
    if (fread(th, 1, 18, f) != 18) { fclose(f); snprintf(err_buf, sizeof(err_buf), "TGA: truncated header"); return -1; }

    uint8_t id_len = th[0];
    uint8_t color_map_type = th[1];
    uint8_t image_type = th[2]; /* 0=none, 1=RLE, 2=raw, 10=RLE+raw */
    uint16_t cm_first = *(uint16_t *)&th[3];
    uint16_t cm_entries = *(uint16_t *)&th[5];
    uint8_t cm_bpp = th[7];
    (void)cm_first; (void)cm_entries; (void)cm_bpp;

    uint16_t x_origin = *(uint16_t *)&th[8];
    uint16_t y_origin = *(uint16_t *)&th[10];
    uint16_t w = *(uint16_t *)&th[12];
    uint16_t h = *(uint16_t *)&th[14];
    uint8_t bpp = th[16];
    uint8_t descriptor = th[17];
    int top_origin = (descriptor & 0x20) != 0;

    (void)x_origin; (void)y_origin;

    if (color_map_type != 0) { fclose(f); snprintf(err_buf, sizeof(err_buf), "TGA: color maps not supported"); return -1; }
    if (image_type != 2 && image_type != 10) { fclose(f); snprintf(err_buf, sizeof(err_buf), "TGA: only type 2/10 supported"); return -1; }
    if (bpp != 24 && bpp != 32) { fclose(f); snprintf(err_buf, sizeof(err_buf), "TGA: only 24/32 bpp supported"); return -1; }
    if (w == 0 || h == 0) { fclose(f); snprintf(err_buf, sizeof(err_buf), "TGA: invalid dimensions"); return -1; }

    /* Skip image ID */
    if (id_len > 0) fseek(f, (long)id_len, SEEK_CUR);

    int bytes_per_pixel = bpp / 8;
    int total = w * h;
    uint32_t *pixels = (uint32_t *)calloc((size_t)total, sizeof(uint32_t));
    if (!pixels) { fclose(f); snprintf(err_buf, sizeof(err_buf), "TGA: out of memory"); return -1; }

    if (image_type == 2) {
        /* Raw */
        int i;
        for (i = 0; i < total; i++) {
            uint8_t px[4];
            if (fread(px, 1, (size_t)bytes_per_pixel, f) != (size_t)bytes_per_pixel) {
                free(pixels); fclose(f);
                snprintf(err_buf, sizeof(err_buf), "TGA: read error"); return -1;
            }
            uint8_t b = px[0], g = px[1], r = px[2];
            uint8_t a = (bytes_per_pixel == 4) ? px[3] : 0xFF;
            pixels[i] = (uint32_t)((a << 24) | (r << 16) | (g << 8) | b);
        }
    } else {
        /* RLE */
        int i = 0;
        while (i < total) {
            uint8_t header;
            if (fread(&header, 1, 1, f) != 1) {
                free(pixels); fclose(f);
                snprintf(err_buf, sizeof(err_buf), "TGA: RLE read error"); return -1;
            }
            int count = (header & 0x7F) + 1;
            if (header & 0x80) {
                /* Run-length packet */
                uint8_t px[4];
                if (fread(px, 1, (size_t)bytes_per_pixel, f) != (size_t)bytes_per_pixel) {
                    free(pixels); fclose(f);
                    snprintf(err_buf, sizeof(err_buf), "TGA: RLE read error"); return -1;
                }
                uint8_t b = px[0], g = px[1], r = px[2];
                uint8_t a = (bytes_per_pixel == 4) ? px[3] : 0xFF;
                uint32_t c = (uint32_t)((a << 24) | (r << 16) | (g << 8) | b);
                int j;
                for (j = 0; j < count && i < total; j++, i++)
                    pixels[i] = c;
            } else {
                /* Raw packet */
                int j;
                for (j = 0; j < count && i < total; j++, i++) {
                    uint8_t px[4];
                    if (fread(px, 1, (size_t)bytes_per_pixel, f) != (size_t)bytes_per_pixel) {
                        free(pixels); fclose(f);
                        snprintf(err_buf, sizeof(err_buf), "TGA: read error"); return -1;
                    }
                    uint8_t b = px[0], g = px[1], r = px[2];
                    uint8_t a = (bytes_per_pixel == 4) ? px[3] : 0xFF;
                    pixels[i] = (uint32_t)((a << 24) | (r << 16) | (g << 8) | b);
                }
            }
        }
    }

    fclose(f);

    /* Flip if bottom-origin (default TGA orientation) */
    if (!top_origin) {
        int32_t row;
        for (row = 0; row < h / 2; row++) {
            uint32_t *a = &pixels[row * w];
            uint32_t *b = &pixels[(h - 1 - row) * w];
            int32_t col;
            for (col = 0; col < w; col++) {
                uint32_t tmp = a[col];
                a[col] = b[col];
                b[col] = tmp;
            }
        }
    }

    *out_w = w;
    *out_h = h;
    *out_px = pixels;
    return 0;
}

/* ---- dispatch --------------------------------------------------------- */

int scene_image_load(const char *path, int *w, int *h, uint32_t **pixels)
{
    if (!path || !w || !h || !pixels) return -1;
    *w = 0; *h = 0; *pixels = NULL;

    /* Detect by extension */
    size_t len = strlen(path);
    if (len >= 4 && strcasecmp(&path[len - 4], ".bmp") == 0)
        return load_bmp(path, w, h, pixels);
    if (len >= 4 && strcasecmp(&path[len - 4], ".tga") == 0)
        return load_tga(path, w, h, pixels);

    /* Try BMP magic first, then TGA */
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err_buf, sizeof(err_buf), "cannot open %s", path); return -1; }
    uint8_t magic[2];
    int ok = (fread(magic, 1, 2, f) == 2);
    fclose(f);
    if (!ok) { snprintf(err_buf, sizeof(err_buf), "cannot read %s", path); return -1; }

    if (magic[0] == 'B' && magic[1] == 'M')
        return load_bmp(path, w, h, pixels);
    /* TGA has no reliable magic; try it */
    return load_tga(path, w, h, pixels);
}
