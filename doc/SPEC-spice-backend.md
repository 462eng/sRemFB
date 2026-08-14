# SPEC — Backend SPICE pour sRemFB (`sremfb-spice`)

Statut : draft pour implémentation · Cible : repo `462eng/sRemFB` · Langue du code : C, style existant du repo

---

## 1. Contexte et objectif

`sremfb-server` capture aujourd'hui les pixels via **EVDI** (moniteur virtuel sur un
poste GNOME/Wayland) et les streame vers des clients SBC (`sremfb-client`) via le
protocole décrit dans `PROTOCOL.md` (v2).

Objectif : ajouter une **seconde source de pixels** — une VM QEMU/KVM exposée en
**SPICE** (cas d'usage principal : Proxmox VE, `vga: qxl`) — sans toucher ni au
protocole réseau ni au client. Le nouveau binaire agit comme **client SPICE**
(via `spice-client-glib`) d'un côté, et comme **serveur sRemFB** de l'autre.

Cas d'usage : panels SBC d'atelier affichant un dashboard hébergé dans une VM
(sans session graphique sur l'hôte, sans indicateur de capture, "no signal"
propre quand la VM s'arrête).

### Non-objectifs (hors périmètre de cette spec)

- Aucune modification de `protocol.h`, du wire format v2, ni de `sremfb-client`.
- Pas d'input clavier/souris vers la VM via le protocole sRemFB (display-only).
- Pas de renouvellement automatique de tickets via l'API Proxmox (phase 2).
- Pas de redirection USB usbredir (phase 2, voir §10).
- Pas de support multi-serveurs SPICE dans un même process (1 process = 1 VM).

---

## 2. Architecture cible

### 2.1 Refactor préalable : extraction d'un cœur commun

Le serveur actuel mélange (a) l'acquisition EVDI et (b) tout le reste :
sessions clients, files par client non bloquantes, conversion RGB565/dithering,
LZ4, encodeur H.264 adaptatif, PING/PONG, stats. Extraire (b) dans un module
réutilisable :

```
server/
  core/           # nouveau : indépendant de la source
    session.c/h   # accept, hello, per-client queue, envoi non bloquant
    encode.c/h    # RAW/LZ4/RGB565+dither/x264, épisodes H.264, congestion
    source.h      # interface de source de frames (voir 2.2)
  evdi/           # backend existant, adapté à source.h
  spice/          # NOUVEAU backend
  main-evdi.c     # binaire sremfb-server (inchangé fonctionnellement)
  main-spice.c    # binaire sremfb-spice
```

Deux binaires distincts pour des dépendances propres :

| Binaire | Libs | Tourne sur |
|---|---|---|
| `sremfb-server` | libevdi, glib, lz4, x264 | poste GNOME (inchangé) |
| `sremfb-spice` | spice-client-glib-2.0, glib, lz4, x264 | hôte PVE **ou** n'importe quelle machine du LAN (SPICE = TCP) |

Dépendance build nouvelle : `libspice-client-glib-2.0-dev`.

**Contrainte forte** : le refactor ne doit introduire **aucune régression**
fonctionnelle du chemin EVDI. Le comportement de `sremfb-server` après refactor
doit être bit-identique sur le wire (mêmes messages, mêmes tailles).

### 2.2 Interface `source.h` (proposition, à ajuster au code réel)

```c
struct sremfb_source_ops {
    /* Démarre la source pour un client dont le hello annonce (xres, yres).
     * Doit aboutir à un premier framebuffer complet ou échouer.
     * Retour : 0 = OK, sinon un sremfb_status (SERVER_FAIL, NO_DEVICE). */
    int  (*acquire)(void *ctx, const struct sremfb_client_hello *hello,
                    struct sremfb_source_geom *out_geom);

    /* Libère la ressource associée au client (déconnexion, remplacement). */
    void (*release)(void *ctx);

    /* Pointeur vers le framebuffer courant (XRGB8888, stride en octets). */
    const uint8_t *(*fb)(void *ctx, int *stride);
};

/* Callbacks source -> core (la source pousse, le core consomme) : */
void sremfb_core_damage(struct client *c, const struct rect *rects, int n);
void sremfb_core_blank(struct client *c, bool blank);   /* -> BLANK/UNBLANK  */
void sremfb_core_source_lost(struct client *c);         /* -> ferme la conn. */
```

Le backend EVDI mappe ses events existants sur ces trois callbacks. Le backend
SPICE fait de même (voir §4). Le core reste propriétaire du cycle
damage-accumulation → build-frame-au-drain (comportement actuel inchangé).

---

## 3. Modèle de session SPICE

- **1 process `sremfb-spice` = 1 VM** (une `SpiceSession`). Multi-VM = plusieurs
  instances (unités systemd template `sremfb-spice@.service`).
- La session SPICE est établie **au démarrage du process** et maintenue en
  permanence (reconnexion avec backoff 1→30 s si la VM redémarre/migre).
  Rationale : latence d'attach client minimale + détection immédiate de la
  disponibilité de la VM.
- Tant que la session SPICE est **down** : les clients sremfb qui se connectent
  reçoivent `server_hello.status = SERVER_FAIL` (le panel reste en "no signal",
  le client retente — comportement client existant, ne pas y toucher).
- **Fan-out** : plusieurs clients sremfb peuvent consommer le **même** canvas
  (miroir). Les files par client du core gèrent déjà les débits hétérogènes.
- **Multi-têtes (phase 1 optionnelle, voir §12)** : mapping MAC client →
  display SPICE id. Par défaut, tous les clients consomment le display 0.

### 3.1 Configuration (`/etc/sremfb-spice.conf`, format env comme l'existant)

| Variable | Défaut | Rôle |
|---|---|---|
| `SREMFB_SPICE_HOST` | — (requis) | hôte SPICE (nœud PVE ou IP VM selon config) |
| `SREMFB_SPICE_PORT` | — (requis) | port SPICE de la VM |
| `SREMFB_SPICE_PASSWORD` | — | mot de passe statique (via QMP `set_password`) |
| `SREMFB_SPICE_TLS` | 0 | 1 = utiliser tls-port + CA (option, si simple) |
| `SREMFB_SPICE_CA_FILE` | — | CA pour TLS (pveproxy : `/etc/pve/pve-root-ca.pem`) |
| `SREMFB_PORT` | 4629 | port d'écoute sRemFB (identique à l'existant) |
| `SREMFB_ALLOW` | — | CIDR allowlist (réutiliser le code existant) |
| `SREMFB_RESIZE` | `agent` | `agent` = piloter la résolution invitée via vdagent ; `scale` = ne jamais toucher l'invité, toujours scaler ; `off` = refuser (`BAD_HELLO`) si géométrie ≠ |
| `SREMFB_DISPLAY_MAP` | — | optionnel : `MAC=display_id,...` (multi-têtes) |

Les variables client/serveur existantes (`SREMFB_NO_H264`, `SREMFB_NO_DITHER`,
`SREMFB_FORCE_H264`…) s'appliquent à l'identique.

---

## 4. Mapping SPICE → protocole sRemFB

Bibliothèque : **spice-client-glib** (GObject, main loop GLib — le serveur
utilise déjà GLib). ⚠️ **Vérifier chaque nom d'API contre les headers installés**
(`/usr/include/spice-client-glib-2.0/`) avant d'écrire le code ; les noms
ci-dessous sont indicatifs et certains ont des variantes dépréciées.

### 4.1 Établissement

1. `spice_session_new()` ; propriétés `host`, `port` (ou `tls-port` + `ca-file`),
   `password`.
2. Signal `channel-new` sur la session : garder les refs vers
   `SpiceMainChannel` et `SpiceDisplayChannel` (display id configuré),
   `spice_channel_connect()` sur chacun. Ignorer inputs/cursor/record/playback
   (ne pas les connecter : pas d'input, et le curseur est **déjà composité
   côté serveur SPICE** quand aucun canal cursor n'est ouvert — à vérifier ;
   sinon, compositer le curseur côté bridge, voir §13 Q3).
3. Signaux du display channel :
   - `display-primary-create` → récupérer `format`, `width`, `height`,
     `stride`, pointeur données. Formats attendus : `SPICE_SURFACE_FMT_32_xRGB`
     (passthrough vers le pipeline XRGB8888 existant). Tout autre format :
     log + conversion ou rejet propre.
   - `display-invalidate` (x, y, w, h) → `sremfb_core_damage()`.
   - `display-primary-destroy` → voir §4.3.
   - `display-mark` → gate "le display est valide" avant de streamer.

### 4.2 Séquence hello client sremfb (le point délicat)

À réception d'un `sremfb_client_hello` (xres, yres) :

1. Si `SREMFB_RESIZE=agent` : demander à l'invité la résolution du client via le
   main channel — `spice_main_channel_update_display(main, id, 0, 0, xres, yres, TRUE)`
   puis envoi de la config moniteurs (API : `spice_main_send_monitor_config()`
   ou équivalent non déprécié). Nécessite **spice-vdagent dans l'invité**.
2. Attendre le `display-primary-create` à la bonne géométrie, **timeout 5 s**.
3. À l'issue :
   - Géométrie exacte obtenue → `server_hello` `OK`, `width/height = xres/yres`,
     streaming direct (zéro copie de scaling).
   - Timeout ou géométrie différente (pas d'agent, invité têtu, autre client
     déjà servi à une autre résolution) → **fallback scale** (voir §5) et
     `server_hello` `OK` quand même (le flux est toujours à la géométrie du
     client, conforme au protocole).
   - Session SPICE down → `SERVER_FAIL`.
4. Le `server_hello` annonce toujours la géométrie **du client** — le wire
   format v2 impose une géométrie de flux fixe par connexion, la source doit
   s'y plier, jamais l'inverse.

**Conflit de géométrie entre clients en fan-out** : premier arrivé, premier
servi pour le resize agent ; les suivants dont la géométrie diffère passent
automatiquement en scale. Log explicite dans le journal.

### 4.3 Événements de cycle de vie

| Événement SPICE | Action sRemFB |
|---|---|
| `display-primary-destroy` (changement de mode imminent, reboot invité) | `sremfb_core_blank(c, true)` → le panel s'éteint ; **ne pas** fermer les connexions |
| `display-primary-create` suivant | repaint full-frame + `UNBLANK` ; si géométrie ≠ flux → mode scale |
| Session SPICE déconnectée (VM stoppée/migrée) | `sremfb_core_source_lost()` sur tous les clients → connexions fermées → panels "no signal" ; boucle de reconnexion SPICE |
| Reconnexion SPICE OK | rien à faire : les clients sremfb retentent d'eux-mêmes |

Note : SPICE n'expose pas le DPMS invité. `primary-destroy` est une
**approximation** de BLANK, documentée comme telle dans le README.

### 4.4 Ce qui ne change pas

PING/PONG, la régulation par délai d'écho, les épisodes H.264 (x264 logiciel),
la conversion RGB565 + dithering, LZ4, les stats 5 s : tout est en aval de la
source et doit fonctionner **sans modification** via le core extrait.

---

## 5. Scaling fallback

Quand géométrie source ≠ géométrie flux :

- Scale **nearest-neighbor** si ratio entier, sinon bilinéaire simple,
  letterbox (bandes noires) si aspect ratio ≠. Pas de lib externe : ~100 lignes
  de C, sur XRGB8888, avant conversion RGB565 éventuelle.
- Les rects de damage source sont **transformés** dans le repère du flux
  (arrondis vers l'extérieur, +1 px de marge pour le bilinéaire).
- Perf cible : négligeable devant LZ4/x264 à 1080p (pas de SIMD requis en v1).

---

## 6. Contrainte côté VM : streaming vidéo SPICE

Le serveur SPICE peut compresser les zones "vidéo" en MJPEG (lossy), ce qui
casse la garantie pixel-exact et ferait double transcodage avec notre H.264.

- Exiger `streaming-video=off` côté QEMU. Sur PVE : vérifier la valeur générée
  par `qemu-server` ; si nécessaire, documenter l'ajout
  `args: -spice ...streaming-video=off` **ou** vérifier si spice-client-glib
  permet de refuser les streams côté client. À trancher pendant l'implémentation ;
  documenter le résultat dans le README.
- Si un `display-stream` arrive malgré tout : log WARNING une fois, et décoder
  si la lib le fait de manière transparente (spice-glib composite normalement
  les streams dans le canvas — à vérifier).

---

## 7. Côté Proxmox VE (documentation à livrer, pas de code)

Section README "Proxmox VE" à rédiger, couvrant :

1. VM : `qm set <vmid> --vga qxl` (ou `qxl2`/`qxl3`… pour multi-têtes),
   spice-vdagent installé dans l'invité (paquet `spice-vdagent`).
2. Port SPICE fixe : par défaut PVE alloue dynamiquement → deux options à
   documenter : (a) `args: -spice port=590X,addr=0.0.0.0,password=...` en
   remplaçant la conf générée (vérifier l'interaction avec la conf `-spice` de
   qemu-server — **point d'attention**, tester), (b) mot de passe via QMP
   `set_password spice ...` dans un hookscript au démarrage.
3. Modèle de sécurité : port SPICE exposé = accès console VM. Bridge/VLAN
   dédié, `SREMFB_ALLOW`, jamais sur le réseau de management. Reprendre le
   wording "dedicated, trusted LAN" du README existant.
4. Unité systemd : `systemd/sremfb-spice@.service` (template, `EnvironmentFile=/etc/sremfb-spice.d/%i.conf`), à ajouter au repo + cibles
   `make install-spice` et paquet Debian correspondant dans `BUILD.md`.

---

## 8. Arborescence des livrables

```
server/core/…                        # refactor (voir 2.1)
server/spice/spice_source.c/h        # backend
main-spice.c                         # main + config + boucle GLib
systemd/sremfb-spice@.service
BUILD.md / BUILD.fr.md               # deps, build, install-spice, PVE
README.md / README.fr.md             # section "Proxmox VE / SPICE backend"
```

`PROTOCOL.md` : **aucune modification** (ajouter au plus une phrase indiquant
que la source des pixels est indépendante du protocole).

---

## 9. Plan de test

1. **Non-régression EVDI** : après refactor, dérouler le test existant
   (`./server/sremfb-server` + `./client/sremfb-client --test 1920x1080 localhost`),
   comparer les `.ppm` produits avant/après refactor (identiques attendus).
2. **SPICE local sans PVE** : QEMU manuel
   `qemu-system-x86_64 -vga qxl -spice port=5900,disable-ticketing=on ...`
   avec un live Linux + vdagent. Client `--test` : vérifier hello → resize
   invité → flux, puis les scénarios :
   - changement de résolution dans l'invité en cours de stream (→ BLANK,
     re-create, scale ou re-lock) ;
   - arrêt/reboot de la VM (→ "no signal", reconnexion propre) ;
   - coupure réseau SPICE (→ `source_lost`, backoff) ;
   - 2 clients `--test` de géométries différentes (fan-out + scale) ;
   - `SREMFB_RESIZE=scale` et `off`.
3. **Congestion** : `SREMFB_FORCE_H264=1` et scénario throttling (tc netem)
   pour valider que les épisodes H.264 fonctionnent inchangés sur cette source.
4. **PVE réel** : VM `vga: qxl`, hookscript password, panel SBC physique.
   Vérifier lock screen, VM stop/start, migration (le bridge pointe le nouveau
   nœud ? → documenter la limite : host fixe en v1).

---

## 10. Phase 2 (ne PAS implémenter, mais ne pas l'empêcher)

- **USB** : chaîne usbip (SBC → hôte bridge, code serveur existant) +
  `SpiceUsbDeviceManager` pour rediriger les devices vhci dans l'invité.
  Garder le module usbip actuel factorisable ; prévoir l'accès root/polkit.
- Tickets PVE via API (renouvellement automatique, spiceproxy).
- Multi-têtes complet avec mapping `SREMFB_DISPLAY_MAP`.
- Suivi de migration de VM (résolution du nœud via API PVE).

---

## 11. Critères d'acceptation

- [ ] `sremfb-server` (EVDI) : comportement wire inchangé après refactor.
- [ ] `sremfb-spice` streame une VM PVE `qxl` vers `sremfb-client --test` et
      vers un SBC réel, en RAW/LZ4 et en épisode H.264 forcé.
- [ ] Resize invité via vdagent fonctionne ; fallback scale fonctionne sans
      vdagent ; `SREMFB_RESIZE=off` rejette proprement (`BAD_HELLO`).
- [ ] VM stop → "no signal" ≤ 7 s côté panel ; VM start → reprise sans
      intervention.
- [ ] 2 clients simultanés sur le même canvas, débits différents, aucun ne
      bloque l'autre (réutilisation des files existantes).
- [ ] `make`, `make install-spice`, paquet Debian, unité systemd, docs EN+FR.
- [ ] Aucun changement dans `protocol.h`, `client/`, ni le format wire.

## 12. Décisions déjà prises (ne pas rouvrir)

- Deux binaires, un core partagé (pas de libevdi sur les machines bridge).
- La géométrie du flux est celle du client, toujours (contrainte protocole v2).
- 1 process = 1 VM ; multi-VM par instances systemd.
- Pas d'input, pas d'auth sRemFB : modèle LAN de confiance existant.

## 13. Questions ouvertes (trancher pendant l'implémentation, documenter)

1. API exacte non dépréciée pour la config moniteurs dans la version de
   spice-client-glib packagée Debian stable (vérifier les headers).
2. Interaction `args: -spice` avec la ligne `-spice` générée par qemu-server
   (duplication acceptée par QEMU ? sinon quelle voie pour fixer le port).
3. Curseur : vérifier si le serveur SPICE composite le curseur dans la primary
   quand le canal cursor n'est pas ouvert ; sinon, compositer côté bridge
   (alpha blend dans le canvas avant damage).
4. `streaming-video=off` : valeur par défaut effective sur PVE, et/ou refus
   côté client possible (§6).
