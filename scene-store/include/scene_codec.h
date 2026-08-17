/*
 * scene_codec.h — OS-side media decoder seam (the video half of the
 * honest boundary; scene_image is the still half).
 *
 * The v0 wire carries only texture REFERENCES; pixels are decoded
 * HERE, in the OS. This module wraps the vendored ffmpeg static build
 * (third_party/ffmpeg — see its LICENSE.md for the locked configure
 * and the written adoption justification) behind a deterministic,
 * single-threaded, pull-style API: mpeg1video/mpeg2video decode
 * (MPEG-PS or raw ES, auto-detected by the demuxer) plus YUV->XRGB
 * conversion. The same input file yields the same frames, byte for
 * byte, on every decode pass and every build — the compositor's
 * texture importer and the replay story both depend on that.
 */
#ifndef SCENE_CODEC_H_INCLUDED
#define SCENE_CODEC_H_INCLUDED

#include <stdint.h>

typedef struct scene_codec scene_codec;

/* Open a media file for decode. On failure returns NULL. All output
 * parameters are optional (NULL allowed); on success:
 *   out_w, out_h   - decoded frame size in pixels (the fixture is
 *                    240x128; other sizes are accepted as-is)
 *   out_fps        - the stream's nominal frame rate, rounded
 *   out_frames     - frames decoded so far (0 until the first pass
 *                    reaches end of stream; see scene_codec_frames) */
scene_codec *scene_codec_open(const char *path,
                              uint32_t *out_w, uint32_t *out_h,
                              unsigned *out_fps, unsigned *out_frames);

/* Decode the next frame into px. The caller must provide space for
 * w*h uint32 values (from scene_codec_open). Output is XRGB, opaque:
 * each pixel is 0xFFRRGGBB (byte order B,G,R,A on little-endian).
 * Returns 1 on success, 0 at end of stream, -1 on a decode error. */
int scene_codec_frame(scene_codec *c, uint32_t *px);

/* Frames decoded so far by the current session; after the first
 * end-of-stream this is the clip's total frame count. */
unsigned scene_codec_frames(scene_codec *c);

void scene_codec_close(scene_codec *c);

#endif /* SCENE_CODEC_H_INCLUDED */
