# Network protocol

**English** · [Français](PROTOCOL.fr.md)

sRemFB's application protocol, shared verbatim by server and client in
[`protocol.h`](protocol.h). Version described here: **v2**
(`SREMFB_PROTO_VER = 2`), including its **feature bits** (delay feedback,
adaptive H.264, keyboard/mouse/gamepad input, sound, negotiated through
the hellos without a version bump — every old/new combination stays
compatible).

## Transport

- **TCP**, a single port (default **4629**), with several simultaneous
  clients told apart by their MAC address (see [README.md](README.md)).
- **UDP** on the same port number, only for sound when negotiated (see
  "Sound").
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
client → server   :  sremfb_input_msg INPUT (16 B, input events, when
                      negotiated)
server → client   :  sremfb_frame_hdr H264 + access unit   (under
                     measured congestion, if negotiated; H264_EOS ends
                     the episode)

UDP, when sound is negotiated:
client → server   :  sremfb_udp_hello AUDIO_HELLO (16 B, ≥ 1/s)
server → client   :  sremfb_udp_echo ECHO (24 B, one per hello)
server → client   :  sremfb_audio_hdr + PCM (≈ 1 packet / 2.7 ms while
                     something plays)
```

## Messages

### `sremfb_client_hello` — 48 bytes, client → server

Sent exactly once, right after the connection.

| Field | Type | Purpose |
|---|---|---|
| `magic` | `u32` | `SREMFB_MAGIC` |
| `proto_ver` | `u16` | `SREMFB_PROTO_VER` (2) |
| `flags` | `u16` | bit 0 `SREMFB_HELLO_FLAG_LZ4` = the client accepts LZ4 · bit 1 `SREMFB_HELLO_FLAG_FEEDBACK` = the client echoes PING as PONG · bit 2 `SREMFB_HELLO_FLAG_H264` = the client decodes H.264 (4:2:0, Annex B) at its resolution · bit 3 `SREMFB_HELLO_FLAG_USB` = the client exports USB devices over usbip (see "USB teleport") · bit 4 `SREMFB_HELLO_FLAG_INPUT` = the client would like to send its input (see "Input") · bit 5 `SREMFB_HELLO_FLAG_AUDIO` = the client plays the server's sound (see "Sound") |
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
| `flags` | `u8` | bit 0 `SREMFB_SRV_FLAG_PING` = PINGs may appear · bit 1 `SREMFB_SRV_FLAG_H264` = the stream may switch to H.264 · bit 2 `SREMFB_SRV_FLAG_INPUT` = INPUT messages are accepted (devices created) · bit 3 `SREMFB_SRV_FLAG_AUDIO` = the client's sound output exists, open the UDP flow. The server only sets a bit the client advertised; older servers always send 0 here |
| `audio_token` | `u16` | with `SREMFB_SRV_FLAG_AUDIO`: token to quote in the UDP AUDIO_HELLOs (formerly `reserved[2]`, always 0 from older servers) |

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

Every upstream message after the hello is **16 bytes** and starts with
`magic` + `type`; the server ignores types it doesn't know (and, on
corruption, skips byte-wise to the next magic, dropping the client past
256 bytes of garbage). Two types:

- `SREMFB_CMSG_PONG` (1), sent only when the server hello advertised
  `SREMFB_SRV_FLAG_PING`;
- `SREMFB_CMSG_INPUT` (2), sent only when the server hello advertised
  `SREMFB_SRV_FLAG_INPUT` (struct `sremfb_input_msg`, see "Input").

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

### `sremfb_input_msg` — 16 bytes, client → server

One Linux evdev event (values from `linux/input-event-codes.h`), same
size and header as `sremfb_client_msg`.

| Field | Type | Purpose |
|---|---|---|
| `magic` | `u32` | `SREMFB_MAGIC` |
| `type` | `u8` | 2 = `SREMFB_CMSG_INPUT` |
| `dev` | `u8` | `enum sremfb_indev`: target device |
| `reserved[2]` | `u8` | reserved (0) |
| `ev_type` | `u16` | `EV_SYN`, `EV_KEY`, `EV_REL` or `EV_ABS` |
| `ev_code` | `u16` | evdev code (`KEY_A`, `BTN_LEFT`, `REL_X`, `ABS_X`…) |
| `ev_value` | `i32` | evdev value |

Devices (`enum sremfb_indev`), each a separate uinput device on the
server:

| Value | Name | Events |
|---|---|---|
| 0 | `ALL` | only with `ev_type` 0: release every key and button held on every device (focus lost); the gamepad goes back to neutral |
| 1 | `KEYBOARD` | `EV_KEY` `KEY_*` (physical positions; the server's layout gives the characters) |
| 2 | `MOUSE` | relative: `REL_X`/`REL_Y`, `REL_WHEEL`/`REL_HWHEEL` (+ `_HI_RES`, 1/120 notch), `BTN_LEFT/RIGHT/MIDDLE/SIDE/EXTRA` |
| 3 | `POINTER` | absolute: `ABS_X`/`ABS_Y` in **stream pixels** (0..width−1, 0..height−1); the server places them on the client's virtual screen; buttons and wheels as `MOUSE` |
| 4 | `GAMEPAD` | Xbox 360 (xpad) layout: `BTN_A/B/X/Y`, `BTN_TL/TR`, `BTN_SELECT/START/MODE`, `BTN_THUMBL/R`; `ABS_X/Y/RX/RY` −32768..32767 (+Y down), triggers `ABS_Z/RZ` 0..255, d-pad `ABS_HAT0X/Y` −1..1 |

## Input

Negotiation, both ways:

1. the client sets `SREMFB_HELLO_FLAG_INPUT` in its hello;
2. the server creates the client's uinput devices **before** sending its
   hello, and only sets `SREMFB_SRV_FLAG_INPUT` if they exist and the
   administrator allowed it (`SREMFB_INPUT=1`, off by default, on top of
   the CIDR allowlist);
3. the client sends **no** INPUT message until that bit arrives, and the
   server ignores INPUT from a client it did not set it for.

Each device applies its events at the `EV_SYN`/`SYN_REPORT` the client
sends explicitly, like a kernel driver (typically one action's events +
a SYN in a single `write`). No autorepeat: the server's compositor
repeats, as with a USB keyboard. The server filters anything undeclared
(codes, ranges) as well as doubled presses and releases of keys that are
not down.

**No stuck keys**: when the client disconnects or is lost (including the
6 s watchdog), the server releases everything still held, re-centers the
gamepad, then destroys the devices; the client sends `ALL` when its
window loses the focus.

Absolute pointer: the server reads the compositor's monitor layout
(`org.gnome.Mutter.DisplayConfig.GetCurrentState`, re-read on every
`MonitorsChanged`), finds the client's virtual screen by its connector
(the EVDI card's `DVI-I-N`), and turns stream pixels into 0..32767
coordinates over the global extents — what libinput expects from an
absolute pointer such as QEMU's USB tablet. Until the layout knows the
connector, absolute events are dropped.

Gamepad: "Microsoft X-Box 360 pad", USB `045e:028e`, so Steam and SDL
apply their stock mapping. No force feedback (rumble) yet.

## Sound

Negotiated both ways, like input:

1. the client sets `SREMFB_HELLO_FLAG_AUDIO` in its hello;
2. the server creates the client's sound output **before** sending its
   hello (a PipeWire output "sRemFB <model>", class `Audio/Sink`, which
   becomes the desktop's default output while the client is there) and
   only sets `SREMFB_SRV_FLAG_AUDIO` + `audio_token` when it exists and
   the administrator did not turn sound off (`SREMFB_AUDIO=0`);
3. the client only opens the UDP flow once that bit is received.

The UDP flow (same port number as TCP) is opened **by the client**: from
the socket it listens on, it sends `AUDIO_HELLO`s carrying the token,
every 250 ms until the first packet, then every second (that is also
the keepalive; after 5 s without a hello the server stops sending). The
server only accepts a hello from the TCP peer's IP address with the
right token, answers each one with an `ECHO` and sends the sound to the
source address. Firewalls and NAT only see a flow going out of the
client. Every datagram starts with `magic` + `type` (`enum
sremfb_udp_type`).

### `sremfb_udp_hello` — 16 bytes, client → server (UDP)

| Field | Type | Role |
|---|---|---|
| `magic` | `u32` | `SREMFB_MAGIC` |
| `type` | `u8` | 2 = `SREMFB_UDP_AUDIO_HELLO` |
| `reserved` | `u8` | 0 |
| `token` | `u16` | `server_hello.audio_token` |
| `t_client_us` | `u64` | client monotonic clock (µs), returned in the ECHO |

### `sremfb_udp_echo` — 24 bytes, server → client (UDP)

| Field | Type | Role |
|---|---|---|
| `magic` | `u32` | `SREMFB_MAGIC` |
| `type` | `u8` | 3 = `SREMFB_UDP_ECHO` |
| `reserved[3]` | `u8` | 0 |
| `t_client_us` | `u64` | the hello's, verbatim |
| `t_server_us` | `u64` | server monotonic clock (µs) at reception |

With t1 = `t_client_us`, t2 = `t_server_us`, t3 = the ECHO's arrival:
clock offset ≈ t2 − (t1 + t3)/2, within ± RTT/2 (the client keeps the
sample with the smallest RTT among the last few). It only feeds the
latency statistics.

### `sremfb_audio_hdr` — 24 bytes + PCM, server → client (UDP)

| Field | Type | Role |
|---|---|---|
| `magic` | `u32` | `SREMFB_MAGIC` |
| `type` | `u8` | 1 = `SREMFB_UDP_AUDIO` |
| `format` | `u8` | 0 = `SREMFB_AUDIO_S16LE_48K_STEREO` (the only format) |
| `frames` | `u16` | frames that follow (1..240) |
| `seq` | `u32` | +1 per packet; a gap = a lost packet |
| `reserved` | `u32` | 0 |
| `t_us` | `u64` | server monotonic clock (µs) when the first frame left the audio graph |

Followed by `frames × 4` bytes of interleaved S16LE stereo PCM at
48 kHz, **no codec**. The server sends one packet per PipeWire graph
cycle (it asks for a 128-frame quantum, 2.7 ms; at most 240 frames =
5 ms per packet, DSCP EF), nothing while nothing plays (output
suspended). No retransmission. The client handles jitter: a short
target buffer, lost packets replaced by as much silence, late packets
ignored, and latency **never** drifts (beyond the target + 10 ms it
drops packets until back on the target) — see
[README.md](README.md#windowed-viewer).

Rate: 192 kB/s of PCM (≈ 1.6 Mbit/s with headers). A codec (Opus) for
Wi-Fi or SBCs is an idea for later; it would get another `format`.

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

Same for input: an old server (≤ 1.4.1) ignores bit 4 of the client
hello and never advertises `SREMFB_SRV_FLAG_INPUT`, so a new client stays
view only; and even if it sent INPUT, those are 16-byte messages with a
magic, which the old server ignores without losing the framing (unknown
type). An old client never sets bit 4: the server creates nothing.

And for sound: an old server ignores bit 5 and sends `flags` bit 3 and
`audio_token` as zero — the new client opens no UDP flow; an old client
does not set bit 5 and ignores the `audio_token` bytes (formerly
`reserved`) — the server creates no sound output. Nothing changes on the
TCP connection.

## USB teleport

Out-of-band, no sRemFB message involved: when the client sets
`SREMFB_HELLO_FLAG_USB` it promises that a standard `usbipd` listens on
its TCP port 3240 with the eligible devices bound to `usbip-host`. The
server attaches those devices (vhci-hcd) while the client is streaming
and detaches them when it disconnects — the same 6 s liveness that
unplugs the virtual monitor also releases the USB devices. Everything
uses the stock usbip protocol; sRemFB only orchestrates the lifecycle.
