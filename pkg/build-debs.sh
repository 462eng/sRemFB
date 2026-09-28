#!/bin/sh -e
# SPDX-License-Identifier: EUPL-1.2
# Construit les paquets Debian de sRemFB dans dist/ :
#   sremfb-server_<ver>_amd64.deb         (PC GNOME/Wayland)
#   sremfb-client_<ver>_arm64.deb         (SBC 64 bits : Pi 3/4/5/500…)
#   sremfb-client_<ver>_armhf.deb         (SBC ARMv7 : Banana Pi M1+, Pi 2…)
#   sremfb-view_<ver>_amd64.deb           (visionneuse fenêtrée SDL3, PC)
#
# Le client est lié en STATIQUE avec liblz4 (extraite des paquets Debian
# de la cible, mises en cache dans pkg/sysroot/) : il ne dépend que de
# libc6, donc fonctionne tel quel sur Debian, Raspberry Pi OS et Armbian.
# Le module noyau n'est pas repackagé : evdi-dkms existe dans Debian et
# n'est nécessaire que côté serveur (déclaré en dépendance).
#
# Prérequis : gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf, et les
# architectures arm64/armhf activées dans dpkg pour apt-get download ;
# libsdl3-dev et dpkg-dev (dpkg-shlibdeps) pour la visionneuse.

VERSION=${1:-1.5.0}
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
make -s -B -C "$TOP/server" VERSION="$VERSION"

build_client() { # $1 = debian arch, $2 = triplet gcc
    echo "== build client ($1)"
    "$2-gcc" -O2 -Wall -Wextra -pthread -I"$TOP" \
        -DSREMFB_VERSION="\"$VERSION\"" \
        -I"$SYSROOT/$1/usr/include" \
        -o "$STAGE/sremfb-client-$1" \
        "$TOP/client/sremfb-client.c" "$TOP/client/v4l2dec.c" \
        "$TOP/client/usbexport.c" \
        "$SYSROOT/$1/usr/lib/$2/liblz4.a"
    "$2-strip" "$STAGE/sremfb-client-$1"
}
echo "== build visionneuse (amd64)"
make -s -B -C "$TOP/view" VERSION="$VERSION"

build_client arm64 aarch64-linux-gnu
build_client armhf arm-linux-gnueabihf

# --- assemblage ---------------------------------------------------------
# /usr/share/doc/<paquet>/copyright (format DEP-5) : EUPL-1.2, texte
# anglais complet (LICENSE) ; le texte français fait également foi.
install_copyright() { # $1 = nom du paquet ; l'arborescence est dans $ROOT
    mkdir -p "$ROOT/usr/share/doc/$1"
    {
        printf 'Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/\n'
        printf 'Upstream-Name: sRemFB\n'
        printf 'Source: https://github.com/462eng/sRemFB\n\n'
        printf 'Files: *\n'
        printf 'Copyright: 2026 Jonathan Roth\n'
        printf 'License: EUPL-1.2\n\n'
        printf 'License: EUPL-1.2\n'
        printf ' The English (LICENSE) and French (LICENSE.fr) texts in the source\n'
        printf ' tree are equally authentic. English text follows.\n .\n'
        sed 's/^[[:space:]]*$/./; s/^/ /' "$TOP/LICENSE"
    } > "$ROOT/usr/share/doc/$1/copyright"
    chmod 644 "$ROOT/usr/share/doc/$1/copyright"
}

