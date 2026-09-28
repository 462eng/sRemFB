# SPEC — sortie DRM/KMS du client (`sremfb-client`)

Statut : draft pour implémentation · Branche : `drm-output` (depuis `main`,
server-agnostic) · Ligne **1.4.x** (thin client de VM), ordre libre ·
Cible C1 : **mono-sortie**. Multi-sorties = phase ultérieure (dépend du
transport (MAC,rôle,head), voir `SPEC-spice-usb-audio.md` sur
`spice-backend`).

---

## 1. Objectif

Le client écrit aujourd'hui les frames décodées dans `/dev/fb0` (fbdev).
Sur un Pi 4/5, l'émulation fbdev du KMS n'expose **qu'un** `/dev/fb0` (une
seule sortie), alors que les deux HDMI sont deux **connecteurs DRM** :
piloter les sorties indépendamment (à terme, une tête de VM par sortie)
impose de passer en **DRM/KMS**.

C1 introduit une **sortie DRM en option**, `fb` restant le défaut :

- **`fb`** (défaut) — le chemin actuel, inchangé.
- **`drm`** (opt-in) — modeset KMS sur un connecteur, **via libdrm**,
  double-buffer + page-flip (**sans déchirure**), blank par DPMS, hotplug
  par uevents DRM natifs.
- **`drm` forcé** dès qu'on configure plusieurs sorties (phase multi).

Bénéfices même en mono-sortie : tear-free, modeset propre (au lieu de
subir fbcon), hotplug DRM natif (remplace la veille `/sys/class/drm`).
**Server-agnostic** : marche contre le serveur EVDI 1.3.x comme contre
SPICE, **aucun changement de protocole**, défaut `fb` inchangé.

### Non-objectifs C1
- Multi-sorties / multi-têtes (phase suivante).
- Atomic KMS, plans overlay, rotation matérielle, scaling KMS.

---

## 2. Compatibilité matérielle — contrainte forte

Cibles clients : Raspberry Pi (vc4-kms-v3d) **et Allwinner A20**
(Cubieboard/Banana Pi M1, ARMv7, driver **sun4i-drm**), plus les autres
SBC ARM.

Vérifié dans le projet noyau `millefeuille` (Buildroot) : bananapi-m1p =
kernel **mainline 6.12.34 LTS**, defconfig **`sunxi`** + patches board, DTS
`sun7i-a20-bananapi-m1-plus`, et `CONFIG_DRM_FBDEV_EMULATION=y` (le
commentaire du fragment note explicitement « sremfb écrit dans /dev/fb0 »).
Donc l'A20 tourne **sun4i-drm + émulation fbdev**, comme le Pi : le chemin
`fb` actuel y marche déjà, et le natif KMS (C1) y est disponible.

