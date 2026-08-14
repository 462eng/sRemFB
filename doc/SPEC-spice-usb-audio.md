# SCOPE — USB redirect + audio pour `sremfb-spice` (thin client de VM)

Statut : scoping, pas d'implémentation · Cible : branche `spice-backend`
(après le backend SPICE 1.4.0) · Objectif : transformer un SBC + panel en
**client léger d'une VM** — clavier/souris/stockage USB dans la VM, et son
de la VM sur le panel.

---

## 1. Contexte

`sremfb-spice` (1.4.0) affiche déjà une VM QEMU/KVM (qxl/SPICE) sur un ou
plusieurs SBC, sans latence perçue (surface hôte + damage, pas de
ré-encodage vidéo). Il manque deux choses pour un vrai thin client :

1. **Input** — aujourd'hui display-only. Le levier élégant : rediriger un
   **clavier/souris USB** branché sur le SBC *dans la VM* (usbredir). Ça
   règle l'input « gratuitement » (HID = USB) et prolonge le modèle de
   téléport USB déjà en prod (1.3.0), **sans toucher au protocole**.
2. **Audio** — le son de la VM doit sortir *sur le panel*. Contrairement à
   l'USB, ce n'est **pas** transparent : spice-glib livre l'audio à la
   machine bridge, pas au SBC. Il faut donc capter le PCM côté bridge, le
   **transporter** jusqu'au SBC (extension protocole, bit de
   fonctionnalité) et le **jouer** (ALSA) côté client.

Ces deux fonctionnalités sont de **natures très différentes** (l'une est
de la plomberie/privilèges, l'autre une nouvelle voie média protocole +
client) : elles sont scopées ensemble ici mais devront être livrées comme
deux chantiers distincts (§7).

### Non-objectifs
- Micro/record de la VM (playback seulement).
- Redirection USB isochrone lourde (webcam) garantie — best-effort.
- Sync lèvres audio/vidéo (pas d'horodatage commun ; latence bornée).
- Changement de la politique d'éligibilité USB côté SBC (inchangée).

---

## 2. USB redirect — architecture

### 2.1 La chaîne (réutilise l'existant)

```
 périph. USB ─► SBC (client/usbexport.c : bind usbip-host, usbipd :3240)
   ──usbip TCP──►  hôte bridge : vhci-hcd  (usbip attach → device LOCAL)
   ──SpiceUsbDeviceManager.connect_device──►  DANS la VM (canal usbredir)
```

- **Côté SBC : rien à changer.** `usbexport.c` exporte déjà les devices
  éligibles (HID + stockage + série, garde-fous NIC-active/disque-monté/
  blacklist Pi) via usbipd. La politique reste côté client.
- **Attache vhci sur le bridge :** identique au réconciliateur root actuel
  (`sremfb-usb-attach` + `sremfb-usb.path`/`.timer`), mais il tourne sur
  l'hôte bridge au lieu du PC EVDI. `usbip attach -r <ip> -b <busid>` rend
  le device du SBC **local** au bridge (un port vhci, un bus/addr libusb).
- **Redirection dans la VM :** `spice_usb_device_manager_get(session)` →
  `spice_usb_device_manager_connect_device_async(mgr, dev, …)` pousse ce
  device local dans la VM via le canal usbredir de *cette* session SPICE.

### 2.2 Le vrai problème : le routage SBC → bonne VM

Un bridge fait tourner **N instances** `sremfb-spice@…` (N VMs). Après
attache vhci, **tous** les devices des SBC deviennent locaux au bridge, et
le `SpiceUsbDeviceManager` de *chaque* session les voit **tous**. Il faut
garantir qu'un device exporté par SBC-X (client de l'instance I, VM-I)
soit redirigé **uniquement** dans VM-I.

Mapping retenu (chaque instance ne gère que SES clients) :

1. L'instance I connaît ses clients streaming (IP/MAC — déjà dans
   `srv->clients`).
2. Les enregistrements vhci `/var/run/vhci_hcd/port*` donnent, par device
   attaché, le couple **(ip source, busid)**. On en déduit le **bus/addr
   libusb local** du device (via `/sys/devices/platform/vhci_hcd.0/`).
