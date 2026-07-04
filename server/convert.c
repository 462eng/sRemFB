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