**L'A20 est une cible de prod validée**, pas prospective : build millefeuille
+ sremfb-client déployé, **résultats excellents** grâce au **GbE natif** du
Banana Pi M1+ (vrai gigabit, non bridé par l'USB contrairement au Pi 3) —
le chemin RAW/LZ4 a toute la bande passante voulue, rendu « comme en
local » sans H.264. Le **test DRM sur A20 est donc critique** (§7), il
valide une cible réelle et ne doit rien casser du chemin `fb` actuel.

Conséquences de conception :

- **API modeset *legacy* uniquement** (`drmModeGetResources`,
  `drmModeGetConnector`, `drmModeGetEncoder`/`GetCrtc`, `drmModeSetCrtc`,
  `drmModePageFlip`, `drmModeAddFB`). **Pas d'atomic** (`DRM_CLIENT_CAP_
  ATOMIC`) : l'atomic est plus récent et inégal selon les drivers/kernels,
  alors que le legacy est supporté partout, sun4i inclus. Un scanout
  plein écran n'a pas besoin d'atomic.
- **Dumb buffers** (`DRM_IOCTL_MODE_CREATE_DUMB` + `MAP_DUMB`) : supportés
  par vc4 et sun4i. C'est notre seule voie d'allocation (pas de GBM/EGL).
- **Format avec repli** : tenter `DRM_FORMAT_RGB565` (colle au chemin
  16bpp du client), sinon `DRM_FORMAT_XRGB8888` (universel). Choix validé
  contre le connecteur/CRTC à l'ouverture ; on annonce le pixfmt retenu
  dans le hello comme aujourd'hui.
- **libdrm** en dépendance : `libdrm2` (runtime) sur **arm64 et armhf** ;
  `libdrm-dev` au build. libdrm est présent partout et cross-compile.
- Pas d'hypothèse sur HDMI-audio KMS (hors sujet ici ; l'audio a son
  propre chantier).

Test de compat A20 = **de première classe** (§7), pas une arrière-pensée.

---

## 3. Abstraction de sortie (miroir de `source_ops` côté serveur)

```c
/* client/output.h */
struct sremfb_output;

struct sremfb_output_ops {
    /* Ouvre la sortie ; renvoie la géométrie réelle (mode KMS ou fb0) et
     * le pixfmt retenu (RGB565/XRGB8888). 0 = OK. */
    int  (*open)(struct sremfb_output *o, int *w, int *h, int *pixfmt);
    /* Buffer d'écriture courant (le back buffer en drm double-buffer). */
    uint8_t *(*backbuf)(struct sremfb_output *o, int *stride);
    /* Publie le back buffer (drm : page-flip + swap ; fb : pan/no-op). */
    void (*present)(struct sremfb_output *o);
    void (*blank)(struct sremfb_output *o, int on);   /* DPMS / FBIOBLANK */
    /* fd à surveiller pour le hotplug de la dalle (-1 si n/a). */
    int  (*hotplug_fd)(struct sremfb_output *o);
    int  (*hotplug_connected)(struct sremfb_output *o); /* état courant */
    void (*close)(struct sremfb_output *o);
};

extern const struct sremfb_output_ops sremfb_output_fb;
extern const struct sremfb_output_ops sremfb_output_drm;
```

- **`client/output_fb.c`** : le chemin actuel refactoré (mmap fb0, blit,
  `FBIOBLANK`, veille `/sys/class/drm` pour le hotplug, `pwrite` fallback).
  Zéro changement de comportement.
- **`client/output_drm.c`** : nouveau, libdrm legacy (§4).
- Le reste du client (décodeur H.264 V4L2, **thread de blit 1.1.3** avec
  framedrop, dithering RGB565, hello, USB) écrit dans `backbuf()` et
  appelle `present()` au lieu de taper fb0 en dur. La boîte-aux-lettres
  1-frame et le `blit_sync()` avant snapshot/close restent identiques.

### Sélection
- `SREMFB_OUTPUT=fb|drm` (défaut `fb`).
- Multi-sorties configurées (phase suivante) ⇒ `drm` forcé (warning si
  l'utilisateur avait mis `fb`).

---

## 4. `output_drm.c` (libdrm, legacy)

1. **open** : `drmOpen`/`open("/dev/dri/cardN")` (`SREMFB_DRM_CARD`, défaut
   = 1ᵉ carte avec un connecteur *connected*) ; `drmSetMaster` (on est le
   seul client KMS, pas de compositeur sur ces panels console).
2. **Connecteur** : `SREMFB_DRM_CONNECTOR` (ex. `HDMI-A-1`) ou le 1ᵉ
   *connected*. Mode = le **preferred** du connecteur (ou `SREMFB_DRM_MODE`
   `WxH[@Hz]`). Encodeur→CRTC via `drmModeGetEncoder`.
3. **Buffers** : deux dumb buffers à la taille du mode ;
   `drmModeAddFB`/`AddFB2` (format retenu) ; `mmap`. Double-buffer.
4. **Set mode** : `drmModeSetCrtc(fd, crtc, fb0, 0,0, &conn, 1, &mode)`.
5. **present** : blit terminé dans le back buffer →
   `drmModePageFlip(..., DRM_MODE_PAGE_FLIP_EVENT, ...)` ; on attend
   l'event de flip (drmHandleEvent sur le fd) avant de réutiliser le buffer
   → tear-free et cadence naturelle. (Le thread de blit sépare déjà ça du
   décodeur/réseau.)
6. **blank** : propriété DPMS du connecteur
   (`drmModeConnectorSetProperty(... "DPMS" ...)`), ou CRTC désactivé en
   repli si le driver n'expose pas DPMS.
7. **hotplug** : moniteur udev (`libudev`) ou `poll` sur le fd DRM pour les
   uevents « hotplug » ; à l'event, relire le statut du connecteur →
   déconnecté = « dalle débranchée » (alimente la logique existante).
   *(Éviter une dépendance libudev si possible : lire
   `/sys/class/drm/<conn>/status` sur uevent brut. À trancher.)*
8. **VT/console** : on garde le grab VT actuel (`SREMFB_TTY`, `KD_GRAPHICS`,
   `VT_ACTIVATE` tty7) pour que fbcon ne se batte pas pour la carte. En
   sortie : `drmDropMaster`, restaurer la VT.

Géométrie : le mode KMS donne `w×h` → annoncés dans le hello (`xres/yres`),
comme fbdev le fait via `FBIOGET_VSCREENINFO`.

---

## 5. Config (client)

| Variable | Défaut | Rôle |
|---|---|---|
| `SREMFB_OUTPUT` | `fb` | `fb` \| `drm` |
| `SREMFB_DRM_CARD` | auto | `/dev/dri/cardN` |
| `SREMFB_DRM_CONNECTOR` | 1ᵉ connected | ex. `HDMI-A-1`, `LCD-1` |
| `SREMFB_DRM_MODE` | preferred | `1920x1080@60` pour forcer |
| `SREMFB_HEAD` | 0 | tête de VM demandée (ignorée par EVDI ; utile SPICE multi-têtes) |

`SREMFB_FBDEV`/`SREMFB_WRITE_MODE` restent pour le backend `fb`.

---

## 6. Packaging / compat

- Client : build dep `libdrm-dev` ; runtime dep **`libdrm2`** ajoutée aux
  paquets arm64 **et** armhf (`Depends: libc6, libdrm2`). Reste léger.
- `make -C client` : lier `-ldrm` (via `pkg-config libdrm` pour les
  `-I`/`-l`). La cross-compil récupère `libdrm-dev:arm64/armhf` dans le
  sysroot (comme lz4 aujourd'hui).
- **Compat descendante** : défaut `fb` → aucun changement pour les clients
  existants (Milim, Aqua). Marche contre serveur EVDI **et** SPICE.

---

## 7. Plan de test

1. **Milim (Pi 500, vc4)** : `SREMFB_OUTPUT=drm` sur `HDMI-A-1` contre le
   **serveur EVDI actuel** → image OK, **sans déchirure** (vs blit fb),
   blank/unblank (GNOME éteint), hotplug (débrancher/rebrancher la dalle),
   Ctrl-C propre (DropMaster, VT rendue).
   *Capacités vérifiées en lecture seule (modetest) : plane primaire
   supporte `RG16` (RGB565) ET `XR24` (XRGB8888) en **LINEAR**, propriété
   **DPMS** présente, mode préféré 1920x1080@60. → format primaire RG16,
   repli XR24 ; blank par DPMS confirmé sur vc4.*
   **✅ VALIDÉ 2026-08-15** (client 1.4.0 en prod DRM) : card1/vc4 auto
   (card0/v3d sauté), scanout RG16 natif, hello 16bpp pixfmt 1, hotplug
   armé, USB intact, rendu « parfait » (visuel).
   **✅ Page-flip (C1.3) VALIDÉ le même soir** sur le chemin RAW : vidéo
   plein écran (mpv testsrc sur le moniteur Milim) → `page-flip active`
   au premier rect exactement 1920x1080+0+0, re-flips sur les frames
   pleines suivantes, et les rects presque-pleins (trim serveur : rognés
   de 3-16 lignes) passent bien en écriture en place. Répartition
   sélective conforme, zéro erreur/repli. Note : le trim
   anti-frame-fantôme rend le rect exactement plein écran rare en damage
   ordinaire (la top bar ne change pas → y1>0) — le flip sert surtout
   les transitions plein écran et, à terme, chaque frame H.264 décodée
   (à confirmer sur Aqua/Pi 3 quand elle reviendra).
2. **A20 (Banana Pi M1 / Cubieboard, sun4i, kernel patché)** : même test,
   **legacy modeset validé** sur sun4i (dumb buffers, page-flip, DPMS,
   format RGB565 ou repli XRGB8888). C'est le test de compat critique.
3. **A/B `fb` vs `drm`** : bascule par env, aucune régression du chemin fb.
4. Régression : client `fb` par défaut inchangé contre Milim/Aqua en prod.

---

## 8. Décisions prises (ne pas rouvrir)
- Sortie via `sremfb_output_ops` ; `fb` défaut, `drm` opt-in ; **libdrm**.
- **Legacy modeset, pas atomic** (compat sun4i/A20 + tous KMS).
- Dumb buffers + page-flip (tear-free) ; format RGB565 → repli XRGB8888.
- C1 = mono-sortie, server-agnostic, sans changement de protocole.
- Le thread de blit 1.1.3 et la logique décodeur/hello ne bougent pas :
  ils passent juste par `backbuf()`/`present()`.

## 9. Questions ouvertes
1. Hotplug : libudev vs uevent brut + lecture `/sys/class/drm/*/status`
   (préférence : éviter libudev pour rester léger).
2. DPMS absent sur certains sun4i ? → repli CRTC off (à valider sur A20).
3. `drmSetMaster` sans logind : suffit-il d'être seul + la VT grab, ou
   besoin d'un `SETMASTER` explicite / droits sur `/dev/dri/cardN` (groupe
   `video`) — à vérifier sur A20 et Pi.
4. RGB565 en scanout KMS : **vc4 OK** (RG16 LINEAR vérifié sur Milim) ;
   **sun4i/A20 à confirmer** sur la carte de test. Sinon XR24 partout.
5. Nom de carte auto : itérer `/dev/dri/card*` et prendre celle qui a un
   connecteur *connected* (ignorer les cartes render-only, ex. v3d).
