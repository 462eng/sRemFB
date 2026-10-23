#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
"""Génère server/kvm/hidmap.c : table evdev KEY_* -> usage HID (page 0x07).

La table est l'inverse de hid_keyboard[256] de drivers/hid/hid-input.c du
noyau Linux : le mapping que la cible appliquera aux rapports du gadget.
Quand plusieurs usages donnent la même touche evdev (0x31/0x32 -> KEY_BACKSLASH,
0x4C/0x9C/0xD8 -> KEY_DELETE...), le plus petit usage l'emporte. Seuls les
usages 0x04..0xE7 (page Keyboard/Keypad proprement dite) sont retenus ; les
entrées 0xE8..0xFF de hid_keyboard[] (touches multimédia d'anciens claviers)
relèvent de la page Consumer, hors périmètre v1 : listées en commentaire.

usage : gen-hidmap.py hid-input.c [input-event-codes.h] > hidmap.c

hid-input.c : /usr/src/linux*/drivers/hid/hid-input.c, ou
  https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/drivers/hid/hid-input.c
input-event-codes.h : défaut /usr/include/linux/input-event-codes.h
"""
import hashlib
import re
import sys


def parse_table(src):
    m = re.search(r'hid_keyboard\[256\]\s*=\s*\{(.*?)\};', src, re.S)
    if not m:
        sys.exit('hid_keyboard[256] introuvable')
    vals = [v.strip() for v in m.group(1).replace('\n', ' ').split(',')]
    vals = [v for v in vals if v]
    if len(vals) != 256:
        sys.exit(f'hid_keyboard : {len(vals)} entrées, 256 attendues')
    return [0 if v == 'unk' else int(v, 0) for v in vals]


def parse_names(hdr):
    """Code -> premier nom KEY_* défini par une valeur numérique."""
    names = {}
    for m in re.finditer(r'^#define\s+(KEY_\w+)\s+(0x[0-9a-fA-F]+|\d+)\b',
                         hdr, re.M):
        code = int(m.group(2), 0)
        names.setdefault(code, m.group(1))
    return names


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    raw = open(sys.argv[1], 'rb').read()
    src = raw.decode('utf-8', errors='replace')
    sha = hashlib.sha256(raw).hexdigest()
    hpath = sys.argv[2] if len(sys.argv) == 3 else \
        '/usr/include/linux/input-event-codes.h'
    names = parse_names(open(hpath, encoding='utf-8').read())
    unk = next(c for c, n in names.items() if n == 'KEY_UNKNOWN')
    table = parse_table(src)

    inv, dups, consumer = {}, [], []
    for usage, key in enumerate(table):
        if key in (0, unk):
            continue
        if usage > 0xE7:
            consumer.append((usage, key))
            continue
        if key in inv:
            dups.append((usage, key))
            continue
        inv[key] = usage

    out = []
    w = out.append
    w('/* SPDX-License-Identifier: EUPL-1.2 */')
    w('/*')
    w(' * GÉNÉRÉ par gen-hidmap.py — ne pas éditer à la main.')
    w(f' * Source : drivers/hid/hid-input.c, sha256 {sha[:16]}…')
    w(' *')
    w(' * evdev KEY_* -> usage HID de la page Keyboard/Keypad (0x07), inverse')
    w(' * de hid_keyboard[] de drivers/hid/hid-input.c (noyau Linux) : la cible')
    w(' * refait le chemin dans l\'autre sens. 0 = pas d\'usage (touche ignorée).')
    w(' * 0xE0..0xE7 sont les modificateurs (bits 0..7 du premier octet du')
    w(' * rapport boot). Plusieurs usages pour une touche : le plus petit gagne.')
    w(' *')
    w(' * Usages écartés (doublons, le premier usage est gardé) :')
    for usage, key in dups:
        w(f' *   0x{usage:02X} {names.get(key, key)} (gardé 0x{inv[key]:02X})')
    w(' * Usages 0xE8..0xFF de hid_keyboard[] (multimédia, page Consumer en')
    w(' * pratique) : hors périmètre v1, non mappés :')
    for usage, key in consumer:
        w(f' *   0x{usage:02X} {names.get(key, key)}')
    w(' */')
    w('#include "kvm.h"')
    w('')
    w('const uint8_t sremfb_hid_usage[KEY_CNT] = {')
    for key in sorted(inv):
        w(f'    [{names.get(key, str(key))}] = 0x{inv[key]:02X},')
    w('};')
    sys.stdout.write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
