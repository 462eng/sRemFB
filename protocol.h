/*
 * sRemFB — simple Remote Frame Buffer. Wire protocol shared by server
 * and client.
 *
 * All integers are little-endian on the wire. Both supported targets
 * (x86-64 server, ARM clients) are little-endian; big-endian hosts are
 * not supported.
 *
 * v2 (sRemFB): the client identifies itself with its MAC address (the
 * server derives the EDID serial from it, so each physical client is a
 * distinct monitor with its own remembered position), and the server can
 * send explicit BLANK/UNBLANK control messages (DPMS pass-through).
 */
#ifndef SREMFB_PROTOCOL_H
#define SREMFB_PROTOCOL_H

#include <stdint.h>

#define SREMFB_MAGIC        0x30624672u   /* bytes 'r','F','b','0' (v1 heritage) */
#define SREMFB_PROTO_VER    2
#define SREMFB_DEFAULT_PORT 4629

/* Pixel format, both on the wire and in the client framebuffer. */
enum sremfb_pixfmt {
    SREMFB_PIX_XRGB8888 = 0,  /* 4 B/px, memory order B,G,R,X (DRM XRGB8888 LE) */
    SREMFB_PIX_RGB565   = 1,  /* 2 B/px, LE uint16, r<<11 | g<<5 | b */
};

enum sremfb_encoding {
    SREMFB_ENC_RAW     = 0,   /* payload = w*h*bytespp raw pixels, no padding */
    SREMFB_ENC_LZ4     = 1,   /* payload = one LZ4 block of those raw pixels */
    SREMFB_ENC_BLANK   = 2,   /* no payload: turn the panel off (DPMS off) */
    SREMFB_ENC_UNBLANK = 3,   /* no payload: turn the panel back on */
};

/* client hello flags */
#define SREMFB_HELLO_FLAG_LZ4 (1u << 0)   /* client accepts SREMFB_ENC_LZ4 */

/* Server hello status codes. */
enum sremfb_status {
    SREMFB_STATUS_OK          = 0,
    SREMFB_STATUS_BAD_HELLO   = 1,   /* malformed/incompatible client hello */
    SREMFB_STATUS_SERVER_FAIL = 2,   /* compositor never lit the connector */
    SREMFB_STATUS_NO_DEVICE   = 3,   /* no free EVDI device for this client */
};

/* client -> server, once, immediately after connect */
struct sremfb_client_hello {
    uint32_t magic;            /* SREMFB_MAGIC */
    uint16_t proto_ver;        /* SREMFB_PROTO_VER */
    uint16_t flags;
    uint16_t xres, yres;       /* framebuffer visible resolution */
    uint8_t  bpp;              /* framebuffer bits_per_pixel: 16 or 32 */
    uint8_t  pixfmt;           /* enum sremfb_pixfmt */
    /* raw fb channel layout, informational */
    uint8_t  red_off, red_len;
    uint8_t  green_off, green_len;
    uint8_t  blue_off, blue_len;
    uint8_t  mac[6];           /* client MAC (all-zero = unknown) */
    char     model[13];        /* attached panel "vendor model" (from its
                                  EDID), space/NUL padded; empty = unknown.
                                  Becomes the virtual monitor's model name. */
    uint8_t  reserved[9];
} __attribute__((packed));

/* server -> client, once, after the compositor set a mode */
struct sremfb_server_hello {
    uint32_t magic;
    uint16_t proto_ver;
    uint16_t status;           /* enum sremfb_status; nonzero => close */
    uint16_t width, height;    /* negotiated stream size (normally xres,yres) */
    uint8_t  pixfmt;           /* wire pixel format of the frames that follow */
    uint8_t  reserved[3];
} __attribute__((packed));

/* server -> client, one per message, followed by payload_len bytes.
 * BLANK/UNBLANK carry no payload and a 0x0 rect. */
struct sremfb_frame_hdr {
    uint32_t magic;            /* SREMFB_MAGIC — resync/corruption guard */
    uint8_t  encoding;         /* enum sremfb_encoding */
    uint8_t  reserved[3];
    uint16_t x, y, w, h;       /* dest rect in stream coords */
    uint32_t payload_len;      /* RAW: w*h*bytespp; BLANK/UNBLANK: 0 */
} __attribute__((packed));

_Static_assert(sizeof(struct sremfb_client_hello) == 48, "client hello size");
_Static_assert(sizeof(struct sremfb_server_hello) == 16, "server hello size");
_Static_assert(sizeof(struct sremfb_frame_hdr)   == 20, "frame header size");

#endif /* SREMFB_PROTOCOL_H */