3. L'instance I ne `connect_device` que les devices dont l'ip source ∈ ses
   clients. Elle ignore ceux des autres instances.
4. `auto-connect` du manager **désactivé** : redirection explicite et
   ciblée (jamais l'USB propre du bridge, jamais le device d'une autre
   VM). Un device SPICE ne peut de toute façon être redirigé que dans une
   seule VM à la fois.

Décision : l'`sremfb-spice` **intègre** le pilotage usbredir (il possède
la `SpiceSession`), l'attache vhci reste **externe et root** (§2.3).

### 2.3 Modèle de privilèges (LE point dur)

Deux opérations, deux niveaux :

| Opération | Privilège | Où |
|---|---|---|
| `usbip attach/detach` (bind vhci-hcd) | **root** | réconciliateur root (existant) |
| `connect_device` (claim libusb de `/dev/bus/usb/<bus>/<addr>`) | accès usbfs | **dans** `sremfb-spice` |

Le hic : `sremfb-spice` tourne en `DynamicUser` (isolé) et `connect_device`
doit **claim** le nœud usbfs → il lui faut les droits dessus. Options :

- **(A) Groupe + règle udev.** Un groupe `sremfb-usb` ; une règle udev pose
  `GROUP="sremfb-usb", MODE="0660"` sur les devices portés par vhci-hcd ;
  `sremfb-spice` tourne sous un user système membre du groupe (au lieu de
  DynamicUser). Isolation quasi conservée, pas de root. **Recommandé.**
- **(B) Helper polkit** façon spice-gtk
  (`spice-client-glib-usb-acl-helper`) : pose une ACL uaccess par device.
  Pensé pour une session desktop → bancal en service headless (pas de
  session logind). À éviter ici.
- **(C) `sremfb-spice` en root.** Le plus simple, mais perd l'isolation
  DynamicUser. Repli si (A) est trop fragile selon les distros.

À trancher au prototype : la stabilité de la règle udev (A) sur les
devices vhci (leur sysfs apparaît après l'attach — même piège de timing
que la règle EVDI historique → prévoir un `.timer`/rescan de secours).

### 2.4 Cycle de vie & config

- Config (par instance) : `SREMFB_USB=1` (défaut on), réutilise les
  garde-fous SBC ; rien de neuf côté serveur hormis l'activation usbredir.
- Le hello client porte déjà le **bit USB** (`SREMFB_HELLO_FLAG_USB`) : une
  instance n'arme la redirection que pour les clients qui l'annoncent.
- Départ client / arrêt VM : `disconnect_device` + le réconciliateur
  détache le port vhci (logique de purge `sta=6` déjà en place).
- Signaux à câbler : `device-added`/`device-removed` du manager (un device
  vhci apparaît/part → (dé)redirige si ip ∈ mes clients),
  `auto-connect-failed` (log).

### 2.5 Topologie alternative (mentionnée, non retenue)

