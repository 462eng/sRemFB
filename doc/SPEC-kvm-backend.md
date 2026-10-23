# SPEC — Backend KVM pour sRemFB (`sremfb-kvm`)

Statut : draft pour implémentation · Branche : `kvm-backend` · Langue du
code : C, style existant du repo.

**Source** : copie des §3 à §6 de `doc/SPEC-ipkvm.md` du dépôt IPKVM
(rédigée le 2026-10-23), qui fait foi pour le reste du projet (matériel,
gadget USB, EDID, feuillet millefeuille, plan de test, lots de travail,
décisions §12, questions ouvertes §13). Les numéros de section sont ceux
de la spec d'origine ; les renvois vers §7–§14 pointent dans ce document-là.

## Écarts d'implémentation (lot A, branche `kvm-backend`)

- **Makefile** : les objets du KVM, cœur compris, sont compilés à part en
  `*.kvm.o` (règle `%.kvm.o: %.c`) avec une variable interne `KVM_CFLAGS`
  (seuls glib-2.0, lz4 et, avec `ENCODER=x264`, x264) : un Pi ou un sysroot
  croisé sans gio/pipewire/libevdi construit `sremfb-kvm`, et `CC`,
  `CFLAGS`, `LDFLAGS`, `PKG_CONFIG` se surchargent sans perdre les `-I`.
  `sremfb-server` est construit à l'identique.
- `ENCODER` valait `x264` par défaut tant que `kvm/encoder_v4l2.c`
  n'existait pas ; depuis le lot C, **`ENCODER=v4l2` est le défaut**, comme
  au §4.1. `kvm/hidmap.c` (lot B) est pris dans `KVM_OBJS` dès qu'il existe.
- `KVM_STATIC=1` : `-Wl,-Bstatic $(pkg-config --static --libs glib-2.0
  liblz4) -Wl,-Bdynamic`, en sortant de la partie statique `-lm`,
  `-pthread`, `-ldl`, `-lrt`, `-lc` (libc et libm restent dynamiques).
- `make check-kvm` : tests unitaires de `diff.c` et `convert_uyvy.c`
  (`kvm/test-kvm`).
- `kvm.h` : contrat §4.1 inchangé ; ajouts marqués « ajout lot A »
  (`SREMFB_KVM_NBUF`, champs internes, interfaces capture.c ↔ testsrc.c,
  `sremfb_kvm_probe()`).
- `input_hid.c` est un **stub** (lot B) ; il contient déjà
  `sremfb_kvm_abs_map()` d'après §4.7.4.
- Mode test : `SIGUSR1` = changement de mode simulé (≈ 700 ms sans signal,
  donc BLANK, puis 1024×768, puis retour au SIGUSR1 suivant) ; `SIGUSR2` =
  cible éteinte / rallumée.

---

## 3. Architecture logicielle

### 3.1 Binaires et dépendances

| Binaire | Source | Libs | Tourne sur |
|---|---|---|---|
| `sremfb-server` | `server/evdi/` + `main-evdi.c` | libevdi, glib/gio, lz4, x264, pipewire | PC GNOME (inchangé) |
| `sremfb-spice` | `server/spice/` + `main-spice.c` (branche `spice-backend`) | spice-client-glib… | bridge PVE (inchangé) |
| **`sremfb-kvm`** | **`server/kvm/` + `main-kvm.c`** | **glib-2.0, lz4** (+ x264 en option de build dev) | **Pi 4** (et PC en mode test, §4.9) |
| `sremfb-view` | `view/` | SDL3, lz4, **+ libavcodec/libavutil** | PC opérateur |

`sremfb-kvm` ne lie ni gio, ni libevdi, ni pipewire, ni x264 : les modules du
cœur qui en dépendent (`core/input.c`, `core/audio.c`, `core/layout.c`,
`core/encoder.c`) sont **remplacés à l'édition de liens** par leurs équivalents
KVM (§4.1). Le cœur n'est pas modifié, sauf les ajustements listés en §4.10.

### 3.2 Arborescence cible (dépôt sRemFB)

```
server/
  core/                  inchangé (voir §4.10 pour les retouches minimales)
  evdi/, spice/          inchangés
  kvm/
    kvm.h                état serveur/client KVM, interfaces internes (§4.1)
    capture.c            source V4L2 : EDID, timings, buffers, événements, fan-out
    diff.c               damage logiciel par tuiles
    convert_uyvy.c       UYVY → BGRx (et RGB24 → BGRx), par tuile
    encoder_v4l2.c       sremfb_enc_* sur /dev/video11
    input_hid.c          sremfb_input_* sur /dev/hidg*
    hidmap.c             table evdev KEY_* → usage HID
    stubs.c              sremfb_audio_* / sremfb_layout_* : no-op
    testsrc.c            source synthétique (mode test sans matériel)
  main-kvm.c
  Makefile               cible `sremfb-kvm` (+ `ENCODER=x264` pour le dev PC)
doc/
  SPEC-kvm-backend.md    copie/lien de la présente spec, §4–§6
```

Tout ce qui n'est pas du code C (gadget, EDID, service, conf, packaging) vit
dans le dépôt IPKVM (§5, §8) : sRemFB fournit un binaire, IPKVM en fait une
application installable.

---

## 4. `sremfb-kvm` — le backend (dépôt sRemFB)

### 4.1 Makefile, `kvm.h` et interfaces internes

`server/Makefile`, nouvelle cible (le `all` reste `sremfb-server`) :

```make
# sremfb-kvm : source V4L2 (TC358743) + HID gadget + encodeur V4L2 M2M.
# Ne lie que glib-2.0 et lz4 : audio/layout sont des stubs, input et
# encoder sont remplacés par leurs variantes kvm/.
KVM_PKGS   = glib-2.0 liblz4
KVM_CORE   = core/net.o core/convert.o core/control.o core/xmit.o core/session.o
KVM_OBJS   = kvm/capture.o kvm/diff.o kvm/convert_uyvy.o kvm/input_hid.o \
             kvm/hidmap.o kvm/stubs.o kvm/testsrc.o main-kvm.o
ENCODER   ?= v4l2                      # v4l2 (Pi) | x264 (dev PC)
ifeq ($(ENCODER),x264)
KVM_ENC    = core/encoder.o
KVM_PKGS  += x264
else
KVM_ENC    = kvm/encoder_v4l2.o
endif
$(KVM_OBJS) $(KVM_ENC): CFLAGS += -Ikvm $(shell pkg-config --cflags $(KVM_PKGS))
sremfb-kvm: $(KVM_CORE) $(KVM_ENC) $(KVM_OBJS)
	$(CC) -o $@ $^ $(shell pkg-config --libs $(KVM_PKGS)) -lm
```

