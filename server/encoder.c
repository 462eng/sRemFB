/*
 * H.264 encoder (libx264) for the adaptive-compression path.
 *
 * Tuned for this exact use: a lossless ordered transport (TCP) feeding a
 * hardware decoder on the SBC. So no B-frames, no lookahead (zerolatency),
 * no periodic keyframes and no scene-cut IDRs — an IDR only opens an
 * episode (or follows a decoder restart); everything else is P-frames
 * where x264's skip blocks make small-damage frames nearly free.
 *
 * Rate control is ABR capped by a ~100 ms VBV so a single frame (IDR
 * included) can never dump much more than a tenth of a second of the
 * target bitrate on the wire — that bounds the delay added per frame,
 * which is the quantity the controller regulates. The target moves at
 * runtime through sremfb_enc_set_bitrate() (AIMD, driven by the measured
 * echo delay).
 *
 * Output is 4:2:0 (the Pi's hardware decoder has no Hi444 support); the
 * BGRx -> I420 conversion lives in convert.c and matches the BT.601
 * limited-range VUI declared here.
 */
#include <glib.h>
#include <stdint.h>
#include <string.h>

#include <x264.h>

#include "sremfb-server.h"

struct SremfbEncoder {
    x264_t *h;
    x264_param_t param;
    x264_picture_t pic;        /* wraps the caller's I420 planes */
    int w, h_px;
    int kbps;
    int64_t pts;
};

struct SremfbEncoder *sremfb_enc_open(int width, int height, int kbps)
{
    struct SremfbEncoder *e = g_new0(struct SremfbEncoder, 1);

    if (x264_param_default_preset(&e->param, "superfast",
                                  "zerolatency") < 0) {
        g_free(e);
        return NULL;
    }
    e->param.i_width = width;
    e->param.i_height = height;
    e->param.i_csp = X264_CSP_I420;
    e->param.b_annexb = 1;
    e->param.b_repeat_headers = 1;     /* SPS/PPS glued to every IDR */
    e->param.i_threads = 4;            /* sliced: latency stays <1 frame */
    e->param.b_sliced_threads = 1;

    /* damage-driven: timestamps are meaningless, fps only scales the VBV */
    e->param.i_fps_num = 60;
    e->param.i_fps_den = 1;
    e->param.b_vfr_input = 0;

    /* IDR only when we force one */
    e->param.i_keyint_max = X264_KEYINT_MAX_INFINITE;
    e->param.i_keyint_min = 1;
    e->param.i_scenecut_threshold = 0;

    e->param.rc.i_rc_method = X264_RC_ABR;
    e->param.rc.i_bitrate = kbps;
    e->param.rc.i_vbv_max_bitrate = kbps;
    e->param.rc.i_vbv_buffer_size = kbps / 10;   /* ~100 ms */

    /* BT.601 limited range, matching sremfb_convert_bgrx_to_i420() */
    e->param.vui.b_fullrange = 0;
    e->param.vui.i_colorprim = 6;      /* SMPTE 170M */
    e->param.vui.i_transfer = 6;
    e->param.vui.i_colmatrix = 6;

    e->param.i_log_level = X264_LOG_WARNING;

    e->h = x264_encoder_open(&e->param);
    if (!e->h) {
        g_free(e);
        return NULL;
    }
    e->w = width;
    e->h_px = height;
    e->kbps = kbps;
    return e;
}

void sremfb_enc_set_bitrate(struct SremfbEncoder *e, int kbps)
{
    if (!e || kbps == e->kbps)
        return;
    e->param.rc.i_bitrate = kbps;
    e->param.rc.i_vbv_max_bitrate = kbps;
    e->param.rc.i_vbv_buffer_size = kbps / 10;
    if (x264_encoder_reconfig(e->h, &e->param) == 0)
        e->kbps = kbps;
}

int sremfb_enc_bitrate(const struct SremfbEncoder *e)
{
    return e ? e->kbps : 0;
}

/* Encodes one full I420 frame (planes y/u/v sized for w x h). Returns the
 * access unit size (0 = x264 buffered nothing to output, impossible with
 * zerolatency; <0 = error). *nal_out points into x264-owned memory, valid
 * until the next call. */
int sremfb_enc_encode(struct SremfbEncoder *e, uint8_t *y, uint8_t *u,
                      uint8_t *v, int force_idr, uint8_t **nal_out,
                      int *is_idr)
{
    x264_nal_t *nals = NULL;
    int n_nal = 0;

    x264_picture_init(&e->pic);
    e->pic.img.i_csp = X264_CSP_I420;
    e->pic.img.i_plane = 3;
    e->pic.img.plane[0] = y;
    e->pic.img.plane[1] = u;
    e->pic.img.plane[2] = v;
    e->pic.img.i_stride[0] = e->w;
    e->pic.img.i_stride[1] = e->w / 2;
    e->pic.img.i_stride[2] = e->w / 2;
    e->pic.i_pts = e->pts++;
    e->pic.i_type = force_idr ? X264_TYPE_IDR : X264_TYPE_AUTO;

    x264_picture_t out;
    int sz = x264_encoder_encode(e->h, &nals, &n_nal, &e->pic, &out);
    if (sz <= 0 || n_nal <= 0)
        return sz < 0 ? -1 : 0;

    /* Annex B NALs of one AU are contiguous in x264's buffer */
    *nal_out = nals[0].p_payload;
    *is_idr = out.b_keyframe;
    return sz;
}

void sremfb_enc_close(struct SremfbEncoder *e)
{
    if (!e)
        return;
    if (e->h)
        x264_encoder_close(e->h);
    g_free(e);
}