SBC `usbredirserver` → QEMU `-device usb-redir,chardev=socket` en direct
(sans vhci ni libusb sur le bridge). Élégant mais : change le SBC (usbredir
au lieu d'usbip), exige du QMP par device côté QEMU, et n'utilise plus
spice-glib. On **garde la chaîne usbip existante** (déjà déployée, testée)
+ `SpiceUsbDeviceManager`.

### 2.6 Ce qui NE change pas
Protocole sRemFB, `sremfb-client`, backend EVDI : **rien**. L'USB redirect
est entièrement côté bridge (SBC export inchangé, serveur redirige).

### 2.7 Stockage local exporté (gadget USB mass-storage) — U3

Étendre le téléport USB à du **stockage local du SBC** (pas un périphérique
USB physique) : un fichier-image (fs-on-file), un blockdev (partition,
NVMe) ou une ISO, exposé à la cible comme **disque/CD USB** via un gadget
`f_mass_storage` sur `dummy_hcd`, puis la **même chaîne** usbip → usbredir.

```
image / blockdev / ISO sur le SBC
   ──►  gadget f_mass_storage (dummy_hcd, UDC virtuel, sans matériel)
   ──►  disque USB sur le bus HÔTE du SBC
   ──usbip──►  bridge (vhci)  ──SpiceUsbDeviceManager──►  DANS la VM
```

**Pourquoi ça marche (là où l'audio-gadget est écarté, §4.4)** : le
stockage USB est **bulk** (fiable, retransmissible, tolérant à la latence)
— le point fort d'usbip (son cas d'usage historique = disques USB
réseau), pas isochrone. Le même mécanisme dummy_hcd, verdict opposé, à
cause du type de transfert.

**Backing LUN** : `f_mass_storage` accepte un **fichier** (fs-on-file),
un **blockdev**, ou une **ISO** avec `cdrom=1` (→ la VM voit un lecteur CD
USB). Mode **RW** (sauvegarde) ou **RO** (distribution / ISO).

**Cas d'usage** :
- **Destination de sauvegarde** (principal) : la VM sauvegarde sur le
  stockage local du SBC — ex. le **NVMe de Milim** exposé en RW. Le stockage
  interne (non-USB) ne peut être « branché » dans la VM que par ce gadget.
- **Stockage personnel nomade** (futur, avec le sélecteur de serveur du
  §3) : le SBC porte les fichiers/ISO de l'utilisateur → apparaissent dans
  **n'importe quelle** VM/serveur auquel il se connecte. Le stockage suit
  la MAC, indépendant de la cible : panel = écran + clavier (usbredir) +
  **stockage** perso.
- **ISO → lecteur CD USB** (`cdrom=1`, RO) : média d'install/outils monté
  dans la VM.

**Garde-fous** (cohérents avec la politique USB existante) :
- **Jamais monté des deux côtés** : export RW seulement si le backing
  n'est **pas monté** côté SBC (sinon corruption : deux writers) ; sinon
  **RO**. C'est le garde-fou « jamais un fs monté » appliqué au backing.
- `sync` + `f_mass_storage` LUN eject/détach propre avant de retirer.

**Kernel SBC** : `CONFIG_USB_DUMMY_HCD` + `CONFIG_USB_CONFIGFS` +
`USB_CONFIGFS_MASS_STORAGE`. Sur **millefeuille** = un fragment kernel à
ajouter (comme usb-serial.fragment) ; sur Pi OS = module `dummy_hcd`.

**Config** (client) : `SREMFB_USB_STORAGE=<spec>[,<spec>…]`, chaque spec
`chemin[:ro|:rw|:cdrom]` — ex. `/dev/nvme0n1p3:rw`, `/data/backup.img:rw`,
`/isos/tools.iso:cdrom`. Éligible seulement si non-monté (RW) ; annoncé
par le même bit hello USB.

**Perf** : bulk over usbip-TCP → dizaines de Mo/s sur gigabit (A20, Milim)
— bon pour sauvegarde / scratch / distribution ; pas du high-IOPS (pour
ça : NBD/virtiofs, hors chemin USB).

---

## 3. Modèle de transport unifié : (MAC, rôle, head)

Une connexion sRemFB est désormais définie par un triplet, **tout sur un
seul port par VM** :

```
connexion = ( MAC , rôle ∈ {video, audio} , head ∈ {0,1,2,…} )
```

- **MAC** — identité du client physique (déjà là ; sert l'identité EDID).
- **rôle** — `video` (défaut, le comportement actuel) ou `audio` (le flux
  son de la VM). SPICE-only : côté EVDI seul `video` existe.
- **head** — quelle **tête d'affichage de la VM** (une VM `qxl2`/`qxl3`
  expose plusieurs canaux display). SPICE-only ; EVDI n'a qu'une tête (0).

### 3.1 Encodage dans le hello (additif, 48 octets inchangés)

Le hello v2 a la place exacte pour ça, **sans changer sa taille** ni casser
la compat (un client 1.3.0 → tout à zéro → `(video, head 0)`) :

- **rôle** : un bit dans `flags` — `SREMFB_HELLO_FLAG_AUDIO` (absent =
  video). `flags` n'utilise que 4 bits sur 16.
- **head** : un octet dans `reserved[9]` — `head_id` (0 par défaut).

### 3.2 Côté serveur SPICE : un process, une VM, toutes les têtes

**1 process = 1 VM reste vrai** (isolation, unité systemd par VM). Ce qui
change : la session connecte **toutes** ses têtes et les sert sur le même
port.

- `SremfbSpiceState` passe d'**un** display + un primary à un **tableau de
  têtes** (canaux display indexés par leur `channel-id`, chacun avec son
  snapshot primary). `on_channel_new` connecte chaque display rencontré.
- Le `src_ctx` de chaque client mémorise **sa** tête ; `display-invalidate`
  sur la tête K ne fan-out qu'aux clients de la tête K.
- Le resize agent vise la bonne tête :
  `spice_main_channel_update_display(main, head_id, …)`.
- Fan-out (plusieurs clients, même tête) : inchangé. Plusieurs têtes,
  plusieurs clients : chacun sur la sienne.

Cela **remplace** le `SREMFB_DISPLAY_MAP` statique (MAC→tête) : le client
demande directement sa tête. (Une map serveur reste possible pour forcer,
mais le défaut « le client choisit » est plus simple.)

### 3.3 Le rôle `audio` = une connexion à part (QoS + anti-HoL)

Le flux audio est **une seconde connexion** (même MAC, `role=audio`),
volontairement séparée de la socket vidéo — pas multiplexée dessus :

1. **Head-of-line.** Sur une socket partagée, une frame RAW de plusieurs Mo
   déjà en cours d'envoi bloquerait le paquet audio derrière (TCP ordonné,
   pas de préemption). Socket distincte → l'audio ne fait jamais la queue.
