/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * Frame capturée -> image maître BGRx (XRGB8888 LE, le format de grabbuf),
 * rectangle par rectangle : seules les tuiles sales sont converties (§4.5).
 *
 * UYVY (4:2:2, octets U0 Y0 V0 Y1 pour deux pixels) -> RGB en BT.601
 * limited range, virgule fixe 8 bits : les coefficients inverses de
 * sremfb_convert_bgrx_to_i420() (core/convert.c) et ceux de yuv_row_to_fb()
 * du client SBC, donc noir 16 -> 0, blanc 235 -> 255. Si G_FMT annonce
 * V4L2_QUANTIZATION_FULL_RANGE (st->full_range), coefficients pleine
 * échelle. Deux pixels par itération, sans branche hors des saturations :
 * écrit pour l'auto-vectorisation (-O2 -ftree-vectorize).
 *
 * RGB24 (octets R G B) -> BGRx : permutation.
 */
#include <linux/videodev2.h>

#include "kvm.h"

static inline uint8_t sat8(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* Coefficients <<8. Limited : Y' = 255/219 (Y-16), Cb/Cr *255/224. */
struct yuv_coef { int ky, yoff, rv, gu, gv, bu; };
static const struct yuv_coef COEF_LIMITED = { 298, 16, 409, 100, 208, 516 };
static const struct yuv_coef COEF_FULL    = { 256,  0, 359,  88, 183, 454 };

static void uyvy_row(uint8_t *restrict dst, const uint8_t *restrict src,
                     int npairs, const struct yuv_coef *k)
{
    for (int i = 0; i < npairs; i++) {
        int u  = src[4 * i + 0] - 128;
        int y0 = k->ky * (src[4 * i + 1] - k->yoff) + 128;
        int v  = src[4 * i + 2] - 128;
        int y1 = k->ky * (src[4 * i + 3] - k->yoff) + 128;
        int cr = k->rv * v;
        int cg = -k->gu * u - k->gv * v;
        int cb = k->bu * u;

        dst[8 * i + 0] = sat8((y0 + cb) >> 8);
        dst[8 * i + 1] = sat8((y0 + cg) >> 8);
        dst[8 * i + 2] = sat8((y0 + cr) >> 8);
        dst[8 * i + 3] = 0xFF;
        dst[8 * i + 4] = sat8((y1 + cb) >> 8);
        dst[8 * i + 5] = sat8((y1 + cg) >> 8);
        dst[8 * i + 6] = sat8((y1 + cr) >> 8);
        dst[8 * i + 7] = 0xFF;
    }
}

static void rgb24_row(uint8_t *restrict dst, const uint8_t *restrict src, int n)
{
    for (int i = 0; i < n; i++) {
        dst[4 * i + 0] = src[3 * i + 2];
        dst[4 * i + 1] = src[3 * i + 1];
        dst[4 * i + 2] = src[3 * i + 0];
        dst[4 * i + 3] = 0xFF;
    }
}

void sremfb_kvm_convert_rect(const SremfbKvmState *st, const uint8_t *src,
                             const struct sremfb_rect *r)
{
    int x1 = MAX(r->x1, 0), y1 = MAX(r->y1, 0);
    int x2 = MIN(r->x2, st->src_w), y2 = MIN(r->y2, st->src_h);
    size_t bpp = sremfb_kvm_src_bpp(st->src_fourcc);
    size_t stride = st->src_stride ? st->src_stride : (size_t)st->src_w * bpp;
    size_t mstride = (size_t)st->src_w * 4;

    if (!st->master || x2 <= x1 || y2 <= y1)
        return;

    if (st->src_fourcc == V4L2_PIX_FMT_RGB24) {
        for (int y = y1; y < y2; y++)
            rgb24_row(st->master + (size_t)y * mstride + (size_t)x1 * 4,
                      src + (size_t)y * stride + (size_t)x1 * 3, x2 - x1);
        return;
    }

    /* UYVY : on travaille par macro-pixels (x pair) */
    const struct yuv_coef *k = st->full_range ? &COEF_FULL : &COEF_LIMITED;
    x1 &= ~1;
    x2 = MIN((x2 + 1) & ~1, st->src_w & ~1);
    if (x2 <= x1)
        return;
    for (int y = y1; y < y2; y++)
        uyvy_row(st->master + (size_t)y * mstride + (size_t)x1 * 4,
                 src + (size_t)y * stride + (size_t)x1 * 2, (x2 - x1) / 2, k);
}