Contraintes : `CC`, `CFLAGS`, `LDFLAGS`, `PKG_CONFIG` doivent pouvoir être
**surchargés depuis la ligne de commande** (cross-compilation du feuillet,
§8.3) sans perdre les `-I` : utiliser `CFLAGS +=` après un `CFLAGS ?=`, ou une
variable `KVM_CFLAGS` interne. `KVM_STATIC=1` lie glib, pcre2 et lz4 en
statique (`-Wl,-Bstatic … -Wl,-Bdynamic`), la libc restant dynamique. Le
`main-kvm.o` reçoit `-DSREMFB_VERSION`.

`server/kvm/kvm.h` (contrat entre les lots) :

```c
#include "core.h"

/* Géométrie et placement d'une image source dans un flux client
 * (letterbox, échelle uniforme, centré). s == 1 et ox == oy == 0 quand les
 * géométries coïncident. Partagé par capture.c (blit) et input_hid.c
 * (projection du pointeur absolu). */
struct sremfb_kvm_fit {
    int src_w, src_h;          /* image capturée */
    int dst_w, dst_h;          /* flux (c->geom) */
    double s;                  /* échelle source -> flux */
    int ox, oy, dw, dh;        /* rectangle utile dans le flux */
};
void sremfb_kvm_fit(struct sremfb_kvm_fit *f, int sw, int sh, int dw, int dh);

/* Source de frames : état serveur (SremfbServer.src). */
typedef struct {
    SremfbServer *srv;
    /* config */
    const char *device;        /* SREMFB_KVM_DEVICE, /dev/video0 */
    const char *edid_path;     /* SREMFB_KVM_EDID */
    uint32_t    pixfmt;        /* V4L2_PIX_FMT_UYVY | RGB24 (SREMFB_KVM_FORMAT) */
    int         tile;          /* SREMFB_KVM_TILE, 32 */
    enum { SREMFB_KVM_GEOM_SOURCE, SREMFB_KVM_GEOM_CLIENT } geometry;
    enum { SREMFB_KVM_RESIZE_SCALE, SREMFB_KVM_RESIZE_RECONNECT } resize;
    /* capture */
    int fd;                    /* -1 : pas de device (mode test) */
    gboolean streaming;        /* STREAMON actif */
    gboolean signal;           /* timings valides, frames attendues */
    int src_w, src_h;          /* mode courant de la source (0 sans signal) */
    unsigned src_fps;          /* informatif (timings) */
    uint32_t src_fourcc;       /* format réellement négocié */
    size_t   src_stride;
    /* buffers V4L2 (mmap), dont un retenu comme "frame précédente" */
    struct sremfb_kvm_buf { void *map; size_t len; gboolean queued; } buf[SREMFB_KVM_NBUF];
    int prev;                  /* index de la frame précédente (-1 : aucune) */
    /* image maître BGRx en géométrie source, remplie tuile par tuile */
    uint8_t *master;
    uint8_t *tile_dirty;       /* diff.c : 1 octet par tuile */
    int tiles_x, tiles_y;
    guint fd_watch_id, poll_id;
    /* source synthétique (testsrc.c) quand fd < 0 */
    struct sremfb_kvm_testsrc *test;
    /* stats */
    guint64 frames, frames_dirty, tiles_dirty_total;
} SremfbKvmState;

/* Par client (SremfbClient.src_ctx). */
typedef struct {
    gboolean streaming;
    struct sremfb_kvm_fit fit; /* recalculé à chaque changement de mode */
} SremfbKvmClient;

extern const struct sremfb_source_ops sremfb_kvm_ops;
int  sremfb_kvm_start(SremfbServer *srv);     /* ouvre, EDID, 1ère détection ; 0 = OK */
void sremfb_kvm_stop(SremfbServer *srv);

/* diff.c : compare cur et prev (même format/stride), marque st->tile_dirty.
 * Renvoie le nombre de tuiles sales. prev == NULL : tout sale. */
int  sremfb_kvm_diff(SremfbKvmState *st, const uint8_t *cur, const uint8_t *prev);
/* diff.c : transforme tile_dirty en rectangles (coordonnées source),
 * au plus `max` (fusion par lignes puis bounding box). */
int  sremfb_kvm_rects(const SremfbKvmState *st, struct sremfb_rect *out, int max);

/* convert_uyvy.c : convertit la zone [x1,x2)x[y1,y2) (multiples de 2 en x)
 * de la frame capturée vers st->master (BGRx). */
void sremfb_kvm_convert_rect(const SremfbKvmState *st, const uint8_t *src,
                             const struct sremfb_rect *r);

/* input_hid.c : projection d'un pointeur absolu en pixels du flux vers
 * 0..32767 sur l'écran de la cible, en inversant le letterbox. */
void sremfb_kvm_abs_map(const struct sremfb_kvm_fit *f, int x, int y,
                        int *hx, int *hy);
```

### 4.2 `main-kvm.c` — configuration

