# Network protocol

**English** · [Français](PROTOCOL.fr.md)

sRemFB's application protocol, shared verbatim by server and client in
[`protocol.h`](protocol.h). Version described here: **v2**
(`SREMFB_PROTO_VER = 2`), including its **feature bits** (delay feedback
and adaptive H.264, negotiated through the hellos without a version
bump — every old/new combination stays compatible).

## Transport

- **TCP**, a single port (default **4629**), with several simultaneous
  clients told apart by their MAC address (see [README.md](README.md)).
- `TCP_NODELAY`, `SO_KEEPALIVE` (idle 10 s / intvl 5 s / cnt 3),
  `TCP_USER_TIMEOUT` 6 s (unACKed data ⇒ the connection dies even
  mid-send), `SO_SNDTIMEO` 20 s, `SO_RCVTIMEO` 5 s. `SIGPIPE` ignored.
- **Liveness** (when the PING feature is negotiated): the server keeps a
  heartbeat PING flowing at least every ~2 s even on a static screen,
  and each side declares the other dead after ~6 s of silence — the
  virtual monitor unplugs and the panel drops to "no signal" in ~6-7 s
  on a network cut. Older peers fall back to the TCP timers (~20-25 s).
- The server checks the address against the CIDR allowlist
  (`SREMFB_ALLOW`) **at accept time**, before it even reads the hello.
- **No encryption, no authentication.** For a dedicated, trusted LAN only.

## Endianness

All integers are **little-endian** on the wire. Both supported targets
(x86-64 server, ARM clients) are little-endian; big-endian hosts are not
supported. Structs are packed and their sizes are checked with
`_Static_assert`.

## Constants

```c
#define SREMFB_MAGIC        0x30624672u   /* bytes 'r','F','b','0' (v1 heritage) */
#define SREMFB_PROTO_VER    2
#define SREMFB_DEFAULT_PORT 4629
```

The `rFb0` magic is kept from v1: it's a resync guard placed at the head
of every message.

## Sequence

```
client → server   :  TCP connect
client → server   :  sremfb_client_hello   (48 B, once)
                     ─ the server picks an EVDI device, builds the EDID
                       and "plugs" it in; the compositor sets a mode ─
server → client   :  sremfb_server_hello   (16 B, once)
                       status != 0  ⇒  the server closes the connection
server → client   :  sremfb_frame_hdr + payload   (repeated, on damage)
                     sremfb_frame_hdr BLANK / UNBLANK   (no payload)
                     sremfb_frame_hdr PING + u64       (if negotiated)
client → server   :  sremfb_client_msg PONG (16 B, echoes each PING)
server → client   :  sremfb_frame_hdr H264 + access unit   (under
                     measured congestion, if negotiated; H264_EOS ends
                     the episode)
```

## Messages

### `sremfb_client_hello` — 48 bytes, client → server

Sent exactly once, right after the connection.

| Field | Type | Purpose |
|---|---|---|
| `magic` | `u32` | `SREMFB_MAGIC` |
| `proto_ver` | `u16` | `SREMFB_PROTO_VER` (2) |
| `flags` | `u16` | bit 0 `SREMFB_HELLO_FLAG_LZ4` = the client accepts LZ4 · bit 1 `SREMFB_HELLO_FLAG_FEEDBACK` = the client echoes PING as PONG · bit 2 `SREMFB_HELLO_FLAG_H264` = the client decodes H.264 (4:2:0, Annex B) at its resolution |
| `xres`, `yres` | `u16` | visible framebuffer resolution |
| `bpp` | `u8` | framebuffer bits per pixel: 16 or 32 |
| `pixfmt` | `u8` | `enum sremfb_pixfmt` |
| `red_off`, `red_len` | `u8` | red channel layout (informational) |
| `green_off`, `green_len` | `u8` | green channel |
| `blue_off`, `blue_len` | `u8` | blue channel |
| `mac[6]` | `u8` | client MAC (all-zero = unknown) → EDID serial |
| `model[13]` | `char` | "vendor model" of the panel attached to the SBC (from its EDID), space/NUL padded; empty = unknown → the virtual monitor's model name |
| `reserved[9]` | `u8` | reserved |

### `sremfb_server_hello` — 16 bytes, server → client

Sent once, after the compositor has set a mode on the connector.

| Field | Type | Purpose |
|---|---|---|
| `magic` | `u32` | `SREMFB_MAGIC` |
| `proto_ver` | `u16` | `SREMFB_PROTO_VER` |
| `status` | `u16` | `enum sremfb_status`; nonzero ⇒ the client closes |
| `width`, `height` | `u16` | negotiated stream size (normally `xres`,`yres`) |
| `pixfmt` | `u8` | pixel format of the frames that follow |
| `flags` | `u8` | bit 0 `SREMFB_SRV_FLAG_PING` = PINGs may appear · bit 1 `SREMFB_SRV_FLAG_H264` = the stream may switch to H.264. The server only sets a bit the client advertised; older servers always send 0 here |
| `reserved[2]` | `u8` | reserved |

Status codes (`enum sremfb_status`):

| Value | Name | Meaning |
|---|---|---|
| 0 | `OK` | stream to follow |
| 1 | `BAD_HELLO` | incompatible or malformed client hello |
| 2 | `SERVER_FAIL` | the compositor never lit the connector |
| 3 | `NO_DEVICE` | no free EVDI device for this client |

