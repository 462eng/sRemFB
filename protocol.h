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
 *   - INPUT: the client forwards keyboard/mouse/gamepad events as INPUT
 *     client messages (evdev type/code/value triplets), which the server
 *     replays on per-client uinput devices. Off unless the server's admin
 *     enabled it (SREMFB_INPUT=1) and confirmed in the server hello.
 *   - AUDIO: the server creates a virtual PipeWire output for the client
 *     and streams what is played there as raw PCM (48 kHz, stereo, S16LE)
 *     over a separate UDP flow. The client opens it from its own UDP
 *     socket with SREMFB_UDP_AUDIO_HELLO datagrams carrying the token of
 *     the server hello (so firewalls and NAT see an outgoing flow); the
 *     server answers each hello with an ECHO (clock offset for latency
 *     estimates) and sends SREMFB_UDP_AUDIO packets back to that address.
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
#define SREMFB_HELLO_FLAG_INPUT    (1u << 4)  /* client would like to send
                                                 INPUT messages (only does
                                                 so once the server hello
                                                 carries SREMFB_SRV_FLAG_INPUT) */
#define SREMFB_HELLO_FLAG_AUDIO    (1u << 5)  /* client plays the server's
                                                 audio (UDP PCM, see
                                                 struct sremfb_audio_hdr) */

/* server hello flags (the server only sets a bit when the client
 * advertised the matching capability) */
#define SREMFB_SRV_FLAG_PING (1u << 0)    /* PING messages may appear */
#define SREMFB_SRV_FLAG_H264 (1u << 1)    /* the stream may switch to H.264 */
#define SREMFB_SRV_FLAG_INPUT (1u << 2)   /* INPUT messages are accepted: the
                                             server's input devices exist */
#define SREMFB_SRV_FLAG_AUDIO (1u << 3)   /* the client's audio output exists:
                                             open the UDP flow with
                                             server_hello.audio_token */

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
    uint16_t audio_token;      /* with SREMFB_SRV_FLAG_AUDIO: to quote in
                                  the UDP AUDIO_HELLO (was reserved) */
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

/* client -> server, fixed 16-byte messages. PONG only when the server
 * advertised SREMFB_SRV_FLAG_PING: sent at the client's position in its
 * receive stream, so the server-side (now - t_echo_us) covers every byte
 * that was queued ahead of the PING. INPUT only when the server advertised
 * SREMFB_SRV_FLAG_INPUT (struct sremfb_input_msg, same size). */
enum sremfb_cmsg_type {
    SREMFB_CMSG_PONG  = 1,
    SREMFB_CMSG_INPUT = 2,
};

/* Target device of an INPUT message. Each one is a separate uinput device
 * on the server, so the desktop classifies them like real hardware. */
enum sremfb_indev {
    SREMFB_INDEV_ALL      = 0,   /* only with ev_type 0: release every key
                                    and button held on every device (focus
                                    lost); a gamepad also re-centers */
    SREMFB_INDEV_KEYBOARD = 1,   /* EV_KEY KEY_* */
    SREMFB_INDEV_MOUSE    = 2,   /* relative: EV_REL X/Y/wheels, BTN_LEFT.. */
    SREMFB_INDEV_POINTER  = 3,   /* absolute: EV_ABS ABS_X/ABS_Y in *stream
                                    pixels* (0..width-1, 0..height-1), the
                                    server maps them onto its desktop;
                                    buttons and wheels as MOUSE */
    SREMFB_INDEV_GAMEPAD  = 4,   /* Xbox 360 layout: BTN_A/B/X/Y, TL/TR,
                                    SELECT/START/MODE, THUMBL/R; ABS_X/Y/
                                    RX/RY -32768..32767, ABS_Z/RZ triggers
                                    0..255, ABS_HAT0X/Y -1..1 */
};

struct sremfb_client_msg {
    uint32_t magic;            /* SREMFB_MAGIC */
    uint8_t  type;             /* enum sremfb_cmsg_type */
    uint8_t  reserved[3];
    uint64_t t_echo_us;        /* PONG: the PING payload, verbatim */
} __attribute__((packed));