2. **QoS.** La socket audio se marque **DSCP EF** (`setsockopt IP_TOS`,
   éventuellement `SO_PRIORITY`) → prioritaire dans le switch / `tc` (la
   boîte à outils `tc htb` déjà utilisée pour brider Aqua). Débit faible
   (48 kHz stéréo 16 bits ≈ 1,5 Mbit/s, ~128 kbit/s en opus) → coût quasi
   nul pour la vidéo.

L'audio est **par VM**, pas par tête (une VM = une sortie son) : la
connexion audio est `(MAC, audio)`, `head` ignoré ; corrélée à la session
vidéo du même MAC.

**Transport — deux variantes :**
- **(B1) 2ᵉ connexion TCP** (recommandé v1) : simple, fiable, marquée DSCP,
  réutilise keepalive / `TCP_USER_TIMEOUT`. Le HoL disparaît car socket
  distincte.
- **(B2) UDP** (upgrade) : latence plus basse, tolérant à la perte (paquet
  perdu = micro-glitch, pas de retransmission qui accumule du retard) —
  mais exige un **jitter-buffer** client + resync. Si B1 retarde sous charge.

### 3.4 Étape suivante prévue : client multi-têtes sur un même SBC

Un **Pi 4/5 a deux sorties écran** (deux HDMI / deux connecteurs DRM). Le
modèle (MAC, rôle, head) le porte naturellement : le **même** SBC ouvre
**une connexion video par sortie locale**, chacune demandant une tête
différente — `(MAC, video, head 0)` sur `/dev/fb0`, `(MAC, video, head 1)`
sur `/dev/fb1`. Un seul SBC pilote alors deux écrans de la VM.

Conséquence à acter côté serveur **dès maintenant** (même si le client
mono-tête vient plus tard) : la règle « même MAC qui se reconnecte =
remplace l'ancienne connexion » doit devenir « même **(MAC, rôle, head)** »
— sinon la 2ᵉ tête du même SBC tuerait la 1ʳᵉ. C'est le seul point du core
générique (session.c) touché par ce modèle ; à faire proprement en phase T.

Le client multi-têtes lui-même (énumérer les sorties DRM/fb locales, une
connexion + un thread de blit par sortie) est un **chantier client à part
(phase C)**, la suite logique une fois le serveur multi-têtes en place.

---

## 4. Audio — spécifique (le transport est en §3.3)

### 4.1 Capter le son : pas `spice_audio_get`

`spice_audio_get(session, ctx)` câblerait le canal playback à un backend
audio **local au bridge** → le son sortirait sur le bridge, pas sur le
panel. Inutilisable. On capte donc le PCM à la main sur le
`SpicePlaybackChannel` :

