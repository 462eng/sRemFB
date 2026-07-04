# Network protocol

**English** · [Français](PROTOCOL.fr.md)

sRemFB's application protocol, shared verbatim by server and client in
[`protocol.h`](protocol.h). Version described here: **v2**
(`SREMFB_PROTO_VER = 2`).

## Transport

- **TCP**, a single port (default **4629**), with several simultaneous
  clients told apart by their MAC address (see [README.md](README.md)).
- `TCP_NODELAY`, `SO_KEEPALIVE` (idle 10 s / intvl 5 s / cnt 3 — detects
  an unplugged SBC even without traffic), `SO_SNDTIMEO` 20 s,
  `SO_RCVTIMEO` 5 s. `SIGPIPE` ignored.
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
```

## Messages

### `sremfb_client_hello` — 48 bytes, client → server

Sent exactly once, right after the connection.

| Field | Type | Purpose |
|---|---|---|
| `magic` | `u32` | `SREMFB_MAGIC` |
| `proto_ver` | `u16` | `SREMFB_PROTO_VER` (2) |
| `flags` | `u16` | bit 0 `SREMFB_HELLO_FLAG_LZ4` = the client accepts LZ4 |
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
| `reserved[3]` | `u8` | reserved |

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
| `reserved[3]` | `u8` | reserved |
| `x`, `y`, `w`, `h` | `u16` | destination rect, in stream coordinates |
| `payload_len` | `u32` | RAW: `w*h*bytespp`; LZ4: block size; BLANK/UNBLANK: 0 |

Encodings (`enum sremfb_encoding`):

| Value | Name | Payload |
|---|---|---|
| 0 | `RAW` | `w*h*bytespp` raw pixels, no padding |
| 1 | `LZ4` | one LZ4 block of those raw pixels |
| 2 | `BLANK` | none: turn the panel off (DPMS off) |
| 3 | `UNBLANK` | none: turn the panel back on |

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
rectangles are sent. Static screen = zero traffic. Each rectangle is
compressed with LZ4 (falls back to RAW if the client didn't set the flag,
or if LZ4 doesn't help).

## Version compatibility

The v1 magic is kept, but `proto_ver` is checked when the hello is
received: a v1 client (24 B) is rejected by a v2 server with `BAD_HELLO`.
The `reserved[]` fields allow the structs to be extended without changing
their size, as long as they stay zero on the older peer's side.