make_deb() { # $1 = nom, $2 = arch ; l'arborescence est déjà dans $ROOT
    install_copyright "$1"
    mkdir -p "$ROOT/DEBIAN"
    find "$ROOT" -type d -exec chmod 755 {} +   # indépendant de l'umask
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
         "$ROOT/usr/lib/systemd/system/gdm.service.d" \
         "$ROOT/usr/lib/udev/hwdb.d" "$ROOT/usr/lib/udev/rules.d" \
         "$ROOT/etc/modules-load.d" "$ROOT/etc/modprobe.d"
install -m 755 "$TOP/server/sremfb-server" "$ROOT/usr/bin/sremfb-server"
strip "$ROOT/usr/bin/sremfb-server"
sed 's|/usr/local/bin|/usr/bin|' "$TOP/systemd/sremfb-server.service" \
    > "$ROOT/usr/lib/systemd/user/sremfb-server.service"
install -m 644 "$TOP/systemd/sremfb-evdi-perms.service" \
    "$ROOT/usr/lib/systemd/system/sremfb-evdi-perms.service"
install -m 644 "$TOP/systemd/gdm-sremfb-evdi-purge.conf" \
    "$ROOT/usr/lib/systemd/system/gdm.service.d/sremfb-evdi-purge.conf"
install -m 755 "$TOP/systemd/sremfb-usb-attach" \
    "$ROOT/usr/libexec/sremfb-usb-attach"
sed 's|/usr/local/libexec|/usr/libexec|' "$TOP/systemd/sremfb-usb.service" \
    > "$ROOT/usr/lib/systemd/system/sremfb-usb.service"
install -m 644 "$TOP/systemd/sremfb-usb.path" \
    "$ROOT/usr/lib/systemd/system/sremfb-usb.path"
install -m 644 "$TOP/systemd/sremfb-usb.timer" \
    "$ROOT/usr/lib/systemd/system/sremfb-usb.timer"
sed 's|/usr/local/libexec|/usr/libexec|' "$TOP/systemd/sremfb-usb-detach.service" \
    > "$ROOT/usr/lib/systemd/system/sremfb-usb-detach.service"
install -m 644 "$TOP/systemd/tmpfiles-sremfb.conf" \
    "$ROOT/usr/lib/tmpfiles.d/sremfb.conf"
install -m 644 "$TOP/systemd/61-sremfb-display-vendor.hwdb" \
    "$ROOT/usr/lib/udev/hwdb.d/"
install -m 644 "$TOP/systemd/60-sremfb-evdi.rules" \
    "$ROOT/usr/lib/udev/rules.d/"
install -m 644 "$TOP/systemd/60-sremfb-uinput.rules" \
    "$ROOT/usr/lib/udev/rules.d/"
install -m 755 "$TOP/tools/sremfb-latency-probe" \
    "$ROOT/usr/bin/sremfb-latency-probe"
install -m 755 "$TOP/server/sremfb-latency-click" \
    "$ROOT/usr/bin/sremfb-latency-click"
strip "$ROOT/usr/bin/sremfb-latency-click"
install -m 644 "$TOP/systemd/modules-load-sremfb.conf" \
    "$ROOT/etc/modules-load.d/sremfb.conf"
install -m 644 "$TOP/systemd/modprobe-sremfb.conf" \
    "$ROOT/etc/modprobe.d/sremfb.conf"
install -m 644 "$TOP/systemd/sremfb-server.conf.example" \
    "$ROOT/etc/sremfb-server.conf"
# unités produites par sed : 644 quel que soit l'umask
chmod 644 "$ROOT"/usr/lib/systemd/system/*.service \
    "$ROOT"/usr/lib/systemd/user/*.service
mkdir -p "$ROOT/DEBIAN"
printf '/etc/modules-load.d/sremfb.conf\n/etc/modprobe.d/sremfb.conf\n/etc/sremfb-server.conf\n' \
    > "$ROOT/DEBIAN/conffiles"
cat > "$ROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh -e
systemd-hwdb update || true
udevadm control --reload 2>/dev/null || true
modprobe evdi || true
# entrées des clients (SREMFB_INPUT=1) : uinput + ACL uaccess tout de suite
modprobe uinput || true
udevadm trigger --action=change --sysname-match=uinput 2>/dev/null || true
# Le service oneshot pose les droits groupe video sur /sys/devices/evdi/*
# de façon fiable au boot (la règle udev seule ne suffit pas : son chmod
# court avant que les attributs existent). --now l'applique tout de suite.
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload 2>/dev/null || true
    systemctl enable --now sremfb-evdi-perms.service 2>/dev/null || true
    # téléport USB : /run/sremfb + réconciliateur usbip (root)
    systemd-tmpfiles --create /usr/lib/tmpfiles.d/sremfb.conf 2>/dev/null || true
    systemctl enable --now sremfb-usb.path sremfb-usb.timer 2>/dev/null || true
    # détachement usbip à l'arrêt (ExecStop=, avant la coupure du réseau)
    systemctl enable --now sremfb-usb-detach.service 2>/dev/null || true
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
# prerm : le script de détachement existe encore, les ports importés
# sont rendus proprement avant la suppression des fichiers
cat > "$ROOT/DEBIAN/prerm" <<'EOF'
#!/bin/sh -e
if [ "$1" = remove ] && command -v systemctl >/dev/null 2>&1; then
    systemctl disable --now sremfb-usb.path sremfb-usb.timer 2>/dev/null || true
    systemctl disable --now sremfb-usb-detach.service 2>/dev/null || true
fi
EOF
chmod 755 "$ROOT/DEBIAN/prerm"
cat > "$ROOT/DEBIAN/postrm" <<'EOF'
#!/bin/sh -e
if [ "$1" = remove ] || [ "$1" = purge ]; then
    if command -v systemctl >/dev/null 2>&1; then
        systemctl disable --now sremfb-evdi-perms.service 2>/dev/null || true
        systemctl disable --now sremfb-usb.path sremfb-usb.timer 2>/dev/null || true
        systemctl disable sremfb-usb-detach.service 2>/dev/null || true
    fi
fi
EOF
chmod 755 "$ROOT/DEBIAN/postrm"
make_deb sremfb-server amd64 \
"Depends: libglib2.0-0t64, liblz4-1, libevdi1, evdi-dkms, libx264-164, usbip,
 libpipewire-0.3-0t64
Suggests: python3-gi, gir1.2-gtk-4.0
Conflicts: rfb-server
Replaces: rfb-server
Description: sRemFB, écran virtuel réseau — serveur (connecteur EVDI)
 Expose un connecteur d'écran virtuel EVDI par client connecté (identifié
 par son adresse MAC) et transfère les zones modifiées, compressées en
 LZ4, vers les sremfb-client du LAN (allowlist CIDR). Mesure la
 congestion par le délai et bascule en H.264 (x264) les clients qui
 savent le décoder quand le lien sature. Attache par usbip les
 périphériques USB que les clients exportent (téléport USB). Sur option
 (SREMFB_INPUT=1), rejoue clavier, souris et manette des clients sur des
 périphériques uinput (sremfb-latency-probe : sonde de mesure de latence,
 python3-gi + GTK 4). Donne aux clients qui le demandent le son du bureau
 (sortie PipeWire par client, PCM brut sur UDP ; SREMFB_AUDIO=0 pour
 couper)."

# ---- sremfb-client (arm64 + armhf) ----
for arch in arm64 armhf; do
    ROOT=$STAGE/client-$arch
    mkdir -p "$ROOT/usr/bin" "$ROOT/usr/lib/systemd/system" "$ROOT/etc" \
             "$ROOT/usr/share/sremfb-client"
    install -m 755 "$STAGE/sremfb-client-$arch" "$ROOT/usr/bin/sremfb-client"
    sed 's|/usr/local/bin|/usr/bin|' "$TOP/systemd/sremfb-client.service" \
        > "$ROOT/usr/lib/systemd/system/sremfb-client.service"
    chmod 644 "$ROOT/usr/lib/systemd/system/sremfb-client.service"
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
Recommends: usbip
Conflicts: rfb-client
Replaces: rfb-client
Description: sRemFB, écran virtuel réseau — client framebuffer
 Reçoit les frames d'un sremfb-server et les écrit directement dans
 /dev/fb0 ; éteint la dalle quand le serveur est absent ou blanke,
 reflète le débranchement de la dalle, décode le H.264 adaptatif
 en matériel (V4L2 M2M, ex. Pi 3) quand le SBC en dispose, et exporte
 les périphériques USB éligibles vers le serveur (usbip, paquet
 usbip requis pour cette fonction).
 LZ4 lié en statique : aucune autre dépendance obligatoire."
done

# ---- sremfb-view (amd64) ----
# arborescence sous debian/<paquet>/ : c'est là que dpkg-shlibdeps attend
# les binaires qu'il analyse
ROOT=$STAGE/view/debian/sremfb-view
mkdir -p "$ROOT/usr/bin"
install -m 755 "$TOP/view/sremfb-view" "$ROOT/usr/bin/sremfb-view"
strip "$ROOT/usr/bin/sremfb-view"
printf 'Source: sremfb\n\nPackage: sremfb-view\nArchitecture: amd64\n' \
    > "$STAGE/view/debian/control"
VIEW_DEPS=$(cd "$STAGE/view" && dpkg-shlibdeps -O \
    -e"debian/sremfb-view/usr/bin/sremfb-view" 2>/dev/null |
    sed -n 's/^shlibs:Depends=//p')
[ -n "$VIEW_DEPS" ] || VIEW_DEPS="libc6, liblz4-1, libsdl3-0"
make_deb sremfb-view amd64 \
"Depends: $VIEW_DEPS
Description: sRemFB, écran virtuel réseau — visionneuse fenêtrée
 Branche un écran virtuel sur un sremfb-server et l'affiche dans une
 fenêtre (SDL3, Wayland natif ou X11) sur un PC Linux. Relaie clavier,
 souris et manette quand le serveur l'autorise (SREMFB_INPUT=1) et joue
 le son du bureau distant (serveur ≥ 1.5.0). Avec un serveur plus ancien :
 image seule."

echo "== paquets dans $DIST :"
ls -l "$DIST"