### `sremfb_frame_hdr` — 20 bytes, server → client

One header per message, followed by `payload_len` bytes. BLANK/UNBLANK
control messages carry no payload and a 0×0 rect.

| Field | Type | Purpose |
|---|---|---|
| `magic` | `u32` | `SREMFB_MAGIC` (resync / corruption guard) |
| `encoding` | `u8` | `enum sremfb_encoding` |
| `reserved[3]` | `u8` | H264: `reserved[0]` bit 0 = `SREMFB_H264_FLAG_IDR` (informational); otherwise reserved |
| `x`, `y`, `w`, `h` | `u16` | destination rect, in stream coordinates |
| `payload_len` | `u32` | RAW: `w*h*bytespp`; LZ4: block size; BLANK/UNBLANK/H264_EOS: 0; PING: 8; H264: access-unit size |

Encodings (`enum sremfb_encoding`):

| Value | Name | Payload |
|---|---|---|
| 0 | `RAW` | `w*h*bytespp` raw pixels, no padding |
| 1 | `LZ4` | one LZ4 block of those raw pixels |
| 2 | `BLANK` | none: turn the panel off (DPMS off) |
| 3 | `UNBLANK` | none: turn the panel back on |
| 4 | `PING` | 8 B: `u64` server monotonic clock (µs), echo it back verbatim in a PONG. 0×0 rect |
| 5 | `H264` | one H.264 Annex B access unit (4:2:0, no B-frames, decode order = display order); the rect is always the full stream |
| 6 | `H264_EOS` | none: the H.264 episode is over — drain the decoder, display everything, then resume reading |

### `sremfb_client_msg` — 16 bytes, client → server

The only upstream message beyond the hello. Sent only when the server
hello advertised `SREMFB_SRV_FLAG_PING` (older servers read and discard
upstream bytes, so a confused client does no harm).

| Field | Type | Purpose |
|---|---|---|
| `magic` | `u32` | `SREMFB_MAGIC` |
| `type` | `u8` | 1 = `SREMFB_CMSG_PONG` |
| `reserved[3]` | `u8` | reserved |
| `t_echo_us` | `u64` | the PING payload, verbatim |

The client emits the PONG **at its position in the receive stream**,
after applying every frame that preceded the PING. TCP being ordered,
the server-side `now − t_echo_us` therefore measures the end-to-end
delay of everything queued ahead — kernel send buffer, network queues
and client processing. That delay is the congestion signal driving the
adaptive encoder (see below); only the server's clock is involved, no
synchronization needed.

## Pixels

`enum sremfb_pixfmt`:

- `SREMFB_PIX_XRGB8888` (0) — 4 B/px, memory order B,G,R,X (DRM XRGB8888
  little-endian). Passthrough to a 32bpp framebuffer.
- `SREMFB_PIX_RGB565` (1) — 2 B/px, `u16` LE, `r<<11 | g<<5 | b`. The
  server converts from EVDI's BGRx, with screen-aligned ordered (Bayer)
  dithering (`SREMFB_NO_DITHER=1` to turn it off).

The effective frame format is the one announced in
`server_hello.pixfmt`.

## Damage

The server only emits a `frame_hdr` on damage: the EVDI kernel module
merges the changed areas into at most 16 rectangles and only those
rectangles are sent. Static screen = zero pixel traffic (just the
28-byte liveness heartbeat every ~2 s when negotiated). Each rectangle
is compressed with LZ4 (falls back to RAW if the client didn't set the
flag, or if LZ4 doesn't help).

## Adaptive H.264

When both sides advertised it, the server switches the stream to H.264
under **measured** congestion (echo delay too high, or delivered rate
below ~15 fps while damage keeps coming) and back once it subsides —
nothing is configured, the link capacity is learned from what actually
drains while the delay says the link is saturated. The stream stays
damage-driven in H.264 mode: one full-frame access unit per damage
event (unchanged areas cost nothing thanks to skip blocks), static
screen = still zero traffic.

Ordering rules that make the switches seamless:

- **RAW → H264**: the first access unit is an IDR (with SPS/PPS), so it
  repaints the full frame; no gap.
- **H264 → RAW**: the server sends `H264_EOS`, then a **full-frame
  RAW/LZ4 repaint**, then normal rects. The client must finish draining
  and displaying its decoder output *before* reading on, so the repaint
  always lands last and the screen ends pixel-exact.
- A new episode always restarts with an IDR.
- `PING` may appear anywhere in the stream, in either mode.

The encoded stream is 4:2:0 (High profile at most), BT.601 limited
range, without B-frames — decodable in order by the V4L2 stateful
hardware decoders of common SBCs (e.g. Raspberry Pi ≤ 3).

## Version compatibility

The v1 magic is kept, but `proto_ver` is checked when the hello is
received: a v1 client (24 B) is rejected by a v2 server with `BAD_HELLO`.
The `reserved[]` fields allow the structs to be extended without changing
their size, as long as they stay zero on the older peer's side — that is
exactly how the v2 feature bits were added: an old client leaves bits
1-2 clear (server never pings nor encodes), an old server sends a zero
`flags` byte (client never writes upstream), and every combination keeps
the plain v2 behavior.
