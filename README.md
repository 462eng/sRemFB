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

## Supported SBCs

| SBC | Status | Notes |
|---|---|---|
| Raspberry Pi 5 / 500 / 500+ | ✅ full support | flawless at 60 fps, no lag |
| Raspberry Pi 4 / 400 | ✅ supported | not tested yet |
| Raspberry Pi 3B+ | ✅ supported | usable at 30 fps, delays on dynamic content |
| Raspberry Pi 3B | ✅ supported | not tested yet |
| Raspberry Pi 2 | ✅ supported | not tested; reduced performance expected (100 Mbit limit) |
| Raspberry Pi 1 | 🟡 likely supported | not tested; reduced performance expected (100 Mbit limit) |
| Banana Pi M1+ | ✅ supported | RGB565 recommended; RAM-limited (videos mostly fine) |

## Testers wanted & upcoming features

**Testers welcome!** The boards marked "not tested" above are waiting for
you, and other SBCs stand a good chance too: the client only needs a
framebuffer (`/dev/fb0`) and libc — Orange Pi, Odroid, Rockchip, other
Allwinner boards… Open an
[issue](https://github.com/462eng/sRemFB/issues) with the board, OS,
resolution and how it feels (fps, latency) — even a simple "it works"
helps.

**In the works** (1.4.x line):

- **SPICE / Proxmox backend — virtual infrastructure on thin clients.**
  The goal: run a fleet of **fully virtualized** workstations (QEMU/KVM
  VMs on Proxmox) where each physical seat is just a $30 SBC behind the
  screen — and **feels exactly like a local PC, indistinguishable in
  use**: smooth display, local keyboard/mouse/USB (teleport), boots
  straight to the desk. The server captures the VM display (spice-glib)
  and streams it to the same clients, no EVDI involved. Working
  prototype validated against QEMU; Proxmox testing underway.
- **Native DRM/KMS client output** — direct scanout (RGB565 or XRGB8888)
  bypassing fbdev, tear-free page-flip on full frames. Validated on a
  Pi 500, running in production at the author's.
- **Multi-display per SBC** — drive both HDMI outputs of a Pi 4/5 as two
  independent monitors (or two VM heads).
- **Audio** — done for `sremfb-view` (raw PCM over UDP, see below);
  still to come: the SBC client, and a codec (Opus) for Wi-Fi.
- **USB redirection into VMs** — today's USB teleport, extended to the
  thin-client scenario (usbredir).

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
- **Measured congestion control**: the server periodically slips a PING
  into the stream and the client echoes it back — the echo delay is the
  end-to-end congestion of everything queued ahead. When that delay says
  the raw path can't hold ~15 fps (e.g. a very dynamic screen behind a
  Pi 3's 100 Mbit port), the server transparently switches that client
  to **H.264** (x264, zerolatency, bitrate driven by the same delay
  signal) and back once the pressure subsides. Per-client and fully
  negotiated: the client advertises the capability only if it has a
  V4L2 stateful hardware decoder (Raspberry Pi ≤ 3: `/dev/video10`);
  everything else keeps the plain RAW/LZ4 path. On the SBC the display
  runs in its own thread and drops late frames: neither the decoder nor
  the network ever blocks on the framebuffer write, and the panel always
  shows the freshest frame. No limits to configure — the link capacity
  is learned by measurement. The same PINGs double as
  a liveness heartbeat: a network cut unplugs the virtual monitor and
  drops the panel to "no signal" in ~6 s (~20-25 s TCP fallback with an
  older peer).
- **Panel hotplug pass-through**: if the SBC's panel is unplugged, the
  client disconnects and the virtual monitor vanishes from the desktop —
  exactly like pulling the cable of a real screen. Replugging it brings
  the monitor back (watched via the DRM connector status, ~2 s).
- **USB teleport**: devices plugged into the SBC (keyboard, mouse,
  storage, serial adapters) are "teleported" to the PC over **usbip**
  while the client streams — plugged in behind the screen, used as
  local PC devices, detached as soon as the client leaves. Measured
  policy, not configured: HID + storage + serial; never hubs, never a
  device carrying an active network interface (a Pi's Ethernet is
  USB!), never a mounted disk; overrides via
  `SREMFB_USB_ALLOW`/`SREMFB_USB_DENY` (hex `vendor[:product]`).
  Disabled by default on a Pi 3 (`SREMFB_USB=1` or `--usb` to force).
  Requires the `usbip` package on both sides; `usbipd` listens on the
  SBC's port 3240 — same trust model as the rest (dedicated LAN).
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
sudo apt install build-essential libglib2.0-dev liblz4-dev libx264-dev evdi-dkms libevdi-dev
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
| `SREMFB_TTY` (client) | `/dev/tty1` | VT taken over (foreground + graphics mode); use one with no getty, e.g. `/dev/tty7` |
| `SREMFB_WRITE_MODE` (client) | `mmap` | `pwrite` if the display lags (deferred-io) |
| `SREMFB_MAC` (client) | auto | override the announced MAC (= monitor identity) |
| `SREMFB_MODEL` (client) | auto | override the announced model name (13 chars max) |
| `SREMFB_NO_LZ4` (client) | — | force raw (compression A/B test) |
| `SREMFB_NO_DITHER` (server) | — | disable RGB565 dithering (A/B test) |
| `SREMFB_NO_H264` (client) | — | don't advertise the hardware H.264 decoder |
| `SREMFB_NO_H264` (server) | — | never switch to H.264 (delay still measured/logged) |
| `SREMFB_FORCE_H264` (server) | — | pin H.264 on capable clients (A/B test) |
| `SREMFB_NO_HOTPLUG` (client) | — | don't watch the panel's DRM connector |
| `SREMFB_USB` (client) | auto | `1`/`0`: force/disable USB teleport (auto: on except Pi 3) |
| `SREMFB_USB_ALLOW` (client) | — | `vendor[:product]` ids always teleported (network/disk guards still win) |
| `SREMFB_USB_DENY` (client) | — | `vendor[:product]` ids never teleported |
| `SREMFB_NO_USB` (server) | — | never attach clients' USB devices |
| `SREMFB_INPUT` (server) | — | `1` = replay allowed clients' keyboard/mouse/gamepad on uinput devices (off by default) |
| `SREMFB_AUDIO` (server) | `1` | `0` = no sound for the clients that ask (otherwise: one PipeWire output per client, the default while it is connected; UDP, same port) |

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
> that card are ignored). The server guards against this four ways:
> devices are kept open in a pool for the whole process lifetime; devices
> are **regenerated from scratch on every startup** (a boot-time service,
> `sremfb-evdi-perms.service`, opens `/sys/devices/evdi/{add,remove_all}`
> to the `video` group — the session user must be a member); devices that
> fail to light up within 10 s are quarantined (the client's next
> reconnect picks another one); and once a wedge has been seen, the
> server **self-heals**: it creates a brand-new device at acquire time
> and plugs it immediately — mutter only accepts a card for a few
> seconds after its creation (bounded budget; if it runs out, a session
> re-login clears mutter).
>
> One case remains outside the server's reach: at boot, `evdi` is loaded
> with its initial devices *before* GDM, so gnome-shell grabs them when
> the session opens, and the server's first startup then regenerates them
> from under mutter — the wedge from the very first second ("compositor
> did not light up the connector within 10s" in a loop). Hence a GDM
> drop-in, `gdm.service.d/sremfb-evdi-purge.conf`, installed by the package
> and by `make install-server`, which purges the EVDI devices before GDM
> starts: the server then creates fresh cards that mutter discovers at
> hotplug. It takes effect on the next GDM restart (`systemctl restart
> gdm3`, or reboot) — a plain re-login is not enough.
>
> Another trap, mid-session: mutter **never** forgets a removed card
> (`remove_all` from a mode switch, a server reset); a new card that gets
> the same `/dev/dri/cardN` number is refused as a duplicate ("Failed to
> hotplug secondary gpu: device already present") and its connector never
> lights up. So the server creates its cards one at a time and only keeps
> those gnome-shell actually opens (seen in `/proc/<pid>/fd`); refused
> numbers stay in place as "dead" cards, never handed out, so the next one
> gets a fresh number. The state lives in
> `$XDG_RUNTIME_DIR/sremfb-evdi-cards` for gnome-shell's lifetime. Limit:
> every stop/`remove_all`/start cycle uses up ~2 numbers (the kernel does
> not give them all back); past card63, log out and back in.

## Windowed viewer

`sremfb-view` is a client for **Linux desktops**: it plugs a virtual
monitor into a remote `sremfb-server` exactly like an SBC does, and
shows it in a window (SDL3, native Wayland) — to use another GNOME
machine from your own desk, e.g. a GPU server.

```sh
sremfb-view [options] <server>       # sremfb-view --help for the details
```

- Built for **latency**: a network thread receives and decompresses
  (LZ4) as soon as bytes arrive and echoes the PINGs; the display thread
  uploads only the damaged rects and always presents the newest picture
  — no frame queue anywhere. VSync is off by default (`--vsync` to turn
  it on). **Never H.264**: the capability isn't advertised, the stream
  stays on RAW/LZ4 rects.
- Geometry `--size WxH` (default 1920x1080), XRGB8888 or `--rgb565`
  (half the bandwidth, dithered by the server).
- Identity: a **locally administered** MAC derived from
  `/etc/machine-id` (stable, never a real NIC's; `--mac` to override),
  model "sremfb-view" (`--model`). GNOME remembers this screen's
  position just like an SBC's.
- **Keyboard, mouse and gamepad** go to the remote desktop when its
  server allows it (`SREMFB_INPUT=1`, off by default; see
  [PROTOCOL.md](PROTOCOL.md#input)), replayed there on uinput devices —
  keys by physical position (the server's layout applies), gamepad seen
  as an Xbox 360 pad by Steam and SDL. Nothing stays held down: focus
  loss, disconnect or a lost link release everything on the remote side.
- Mouse **absolute** by default (desktop/KVM use: the remote cursor
  follows yours). **Tap Right Ctrl** to **capture** it for games:
  relative mouse (raw, pointer locked) and a keyboard grab, so Super,
  Alt+Tab… go to the remote side too (GNOME asks once to allow the
  shortcut inhibition). Tap Right Ctrl again to release; focus loss
  releases too. Right Ctrl is never forwarded; **Right Ctrl+F**
  fullscreen, **Right Ctrl+Q** quit. The title shows the mode.
- Without input (old server, `SREMFB_INPUT` off, `--no-input`): view
  only, **F11** fullscreen, **hold Escape 1 s** to quit. Closing the
  window always quits. Resizable window (aspect ratio kept, black bars).
- `--latency-test N` measures input → remote render → capture → network
  → decoded here, on the viewer's clock alone, against
  `sremfb-latency-probe` (package tool, python3-gi + GTK 4) running
  fullscreen on the virtual screen: min/median/p95/max over N trials,
  for a key (`--latency-input key`), the absolute pointer (`abs`), the
  relative mouse (`rel`) — the probe window's reaction — or the remote
  cursor itself reaching the probe pixel (`cursor`, probe started with
  `--ignore-motion`). "rx" = pixel decoded, "shown" = after the
  local `SDL_RenderPresent`; the local compositor, scanout and the
  monitor's own lag are not counted.
- **Sound** from the remote desktop (server ≥ 1.4.1+holo3, PipeWire):
  while the viewer is connected, the server creates an output
  "sRemFB <model>", makes it the **default output** (games, Steam, the
  desktop move onto it) and puts the previous one back when it leaves.
  Sound arrives as **raw PCM** 48 kHz stereo (no codec, no encoding
  delay) on a separate UDP flow (same port number, opened by the viewer:
  nothing to open in a client-side firewall), 2.7 ms packets, and goes
  to this machine's default output through SDL3. **Short** buffer:
  10 ms target (`--audio-buffer MS`), 128-frame device period
  (`--audio-frames`); beyond target + 10 ms packets are **dropped** to
  get back to it — latency never drifts; a lost packet = as much
  silence; the drift between the two sound clocks is corrected smoothly
  (±0.3 % at most). Measured on a 2.5 GbE link: about 15 ms from the
  input event to the sound leaving the sound card (estimate, see
  `--latency-test`). `--no-audio` to go without.
- No gamepad rumble yet (force feedback not forwarded).
- Disconnects handled like the SBC client: 6 s heartbeat, automatic
  reconnection with backoff.
- `--stats`: with sound, an extra `audio:` line — packets/s, lost,
  late, dropped, underruns, buffer depth, network delay (clocks aligned
  by the UDP ECHOs), drift correction, estimated latency (network +
  buffer + one packet + device period; the local sound server and DAC
  not counted). `--latency-test` with `sremfb-latency-probe --click`:
  every event also clicks in the server's default output, and the test
  adds "snd rx" (click received) and "snd out" (click leaving the sound
  card, estimated).
- `--stats`: every 5 s on stderr, received and presented fps, MB/s, LZ4
  time, upload+present time, queueing delay estimated from the PINGs.
  `--dump N`: saves N frames as PPM (validation without a screen).

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
- A slow client never delays the others: sends are **non-blocking** and
  each client has its own queue. Damage accumulates there as a dirty
  region and the next frame is only built (converted, compressed or
  encoded) when that client's socket can take it, **from the freshest
  pixels available** — a lagging client receives fewer frames, never
  stale ones, and nobody else notices.

## License

[MIT](LICENSE) — © 2026 Jonathan Roth.
