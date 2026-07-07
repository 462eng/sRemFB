#!/bin/sh -e
# Construit les paquets Debian de sRemFB dans dist/ :
#   sremfb-server_<ver>_amd64.deb         (PC GNOME/Wayland)
#   sremfb-client_<ver>_arm64.deb         (SBC 64 bits : Pi 3/4/5/500…)
#   sremfb-client_<ver>_armhf.deb         (SBC ARMv7 : Banana Pi M1+, Pi 2…)
#
# Le client est lié en STATIQUE avec liblz4 (extraite des paquets Debian
# de la cible, mises en cache dans pkg/sysroot/) : il ne dépend que de
# libc6, donc fonctionne tel quel sur Debian, Raspberry Pi OS et Armbian.
# Le module noyau n'est pas repackagé : evdi-dkms existe dans Debian et
# n'est nécessaire que côté serveur (déclaré en dépendance).
#
# Prérequis : gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf, et les
# architectures arm64/armhf activées dans dpkg pour apt-get download.

VERSION=${1:-1.1.0}
MAINT=${MAINT:-"Jonathan Roth <jr@462eng.fr>"}
TOP=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DIST=$TOP/dist
SYSROOT=$TOP/pkg/sysroot
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$DIST" "$SYSROOT"

# --- liblz4 statique des cibles (cache) --------------------------------
fetch_lz4() { # $1 = debian arch
    if [ ! -e "$SYSROOT/$1"/usr/lib/*/liblz4.a ]; then
        echo "== téléchargement liblz4-dev:$1"
        (cd "$SYSROOT" && apt-get download "liblz4-dev:$1" >/dev/null)
        mkdir -p "$SYSROOT/$1"
        dpkg -x "$SYSROOT"/liblz4-dev_*_"$1".deb "$SYSROOT/$1"
        rm -f "$SYSROOT"/liblz4-dev_*_"$1".deb
    fi
}
fetch_lz4 arm64
fetch_lz4 armhf

# --- binaires -----------------------------------------------------------
echo "== build serveur (amd64)"
make -s -C "$TOP/server"

build_client() { # $1 = debian arch, $2 = triplet gcc
    echo "== build client ($1)"
    "$2-gcc" -O2 -Wall -Wextra -I"$TOP" \
        -I"$SYSROOT/$1/usr/include" \
        -o "$STAGE/sremfb-client-$1" \
        "$TOP/client/sremfb-client.c" "$TOP/client/v4l2dec.c" \
        "$SYSROOT/$1/usr/lib/$2/liblz4.a"
    "$2-strip" "$STAGE/sremfb-client-$1"
}
build_client arm64 aarch64-linux-gnu
build_client armhf arm-linux-gnueabihf

# --- assemblage ---------------------------------------------------------
make_deb() { # $1 = nom, $2 = arch ; l'arborescence est déjà dans $ROOT
    mkdir -p "$ROOT/DEBIAN"
    cat > "$ROOT/DEBIAN/control" <<EOF
Package: $1
Version: $VERSION
Architecture: $2
Maintainer: $MAINT
Section: video
Priority: optional
Installed-Size: $(du -ks "$ROOT" | cut -f1)
$3
EOF
    dpkg-deb --build --root-owner-group "$ROOT" \
        "$DIST/${1}_${VERSION}_${2}.deb" >/dev/null
}

# ---- sremfb-server (amd64) ----
ROOT=$STAGE/server
mkdir -p "$ROOT/usr/bin" "$ROOT/usr/lib/systemd/user" \
         "$ROOT/usr/lib/systemd/system" \
         "$ROOT/usr/lib/udev/hwdb.d" "$ROOT/usr/lib/udev/rules.d" \
         "$ROOT/etc/modules-load.d" "$ROOT/etc/modprobe.d"
install -m 755 "$TOP/server/sremfb-server" "$ROOT/usr/bin/sremfb-server"
strip "$ROOT/usr/bin/sremfb-server"
sed 's|/usr/local/bin|/usr/bin|' "$TOP/systemd/sremfb-server.service" \
    > "$ROOT/usr/lib/systemd/user/sremfb-server.service"
install -m 644 "$TOP/systemd/sremfb-evdi-perms.service" \
    "$ROOT/usr/lib/systemd/system/sremfb-evdi-perms.service"
install -m 644 "$TOP/systemd/61-sremfb-display-vendor.hwdb" \
    "$ROOT/usr/lib/udev/hwdb.d/"
install -m 644 "$TOP/systemd/60-sremfb-evdi.rules" \
    "$ROOT/usr/lib/udev/rules.d/"
install -m 644 "$TOP/systemd/modules-load-sremfb.conf" \
    "$ROOT/etc/modules-load.d/sremfb.conf"
install -m 644 "$TOP/systemd/modprobe-sremfb.conf" \
    "$ROOT/etc/modprobe.d/sremfb.conf"
install -m 644 "$TOP/systemd/sremfb-server.conf.example" \
    "$ROOT/etc/sremfb-server.conf"
mkdir -p "$ROOT/DEBIAN"
printf '/etc/modules-load.d/sremfb.conf\n/etc/modprobe.d/sremfb.conf\n/etc/sremfb-server.conf\n' \
    > "$ROOT/DEBIAN/conffiles"
cat > "$ROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh -e
systemd-hwdb update || true
udevadm control --reload 2>/dev/null || true
modprobe evdi || true
# Le service oneshot pose les droits groupe video sur /sys/devices/evdi/*
# de façon fiable au boot (la règle udev seule ne suffit pas : son chmod
# court avant que les attributs existent). --now l'applique tout de suite.
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload 2>/dev/null || true
    systemctl enable --now sremfb-evdi-perms.service 2>/dev/null || true
fi
# secours si systemd absent (chroot, etc.)
if [ -e /sys/devices/evdi/add ]; then
    chgrp video /sys/devices/evdi/add /sys/devices/evdi/remove_all || true
    chmod 664 /sys/devices/evdi/add /sys/devices/evdi/remove_all || true
fi
echo "sremfb-server : plages autorisées dans /etc/sremfb-server.conf, puis :"
echo "  systemctl --user enable --now sremfb-server"
EOF
chmod 755 "$ROOT/DEBIAN/postinst"
cat > "$ROOT/DEBIAN/postrm" <<'EOF'
#!/bin/sh -e
if [ "$1" = remove ] || [ "$1" = purge ]; then
    if command -v systemctl >/dev/null 2>&1; then
        systemctl disable --now sremfb-evdi-perms.service 2>/dev/null || true
    fi
fi
EOF
chmod 755 "$ROOT/DEBIAN/postrm"
make_deb sremfb-server amd64 \
"Depends: libglib2.0-0t64, liblz4-1, libevdi1, evdi-dkms, libx264-164
Conflicts: rfb-server
Replaces: rfb-server
Description: sRemFB, écran virtuel réseau — serveur (connecteur EVDI)
 Expose un connecteur d'écran virtuel EVDI par client connecté (identifié
 par son adresse MAC) et transfère les zones modifiées, compressées en
 LZ4, vers les sremfb-client du LAN (allowlist CIDR). Mesure la
 congestion par le délai et bascule en H.264 (x264) les clients qui
 savent le décoder quand le lien sature."

# ---- sremfb-client (arm64 + armhf) ----
for arch in arm64 armhf; do
    ROOT=$STAGE/client-$arch
    mkdir -p "$ROOT/usr/bin" "$ROOT/usr/lib/systemd/system" "$ROOT/etc" \
             "$ROOT/usr/share/sremfb-client"
    install -m 755 "$STAGE/sremfb-client-$arch" "$ROOT/usr/bin/sremfb-client"
    sed 's|/usr/local/bin|/usr/bin|' "$TOP/systemd/sremfb-client.service" \
        > "$ROOT/usr/lib/systemd/system/sremfb-client.service"
    install -m 644 "$TOP/systemd/sremfb.conf.example" "$ROOT/etc/sremfb.conf"
    install -m 644 "$TOP/systemd/sremfb.conf.example" \
        "$ROOT/usr/share/sremfb-client/sremfb.conf.example"
    mkdir -p "$ROOT/DEBIAN"
    printf '/etc/sremfb.conf\n' > "$ROOT/DEBIAN/conffiles"
    cat > "$ROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh -e
# migration depuis rFb : reprend /etc/rfb.conf si /etc/sremfb.conf est
# encore l'exemple d'origine
if [ -f /etc/rfb.conf ] && \
   cmp -s /etc/sremfb.conf /usr/share/sremfb-client/sremfb.conf.example; then
    sed 's/^RFB_/SREMFB_/; s/^# *RFB_/# SREMFB_/' /etc/rfb.conf > /etc/sremfb.conf
    echo "sremfb-client : configuration migrée depuis /etc/rfb.conf"
fi
systemctl daemon-reload 2>/dev/null || true
echo "sremfb-client : vérifier SREMFB_SERVER dans /etc/sremfb.conf puis :"
echo "  systemctl enable --now sremfb-client"
EOF
    chmod 755 "$ROOT/DEBIAN/postinst"
    make_deb sremfb-client "$arch" \
"Depends: libc6
Conflicts: rfb-client
Replaces: rfb-client
Description: sRemFB, écran virtuel réseau — client framebuffer
 Reçoit les frames d'un sremfb-server et les écrit directement dans
 /dev/fb0 ; éteint la dalle quand le serveur est absent ou blanke,
 reflète le débranchement de la dalle, et décode le H.264 adaptatif
 en matériel (V4L2 M2M, ex. Pi 3) quand le SBC en dispose.
 LZ4 lié en statique : aucune autre dépendance."
done

echo "== paquets dans $DIST :"
ls -l "$DIST"
