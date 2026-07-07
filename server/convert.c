#include <stdlib.h>
#include <string.h>

#include "sremfb-server.h"

/* 4x4 Bayer ordered-dither matrix (values 0..15). Spreads the 8->5/6 bit
 * quantization error spatially, which removes banding on gradients at the
 * cost of a faint regular pattern. Disable with SREMFB_NO_DITHER=1. */
static const uint8_t bayer[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

static int dither_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0)
        enabled = getenv("SREMFB_NO_DITHER") == NULL;
    return enabled;
}

static inline int clamp255(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* src is one row of BGRx pixels (XRGB8888: as LE uint32, 0xXXRRGGBB).
 * Converts/copies to the client's wire format. x0,y are the absolute
 * screen coordinates of the first pixel, so the dither matrix stays
 * phase-aligned across partial-rect updates (otherwise rect seams would
 * shimmer on gradients). */
void sremfb_convert_bgrx_row(uint8_t *dst, const uint8_t *src, uint32_t width,
                             uint8_t pixfmt, uint32_t x0, uint32_t y)
{
    if (pixfmt == SREMFB_PIX_XRGB8888) {
        memcpy(dst, src, (size_t)width * 4);
        return;
    }

    /* SREMFB_PIX_RGB565 */
    const uint32_t *s = (const uint32_t *)src;
    uint16_t *d = (uint16_t *)dst;

    if (!dither_enabled()) {
        for (uint32_t i = 0; i < width; i++) {
            uint32_t px = s[i];
            d[i] = (uint16_t)(((px >> 8) & 0xF800) |
                              ((px >> 5) & 0x07E0) |
                              ((px >> 3) & 0x001F));
        }
        return;
    }

    const uint8_t *brow = bayer[y & 3];
    for (uint32_t i = 0; i < width; i++) {
        uint32_t px = s[i];
        int t = brow[(x0 + i) & 3];
        /* bias by (t/16 - 0.5) * quant_step: step 8 for 5-bit, 4 for 6-bit */
        int r = clamp255((int)((px >> 16) & 0xFF) + (t >> 1) - 4);
        int g = clamp255((int)((px >> 8) & 0xFF) + (t >> 2) - 2);
        int b = clamp255((int)(px & 0xFF) + (t >> 1) - 4);
        d[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
}

/* BGRx full frame -> planar I420 for the H.264 encoder. BT.601 limited
 * range, fixed point — must stay in sync with the VUI declared in
 * encoder.c (colorprim/transfer/colmatrix 6, fullrange 0) and with the
 * client's fallback YUV->RGB565 converter. Chroma is a 2x2 box average
 * (width/height are even in practice; odd edges reuse the last column or
 * row). ~2-4 ms at 1080p on a desktop core: fine in the main loop. */
void sremfb_convert_bgrx_to_i420(uint8_t *yp, uint8_t *up, uint8_t *vp,
                                 const uint8_t *bgrx, uint32_t w, uint32_t h)
{
    for (uint32_t j = 0; j < h; j++) {
        const uint32_t *s = (const uint32_t *)(bgrx + (size_t)j * w * 4);
        uint8_t *dy = yp + (size_t)j * w;
        for (uint32_t i = 0; i < w; i++) {
            uint32_t px = s[i];
            int r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
            dy[i] = (uint8_t)((66 * r + 129 * g + 25 * b + 128 + 4096) >> 8);
        }
    }
    uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    for (uint32_t j = 0; j < ch; j++) {
        uint32_t y0 = j * 2, y1 = y0 + 1 < h ? y0 + 1 : y0;
        const uint32_t *s0 = (const uint32_t *)(bgrx + (size_t)y0 * w * 4);
        const uint32_t *s1 = (const uint32_t *)(bgrx + (size_t)y1 * w * 4);
        uint8_t *du = up + (size_t)j * cw;
        uint8_t *dv = vp + (size_t)j * cw;
        for (uint32_t i = 0; i < cw; i++) {
            uint32_t x0c = i * 2, x1c = x0c + 1 < w ? x0c + 1 : x0c;
            uint32_t p[4] = { s0[x0c], s0[x1c], s1[x0c], s1[x1c] };
            int r = 0, g = 0, b = 0;
            for (int k = 0; k < 4; k++) {
                r += (p[k] >> 16) & 0xFF;
                g += (p[k] >> 8) & 0xFF;
                b += p[k] & 0xFF;
            }
            r >>= 2; g >>= 2; b >>= 2;
            du[i] = (uint8_t)((-38 * r - 74 * g + 112 * b + 128 + 32768) >> 8);
            dv[i] = (uint8_t)((112 * r - 94 * g - 18 * b + 128 + 32768) >> 8);
        }
    }
}
