/*
 * Minimal EDID 1.4 block for the virtual connector.
 *
 * The whole point is a STABLE identity: GNOME keys the monitor layout in
 * ~/.config/monitors.xml on vendor/product/name/serial, so keeping these
 * constant makes the virtual screen get its position back on every plug —
 * like a real monitor, reboots included. The serial derives from the
 * client's MAC address, so each physical client appears as a distinct
 * monitor with its own remembered position. Two detailed timings expose the client's
 * framebuffer size at 60 Hz (preferred) and 30 Hz, so the frame rate can
 * be capped from GNOME's display panel. Physical size is a fixed 24-inch
 * diagonal.
 */
#include <math.h>
#include <string.h>

#include "sremfb-server.h"

/* Reduced-blanking detailed timing descriptor. Exact porch values are
 * irrelevant for a virtual link; only the resolution and the resulting
 * refresh rate matter. */
static void put_dtd(uint8_t *d, uint32_t width, uint32_t height,
                    uint32_t refresh, uint32_t wmm, uint32_t hmm)
{
    uint32_t hblank = 160, vblank = 30;
    uint32_t htotal = width + hblank, vtotal = height + vblank;
    uint32_t pclk = (htotal * vtotal * refresh + 5000) / 10000; /* 10 kHz */

    d[0] = (uint8_t)(pclk & 0xFF);
    d[1] = (uint8_t)(pclk >> 8);
    d[2] = (uint8_t)(width & 0xFF);
    d[3] = (uint8_t)(hblank & 0xFF);
    d[4] = (uint8_t)(((width >> 8) << 4) | (hblank >> 8));
    d[5] = (uint8_t)(height & 0xFF);
    d[6] = (uint8_t)(vblank & 0xFF);
    d[7] = (uint8_t)(((height >> 8) << 4) | (vblank >> 8));
    d[8] = 48;                             /* hsync front porch */
    d[9] = 32;                             /* hsync width */
    d[10] = (3 << 4) | 5;                  /* vsync front porch 3, width 5 */
    d[11] = 0;
    d[12] = (uint8_t)(wmm & 0xFF);
    d[13] = (uint8_t)(hmm & 0xFF);
    d[14] = (uint8_t)(((wmm >> 8) << 4) | (hmm >> 8));
    d[17] = 0x1E;                          /* digital, separate +h +v sync */
}

void sremfb_edid_build(uint8_t out[128], uint32_t width, uint32_t height,
                       uint32_t serial, const char model[13])
{
    /* model name: the client panel's "vendor model" if it sent one
     * (printable ASCII only), our project name otherwise */
    char name[14] = "462eng-sRemFB";
    if (model) {
        int n = 0;
        for (int i = 0; i < 13 && model[i]; i++) {
            char ch = model[i];
            if (ch < 0x20 || ch > 0x7E)
                break;
            name[n++] = ch;
        }
        while (n > 0 && name[n - 1] == ' ')
            n--;
        if (n > 0)
            name[n] = '\0';
    }

    uint8_t *e = out;
    /* fixed 24-inch diagonal, whatever the resolution */
    double hyp = sqrt((double)width * width + (double)height * height);
    uint32_t wmm = (uint32_t)(24.0 * 25.4 * width / hyp + 0.5);
    uint32_t hmm = (uint32_t)(24.0 * 25.4 * height / hyp + 0.5);
    uint32_t wcm = MIN((wmm + 5) / 10, 255);
    uint32_t hcm = MIN((hmm + 5) / 10, 255);

    memset(e, 0, 128);

    /* header */
    static const uint8_t magic[8] = { 0x00, 0xFF, 0xFF, 0xFF,
                                      0xFF, 0xFF, 0xFF, 0x00 };
    memcpy(e, magic, 8);
    e[8] = 0x48; e[9] = 0xC2;              /* PNP vendor "RFB" */
    e[10] = 0x01; e[11] = 0x00;            /* product code 1 (LE) */
    e[12] = (uint8_t)(serial & 0xFF);      /* serial number (LE u32) */
    e[13] = (uint8_t)((serial >> 8) & 0xFF);
    e[14] = (uint8_t)((serial >> 16) & 0xFF);
    e[15] = (uint8_t)(serial >> 24);
    e[16] = 1; e[17] = 36;                 /* week 1 of 2026 */
    e[18] = 1; e[19] = 4;                  /* EDID 1.4 */
    e[20] = 0xA0;                          /* digital input, 8 bpc */
    e[21] = (uint8_t)wcm;                  /* screen size in cm */
    e[22] = (uint8_t)hcm;
    e[23] = 120;                           /* gamma 2.2 */
    e[24] = 0x06;                          /* sRGB, preferred timing native */

    /* sRGB chromaticity coordinates */
    static const uint8_t chroma[10] = { 0xEE, 0x91, 0xA3, 0x54, 0x4C,
                                        0x99, 0x26, 0x0F, 0x50, 0x54 };
    memcpy(e + 25, chroma, 10);

    /* established timings: none; standard timings: all unused */
    memset(e + 38, 0x01, 16);

    /* descriptors 1-2: detailed timings — 60 Hz (preferred) and 30 Hz,
     * which GNOME offers as refresh rate choices in the display panel */
    put_dtd(e + 54, width, height, 60, wmm, hmm);
    put_dtd(e + 72, width, height, 30, wmm, hmm);

    /* descriptor 3: display name (max 13 chars, LF-terminated, space
     * padded) */
    {
        uint8_t *d = e + 90;
        size_t len = strlen(name);
        d[3] = 0xFC;
        memset(d + 5, 0x20, 13);
        memcpy(d + 5, name, len);
        if (len < 13)
            d[5 + len] = 0x0A;
    }

    /* descriptor 4: display range limits */
    {
        uint8_t *d = e + 108;
        d[3] = 0xFD;
        d[5] = 24;  d[6] = 61;             /* vertical 24-61 Hz */
        d[7] = 10;  d[8] = 160;            /* horizontal 10-160 kHz */
        d[9] = 60;                         /* max pixel clock 600 MHz */
        d[10] = 0x01;                      /* range limits only */
        d[11] = 0x0A;
        memset(d + 12, 0x20, 6);
    }

    /* no extension blocks; checksum makes the byte sum 0 mod 256 */
    e[126] = 0;
    uint8_t sum = 0;
    for (int i = 0; i < 127; i++)
        sum = (uint8_t)(sum + e[i]);
    e[127] = (uint8_t)(256 - sum);
}
