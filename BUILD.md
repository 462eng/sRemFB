# Building and deploying

**English** · [Français](BUILD.fr.md)

Two C binaries, no exotic build system — just plain `Makefile`s.

- `server/sremfb-server` — on the PC (x86-64, GNOME/Wayland).
- `client/sremfb-client` — on the SBC (ARM), or in test mode on the PC.

`make` at the top level builds both. Each subdirectory also has its own
`Makefile` if you only want one side.

## Dependencies

### Server (PC)

```sh
sudo apt install build-essential libglib2.0-dev liblz4-dev libx264-dev \
                 evdi-dkms libevdi-dev
```

- `glib-2.0` — event loop, sources, utilities.
- `liblz4` — rectangle compression.
- `libx264` — H.264 encoding for the adaptive-compression path (engaged
  per client, under measured congestion).
- `libevdi` + `evdi-dkms` — the EVDI kernel module (the DisplayLink
  driver) and its library. `evdi-dkms` builds the module for the running
  kernel, so a working `dkms` and the kernel headers are required.
- `-lm` (libc math) for dithering.

Tested with libevdi/evdi-dkms **1.14.8** (Debian trixie).

### Client (SBC)

Only build dependency: `liblz4-dev`.

```sh
sudo apt install build-essential liblz4-dev
```

The client is deliberately pure C: no GLib, no DRM, nothing but libc and
lz4 — the optional hardware H.264 decoding is plain V4L2 ioctls against
the kernel UAPI headers. It compiles as-is on Debian, Raspberry Pi OS
and Armbian.

## Local build

```sh
make                 # server/sremfb-server + client/sremfb-client
make -C server       # server only
make -C client       # client only
make clean
```

Default flags: `-O2 -g -Wall -Wextra`. Override via `CFLAGS`.

## Local install (no package)

### Server (on the PC, as root)

```sh
sudo make install-server
```

Installs:

- the binary into `/usr/local/bin/`;
- the systemd **user** unit into `/etc/systemd/user/` (the server runs in
  the graphical session, not as a system service);
- `/etc/modules-load.d/sremfb.conf` and `/etc/modprobe.d/sremfb.conf`
  (loads `evdi` at boot with `initial_device_count=2`);
- the hwdb `61-sremfb-display-vendor.hwdb` (EDID vendor label);
- the udev rule `60-sremfb-evdi.rules` (`video` group rights on EVDI
  device creation — see [README.md](README.md), "Multiple screens");
- `/etc/sremfb-server.conf` (unless it already exists).

Then, the first time:

```sh
sudo modprobe evdi     # automatic at boot afterwards
systemctl --user daemon-reload
systemctl --user enable --now sremfb-server
```

The session user must belong to the `video` group so the server can
regenerate the EVDI devices at startup (`id -nG | grep video`; otherwise
`sudo usermod -aG video $USER` then re-login).

### Client (on the SBC, as root)

```sh
sudo make install-client
sudo nano /etc/sremfb.conf         # SREMFB_SERVER=<PC ip>
sudo systemctl daemon-reload
sudo systemctl enable --now sremfb-client
```

`PREFIX` (default `/usr/local`) and `DESTDIR` are honored by both install
targets.

## Debian packages

`pkg/build-debs.sh` produces three `.deb`s in `dist/`:

| Package | Arch | Target |
|---|---|---|
| `sremfb-server` | amd64 | GNOME/Wayland PC |
| `sremfb-client` | arm64 | 64-bit SBC (Pi 3/4/5/500, etc.) |
| `sremfb-client` | armhf | ARMv7 SBC (Banana Pi M1+, Pi 2, etc.) |

```sh
./pkg/build-debs.sh              # version 1.2.0 by default
./pkg/build-debs.sh 3.1.0        # explicit version
```

Details:

- **Client statically linked to lz4.** The script downloads `liblz4-dev`
  for each target architecture (`apt-get download`, cached in
  `pkg/sysroot/`) and links `liblz4.a` in. The client package therefore
  depends only on `libc6` — no lz4 version constraint on the target.
- **Cross-compiling** the client: needs the `gcc-aarch64-linux-gnu` and
  `gcc-arm-linux-gnueabihf` toolchains, plus the `arm64`/`armhf`
  architectures enabled in dpkg for `apt-get download`:

  ```sh
  sudo apt install gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf
  sudo dpkg --add-architecture arm64
  sudo dpkg --add-architecture armhf
  sudo apt update
  ```

- **The kernel module is not repackaged**: `evdi-dkms` exists in Debian
  and is only needed on the server side (declared as a `Depends`).
- The packages **replace** the old `rfb-server`/`rfb-client`
  (`Conflicts`/`Replaces`). The client postinst automatically migrates
  `/etc/rfb.conf` → `/etc/sremfb.conf` (`RFB_*` → `SREMFB_*` variables) if
  the config hasn't been touched since installation.
- The server postinst updates the hwdb, reloads udev, loads `evdi`, and
  immediately applies the `video` group rights on
  `/sys/devices/evdi/{add,remove_all}`.

The package maintainer field defaults to `Jonathan Roth <jr@462eng.fr>`;
override it with the `MAINT` environment variable
(`MAINT="Name <you@example.com>" ./pkg/build-debs.sh`).

### Installing a package

```sh
# server
sudo apt install ./dist/sremfb-server_1.2.0_amd64.deb
# client (on the SBC)
sudo apt install ./dist/sremfb-client_1.2.0_arm64.deb
```

On an already-modified config, `dpkg -i --force-confold` keeps the
existing file.

## Publishing to an APT repository (optional)

To distribute the packages through an APT repository managed with
`reprepro` (adapt the host, codename and component to your infrastructure):

```sh
scp dist/*.deb REPO_HOST:/tmp/
ssh REPO_HOST 'reprepro -b /srv/debian -C main includedeb CODENAME /tmp/sremfb-*.deb'
```

The repository must declare the `amd64 arm64 armhf` architectures and may
be signed with the GPG key of your choice (`SignWith` in
`conf/distributions`).

## Testing without hardware

The client runs in test mode on the PC itself (no framebuffer, no console
takeover):

```sh
./server/sremfb-server &
./client/sremfb-client --test 1920x1080 localhost
```

A 1920×1080 screen appears in **Settings → Displays**. The client saves
one frame out of 30 to `sremfb-test-NNNN.ppm` (checking colors, blended
cursor, damage). A second test screen with a different MAC:

```sh
SREMFB_MAC=02:00:00:00:00:02 ./client/sremfb-client --test 1920x1080 localhost
```

Ctrl-C on the client → the screen "unplugs" on the PC side.
