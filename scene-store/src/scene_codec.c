/*
 * scene_codec.c — deterministic media decode at the OS seam.
 *
 * libav wrap: avformat_open_input (auto-detects PS vs raw ES),
 * avcodec send/receive, sws YUV->BGRA. BGRA in memory is exactly the
 * compositor's XRGB32 convention on little-endian (byte order
 * B,G,R,A == uint32 0xAARRGGBB), so decoded texels feed
 * scene_compositor_register_texture_layer without a repack.
 *
 * Determinism: single thread (thread_count = 1), quiet log level, no
 * timing input anywhere; avcodec_send_packet/receive_frame is
 * deterministic for a fixed byte stream. The fixture clip decodes
 * 100/100 frames byte-identical across passes (test_codec.c).
 */
#include "scene_codec.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct scene_codec {
    AVFormatContext *fmt;
    AVCodecContext  *dec;
    struct SwsContext *sws;
    const AVCodec   *codec;
    int              stream_idx;
    int              eof;
    unsigned         frames;
    unsigned         w, h;
    AVPacket        *pkt;
    AVFrame         *frame;
    unsigned char   *rgb;
};

scene_codec *scene_codec_open(const char *path,
                              uint32_t *out_w, uint32_t *out_h,
                              unsigned *out_fps, unsigned *out_frames)
{
    scene_codec *c;

    c = (scene_codec *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    av_log_set_level(AV_LOG_QUIET);

    c->stream_idx = -1;
    if (avformat_open_input(&c->fmt, path, NULL, NULL) < 0) goto fail;
    if (avformat_find_stream_info(c->fmt, NULL) < 0) goto fail;
    c->stream_idx = av_find_best_stream(c->fmt, AVMEDIA_TYPE_VIDEO, -1,
                                        -1, &c->codec, 0);
    if (c->stream_idx < 0 || !c->codec) goto fail;
    c->dec = avcodec_alloc_context3(c->codec);
    if (!c->dec) goto fail;
    if (avcodec_parameters_to_context(c->dec,
            c->fmt->streams[c->stream_idx]->codecpar) < 0) goto fail;
    c->dec->thread_count = 1;            /* deterministic decode */
    if (avcodec_open2(c->dec, c->codec, NULL) < 0) goto fail;

    c->w = (unsigned)c->dec->width;
    c->h = (unsigned)c->dec->height;
    if (!c->w || !c->h) goto fail;
    c->pkt = av_packet_alloc();
    c->frame = av_frame_alloc();
    c->rgb = (unsigned char *)malloc((size_t)c->w * c->h * 4);
    if (!c->pkt || !c->frame || !c->rgb) goto fail;
    c->sws = sws_getContext((int)c->w, (int)c->h, AV_PIX_FMT_YUV420P,
                            (int)c->w, (int)c->h, AV_PIX_FMT_BGRA,
                            SWS_BILINEAR, NULL, NULL, NULL);
    if (!c->sws) goto fail;

    if (out_w) *out_w = c->w;
    if (out_h) *out_h = c->h;
    if (out_fps) {
        AVRational fr = av_guess_frame_rate(c->fmt,
                                            c->fmt->streams[c->stream_idx],
                                            NULL);
        *out_fps = fr.den ? (unsigned)((fr.num + fr.den / 2) / fr.den) : 0;
    }
    if (out_frames) *out_frames = c->frames;
    return c;

fail:
    scene_codec_close(c);
    return NULL;
}

static int decode_pump(scene_codec *c)
{
    for (;;) {
        int r = avcodec_receive_frame(c->dec, c->frame);
        if (r == 0) {
            uint8_t *dst[1] = { c->rgb };
            int dst_stride[1] = { (int)c->w * 4 };
            sws_scale(c->sws,
                      (const uint8_t *const *)c->frame->data,
                      c->frame->linesize, 0, (int)c->h,
                      dst, dst_stride);
            c->frames++;
            return 1;
        }
        if (r == AVERROR(EAGAIN)) {
            int sr = av_read_frame(c->fmt, c->pkt);
            if (sr < 0) {
                avcodec_send_packet(c->dec, NULL);   /* flush */
                continue;
            }
            if (c->pkt->stream_index == c->stream_idx) {
                int er = avcodec_send_packet(c->dec, c->pkt);
                if (er < 0) return -1;
            }
            av_packet_unref(c->pkt);
            continue;
        }
        if (r == AVERROR_EOF) {
            c->eof = 1;
            return 0;
        }
        return -1;
    }
}

int scene_codec_frame(scene_codec *c, uint32_t *px)
{
    int r;

    if (!c || !px) return -1;
    if (c->eof) return 0;
    r = decode_pump(c);
    if (r == 1) memcpy(px, c->rgb, (size_t)c->w * c->h * 4);
    return r;
}

unsigned scene_codec_frames(scene_codec *c)
{
    return c ? c->frames : 0;
}

void scene_codec_close(scene_codec *c)
{
    if (!c) return;
    if (c->sws) sws_freeContext(c->sws);
    av_frame_free(&c->frame);
    av_packet_free(&c->pkt);
    if (c->dec) avcodec_free_context(&c->dec);
    if (c->fmt) avformat_close_input(&c->fmt);
    free(c->rgb);
    free(c);
}
