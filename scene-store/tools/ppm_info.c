/* Read PPM, sample key pixels */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ppm_info file.ppm\n"); return 1; }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    char magic[3] = {0};
    uint32_t w, h, maxval;
    fscanf(fp, "%2s", magic);
    fscanf(fp, " %u %u %u", &w, &h, &maxval);
    fgetc(fp); /* consume newline */
    fprintf(stderr, "PPM %s %ux%u max=%u\n", magic, w, h, maxval);
    uint8_t *px = (uint8_t *)malloc(w * h * 3);
    fread(px, 3, w * h, fp);
    fclose(fp);

    /* Sample pixels at key positions */
    int samples[][2] = {
        {512, 100},   /* center-top: should be desktop bg */
        {512, 750},   /* center-bottom: should be panel */
        {16, 750},    /* left-bottom: should be start button */
        {960, 750},   /* right-bottom: should be clock area */
        {512, 384},   /* dead center */
        {512, 40},    /* top area: panel? */
    };
    const char *labels[] = {
        "center-top(512,100)", "center-bottom(512,750)", "left-btn(16,750)",
        "right-clock(960,750)", "dead-center(512,384)", "top(512,40)"
    };
    int nsamples = sizeof(samples)/sizeof(samples[0]);
    int i;
    for (i = 0; i < nsamples; i++) {
        int x = samples[i][0], y = samples[i][1];
        if (x >= (int)w || y >= (int)h) continue;
        uint8_t *p = px + (y * w + x) * 3;
        fprintf(stderr, "  %s: R=%3u G=%3u B=%3u  (#%02X%02X%02X)\n",
                labels[i], p[0], p[1], p[2], p[0], p[1], p[2]);
    }

    free(px);
    return 0;
}
