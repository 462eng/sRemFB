#!/bin/sh -e
# Construit les paquets Debian de sRemFB dans dist/ :
#   sremfb-server_<ver>_amd64.deb         (PC GNOME/Wayland)
#   sremfb-client_<ver>_arm64.deb         (SBC 64 bits : Pi 3/4/5/500…)
#   sremfb-client_<ver>_armhf.deb         (SBC ARMv7 : Banana Pi M1+, Pi 2…)
#
# Le client est lié en STATIQUE avec liblz4 (extraite des paquets Debian
# de la cible, mises en cache dans pkg/sysroot/) et en DYNAMIQUE avec
# libdrm (sortie DRM/KMS opt-in) : il dépend de libc6 + libdrm2, présents
# tels quels sur Debian, Raspberry Pi OS et Armbian.
# Le module noyau n'est pas repackagé : evdi-dkms existe dans Debian et
# n'est nécessaire que côté serveur (déclaré en dépendance).
#
# Prérequis : gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf, et les
# architectures arm64/armhf activées dans dpkg pour apt-get download.

VERSION=${1:-1.4.0}
MAINT=${MAINT:-"Jonathan Roth <jr@462eng.fr>"}
TOP=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DIST=$TOP/dist
SYSROOT=$TOP/pkg/sysroot
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$DIST" "$SYSROOT"

# --- libs des cibles (cache) -------------------------------------------
# liblz4 en statique ; libdrm en dynamique : le sysroot fournit les
# en-têtes (libdrm-dev) et le .so (libdrm2 + symlink de libdrm-dev) pour
# l'édition de liens croisée.
fetch_pkg() { # $1 = debian arch, $2 = paquet, $3 = glob du fichier attendu
    if ! ls "$SYSROOT/$1"/$3 >/dev/null 2>&1; then
        echo "== téléchargement $2:$1"
        (cd "$SYSROOT" && apt-get download "$2:$1" >/dev/null)
        mkdir -p "$SYSROOT/$1"
        dpkg -x "$SYSROOT/$2"_*_"$1".deb "$SYSROOT/$1"
        rm -f "$SYSROOT/$2"_*_"$1".deb
    fi
}
for a in arm64 armhf; do
    fetch_pkg "$a" liblz4-dev 'usr/lib/*/liblz4.a'
    fetch_pkg "$a" libdrm2    'usr/lib/*/libdrm.so.2*'
    fetch_pkg "$a" libdrm-dev 'usr/lib/*/libdrm.so'
done

# --- binaires -----------------------------------------------------------
echo "== build serveur (amd64)"
make -s -C "$TOP/server"

build_client() { # $1 = debian arch, $2 = triplet gcc
    echo "== build client ($1)"
    "$2-gcc" -O2 -Wall -Wextra -pthread -I"$TOP" \
        -I"$SYSROOT/$1/usr/include" \
        -I"$SYSROOT/$1/usr/include/libdrm" \
        -o "$STAGE/sremfb-client-$1" \
        "$TOP/client/sremfb-client.c" "$TOP/client/v4l2dec.c" \
        "$TOP/client/usbexport.c" \
        "$TOP/client/output_fb.c" "$TOP/client/output_drm.c" \
        "$SYSROOT/$1/usr/lib/$2/liblz4.a" \
        -L"$SYSROOT/$1/usr/lib/$2" -ldrm
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
mkdir -p "$ROOT/usr/bin" "$ROOT/usr/libexec" "$ROOT/usr/lib/systemd/user" \
         "$ROOT/usr/lib/systemd/system" "$ROOT/usr/lib/tmpfiles.d" \
         "$ROOT/usr/lib/udev/hwdb.d" "$ROOT/usr/lib/udev/rules.d" \
         "$ROOT/etc/modules-load.d" "$ROOT/etc/modprobe.d"
install -m 755 "$TOP/server/sremfb-server" "$ROOT/usr/bin/sremfb-server"
strip "$ROOT/usr/bin/sremfb-server"
sed 's|/usr/local/bin|/usr/bin|' "$TOP/systemd/sremfb-server.service" \
    > "$ROOT/usr/lib/systemd/user/sremfb-server.service"
install -m 644 "$TOP/systemd/sremfb-evdi-perms.service" \
    "$ROOT/usr/lib/systemd/system/sremfb-evdi-perms.service"
install -m 755 "$TOP/systemd/sremfb-usb-attach" \
    "$ROOT/usr/libexec/sremfb-usb-attach"
sed 's|/usr/local/libexec|/usr/libexec|' "$TOP/systemd/sremfb-usb.service" \
    > "$ROOT/usr/lib/systemd/system/sremfb-usb.service"
install -m 644 "$TOP/systemd/sremfb-usb.path" \
    "$ROOT/usr/lib/systemd/system/sremfb-usb.path"
install -m 644 "$TOP/systemd/sremfb-usb.timer" \
    "$ROOT/usr/lib/systemd/system/sremfb-usb.timer"
install -m 644 "$TOP/systemd/tmpfiles-sremfb.conf" \
    "$ROOT/usr/lib/tmpfiles.d/sremfb.conf"
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
    # téléport USB : /run/sremfb + réconciliateur usbip (root)
    systemd-tmpfiles --create /usr/lib/tmpfiles.d/sremfb.conf 2>/dev/null || true
    systemctl enable --now sremfb-usb.path sremfb-usb.timer 2>/dev/null || true
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
        systemctl disable --now sremfb-usb.path sremfb-usb.timer 2>/dev/null || true
    fi
fi
EOF
chmod 755 "$ROOT/DEBIAN/postrm"
make_deb sremfb-server amd64 \
"Depends: libglib2.0-0t64, liblz4-1, libevdi1, evdi-dkms, libx264-164, usbip
Conflicts: rfb-server
Replaces: rfb-server
Description: sRemFB, écran virtuel réseau — serveur (connecteur EVDI)
 Expose un connecteur d'écran virtuel EVDI par client connecté (identifié
 par son adresse MAC) et transfère les zones modifiées, compressées en
 LZ4, vers les sremfb-client du LAN (allowlist CIDR). Mesure la
 congestion par le délai et bascule en H.264 (x264) les clients qui
 savent le décoder quand le lien sature. Attache par usbip les
 périphériques USB que les clients exportent (téléport USB)."

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
"Depends: libc6, libdrm2
Recommends: usbip
Conflicts: rfb-client
Replaces: rfb-client
Description: sRemFB, écran virtuel réseau — client framebuffer/DRM
 Reçoit les frames d'un sremfb-server et les écrit dans /dev/fb0
 (défaut) ou en scanout DRM/KMS natif (SREMFB_OUTPUT=drm : modeset
 legacy, RGB565 ou XRGB8888) ; éteint la dalle quand le serveur est
 absent ou blanke, reflète le débranchement de la dalle, décode le
 H.264 adaptatif en matériel (V4L2 M2M, ex. Pi 3) quand le SBC en
 dispose, et exporte les périphériques USB éligibles vers le serveur
 (usbip, paquet usbip requis pour cette fonction).
 LZ4 lié en statique : seules libc6 et libdrm2 sont requises."
done

echo "== paquets dans $DIST :"
ls -l "$DIST"