Même style que `main-spice.c` : variables d'environnement (EnvironmentFile /
`config/etc/sremfb-kvm.conf`), quelques flags. `SREMFB_INPUT` est **actif par
défaut** (c'est un KVM) ; `SREMFB_INPUT=0` pour un mode « regard seul ».

| Variable | Défaut | Rôle |
|---|---|---|
| `SREMFB_PORT` / `--port` | 4629 | écoute TCP (inchangé) |
| `SREMFB_ALLOW` / `--allow` | — | allowlist CIDR (inchangé) — **requise par la conf livrée** |
| `SREMFB_INPUT` | **1** | 0 = aucune entrée vers la cible |
| `SREMFB_KVM_DEVICE` | `/dev/video0` | nœud de capture ; `test:WxH[@fps]` = source synthétique ; `file:chemin@WxH[@fps]` = rejeu UYVY brut en boucle |
| `SREMFB_KVM_EDID` | `/usr/share/sremfb/edid-1080p50.bin` | EDID chargé au démarrage (vide = ne pas toucher) |
| `SREMFB_KVM_FORMAT` | `uyvy` | `uyvy` \| `rgb24` (format demandé au pont) |
| `SREMFB_KVM_GEOMETRY` | `source` | `source` = le flux prend la résolution de la cible à la connexion ; `client` = le flux prend la résolution demandée par le client |
| `SREMFB_KVM_RESIZE` | `scale` | à un changement de mode de la cible : `scale` = letterbox dans le flux existant ; `reconnect` = couper les clients (ils reviennent en `source`) |
| `SREMFB_KVM_TILE` | 32 | côté des tuiles de diff (16..128, multiple de 2) |
| `SREMFB_KVM_ENCODER` | `/dev/video11` | encodeur V4L2 M2M |
| `SREMFB_HID_KBD` / `SREMFB_HID_MOUSE` / `SREMFB_HID_POINTER` | `/dev/hidg0` / `1` / `2` | nœuds du gadget ; vide = fonction absente |
| `SREMFB_NO_H264`, `SREMFB_FORCE_H264`, `SREMFB_NO_DITHER` | — | inchangés (contrôleur, convert) |
| `SREMFB_NO_USB` | **1** (forcé) | pas de téléport usbip côté KVM |

Séquence de `main()` : parse → `sremfb_input_init()` (sauf `SREMFB_INPUT=0`) →
`net_allow_parse` → `server.source = &sremfb_kvm_ops; server.src = &kvm;` →
`sremfb_kvm_start()` (échec = exit 1 avec message, **sauf** absence de signal,
qui n'est pas une erreur) → `sremfb_serve()` → `sremfb_kvm_stop()`.

### 4.3 `capture.c` — la source V4L2

#### 4.3.1 Démarrage (`sremfb_kvm_start`)

1. `open(device, O_RDWR|O_NONBLOCK|O_CLOEXEC)`, `VIDIOC_QUERYCAP` (exige
   `V4L2_CAP_VIDEO_CAPTURE` + `STREAMING`).
2. `VIDIOC_SUBSCRIBE_EVENT` `V4L2_EVENT_SOURCE_CHANGE` (le tc358743 le génère à
   la perte/apparition de signal et au changement de mode).
3. EDID : lecture du fichier (256 octets, 2 blocs ; checksum vérifiée, message
   clair sinon), `VIDIOC_S_EDID` (pad 0, start_block 0, blocks 2). Si l'ioctl
   échoue (EINVAL sur un device sans EDID, mode test), avertir et continuer.
4. Première détection : `probe_signal()` (§4.3.4).
5. Armer : watch `G_IO_PRI` sur le fd pour les événements (toujours), watch
   `G_IO_IN` pour les frames (seulement en streaming), **et** un timer de
   secours (1 s) qui refait `VIDIOC_QUERY_DV_TIMINGS` tant qu'il n'y a pas de
   signal (ceinture et bretelles : l'événement peut se perdre au boot).

#### 4.3.2 Formats et buffers

- `VIDIOC_S_DV_TIMINGS` (timings détectés) puis `VIDIOC_S_FMT` avec le pixfmt
  configuré ; relire le format retourné (`src_fourcc`, `src_stride`,
  `sizeimage`). Si le pont refuse UYVY → essayer RGB24, puis échouer.
- `VIDIOC_REQBUFS` `V4L2_MEMORY_MMAP`, `SREMFB_KVM_NBUF = 4` ; mmap ; tout
  mettre en file ; `VIDIOC_STREAMON`.
- **Frame précédente retenue** : après le diff, le buffer « précédent » est
  remis en file et le buffer courant devient le précédent. Il y a donc en
  permanence 1 buffer hors du driver, 3 dedans. Zéro copie de la frame UYVY.
- Arrêt (`stop_stream()`) : `STREAMOFF`, `munmap`, `REQBUFS 0`, `prev = -1`,
  `signal = FALSE`.

#### 4.3.3 Réception d'une frame (watch `G_IO_IN`)

```
DQBUF cur (non bloquant ; EAGAIN → retour)
si bytesused == 0 ou flag ERROR : requeue, retour (frame tronquée au changement de mode)
n = sremfb_kvm_diff(st, cur, prev)            # prev == NULL à la 1ère frame : tout sale
si n > 0 :
    nrect = sremfb_kvm_rects(st, rects, 16)
    pour chaque rect : sremfb_kvm_convert_rect(st, cur, rect)      # → master BGRx
    pour chaque client streaming : blit(rects) + sremfb_xmit_damage(c, rects_flux, n, bytespp)
si prev >= 0 : QBUF prev
prev = cur
```

- **Fan-out** : plusieurs clients peuvent regarder la même cible (lecture
  seule ou non) — les files par client du cœur gèrent les débits hétérogènes,
  exactement comme le backend SPICE.
- **Blit** par client : si `fit.s == 1`, `memcpy` ligne à ligne du rect depuis
  `master` vers `c->grabbuf` ; sinon mise à l'échelle **plus proche voisin**
  (comme `blit_region()` de `spice_source.c`, dont le code est à reprendre) et
  bandes noires. Le rect converti en coordonnées flux est arrondi vers
  l'extérieur.
- `sremfb_xmit_damage()` fait déjà le trim contre `shadowbuf` : inutile de
  re-vérifier ici.
- Un seul `DQBUF` par passage de boucle : le main loop respire (autres clients,
  entrées, PONG).

#### 4.3.4 Signal, perte, changement de mode

`probe_signal()` : `VIDIOC_QUERY_DV_TIMINGS`.

- **ENOLINK / ENOLCK / ERANGE** (pas de signal, signal instable, hors plage) :
  si on streamait → `stop_stream()`, **BLANK** à tous les clients streaming
  (« pas de signal » = écran éteint, comme le DPMS d'EVDI), `signal = FALSE`,
  timer 1 s armé. Un client qui se connecte sans signal est **accepté** (§4.3.6)
  et reçoit BLANK juste après son hello : on doit pouvoir s'attacher à une
  cible éteinte.
- **Timings valides** : comparer à `(src_w, src_h)`.
  - identiques et déjà en streaming : rien ;
  - sinon : `stop_stream()`, appliquer timings + format, `start_stream()`,
    `signal = TRUE`, puis politique `resize` pour chaque client streaming :
    - `scale` : recalculer `fit`, noircir `grabbuf` (bandes), marquer tout sale
      (`dirty_all`), **UNBLANK** ;
    - `reconnect` : `sremfb_schedule_client_lost(c)` — la visionneuse revient
      d'elle-même (backoff 1–5 s) et obtient la nouvelle géométrie.

Événement `V4L2_EVENT_SOURCE_CHANGE` (watch `G_IO_PRI`, `VIDIOC_DQEVENT` en
boucle) → `probe_signal()`. Le driver signale aussi la perte de signal par
des DQBUF en erreur : les deux chemins convergent vers `probe_signal()`.

Débounce : un changement de mode produit souvent plusieurs événements en
moins d'une seconde (BIOS → chargeur → OS). Ne pas restreamer plus d'une fois
par 300 ms ; un `probe_signal()` différé par timer absorbe la rafale.

#### 4.3.5 Géométrie du flux

`acquire()` fixe `c->geom` **une fois pour toute la connexion** (contrainte du
protocole : pas de renégociation) :

| `SREMFB_KVM_GEOMETRY` | signal présent | sans signal |
|---|---|---|
| `source` (défaut) | `geom = (src_w, src_h)` — le hello serveur annonce la résolution de la cible, `sremfb-view` dimensionne sa texture dessus (il accepte déjà `width/height` ≠ demandé) | `geom = (hello.xres, hello.yres)` |
| `client` | `geom = (hello.xres, hello.yres)`, source letterboxée | idem |

Le hello serveur est envoyé **immédiatement** (`sremfb_xmit_hello`) : à la
différence d'EVDI, rien n'est asynchrone. Flags : `PING` si feedback, `H264`
si `feedback && h264_cap && !h264_failed && !SREMFB_NO_H264`, `INPUT` via
`sremfb_input_start(c)` (appelé **avant** le hello, comme dans `evdi.c`),
jamais `AUDIO`.

Rappel : `sremfb-client` (panel) envoie la géométrie de son écran ; en mode
`source`, si elle diffère, le client refuse (`sh.width != xres` → le client SBC
vérifie ?). **À vérifier dans `client/sremfb-client.c`** ; si le client SBC
exige l'égalité, `acquire()` doit détecter un hello SBC (`model` non vide /
`usb_cap`) et retomber en `client` pour lui. Noté §13 Q3.

#### 4.3.6 `acquire()` / `release()`

```
acquire(c):
    geom selon §4.3.5 ; src_ctx = g_new0(SremfbKvmClient) ; alloc grabbuf/shadowbuf/rectbuf/sendbuf
    (alloc_buffers() de spice_source.c, à factoriser dans kvm/capture.c)
    fit = sremfb_kvm_fit(src_w, src_h, geom)  (ou identité sans signal)
    flags = PING/H264 ; flags |= sremfb_input_start(c)
    sremfb_xmit_hello(c, flags) ; c->state = STREAMING ; streaming = TRUE
    sremfb_ctl_start(c)
    si signal : blit plein master → grabbuf, UNBLANK, dirty_all, kick
    sinon     : BLANK
    retour OK
release(c): sremfb_xmit_reset ; sremfb_ctl_stop ; free_buffers ; g_free(src_ctx)
```

`SREMFB_STATUS_SERVER_FAIL` n'est renvoyé que si le device n'est pas ouvert
du tout (fd < 0 hors mode test).

### 4.4 `diff.c` — damage logiciel

- Grille de tuiles `tile × tile` pixels (défaut 32 → 60×34 tuiles en 1080p).
  Dernière colonne/ligne tronquées.
- Comparaison **exacte** (la capture HDMI est numérique : des pixels identiques
  le restent — pas de seuil en v1 ; `SREMFB_KVM_TILE` est le seul réglage).
  Implémentation : pour chaque ligne de tuiles, pour chaque ligne de pixels,
  `memcmp` par tuile de `tile × bytespp` octets ; une tuile déjà sale n'est plus
  comparée (sortie anticipée par ligne). Budget : une frame UYVY 1080p = 4,1 Mo,
  `memcmp` séquentiel ≈ 1 ms sur A72 ; à 50 fps c'est 5 % d'un cœur.
- `sremfb_kvm_rects()` : par ligne de tuiles, fusionner les tuiles sales
  consécutives en un rect ; fusionner verticalement deux rects de mêmes x1/x2
  adjacents ; si plus de `max` rects, remplacer par leur bounding box (le cœur
  fait la même chose dans `dirty_add()` à 16). Les rects sont en pixels
  source, x1/x2 pairs (UYVY = 2 pixels par macro-pixel).
- Première frame après `start_stream()` : tout sale (prev == NULL).
- Statistiques (log périodique §4.11) : frames reçues, frames avec damage,
  tuiles sales cumulées — pour régler `tile`.

### 4.5 `convert_uyvy.c` — UYVY/RGB24 → BGRx

- `sremfb_kvm_convert_rect()` ne convertit que les rects sales vers `master`
  (`BGRx` = `XRGB8888` LE, le format de `grabbuf`).
- UYVY → RGB : BT.601 **limited range** (ce que sort le TC358743 pour une
  source RGB convertie en YUV 4:2:2 ; les coefficients inverses de ceux de
  `sremfb_convert_bgrx_to_i420()` dans `convert.c` et de `yuv_row_to_fb()` du
  client). Virgule fixe, 2 pixels par itération, code écrit pour être
  auto-vectorisé (`-O2 -ftree-vectorize` ; NEON intrinsics seulement si la
  mesure l'impose). Budget plein cadre 1080p : < 4 ms.
- RGB24 → BGRx : permutation d'octets, trivial.
- Contrôles du pont à vérifier sur matériel (§13 Q4) : certains TC358743
  exposent le range (full/limited) via les contrôles V4L2 standard
  (`V4L2_CID_DV_RX_RGB_RANGE`) ; relire le `colorspace`/`quantization`
  retournés par `G_FMT` et les logger, et choisir les coefficients en fonction
  (`V4L2_QUANTIZATION_FULL_RANGE` → coefficients full).

### 4.6 `encoder_v4l2.c` — encodeur H.264 matériel

Implémente **l'API de `core/encoder.c` à l'identique** (`sremfb_enc_open`,
`sremfb_enc_set_bitrate`, `sremfb_enc_bitrate`, `sremfb_enc_encode`,
`sremfb_enc_close`) — le cœur (`xmit.c`, `control.c`) ne sait pas qu'il parle
au matériel.

- Device `SREMFB_KVM_ENCODER` (`/dev/video11`, `bcm2835-codec`, rôle encode,
  `V4L2_CAP_VIDEO_M2M_MPLANE`). Modèle d'implémentation : `client/v4l2dec.c`
  (ioctls bruts, pas de libv4l) — même structure, dans l'autre sens.
- OUTPUT (entrée brute) : `V4L2_PIX_FMT_YUV420` (I420, ce que `yuvbuf` contient
  déjà), `MMAP`, 2 buffers ; la frame est copiée plans par plans dans le buffer
  (3 Mo en 1080p, ~1 ms). CAPTURE (bitstream) : `V4L2_PIX_FMT_H264`, `MMAP`,
  4 buffers de 1 Mo.
- Contrôles à l'ouverture :
  `BITRATE_MODE = CBR`, `BITRATE = kbps×1000` (borné à **25 000 kb/s**, maximum
  du driver — `control.c` peut demander 40 000), `H264_PROFILE = HIGH`,
  `H264_LEVEL = 4.0`, `REPEAT_SEQ_HEADER = 1` (SPS/PPS avec chaque IDR : chaque
  épisode commence par un IDR que le décodeur doit pouvoir avaler seul),
  `HEADER_MODE = JOINED_WITH_1ST_FRAME`, `H264_I_PERIOD` au maximum accepté
  (pas d'IDR périodique — le cœur n'en veut pas, cf. `encoder.c`), B-frames 0.
  Tout contrôle refusé est loggé et ignoré, sauf le format.
- `sremfb_enc_encode(force_idr)` est **synchrone** comme x264 : si `force_idr`
  → `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME` ; QBUF OUTPUT ; `poll()` avec timeout
  (200 ms) ; DQBUF CAPTURE ; renvoie `(pointeur, taille, is_idr)` où
  `is_idr = flags & V4L2_BUF_FLAG_KEYFRAME`. Le buffer CAPTURE est remis en file
  **au début de l'appel suivant** (le cœur garantit que la frame précédente est
  partie : « valid until the next call »). Timeout ou erreur → `-1`, le cœur
  bascule en RAW (`h264_failed`).
  Latence ajoutée ≈ un encodage (10–30 ms en 1080p) pendant lequel le main
  loop attend : accepté en v1, rendu asynchrone en phase 2 (§14).
- `sremfb_enc_set_bitrate()` : `VIDIOC_S_CTRL BITRATE` à chaud (le driver
  l'accepte en streaming).
- VUI : le bloc matériel n'écrit pas forcément les infos colorimétriques ;
  **par convention de protocole le flux est BT.601 limited** (PROTOCOL.md) et
  le décodeur doit l'appliquer quoi que dise (ou ne dise pas) le VUI — §6.
- Build dev PC : `make sremfb-kvm ENCODER=x264` relie `core/encoder.o` à la
  place ; le mode test §4.9 permet alors de valider tout le chemin H.264 avec
  `sremfb-view` sans Pi.

#### Écarts d'implémentation (lot C, branche `kvm-encoder`)

Implémentation écrite d'après le source du driver (`bcm2835-v4l2-codec.c`,
`rpi-6.12.y`) et validée contre un faux driver seulement : **rien n'a tourné
sur un Pi** (voir la liste « à valider » plus bas).

- **Ordre des ioctls** : S_FMT CAPTURE (H264) puis S_FMT OUTPUT (YUV420),
  S_PARM, REQBUFS CAPTURE puis OUTPUT, **contrôles après REQBUFS** : le
  driver ne crée le composant MMAL qu'au premier REQBUFS et ne pousse les
  contrôles au firmware qu'une fois celui-ci créé ; posés avant, ils seraient
  appliqués sans retour d'erreur (`v4l2_ctrl_handler_setup`). Puis
  QUERYBUF/mmap, QBUF de tous les CAPTURE, STREAMON OUTPUT et CAPTURE.
- **Géométrie OUTPUT relue** : en encode, le driver arrondit `bytesperline`
  à 64 (1366 → 1408) et ne touche pas la hauteur ; la frame est copiée ligne
  à ligne quand le stride diffère, U à `bpl × hauteur`, V à
  `U + bpl/2 × hauteur/2`, un seul plan mémoire (`num_planes = 1`). Largeur
  rabotée par le driver (> 1920 ou < 32) ou dimensions impaires → l'ouverture
  échoue (x264 refuse aussi l'impair en 4:2:0) et le client reste en RAW.
- **Level 4.2** (repli 4.1 puis 4.0), profil High (repli Main puis
  Constrained Baseline) : 4.0 ne couvre que 1080p30, alors que la capture va
  jusqu'à 1080p50 et que x264 annonce déjà 4.2 (fps 60). S_PARM = 60 i/s,
  comme x264 (le débit par frame du contrôle de débit en dépend).
- **CAPTURE** : 4 buffers de `max(1 Mio, w × h)` (2 Mio en 1080p, au lieu
  des 768 Kio par défaut du driver ; le driver accepte une `sizeimage` plus
  grande sur un format compressé). Une AU qui déborde arrive en plusieurs
  buffers de même timestamp : elle est recollée dans un tampon à part (AU
  complète = une tranche vue et un buffer pas plein ; 50 ms de grâce si le
  dernier buffer était plein). Tout buffer d'un autre timestamp est rendu
  au driver sans être lu (reliquat d'une frame abandonnée, ou le buffer de
  rebut à timestamp nul que le firmware peut sortir en premier — constaté
  par ustreamer), sauf s'il ne contient que des en-têtes (SEI/SPS/PPS/AUD),
  au cas où le firmware les sortirait à part sans recopier le pts.
- **`is_idr`** = `V4L2_BUF_FLAG_KEYFRAME` **ou** une NAL de type 5 dans l'AU.
  `force_idr` qui ne donne pas d'IDR (ou `FORCE_KEY_FRAME` refusé) → `-1` :
  un épisode ouvert sur une P serait prédit d'une image périmée côté client,
  le RAW est préférable. `FORCE_KEY_FRAME` est aussi envoyé pour la première
  frame (inoffensif).
- **Comparaison** (lecture seule, aucun code repris) : ustreamer
  (`src/ustreamer/m2m.c`, PiKVM) suit la même séquence (contrôles par
  S_CTRL, S_FMT des deux files, S_PARM, REQBUFS/mmap/QBUF, STREAMON,
  `FORCE_KEY_FRAME` avant QBUF, poll POLLIN, appariement par timestamp,
  `sizeimage` CAPTURE agrandie à 1,5 Mio pour les grosses IDR, level 4.2
  pour 1080p au-delà de 30 i/s). Différences : il pose les contrôles avant
  S_FMT (même effet, sans retour d'erreur), borne les QP à 16–32 (ici les
  défauts du driver, 20–51 : réglage à reprendre sur Pi si le texte bave)
  et garde une IDR périodique (GOP), que le cœur sRemFB ne veut pas.
- **Timeouts** : 200 ms par frame, **1 s pour la première** (démarrage du
  composant). Timeout, `V4L2_BUF_FLAG_ERROR`, `LAST` ou erreur d'ioctl → `-1`,
  le cœur repasse en RAW pour ce client (`h264_failed`).
- **Débit** : borné à 25–25 000 kb/s ; `sremfb_enc_bitrate()` renvoie la
  valeur effective (bornée), si bien que l'AIMD de `control.c` plafonne à
  25 000 sans erreur. Pas de VBV réglable : une IDR n'est pas limitée aux
  ~100 ms de débit que garantit x264.
- **Device** : `SREMFB_KVM_ENCODER` lu par l'encodeur lui-même (défaut
  `/dev/video11`) ; l'ouverture vérifie M2M_MPLANE et la présence de
  YUV420 → H264 (mauvais nœud → message explicite). Sans device (PC), chaque
  client qui ouvre un épisode H.264 essaie une fois, échoue avec un
  avertissement clair, et reste en RAW/LZ4 ; le serveur continue (le cœur
  journalise ensuite « x264 init failed, staying raw », libellé de
  `core/xmit.c` laissé tel quel).
- **`--probe`** : après le rapport du device, un **essai d'encodage réel**
  1080p par l'API du cœur (IDR, P, IDR forcée, débit 40 000 demandé) avec
  tailles, `is_idr` et durée ; avec `ENCODER=x264`, seul l'essai est fait.
  Le Makefile passe `-DSREMFB_KVM_ENC` à `capture.c` (témoin
  `kvm/.encoder-*` pour recompiler quand `ENCODER` change).
- `kvm.h` inchangé.

À valider sur Pi (le faux driver ne le prouve pas) :

1. Disposition mémoire YUV420 réellement lue par le firmware en 1080
   lignes (hauteur non multiple de 16) et avec un stride arrondi (1366×768,
   1600×900) : U à `bpl × 1080` ? Sinon chroma décalée → passer par
   `ALIGN(h, 16)` + sélection CROP.
2. `FORCE_KEY_FRAME` appliqué **à la frame qui suit** (pas une frame plus
   tard) ; sinon chaque épisode retombe en RAW (« IDR demandée, frame
   non-IDR reçue »).
3. SPS/PPS dans le même buffer que l'IDR (HEADER_MODE + REPEAT_SEQ_HEADER),
   première frame IDR, KEYFRAME posé.
4. `H264_I_PERIOD = 0x7FFFFFFF` sans IDR périodique ni effet de bord du
   firmware (repli : 0 = « une IDR initiale puis que des P » selon raspivid).
5. Level 4.2 accepté par le firmware ; CBR réellement suivi, `BITRATE`
   modifiable en streaming ; taille des IDR 1080p d'un bureau chargé à
   25 Mb/s (< 2 Mio ?).
6. Latence d'encodage 1080p (attendu 10–30 ms), délais 200 ms / 1 s
   suffisants ; CMA disponible pour 2×3 Mo + 4×2 Mio en plus de la capture.
7. VUI écrit par le firmware (le protocole impose BT.601 limited de toute
   façon) ; décodage du flux par `sremfb-view` et par le décodeur SBC.

### 4.7 `input_hid.c` + `hidmap.c` — entrées vers la cible

#### 4.7.1 Principe

Même API que `core/input.c` (`sremfb_input_init/start/stop/msg`). Les messages
INPUT (un événement evdev par message, effectifs au `SYN_REPORT`) sont
accumulés dans l'état d'un périphérique HID et **un rapport est écrit au
`SYN_REPORT`** si quelque chose a changé. Trois fonctions HID du gadget (§5.1) :

| `sremfb_indev` | nœud | rapport |
|---|---|---|
| `KEYBOARD` | `/dev/hidg0` | clavier boot protocol, 8 octets : `[modificateurs][réservé][k1..k6]` |
| `MOUSE` (relatif) | `/dev/hidg1` | 5 octets : `[boutons b0..b4][dx int8][dy int8][wheel int8][hwheel int8]` |
| `POINTER` (absolu) | `/dev/hidg2` | 7 octets : `[boutons][X u16 LE 0..32767][Y u16 LE][wheel int8][hwheel int8]` |
| `GAMEPAD` | — | ignoré en v1 (loggé une fois) |

- Ouverture en `O_RDWR|O_NONBLOCK|O_CLOEXEC` dans `sremfb_input_start()` ;
  un nœud absent = fonction absente ; `SREMFB_SRV_FLAG_INPUT` renvoyé si au
  moins un nœud est ouvert. **Un seul client à la fois** peut tenir les
  entrées (le gadget est unique) : le premier client streaming avec
  `input_cap` les prend ; les suivants n'obtiennent pas le flag INPUT (log).
  À sa déconnexion, le prochain client qui se (re)connecte les obtient.
- `write()` non bloquant : quand la cible ne lit pas (éteinte, en veille, pas
  encore énumérée, BIOS sans souris absolue), `EAGAIN`/`ESHUTDOWN` → le rapport
  est **abandonné** sans jamais bloquer le main loop ; un message de log, une
  fois par minute au plus. Après une erreur « dure » (`ENODEV`), tenter de
  réouvrir le nœud à la prochaine connexion client.
- Rapports de sortie (LED clavier) : le nœud `hidg0` est lisible ; lire et
  jeter (watch `G_IO_IN`) pour ne pas laisser grossir la file.
- `sremfb_input_stop()` et `SREMFB_INDEV_ALL/ev_type 0` : **relâcher tout** —
  rapport clavier nul, boutons nuls sur les deux souris. Rien ne reste enfoncé
  après une coupure réseau (même garantie que `core/input.c`).

#### 4.7.2 Clavier

- `hidmap.c` : table `uint8_t sremfb_hid_usage[KEY_CNT]` evdev → usage HID
  (page 0x07). **Générée** par inversion de `hid_keyboard[256]` de
  `drivers/hid/hid-input.c` du noyau Linux (le mapping de référence, celui que
  la cible appliquera à l'envers), complétée pour les touches media/consumer
  absentes (ignorées en v1). Fichier commenté, chaque ligne `[KEY_X] = 0xNN`.
- Modificateurs → bits du premier octet : `LEFTCTRL`→0, `LEFTSHIFT`→1,
  `LEFTALT`→2, `LEFTMETA`→3, `RIGHTCTRL`→4, `RIGHTSHIFT`→5, `RIGHTALT`→6,
  `RIGHTMETA`→7.
- 6 touches simultanées (6KRO) : une 7ᵉ pression est ignorée (et loggée en
  debug) ; relâchement = retrait de la liste. Bitmap `down[KEY_CNT/8]` comme
  dans `core/input.c` pour le release-all et pour refuser les doubles
  pressions.
- Pas d'autorepeat : la cible répète elle-même (comme un vrai clavier USB).
- La visionneuse envoie des **codes par position physique** (scancode SDL →
  evdev, `view/keymap.c`) : la disposition appliquée est celle de la cible,
  comme pour un clavier USB branché dessus. Comportement voulu, à documenter.

#### 4.7.3 Souris relative

`REL_X`/`REL_Y`/`REL_WHEEL`/`REL_HWHEEL` accumulés jusqu'au `SYN_REPORT` ; une
accumulation hors de −127..127 est découpée en plusieurs rapports.
`REL_WHEEL_HI_RES`/`REL_HWHEEL_HI_RES` : ignorés (la visionneuse envoie les deux
formes ; on garde la grossière). Boutons `BTN_LEFT/RIGHT/MIDDLE/SIDE/EXTRA` →
bits 0..4.

#### 4.7.4 Pointeur absolu

`ABS_X`/`ABS_Y` en pixels du **flux** (0..geom−1, cf. `protocol.h`) →
`sremfb_kvm_abs_map(fit, x, y, &hx, &hy)` :

```
sx = (x - fit.ox) / fit.s ; sy = (y - fit.oy) / fit.s        # inversion du letterbox
sx = clamp(sx, 0, src_w-1) ; sy = clamp(sy, 0, src_h-1)      # un clic dans les bandes = bord
hx = (sx + 0.5) * 32768 / src_w ; hy = (sy + 0.5) * 32768 / src_h ; clamp 0..32767
```

Sans signal (`src_w == 0`) : utiliser `geom` comme source (identité). Les
boutons et molettes sur le device POINTER suivent le même rapport (7 octets).
Comme dans `core/input.c`, un clic reçu **avant** le premier `ABS` de la
connexion est jeté (`abs_placed`).

La visionneuse est en souris absolue par défaut ; la souris relative sert au
mode « capturé » (Ctrl droit) et aux BIOS sans support tablette.

### 4.8 `stubs.c` — audio, layout, USB

- `sremfb_audio_init/start/stop/shutdown` : no-op, `start` renvoie 0 (pas de
  flag AUDIO).
- `sremfb_layout_init/client/client_gone` : no-op.
- Téléport USB : `main-kvm.c` fait `setenv("SREMFB_NO_USB", "1", 0)` ; le
  cœur n'écrit alors jamais dans `/run/sremfb`.

### 4.9 `testsrc.c` — mode test sans matériel

`SREMFB_KVM_DEVICE=test:1920x1080@50` : un `g_timeout` à la cadence demandée
produit des frames UYVY dans des buffers internes présentés à `capture.c` par
le **même chemin** (diff → convert → fan-out) que les frames V4L2 (`fd < 0`,
`DQBUF` remplacé par un appel direct). Contenu : fond fixe (mire, texte
statique), un rectangle qui se déplace, un compteur de frames en haut à gauche,
et sur demande (`SIGUSR1`) un « changement de mode » simulé vers 1024×768 puis
retour, pour tester §4.3.4 et le letterbox. `file:chemin@WxH@fps` rejoue un
fichier UYVY brut en boucle (capturable sur le Pi avec `v4l2-ctl --stream-to`).

Ce mode permet aux agents de développer et tester **sur le PC** (`make
sremfb-kvm ENCODER=x264` + `sremfb-view`), et au Pi d'être validé sans cible
branchée.

### 4.10 Retouches dans `core/` (minimales, sans effet sur EVDI/SPICE)

- `core/core.h` : rien à changer a priori. Si `kvm.h` a besoin d'un champ par
  client, il va dans `src_ctx`.
- `core/session.c` : `sremfb_client_lost()` appelle `sremfb_audio_stop`,
  `sremfb_layout_client_gone` → satisfaits par `stubs.c`. Rien à changer.
- `core/xmit.c` / `control.c` : rien. Le plafond `KBPS_MAX 40000` est borné
  côté `encoder_v4l2.c`.
- `core/convert.c` : la conversion BGRx→I420 est réutilisée telle quelle pour
  nourrir l'encodeur (phase 2 : court-circuit UYVY→I420 direct, §14).
- `core/Makefile` : la factorisation de `alloc_buffers()/free_buffers()`
  (dupliquée dans `spice_source.c`) peut rester locale à `kvm/` pour ne pas
  toucher à la branche `spice-backend`.

### 4.11 Journal et statistiques

Au style de `send_stats()` d'`evdi.c` : toutes les 10 s par client,
`[mac] 50.0 fps capture, 12.3 fps damage, 8 % tuiles sales, mode raw, 42 Mb/s
wire`, plus un message à chaque événement de source (signal perdu/retrouvé,
mode `1920x1080p50 UYVY`), chaque bascule RAW/H.264 (déjà dans le cœur) et
chaque prise/relâche des entrées.

`sremfb-kvm --probe` : sans servir, ouvre le device, pose l'EDID, interroge
les timings et le format, ouvre l'encodeur (formats et contrôles acceptés) et
les nœuds HID, imprime un rapport texte et sort (0 = tout est là, 1 sinon).
C'est la base de `tools/kvm-diag` (§8.5) et il remplace `v4l2-ctl` sur
l'image millefeuille.

---

## 5. Fichiers livrés avec `sremfb-kvm` (dépôt IPKVM, `feuillet/` et `pi-os/`)

### 5.1 `feuillet/usr/libexec/sremfb-kvm-gadget` — gadget USB (configfs)

Script shell POSIX, `start|stop|status`, idempotent, utilisable par systemd
(Pi OS) comme par le `run` nanoinit (millefeuille). Monte `/sys/kernel/config`
s'il ne l'est pas. Gadget `/sys/kernel/config/usb_gadget/sremfb` :

| Attribut | Valeur |
|---|---|
| `idVendor` / `idProduct` | `0x1d6b` / `0x0104` (Linux Foundation, multifunction composite — les mêmes ids que les devices uinput de `core/input.c`) |
| `bcdUSB` / `bcdDevice` | `0x0200` / version sRemFB |
| `strings/0x409` | manufacturer `sRemFB`, product `sRemFB KVM`, serialnumber = MAC Ethernet du Pi |
| `configs/c.1` | `bmAttributes 0xC0` (self-powered), `MaxPower 2` |
| `functions/hid.kbd` | `protocol 1`, `subclass 1` (boot keyboard, utilisable au BIOS), `report_length 8` |
| `functions/hid.mouse` | `protocol 0`, `subclass 0`, `report_length 5` |
| `functions/hid.pointer` | `protocol 0`, `subclass 0`, `report_length 7` |
| `UDC` | premier de `/sys/class/udc` (`fe980000.usb` sur Pi 4) |

Descripteurs de rapport (hex, à écrire tels quels dans `report_desc`) :

```
kbd (boot keyboard, 8 octets, + 1 octet de sortie LED) :
05 01 09 06 A1 01 05 07 19 E0 29 E7 15 00 25 01 75 01 95 08 81 02 95 01 75 08
81 01 95 05 75 01 05 08 19 01 29 05 91 02 95 01 75 03 91 01 95 06 75 08 15 00
25 65 05 07 19 00 29 65 81 00 C0

mouse (relatif, 5 boutons, X/Y int8, wheel, AC Pan) :
05 01 09 02 A1 01 09 01 A1 00 05 09 19 01 29 05 15 00 25 01 95 05 75 01 81 02
95 01 75 03 81 01 05 01 09 30 09 31 15 81 25 7F 75 08 95 02 81 06 09 38 15 81
25 7F 75 08 95 01 81 06 05 0C 0A 38 02 15 81 25 7F 75 08 95 01 81 06 C0 C0

pointer (absolu, 5 boutons, X/Y u16 0..32767, wheel, AC Pan) :
05 01 09 02 A1 01 09 01 A1 00 05 09 19 01 29 05 15 00 25 01 95 05 75 01 81 02
95 01 75 03 81 01 05 01 09 30 09 31 15 00 26 FF 7F 75 10 95 02 81 02 09 38 15
81 25 7F 75 08 95 01 81 06 05 0C 0A 38 02 15 81 25 7F 75 08 95 01 81 06 C0 C0
```

L'ordre des fonctions (kbd, mouse, pointer) fixe la numérotation `hidg0..2`.
`stop` : vider `UDC`, retirer les liens de config, les fonctions, les strings,
le gadget. Noyau : `dwc2` en périphérique (`dtoverlay=dwc2,dr_mode=peripheral`),
`libcomposite`, `usb_f_hid` — modules du noyau Pi, chargés par `modprobe` dans
le script (`bcm2711_defconfig` les fournit en `=m`, §7.2).

### 5.2 `feuillet/usr/share/sremfb/edid-1080p50.bin` / `.txt`

EDID 1.3 + bloc CEA-861 (256 octets), nom de moniteur `sRemFB KVM`, modes :
**1920×1080@50 (préféré)**, 1280×720@60, 1024×768@60, 800×600@60, 640×480@60 ;
pas de 1080p60 ni de 1920×1200 (hors capacité 2 lanes). Point de départ : les
EDID de PiKVM (`kvmd/configs/kvmd/edid/`), à ré-émettre avec les bonnes
chaînes et checksums (`edid-decode` pour vérifier). Le `.txt` est le format hex
accepté par `v4l2-ctl --set-edid=file=` (débogage à la main). Audio : pas de
bloc audio en v1 (sinon la cible y envoie du son que personne ne lit).

### 5.3 `pi-os/sremfb-kvm.service` + `sremfb-kvm.conf.example` (dev Pi OS)

Unité **système** (pas de session utilisateur sur le Pi) : `After=network-online`,
`ExecStartPre=/usr/libexec/sremfb-kvm-gadget start`, `ExecStart=/usr/bin/sremfb-kvm`,
`EnvironmentFile=/etc/sremfb-kvm.conf`, `Restart=always`, `User=root` (hidg,
video — ou udev rules + groupes `video`/`input` si on veut descendre de
privilèges, §13 Q6). `make install-pios` dans le dépôt IPKVM copie binaire,
gadget, EDID, unité et conf sur un Pi OS (build natif sur le Pi ou croisé).
Pas de paquet .deb en v1.

---

## 6. `sremfb-view` — décodage H.264 (dépôt sRemFB, `view/`)

Résumé des contraintes (détail dans l'exploration du code) : le serveur émet
une AU Annex B complète par message, **High profile** (x264 `superfast`, 4
slices), I420 BT.601 limited, pas de B-frames, un IDR avec SPS/PPS **seulement
en début d'épisode**, et aucun message client pour redemander un IDR. L'épisode
se termine par `H264_EOS` suivi d'un snapshot RAW/LZ4 plein cadre.

### 6.1 Décisions

- **libavcodec + libavutil** (FFmpeg), pas openh264 (Baseline seulement).
  `avcodec_find_decoder(AV_CODEC_ID_H264)`, `flags |= AV_CODEC_FLAG_LOW_DELAY`,
  `thread_type = FF_THREAD_SLICE` (le frame-threading ajouterait N images de
  latence ; les 4 slices du serveur rendent le slice-threading utile),
  `thread_count = 0` (auto). Pas de libswscale : conversion maison.
- **Option (a)** : décodage **synchrone dans le thread réseau**, conversion
  YUV420P → format fil (XRGB8888 ou RGB565 avec le dither existant) dans
  `FB.pixels` avec `damage_all = 1`, puis `publish()` immédiat. Conserve
  intacts le rendu, `dump_ppm`, `--test shot`, la sonde de latence et la
  sémantique du PONG (« tout ce qui précède a été appliqué » — le serveur
  mesure ainsi le coût du décodeur). La conversion reprend `yuv_row_to_fb()`
  du client SBC (BT.601 limited, virgule fixe) ; **ignorer le VUI** du flux
  (convention de protocole, §4.6). Une texture YUV SDL (option b) reste une
  optimisation possible plus tard.
- Hello : `SREMFB_HELLO_FLAG_H264` sauf `--no-h264` / `SREMFB_NO_H264` /
  `h264_broken`. Logger `srv_flags & SREMFB_SRV_FLAG_H264`.
- `net.c` `stream_loop()` : branches `SREMFB_ENC_H264` (valider `x==0, y==0,
  w==W, h==H`, `0 < payload_len ≤ 1 MiB`, lire dans un `aubuf` paresseux avec
  `AV_INPUT_BUFFER_PADDING_SIZE`, `avcodec_send_packet`/`receive_frame`,
  vérifier `frame->width/height == W/H` sinon copier le min) et
  `SREMFB_ENC_H264_EOS` (`send_packet(NULL)`, recevoir jusqu'à `AVERROR_EOF`,
  afficher la dernière image, `avcodec_flush_buffers()`, et seulement ensuite
  lire le message suivant — le snapshot RAW doit arriver après). Contexte
  ouvert à la première AU, fermé en fin de connexion.
- Erreur de décodage : `h264_broken = 1` (persistant pour le process), couper
  la connexion, se reconnecter sans annoncer H.264 (comportement identique au
  client SBC).
- Le batching 2 ms de RAW/LZ4 ne s'applique pas : une AU = une image, publiée
  tout de suite (l'en-tête non-RAW flushe déjà le lot en attente).
- Stats (`--stats`) : AU reçues, IDR, ms/AU de décodage, mode courant.
- Sonde de latence : test de pixel après chaque frame décodée avec une
  tolérance (quantification) au lieu de l'égalité exacte.
- Build : `view/Makefile` ajoute `pkg-config libavcodec libavutil` ; `make
  H264=0` pour s'en passer ; `pkg/build-debs.sh` ajoute `libavcodec61` (Debian
  13) au repli de dépendances ; BUILD.md / README (« jamais de H.264» → à
  réécrire) / commentaires d'en-tête mis à jour ; option `--no-h264` dans
  `usage()`.
- Test sans Pi : serveur EVDI avec `SREMFB_FORCE_H264=1`, ou `sremfb-kvm
  ENCODER=x264` en mode test. Corpus : `SREMFB_TEST_H264_SINK` du client SBC
  produit un fichier Annex B rejouable.

---

