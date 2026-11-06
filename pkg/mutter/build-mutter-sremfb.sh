#!/bin/sh
# Reconstruit mutter 48.7 (Debian 13) avec les patches sremfb de pkg/mutter/patches.
# Usage : pkg/mutter/build-mutter-sremfb.sh <rang>   (ex. 2 -> +sremfb2)
# Les .deb arrivent dans dist/mutter-48.7-sremfb<rang>/. Rien n'est installé.
# Construire sur un disque local (pas le ~ NFS de Riza) : WORK=/chemin/local.
set -eu
RANG=${1:?rang sremfb attendu}
TOP=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${WORK:-$(mktemp -d /var/tmp/mutter-sremfb.XXXXXX)}
VER=48.7-0+deb13u1
cd "$WORK" || exit 1
apt-get source --download-only "mutter=$VER" || exit 1
dpkg-source -x "mutter_$VER.dsc" mutter-48.7 || exit 1
cd mutter-48.7 || exit 1
mkdir -p debian/patches/sremfb
# Ordre : sremfb1 d'abord, puis les rétroportages numérotés.
for p in mutter-48.7-native-remove-hot-unplugged-gpus.patch \
         mutter-48.7-01-native-do-not-keep-references-to-offscreens-on-detach.patch \
         mutter-48.7-02-native-no-detached-onscreens-in-power-saving.patch \
         mutter-48.7-03-native-always-mark-onscreens-detached-on-rebuild.patch; do
  cp "$TOP/pkg/mutter/patches/$p" debian/patches/sremfb/ || exit 1
  echo "sremfb/$p" >> debian/patches/series
done
DEBEMAIL=jr@462eng.fr DEBFULLNAME="Jonathan Roth" \
  dch -v "$VER+sremfb$RANG" -D trixie --force-distribution \
  "sremfb$RANG : patches de pkg/mutter/patches (dépôt rFb)." || exit 1
DEB_BUILD_OPTIONS="nocheck parallel=$(nproc)" dpkg-buildpackage -b -uc -us || exit 1
mkdir -p "$TOP/dist/mutter-48.7-sremfb$RANG"
cp ../*_"$VER+sremfb$RANG"_*.deb "$TOP/pkg/mutter/patches/"*.patch \
   "$TOP/dist/mutter-48.7-sremfb$RANG/" || exit 1
echo "OK : $TOP/dist/mutter-48.7-sremfb$RANG"