/* client -> server, INPUT: one Linux evdev event (linux/input-event-codes.h
 * values). Events of one device take effect at its EV_SYN/SYN_REPORT,
 * which the client sends explicitly, exactly like a kernel driver. */
struct sremfb_input_msg {
    uint32_t magic;            /* SREMFB_MAGIC */
    uint8_t  type;             /* SREMFB_CMSG_INPUT */
    uint8_t  dev;              /* enum sremfb_indev */
    uint8_t  reserved[2];
    uint16_t ev_type;          /* EV_SYN, EV_KEY, EV_REL, EV_ABS */
    uint16_t ev_code;
    int32_t  ev_value;
} __attribute__((packed));

/*
 * Audio (SREMFB_SRV_FLAG_AUDIO): datagrams on UDP, same port number as the
 * TCP listener. Every datagram starts with magic + type.
 *
 *   client -> server  AUDIO_HELLO (struct sremfb_udp_hello, 16 B), from
 *                     the socket the client receives on: at least once a
 *                     second while it wants audio (it is also the
 *                     keepalive; the server stops sending after ~5 s
 *                     without one). The server only accepts it from the
 *                     TCP peer's IP address with the right token.
 *   server -> client  ECHO (struct sremfb_udp_echo, 24 B), one per hello:
 *                     t_client_us back + the server clock at reception.
 *   server -> client  AUDIO (struct sremfb_audio_hdr + frames * 4 bytes of
 *                     interleaved S16LE stereo at 48 kHz), one every
 *                     ~2.7 ms while something plays (at most 240 frames =
 *                     5 ms per packet). seq increments by one per packet;
 *                     t_us is the server's monotonic clock when the first
 *                     frame left the audio graph. No retransmission: a
 *                     missing seq is a lost packet.
 */
enum sremfb_udp_type {
    SREMFB_UDP_AUDIO       = 1,
    SREMFB_UDP_AUDIO_HELLO = 2,
    SREMFB_UDP_ECHO        = 3,
};

enum sremfb_audio_fmt {
    SREMFB_AUDIO_S16LE_48K_STEREO = 0,
};

#define SREMFB_AUDIO_RATE       48000
#define SREMFB_AUDIO_CHANNELS   2
#define SREMFB_AUDIO_MAX_FRAMES 240       /* 5 ms: 960 B of payload */

struct sremfb_audio_hdr {
    uint32_t magic;            /* SREMFB_MAGIC */
    uint8_t  type;             /* SREMFB_UDP_AUDIO */
    uint8_t  format;           /* enum sremfb_audio_fmt */
    uint16_t frames;           /* sample frames that follow */
    uint32_t seq;
    uint32_t reserved;
    uint64_t t_us;             /* server CLOCK_MONOTONIC, µs */
} __attribute__((packed));

struct sremfb_udp_hello {
    uint32_t magic;            /* SREMFB_MAGIC */
    uint8_t  type;             /* SREMFB_UDP_AUDIO_HELLO */
    uint8_t  reserved;
    uint16_t token;            /* server_hello.audio_token */
    uint64_t t_client_us;      /* client clock, echoed back */
} __attribute__((packed));

struct sremfb_udp_echo {
    uint32_t magic;            /* SREMFB_MAGIC */
    uint8_t  type;             /* SREMFB_UDP_ECHO */
    uint8_t  reserved[3];
    uint64_t t_client_us;      /* from the hello, verbatim */
    uint64_t t_server_us;      /* server CLOCK_MONOTONIC at reception */
} __attribute__((packed));

_Static_assert(sizeof(struct sremfb_client_hello) == 48, "client hello size");
_Static_assert(sizeof(struct sremfb_server_hello) == 16, "server hello size");
_Static_assert(sizeof(struct sremfb_frame_hdr)   == 20, "frame header size");
_Static_assert(sizeof(struct sremfb_client_msg)  == 16, "client msg size");
_Static_assert(sizeof(struct sremfb_input_msg)   == 16, "input msg size");
_Static_assert(sizeof(struct sremfb_audio_hdr)   == 24, "audio header size");
_Static_assert(sizeof(struct sremfb_udp_hello)   == 16, "udp hello size");
_Static_assert(sizeof(struct sremfb_udp_echo)    == 24, "udp echo size");

#endif /* SREMFB_PROTOCOL_H */
