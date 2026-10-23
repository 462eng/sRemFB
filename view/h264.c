/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * sremfb-view H.264 decoding (libavcodec), used by net.c when the server
 * switches the stream to SREMFB_ENC_H264 — built only with H264=1.
 *
 * Synchronous on purpose, in the network thread: the server sends no
 * B-frames and one complete access unit per message, so with
 * AV_CODEC_FLAG_LOW_DELAY every AU yields its picture straight away.
 * Slice threading only (the server's x264 emits 4 slices): frame
 * threading would hold N pictures back, which is latency for nothing.
 *
 * The picture is converted on the CPU to the wire format (XRGB8888, or
 * RGB565 with the server's 4x4 ordered dither), so the display side,
 * the dumps and the latency probe never see YUV. The colour matrix is
 * BT.601 limited range by protocol convention, whatever the stream's
 * VUI says (or omits: the Pi's hardware encoder may write none).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/log.h>

#include "view.h"

struct view_h264 {
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;
};

static void log_averr(const char *what, int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(err, buf, sizeof(buf));
    view_log("H.264: %s: %s", what, buf);
}

struct view_h264 *view_h264_open(void)
{
    static int logged;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    struct view_h264 *d;

    if (!codec) {
        view_log("H.264: libavcodec has no H.264 decoder");
        return NULL;
    }
    d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    if (!logged) {                      /* keep libav's chatter for real
                                           errors */
        av_log_set_level(AV_LOG_ERROR);
        logged = 1;
    }
    d->ctx = avcodec_alloc_context3(codec);
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (!d->ctx || !d->pkt || !d->frame) {
        view_log("H.264: out of memory");
        view_h264_close(d);
        return NULL;
    }
    d->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    d->ctx->thread_type = FF_THREAD_SLICE;
    d->ctx->thread_count = 0;           /* auto */
    int rc = avcodec_open2(d->ctx, codec, NULL);
    if (rc < 0) {
        log_averr("cannot open the decoder", rc);
        view_h264_close(d);
        return NULL;
    }
    view_log("H.264: %s decoder ready (libavcodec %s, %d slice threads)",
             codec->name, LIBAVCODEC_IDENT, d->ctx->thread_count);
    return d;
}

int view_h264_send(struct view_h264 *d, const uint8_t *au, size_t len)
{
    int rc;

    if (au) {
        /* not refcounted: libavcodec copies it (au is the caller's
         * padded receive buffer) */
        d->pkt->data = (uint8_t *)au;
        d->pkt->size = (int)len;
        rc = avcodec_send_packet(d->ctx, d->pkt);
        d->pkt->data = NULL;
        d->pkt->size = 0;
    } else {
        rc = avcodec_send_packet(d->ctx, NULL);     /* drain */
    }
    if (rc < 0) {
        /* EAGAIN can't happen: every picture is received after each
         * send, and the decoder holds none back (LOW_DELAY) */
        log_averr(au ? "decoding failed" : "drain failed", rc);
        return -1;
    }
    return 0;
}

int view_h264_receive(struct view_h264 *d, struct view_h264_pic *p)
{
    int rc = avcodec_receive_frame(d->ctx, d->frame);

    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF)
        return 0;
    if (rc < 0) {
        log_averr("decoding failed", rc);
        return -1;
    }
    if (d->frame->format != AV_PIX_FMT_YUV420P &&
        d->frame->format != AV_PIX_FMT_YUVJ420P) {
        view_log("H.264: unsupported decoded format %d (want 4:2:0 8-bit)",
                 d->frame->format);
        return -1;
    }
    if (d->frame->decode_error_flags) {
        /* the transport is lossless, so this is a stream the decoder
         * doesn't get — and with no IDR before the episode ends, the
         * damage would stay on screen: give up on H.264 instead */
        view_log("H.264: picture decoded with errors (flags 0x%x)",
                 (unsigned)d->frame->decode_error_flags);
        return -1;
    }
    for (int i = 0; i < 3; i++) {
        p->plane[i] = d->frame->data[i];
        p->stride[i] = d->frame->linesize[i];
    }
    p->w = (unsigned)d->frame->width;
    p->h = (unsigned)d->frame->height;
    p->key = (d->frame->flags & AV_FRAME_FLAG_KEY) != 0;
    return 1;
}

void view_h264_flush(struct view_h264 *d)
{
    avcodec_flush_buffers(d->ctx);      /* also ends the draining state */
}

void view_h264_close(struct view_h264 *d)
{
    if (!d)
        return;
    av_frame_free(&d->frame);
    av_packet_free(&d->pkt);
    avcodec_free_context(&d->ctx);
    free(d);
}

/* ------------------------------------------------------- conversion */

static inline int clamp255(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* Same matrix as the server's convert.c (ordered dither towards RGB565,
 * phase-aligned on screen coordinates). */
static const uint8_t bayer[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

/* BT.601 limited range, fixed point — the inverse of the server's
 * sremfb_convert_bgrx_to_i420() and the same arithmetic as the SBC
 * client's yuv_row_to_fb(). One row at a time, the pixel format branch
 * hoisted out of the loops so they stay simple enough to vectorize. */
static void row_xrgb(uint32_t *d, const uint8_t *yr, const uint8_t *ur,
                     const uint8_t *vr, unsigned w)
{
    for (unsigned x = 0; x < w; x++) {
        int c = 298 * ((int)yr[x] - 16) + 128;
        int du = (int)ur[x / 2] - 128, dv = (int)vr[x / 2] - 128;
        int r = clamp255((c + 409 * dv) >> 8);
        int g = clamp255((c - 100 * du - 208 * dv) >> 8);
        int b = clamp255((c + 516 * du) >> 8);
        d[x] = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
    }
}

static void row_rgb565(uint16_t *d, const uint8_t *yr, const uint8_t *ur,
                       const uint8_t *vr, unsigned w, const uint8_t *brow)
{
    for (unsigned x = 0; x < w; x++) {
        int c = 298 * ((int)yr[x] - 16) + 128;
        int du = (int)ur[x / 2] - 128, dv = (int)vr[x / 2] - 128;
        int t = brow[x & 3];
        /* dither bias as in convert.c: (t/16 - 0.5) * quantization step */
        int r = clamp255(clamp255((c + 409 * dv) >> 8) + (t >> 1) - 4);
        int g = clamp255(clamp255((c - 100 * du - 208 * dv) >> 8) +
                         (t >> 2) - 2);
        int b = clamp255(clamp255((c + 516 * du) >> 8) + (t >> 1) - 4);
        d[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
}

void view_h264_convert(const struct view_h264_pic *p, uint8_t *dst,
                       size_t dst_stride, unsigned w, unsigned h,
                       uint8_t pixfmt)
{
    for (unsigned y = 0; y < h; y++) {
        const uint8_t *yr = p->plane[0] + (size_t)y * (size_t)p->stride[0];
        const uint8_t *ur = p->plane[1] + (size_t)(y / 2) * (size_t)p->stride[1];
        const uint8_t *vr = p->plane[2] + (size_t)(y / 2) * (size_t)p->stride[2];
        uint8_t *row = dst + (size_t)y * dst_stride;

        if (pixfmt == SREMFB_PIX_RGB565)
            row_rgb565((uint16_t *)row, yr, ur, vr, w, bayer[y & 3]);
        else
            row_xrgb((uint32_t *)row, yr, ur, vr, w);
    }
}