- `playback-start(format, channels, frequency)` — le décodage Opus/CELT est
  fait par spice-glib → on reçoit du **PCM**.
- `playback-data(data, size)` — trames PCM. `playback-stop` — fin.
- (optionnel) volume/mute.

### 4.2 Messages (sur la connexion `role=audio`)

- `AUDIO_FORMAT` : {codec=PCM|opus, channels, rate, bits} — à chaque
  playback-start.
- `AUDIO_DATA` : trames audio.

### 4.3 Côté client (`sremfb-client`)

- Annonce `SREMFB_HELLO_FLAG_AUDIO` **si** une sortie ALSA est dispo, et
  ouvre la connexion `role=audio`.
- Nouveau module `client/audio.c` : PCM ALSA (`SREMFB_AUDIO_DEV`, défaut
  HDMI), file bornée, buffer court (~50–100 ms), pas de sync A/V ;
  sous-alimentation → silence, jamais de blocage. **À trancher** : libasound
  vs `/dev/snd` ioctls bruts (fidèle au C-pur du client).
- Matériel : le panel doit avoir une sortie audio (HDMI/DAC/USB-audio) ;
  sinon pas de bit annoncé, pas d'audio (dégradation propre).

### 4.4 Alternative écartée : haut-parleur USB virtuel (façon EVDI)

SBC présentant une carte son USB virtuelle (`dummy_hcd` + gadget `f_uac2`,
sans UDC matériel), exportée par usbip, la VM joue dedans. Avantage : aucun
changement de protocole. **Écartée** car (1) l'audio USB est **isochrone**
et usbip le tunnelise sur TCP → coupures/latence, contre la qualité « on se
croirait en local » ; (2) **pas de QoS** possible (noyé dans les URB), alors
qu'un flux dédié marqué DSCP est peu coûteux et anti-HoL (§3.3). Reste
mesurable (spike) si l'on voulait zéro touche protocole, mais non retenue.

---

## 5. Découpage des livrables

| Phase | Contenu | Touche |
|---|---|---|
| **T1** | hello : bit `role`, octet `head` ; core générique : clé de connexion **(MAC, rôle, head)** au lieu de MAC seul (remplacement/reconnexion) | protocole + core |
| **T2** | serveur SPICE : tableau de têtes (connexion de tous les display channels, snapshot primary par tête, invalidate/resize par tête, fan-out par tête) | serveur SPICE |
| **U1** | usbredir : routage ip→VM, `connect_device` explicite, privilèges (A), réconciliateur bridge, garde-fous | bridge only |
| **U2** | robustesse usbredir : hotplug, départ client, détach propre | bridge only |
| **U3** | stockage local exporté (§2.7) : gadget `f_mass_storage`/`dummy_hcd` (fichier/blockdev/ISO, RW/RO/cdrom), garde-fous double-mount, fragment kernel millefeuille, config `SREMFB_USB_STORAGE` | client (SBC) |
| **A1** | audio : `AUDIO_FORMAT`/`AUDIO_DATA` + 2ᵉ connexion `role=audio` (DSCP EF) | protocole |
| **A2** | serveur : tap `SpicePlaybackChannel` → flux audio dédié | serveur SPICE |
| **A3** | client : connexion audio + `audio.c` ALSA + annonce du bit | client |
| **C1** | client : abstraction `sremfb_output_ops` (fb défaut + **drm via libdrm**), **mono-sortie** — modeset KMS 1 connecteur, dumb buffers + page-flip, blank DPMS, hotplug uevent DRM. `SREMFB_OUTPUT=fb\|drm` | client |
| **C2** | client **multi-sorties** (Pi 4/5) : une connexion video par sortie DRM (chacune sa tête), un thread de blit par sortie ; drm forcé | client |

Ordre conseillé : **T** (fondation transport, débloque le multi-têtes
serveur) → **U** (USB, autonome) → **A** (audio) → **C2** (client
multi-têtes). T1 est petit mais **prérequis** de C2 (la clé de connexion).

