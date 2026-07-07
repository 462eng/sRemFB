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
 *
 * v2 feature bits (still proto_ver 2 — negotiated through hello flags,
 * safe with either side older):
 *   - FEEDBACK: the server may interleave PING control messages in the
 *     frame stream; the client echoes each one back as a PONG (its first
 *     and only upstream message beyond the hello). Because TCP is ordered,
 *     the echo time measures the end-to-end delay of the whole queue in
 *     front of it — the congestion signal driving the adaptive encoder.
 *   - H264: the server may switch the stream to H.264 video (one Annex B
 *     access unit per message) when the measured delay says the raw path
 *     can't keep up, and back when the pressure subsides.
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
    SREMFB_ENC_PING    = 4,   /* payload = u64 LE, the server's monotonic
                                 clock in µs; echo it back verbatim in a
                                 PONG client message. 0x0 rect. */
    SREMFB_ENC_H264    = 5,   /* payload = one H.264 Annex B access unit
                                 (4:2:0, no B-frames, decode order = display
                                 order); rect is always the full stream.
                                 reserved[0] carries SREMFB_H264_FLAG_*. */
    SREMFB_ENC_H264_EOS = 6,  /* no payload: the H.264 episode is over —
                                 drain the decoder, display everything, then
                                 resume; the next message repaints the full
                                 frame in RAW/LZ4. */
};

/* SREMFB_ENC_H264 frame_hdr.reserved[0] bits (informational) */
#define SREMFB_H264_FLAG_IDR (1u << 0)    /* access unit starts with an IDR */

/* client hello flags */
#define SREMFB_HELLO_FLAG_LZ4      (1u << 0)  /* client accepts SREMFB_ENC_LZ4 */
#define SREMFB_HELLO_FLAG_FEEDBACK (1u << 1)  /* client echoes PING as PONG */
#define SREMFB_HELLO_FLAG_H264     (1u << 2)  /* client can decode
                                                 SREMFB_ENC_H264 at its
                                                 resolution (implies it also
                                                 handles H264_EOS) */
#define SREMFB_HELLO_FLAG_USB      (1u << 3)  /* client exports USB devices
                                                 over usbip (usbipd on TCP
                                                 3240): the server may attach
                                                 them while streaming and
                                                 must detach when the client
                                                 leaves */

/* server hello flags (the server only sets a bit when the client
 * advertised the matching capability) */
#define SREMFB_SRV_FLAG_PING (1u << 0)    /* PING messages may appear */
#define SREMFB_SRV_FLAG_H264 (1u << 1)    /* the stream may switch to H.264 */

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
    uint8_t  flags;            /* SREMFB_SRV_FLAG_* (was reserved, always 0
                                  from older servers) */
    uint8_t  reserved[2];
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

/* client -> server, only when the server advertised SREMFB_SRV_FLAG_PING.
 * Sent at the client's position in its receive stream, so the server-side
 * (now - t_echo_us) covers every byte that was queued ahead of the PING. */
enum sremfb_cmsg_type {
    SREMFB_CMSG_PONG = 1,
};

struct sremfb_client_msg {
    uint32_t magic;            /* SREMFB_MAGIC */
    uint8_t  type;             /* enum sremfb_cmsg_type */
    uint8_t  reserved[3];
    uint64_t t_echo_us;        /* PONG: the PING payload, verbatim */
} __attribute__((packed));

_Static_assert(sizeof(struct sremfb_client_hello) == 48, "client hello size");
_Static_assert(sizeof(struct sremfb_server_hello) == 16, "server hello size");
_Static_assert(sizeof(struct sremfb_frame_hdr)   == 20, "frame header size");
_Static_assert(sizeof(struct sremfb_client_msg)  == 16, "client msg size");

#endif /* SREMFB_PROTOCOL_H */
