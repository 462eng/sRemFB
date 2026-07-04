# sRemFB — simple Remote Frame Buffer

**English** · [Français](README.fr.md)

A minimal "network monitor": `sremfb-server` exposes **one virtual
display connector per client** (the EVDI kernel module, i.e. the
DisplayLink driver) on a GNOME/Wayland PC and streams the image over the
LAN to `sremfb-client`, a console daemon that writes the pixels straight
into the `/dev/fb0` of an SBC (Raspberry Pi, Banana Pi…).

Each connector behaves **like a physical port**: while no client is
connected the port is "unplugged" and invisible. When a client connects
it's a hotplug — GNOME extends the desktop and restores the screen's
position. Clients are **identified by their MAC address** (the EDID
serial is derived from the MAC, the model name is taken from the EDID of
the panel attached to the SBC): each physical client keeps its own
position remembered in `monitors.xml`, whatever the connection order.
When the client leaves, it's an unplugged cable. No capture session, so
**no screen-recording indicator**, and the lock screen shows just like on
a real monitor. On the SBC side the panel is **turned off**
(`FBIOBLANK`) while the server is unreachable — "no signal" — and when
GNOME blanks its outputs (DPMS pass-through).

> **Designed for a dedicated, trusted LAN** — no encryption, no
> authentication; a CIDR allowlist (`SREMFB_ALLOW`) still restricts which
> addresses are accepted.

## Documentation

- **[BUILD.md](BUILD.md)** — dependencies, building, installation, Debian
  packages, cross-compilation, testing without hardware.
- **[PROTOCOL.md](PROTOCOL.md)** — the network protocol (v2), field by
  field.

## How it works