**C1 est indépendant et server-agnostic** (pur chemin de peinture local,
marche aussi contre le serveur EVDI 1.3.x, aucun changement de protocole,
défaut `fb` inchangé) → livrable à part, **cible 1.4.1**, avant ou après T.
C2 dépend de T (clé (MAC,rôle,head)) + T2 (serveur multi-têtes).

---

## 6. Plan de test

- **Multi-têtes serveur (T)** : VM `qxl2`, deux clients `--test` demandant
  head 0 et head 1 → deux canvas distincts ; un 3ᵉ client sur head 0 →
  fan-out. Client 1.3.0 (head 0 implicite) → inchangé.
- **USB** : clavier USB sur le SBC → apparaît dans la VM (input !), clé USB
  → montée. Deux VMs : pas de fuite de device. Départ client → détach.
- **Audio** : lecture dans la VM → son sur la sortie du SBC ; QoS vérifié
  sous bridage `tc` (l'audio tient quand la vidéo sature). Compat 1.3.0.
- **Client multi-têtes (C)** : Pi 4/5 deux HDMI → deux têtes de la VM sur
  un seul SBC.

---

## 7. Décisions prises (ne pas rouvrir)
- Modèle **(MAC, rôle, head)** sur un port par VM ; hello additif (bit
  `role` + octet `head` dans `reserved[]`), 48 octets inchangés, compat
  totale. Remplace le `SREMFB_DISPLAY_MAP` statique.
- **1 process = 1 VM** conservé (isolation) ; le multi-têtes est **intra-VM**
  (une session, plusieurs display channels), pas du multi-VM sur un port.
- Clé de connexion serveur = (MAC, rôle, head), pas MAC seul (sinon la 2ᵉ
  tête d'un même SBC tuerait la 1ʳᵉ).
- usbredir réutilise la chaîne usbip existante (pas usbredirserver direct) ;
  chaque `sremfb-spice` pilote usbredir pour SES clients (routage ip→VM).
- Le **stockage local** (U3) passe par un gadget `f_mass_storage`/`dummy_hcd`
  sur le SBC → même chaîne usbip→usbredir. Bulk (fiable), donc retenu là où
  l'audio-gadget est écarté. Backing fichier/blockdev/ISO, RW seulement si
  non-monté côté SBC. Stockage réseau (NBD/virtiofs) = hors chemin USB, pour
  le high-IOPS uniquement.
- L'audio a son **propre flux** (`role=audio`, DSCP EF) — condition du QoS
  et de l'anti-HoL ; feature-bit, ignoré des vieux clients. Haut-parleur
  USB virtuel écarté (§4.4).
- Ordre : T → U → A → C2.
- Client : sortie via `sremfb_output_ops` (fb défaut + drm opt-in) ; **drm
  via libdrm** (pas d'ioctls bruts) ; **mono-sortie d'abord (C1, 1.4.1)**,
  server-agnostic ; multi-sorties (C2) ensuite. `fb` reste le défaut.

## 8. Questions ouvertes
1. Privilèges usbredir : règle udev groupe (A) fiable sur les devices vhci
   selon les distros ? sinon repli root (C).
2. Mapping vhci-port → bus/addr libusb : chemin sysfs stable
   (`/sys/devices/platform/vhci_hcd.0/`) à valider sur la cible.
3. Client audio : libasound vs `/dev/snd` ioctls (zéro dépendance) ?
4. Codec audio : PCM brut (priorisé donc OK) ou opus dès la v1 ?
5. Un même SBC peut-il fournir clavier à VM-A et souris à VM-B ? (routage
   par device — supporté par le mapping §2.2, à confirmer.)
6. QoS : DSCP EF seul, ou aussi `SO_PRIORITY` + classe `tc` documentée ?
7. Nombre max de têtes à gérer (borne du tableau) et découverte : auto via
   les display channels, ou borne configurable ?
8. Client multi-têtes (C) : une sortie sans écran branché doit-elle ouvrir
   sa connexion (tête « en attente ») ou rester dormante jusqu'au hotplug ?
9. Stockage U3 : perf réelle bulk-over-usbip sur gigabit (A20/Milim) ?
   hotplug (attacher/détacher un LUN à chaud), multi-LUN, éjection sûre
   (sync + détach) — et `dummy_hcd` dispo en module sur Pi OS ?
