/*
 * Hardware H.264 decoding through a V4L2 stateful memory-to-memory
 * decoder (pure ioctls, no library) — on a Raspberry Pi 3 this is
 * /dev/video10, the bcm2835-codec exposing the VideoCore IV decoder.
 * Only used when the server switches the stream to SREMFB_ENC_H264
 * under measured congestion; sremfb-client.c falls back to the plain
 * RAW/LZ4 path (and stops advertising the capability) on any failure.
 */
#ifndef SREMFB_V4L2DEC_H
#define SREMFB_V4L2DEC_H

#include <stddef.h>
#include <stdint.h>

/* One decoded frame handed to the client for display. data points into
 * the driver's mmapped capture buffer, valid only during the callback.
 * For NV12/YU12 the chroma follows the luma at stride*coded_h. */
struct v4l2dec_frame {
    const uint8_t *data;
    uint32_t fourcc;           /* V4L2_PIX_FMT_RGB565 / NV12 / YUV420 */
    uint32_t w, h;             /* visible size */
    uint32_t stride;           /* luma (or RGB) bytes per line */
    uint32_t coded_h;          /* buffer height (chroma plane offset) */
};

/* Implemented by sremfb-client.c: convert (if needed) and blit. */
void v4l2dec_emit(const struct v4l2dec_frame *f);

/* Scans /dev/video0..31 for an M2M decoder taking H.264; remembers the
 * node. Returns 1 if found (the hello then advertises H264). */
int v4l2dec_probe(void);

/* Prepares the decoder for a stream of WxH access units (lazy, called on
 * the first SREMFB_ENC_H264 message). fb_rgb565 selects the preferred
 * output: nonzero tries the decoder's direct RGB565 path first, falling
 * back to NV12/YU12 + CPU conversion in the emit callback. */
int v4l2dec_open(unsigned w, unsigned h, int fb_rgb565);

int  v4l2dec_feed(const uint8_t *au, size_t len);  /* one access unit */
int  v4l2dec_pump(void);       /* drain events + decoded frames (poll hit) */
int  v4l2dec_drain(void);      /* SREMFB_ENC_H264_EOS: display everything */
void v4l2dec_close(void);
int  v4l2dec_fd(void);         /* for poll(); -1 when closed */

#endif /* SREMFB_V4L2DEC_H */