- In its hello the client announces: the framebuffer geometry
  (`FBIOGET_VSCREENINFO`), the **MAC** of the interface it uses, and the
  "vendor model" of the attached panel (read from
  `/sys/class/drm/*/edid`). The server builds an EDID of that exact size
  (vendor `RFB`, product = the remote panel's model, serial = the MAC)
  and "plugs" it into a free EVDI device. The compositor drives it like
  any monitor; the server pulls the pixels through `libevdi` (the cursor
  is blended in by the kernel).
- **Several clients at once** on a single port: one EVDI device per
  client (see "Multiple screens"). A reconnect from the same MAC replaces
  the stale connection (SBC rebooted).
- The EDID advertises the resolution at **60 Hz and 30 Hz**: the source
  frame rate can be capped from Settings → Displays → Refresh Rate.
- Damage-driven transfer: the kernel merges the changed areas into at
  most 16 rectangles and only those rectangles are sent → a static screen
  means zero traffic. Each rectangle is compressed with **LZ4** (raw
  fallback).
- Client-side formats: 32bpp XRGB8888 (passthrough) and 16bpp RGB565
  (server-side conversion with ordered dithering; `SREMFB_NO_DITHER=1` to
  turn it off).
- When GNOME powers the displays off, the server sends a **BLANK**
  message: the client turns the panel off (`FBIOBLANK`) and turns it back
  on when it returns.
- Per-client stats every 5 s in the journal: fps, throughput,
  compression ratio (`journalctl --user -u sremfb-server -f`).

## Quick start

Full details and Debian packages: **[BUILD.md](BUILD.md)**.

PC (server):

```sh
sudo apt install build-essential libglib2.0-dev liblz4-dev evdi-dkms libevdi-dev
make
sudo make install-server
sudo modprobe evdi          # first time only (loaded at boot afterwards)
systemctl --user daemon-reload && systemctl --user enable --now sremfb-server
```

SBC (client), only dependency `liblz4-dev`:

```sh
make -C client
sudo make install-client
sudo nano /etc/sremfb.conf  # SREMFB_SERVER=<PC ip>
sudo systemctl daemon-reload && sudo systemctl enable --now sremfb-client
```

Testing without an SBC (on the PC):

```sh
./server/sremfb-server &
./client/sremfb-client --test 1920x1080 localhost
```

A 1920×1080 screen appears in Settings → Displays; the client writes one
frame out of 30 to `sremfb-test-NNNN.ppm`. Ctrl-C on the client → the
screen "unplugs".

## Configuration

| Variable | Default | Purpose |
|---|---|---|
| `SREMFB_PORT` (server & client) | 4629 | TCP port |
| `SREMFB_ALLOW` (server) | — | allowed IPv4 ranges, comma-separated CIDRs (empty = accept everything); `/etc/sremfb-server.conf` |
| `SREMFB_SERVER` (client) | — | server host/IP (required) |
| `SREMFB_FBDEV` (client) | `/dev/fb0` | framebuffer device |
| `SREMFB_TTY` (client) | `/dev/tty1` | VT switched to graphics mode |
| `SREMFB_WRITE_MODE` (client) | `mmap` | `pwrite` if the display lags (deferred-io) |
| `SREMFB_MAC` (client) | auto | override the announced MAC (= monitor identity) |
| `SREMFB_MODEL` (client) | auto | override the announced model name (13 chars max) |
| `SREMFB_NO_LZ4` (client) | — | force raw (compression A/B test) |
| `SREMFB_NO_DITHER` (server) | — | disable RGB565 dithering (A/B test) |

The client runs as root by default (`/dev/fb0` access + console ioctls).
The server runs as the session user: access to `/dev/dri/cardN` (the EVDI
device) comes from the logind seat ACL, and the EVDI ioctls are
unprivileged.

## Multiple screens

One connected client = one EVDI device. The number of devices created at
boot therefore sets how many screens can run at once (2 by default):

```sh
sudo sed -i 's/initial_device_count=2/initial_device_count=4/' /etc/modprobe.d/sremfb.conf
echo 2 | sudo tee /sys/devices/evdi/add     # without waiting for a reboot
```

Nothing else to configure: every SBC points at the same
`SREMFB_SERVER`/port and each is recognized by its MAC (independent GNOME
position).

> **mutter gotcha.** mutter does not survive reopening an EVDI device it
> was already driving ("Failed to reopen cardN: EBUSY", then hotplugs on
> that card are ignored). The server guards against this three ways:
> devices are kept open in a pool for the whole process lifetime; devices
> are **regenerated from scratch on every startup** (the udev rule
> `60-sremfb-evdi.rules` opens `/sys/devices/evdi/{add,remove_all}` to the
> `video` group — the session user must be a member); and as a last
> resort, devices that fail to light up within 10 s are quarantined (the
> client's next reconnect picks another one).

## Notes

- Each screen's position is set **once** in Settings → Displays; GNOME
  remembers it in `~/.config/monitors.xml`, indexed on the EDID identity
  (vendor `RFB` / panel model / serial = MAC). Changing the panel
  attached to the SBC changes the model, hence the identity — just like a
  real monitor swap.
- GNOME builds the Settings label as "vendor + diagonal". The udev hwdb
  (`61-sremfb-display-vendor.hwdb`) registers the EDID vendor `RFB` under
  the name **"462eng sRemFB"** → "462eng sRemFB 24\"" (after a session
  re-login, since gnome-shell keeps the old table in memory). The
  advertised diagonal is fixed (24").
- `install-server`/the package drop `/etc/modules-load.d/sremfb.conf` and
  `/etc/modprobe.d/sremfb.conf` (`evdi initial_device_count=2`). The
  devices show up as `cardN` with a `DVI-I-N` connector marked
  "disconnected".
- Validated on GNOME 48 Wayland (mutter drives the EVDI cards as a
  secondary GPU, the standard DisplayLink path). Under X11 the devices
  would need to be declared in xorg.conf — untested.
- Bandwidth: at 1080p/32bpp a full frame is ~8.3 MB, but thanks to damage
  only the changed rectangles go over the wire. RGB565 halves that.
  Static content: zero traffic.
- Known limitation: sends are blocking, so a congested or dead client can
  delay the others up to `SO_SNDTIMEO` (20 s) — head-of-line blocking. A
  possible fix (not implemented): non-blocking sockets + a "latest frame"
  buffer per client.

## License

[MIT](LICENSE) — © 2026 Jonathan Roth.
