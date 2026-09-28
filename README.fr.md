# sRemFB — simple Remote Frame Buffer

[English](README.md) · **Français**

Un « moniteur réseau » minimal : `sremfb-server` expose un **connecteur
d'écran virtuel par client** (module noyau EVDI, le pilote DisplayLink)
sur un PC GNOME/Wayland et transfère l'image sur le LAN vers
`sremfb-client`, un daemon console qui écrit les pixels directement dans
`/dev/fb0` d'un SBC (Raspberry Pi, Banana Pi…).

Chaque connecteur se comporte **comme un port physique** : tant qu'aucun
client n'est connecté, le port est « débranché » et invisible. Quand un
client se connecte, c'est un hotplug : GNOME étend le bureau et restaure
la position de l'écran. Les clients sont **identifiés par leur adresse
MAC** (série EDID dérivée du MAC, nom de modèle repris de l'EDID du
panneau branché au SBC) : chaque client physique garde sa propre position
mémorisée dans `monitors.xml`, quel que soit l'ordre de connexion. Quand
le client part, c'est un câble débranché. Pas de session de capture, donc
**pas d'icône d'enregistrement d'écran**, et l'écran de verrouillage
s'affiche comme sur un vrai moniteur. Côté SBC, la dalle est **éteinte**
(`FBIOBLANK`) tant que le serveur est injoignable — « pas de signal » —
et quand GNOME blanke ses écrans (DPMS transmis).

> **Prévu pour un LAN dédié et de confiance** — aucun chiffrement, aucune
> authentification ; une allowlist CIDR (`SREMFB_ALLOW`) limite quand même
> les adresses acceptées.

## Documentation

- **[BUILD.fr.md](BUILD.fr.md)** — dépendances, compilation, installation,
  paquets Debian, cross-compilation, test sans matériel.
- **[PROTOCOL.fr.md](PROTOCOL.fr.md)** — le protocole réseau (v2), champ
  par champ.

## SBC supportés

| SBC | État | Notes |
|---|---|---|
| Raspberry Pi 5 / 500 / 500+ | ✅ support complet | parfait à 60 fps, aucun lag |
| Raspberry Pi 4 / 400 | ✅ supporté | non testé pour le moment |
| Raspberry Pi 3B+ | ✅ supporté | utilisable à 30 fps, délais en cas d'affichage dynamique |
| Raspberry Pi 3B | ✅ supporté | non testé pour le moment |
| Raspberry Pi 2 | ✅ supporté | non testé ; performances réduites attendues (limitation 100 Mbit) |
| Raspberry Pi 1 | 🟡 support probable | non testé ; performances réduites attendues (limitation 100 Mbit) |
| Banana Pi M1+ | ✅ supporté | RGB565 recommandé ; limité par sa RAM (vidéos globalement ok) |

## Testeurs recherchés & fonctionnalités à venir

**Testeurs bienvenus !** Les cartes marquées « non testé » ci-dessus ne
demandent qu'à l'être, et les autres SBC ont toutes leurs chances : le
client n'exige qu'un framebuffer (`/dev/fb0`) et la libc — Orange Pi,
Odroid, Rockchip, autres Allwinner… Ouvrez une
[issue](https://github.com/462eng/sRemFB/issues) avec la carte, l'OS, la
résolution et le ressenti (fps, latence) — même un simple « ça marche »
aide.

**En préparation** (ligne 1.4.x) :

- **Backend SPICE / Proxmox — l'infra virtuelle sur thin clients.** Le
  but : monter un parc de postes de travail **entièrement virtualisé**
  (VM QEMU/KVM sur Proxmox) dont chaque poste physique n'est qu'un SBC à
  quelques dizaines d'euros derrière l'écran — et qui **s'utilise comme
  un PC local, sans qu'on puisse faire la différence** : écran fluide,
  clavier/souris/USB locaux (téléport), démarrage sur la dalle. Le
  serveur capture l'écran de la VM (spice-glib) et le diffuse aux mêmes
  clients, sans EVDI. Prototype fonctionnel validé contre QEMU ; test
  Proxmox en cours.
- **Sortie DRM/KMS native côté client** — scanout direct (RGB565 ou
  XRGB8888) sans passer par fbdev, page-flip sans déchirure sur les
  frames pleines. Validé sur Pi 500, en prod chez l'auteur.
- **Multi-écrans par SBC** — piloter les deux sorties HDMI d'un Pi 4/5
  comme deux moniteurs (ou deux têtes de VM) indépendants.
- **Audio** — fait pour `sremfb-view` (PCM brut sur UDP, voir plus
  bas) ; reste le client SBC, et un codec (Opus) pour le Wi-Fi.
- **Redirection USB vers les VM** — le téléport USB actuel, étendu au
  scénario thin client (usbredir).

## Fonctionnement

- Le client annonce dans son hello : géométrie du framebuffer
  (`FBIOGET_VSCREENINFO`), **MAC** de l'interface utilisée et « vendor
  modèle » du panneau branché (lu dans `/sys/class/drm/*/edid`). Le
  serveur construit un EDID à cette taille exacte (vendor `RFB`, produit
  = modèle du panneau distant, série = MAC) et le « branche » sur un
  device EVDI libre. Le compositeur le pilote comme n'importe quel
  moniteur ; le serveur récupère les pixels par `libevdi` (curseur
  incrusté par le noyau).
- **Plusieurs clients simultanés** sur un seul port : un device EVDI par
  client (voir « Plusieurs écrans »). Une reconnexion avec le même MAC
  remplace la connexion périmée (SBC redémarré).
- L'EDID expose la résolution à **60 Hz et 30 Hz** : la cadence source se
  limite depuis Réglages → Affichage → Fréquence de rafraîchissement.
- Transfert piloté par le damage : le noyau fusionne les zones modifiées
  en 16 rectangles max et seuls ces rectangles partent → écran statique =
  zéro trafic. Chaque rectangle est compressé en **LZ4** (repli raw).
- **Contrôle de congestion mesuré** : le serveur glisse périodiquement un
  PING dans le flux, le client le renvoie — le délai d'écho mesure la
  congestion de bout en bout de tout ce qui était en file devant. Quand
  ce délai dit que le chemin brut ne tient plus ~15 fps (ex. un écran
  très dynamique derrière le port 100 Mbit d'un Pi 3), le serveur bascule
  ce client en **H.264** de façon transparente (x264, zerolatency,
  bitrate piloté par le même signal de délai), et revient quand la
  pression retombe. Par client et entièrement négocié : le client
  n'annonce la capacité que s'il a un décodeur matériel V4L2 stateful
  (Raspberry Pi ≤ 3 : `/dev/video10`) ; tout le reste garde le chemin
  RAW/LZ4. Côté SBC l'affichage tourne dans son propre thread et saute
  les frames en retard (*framedrop*) : ni le décodeur ni le réseau ne
  bloquent sur l'écriture framebuffer, la dalle montre toujours la
  frame la plus fraîche. Aucune limite à configurer — la capacité du
  lien s'apprend par la mesure. Les mêmes PING servent de battement de cœur : une
  coupure réseau débranche l'écran virtuel et passe la dalle en « no
  signal » en ~6 s (repli TCP ~20-25 s avec un ancien pair).
- **Hotplug de la dalle répercuté** : si la dalle du SBC est débranchée,
  le client se déconnecte et l'écran virtuel disparaît du bureau —
  exactement comme un câble tiré sur un vrai moniteur. La rebrancher
  fait revenir l'écran (surveillance du statut du connecteur DRM, ~2 s).
- **Téléport USB** : les périphériques branchés au SBC (clavier, souris,
  stockage, adaptateurs série) sont « téléportés » vers le PC par
  **usbip** tant que le client streame — branchés derrière l'écran,
  utilisés comme des périphériques locaux du PC, détachés dès que le
  client part. Politique mesurée, pas configurée : HID + stockage +
  série ; jamais les hubs, ni un device qui porte une interface réseau
  active (l'Ethernet d'un Pi est de l'USB !), ni un disque monté ;
  forçage possible par `SREMFB_USB_ALLOW`/`SREMFB_USB_DENY`
  (`vendor[:product]` hexa). Désactivé par défaut sur Pi 3
  (`SREMFB_USB=1` ou `--usb` pour forcer). Requiert le paquet `usbip`
  des deux côtés ; `usbipd` écoute sur le port 3240 du SBC — même
  modèle de confiance que le reste (LAN dédié).
- Formats côté client : 32bpp XRGB8888 (passthrough) et 16bpp RGB565
  (conversion serveur avec dithering ordonné ; `SREMFB_NO_DITHER=1` pour
  couper).
- Quand GNOME éteint les écrans, le serveur envoie un message **BLANK** :
  le client coupe la dalle (`FBIOBLANK`), et la rallume au retour.
- Stats toutes les 5 s par client dans le journal : fps, débit, ratio de
  compression (`journalctl --user -u sremfb-server -f`).

## Démarrage rapide

Détails complets et paquets Debian : **[BUILD.fr.md](BUILD.fr.md)**.

PC (serveur) :

```sh
sudo apt install build-essential libglib2.0-dev liblz4-dev libx264-dev evdi-dkms libevdi-dev
make
sudo make install-server
sudo modprobe evdi          # première fois seulement (auto au boot ensuite)
systemctl --user daemon-reload && systemctl --user enable --now sremfb-server
```

SBC (client), seule dépendance `liblz4-dev` :

```sh
make -C client
sudo make install-client
sudo nano /etc/sremfb.conf  # SREMFB_SERVER=<ip du PC>
sudo systemctl daemon-reload && sudo systemctl enable --now sremfb-client
```

Test sans SBC (sur le PC) :

```sh
./server/sremfb-server &
./client/sremfb-client --test 1920x1080 localhost
```

Un écran 1920×1080 apparaît dans Réglages → Affichage ; le client écrit
une frame sur 30 dans `sremfb-test-NNNN.ppm`. Ctrl-C sur le client →
l'écran « se débranche ».

## Configuration

| Variable | Défaut | Rôle |
|---|---|---|
| `SREMFB_PORT` (serveur & client) | 4629 | port TCP |
| `SREMFB_ALLOW` (serveur) | — | plages IPv4 autorisées, CIDR séparés par virgules (vide = tout accepter) ; `/etc/sremfb-server.conf` |
| `SREMFB_SERVER` (client) | — | hôte/IP du serveur (requis) |
| `SREMFB_FBDEV` (client) | `/dev/fb0` | device framebuffer |
| `SREMFB_TTY` (client) | `/dev/tty1` | VT prise en main (avant-plan + mode graphique) ; en préférer une sans getty, ex. `/dev/tty7` |
| `SREMFB_WRITE_MODE` (client) | `mmap` | `pwrite` si l'affichage traîne (deferred-io) |
| `SREMFB_MAC` (client) | auto | forcer le MAC annoncé (= identité du moniteur) |
| `SREMFB_MODEL` (client) | auto | forcer le nom de modèle annoncé (13 car. max) |
| `SREMFB_NO_LZ4` (client) | — | forcer le raw (A/B test compression) |
| `SREMFB_NO_DITHER` (serveur) | — | couper le dithering RGB565 (A/B test) |
| `SREMFB_NO_H264` (client) | — | ne pas annoncer le décodeur H.264 matériel |
| `SREMFB_NO_H264` (serveur) | — | ne jamais basculer en H.264 (le délai reste mesuré/loggé) |
| `SREMFB_FORCE_H264` (serveur) | — | épingler le H.264 sur les clients capables (A/B test) |
| `SREMFB_NO_HOTPLUG` (client) | — | ne pas surveiller le connecteur DRM de la dalle |
| `SREMFB_USB` (client) | auto | `1`/`0` : forcer/couper le téléport USB (auto : actif sauf Pi 3) |
| `SREMFB_USB_ALLOW` (client) | — | ids `vendor[:product]` toujours téléportés (les gardes réseau/disque priment) |
| `SREMFB_USB_DENY` (client) | — | ids `vendor[:product]` jamais téléportés |
| `SREMFB_NO_USB` (serveur) | — | ne jamais attacher les périphériques USB des clients |
| `SREMFB_INPUT` (serveur) | — | `1` = rejouer clavier/souris/manette des clients admis sur des périphériques uinput (désactivé par défaut) |
| `SREMFB_LAYOUT` (serveur) | `1` | `0` = ne pas réappliquer la configuration d'écrans mémorisée quand seul le connecteur d'un client a changé (voir les notes) |
| `SREMFB_AUDIO` (serveur) | `1` | `0` = pas de son pour les clients qui le demandent (sinon : une sortie PipeWire par client, par défaut tant qu'il est connecté ; UDP même port) |

Le client tourne en root par défaut (accès `/dev/fb0` + ioctl console).
Le serveur tourne en user de session : l'accès à `/dev/dri/cardN`
(device EVDI) vient de l'ACL logind du siège, les ioctls EVDI sont non
privilégiés.

## Plusieurs écrans

Un client connecté = un device EVDI. Le nombre de devices créés au boot
fixe donc le nombre d'écrans simultanés (2 par défaut) :

```sh
sudo sed -i 's/initial_device_count=2/initial_device_count=4/' /etc/modprobe.d/sremfb.conf
echo 2 | sudo tee /sys/devices/evdi/add     # sans attendre le reboot
```

Rien d'autre à configurer : tous les SBC pointent vers le même
`SREMFB_SERVER`/port, et chacun est reconnu à son MAC (position GNOME
indépendante).

> **Piège mutter.** mutter ne survit pas à la réouverture d'un device
> EVDI déjà piloté (« Failed to reopen cardN: EBUSY » puis hotplugs
> ignorés sur cette card). Le serveur s'en protège quatre fois : devices
> gardés ouverts en pool toute la vie du process ; devices **régénérés à
> neuf à chaque démarrage** (un service au boot, `sremfb-evdi-perms.service`,
> ouvre `/sys/devices/evdi/{add,remove_all}` au groupe `video` —
> l'utilisateur de session doit en faire partie) ; quarantaine des devices
> qui ne s'allument pas en 10 s (la reconnexion du client en prend un
> autre) ; et dès qu'un grippage a été constaté, le serveur
> **s'auto-guérit** : il crée un device tout neuf au moment de
> l'acquisition et le branche immédiatement — mutter n'accepte une carte
> que quelques secondes après sa création (budget borné ; épuisé, une
> reconnexion de session remet mutter d'aplomb).
>
> Un cas échappe au serveur : au boot, `evdi` est chargé avec ses devices
> initiaux *avant* GDM, donc gnome-shell les saisit à l'ouverture de
> session, et le premier démarrage du serveur les régénère ensuite sous
> les pieds de mutter — grippage dès la première seconde (« compositor
> did not light up the connector within 10s » en boucle). D'où un drop-in
> GDM, `gdm.service.d/sremfb-evdi-purge.conf`, posé par le paquet et par
> `make install-server`, qui purge les devices EVDI avant le démarrage de
> GDM : le serveur crée alors des cartes neuves que mutter découvre à
> chaud. Il prend effet au prochain redémarrage de GDM (`systemctl restart
> gdm3`, ou reboot) — une simple reconnexion de session ne suffit pas.
>
> Autre piège, en cours de session : mutter n'oublie **jamais** une carte
> retirée (`remove_all` d'un changement de mode, reset du serveur) ; une
> nouvelle carte qui hérite du même numéro `/dev/dri/cardN` est refusée
> comme doublon (« Failed to hotplug secondary gpu: device already
> present ») et son connecteur ne s'allume pas. Le serveur crée donc ses
> cartes une à une et ne garde que celles que gnome-shell ouvre
> effectivement (vu dans `/proc/<pid>/fd`) ; les numéros refusés restent
> en place comme cartes « mortes », jamais attribuées, pour que la
> suivante reçoive un numéro neuf. L'état vit dans
> `$XDG_RUNTIME_DIR/sremfb-evdi-cards` pour la durée de vie de
> gnome-shell. Limite : chaque cycle arrêt/`remove_all`/démarrage
> consomme ~2 numéros (le noyau ne les rend pas tous) ; au-delà de
> card63, reconnecter la session.

## Visionneuse fenêtrée

`sremfb-view` est un client pour **PC Linux de bureau** : il branche un
écran virtuel sur un `sremfb-server` distant exactement comme un SBC, et
l'affiche dans une fenêtre (SDL3, Wayland natif) — pour utiliser une
autre machine GNOME depuis son poste, par exemple un serveur GPU.

```sh
sremfb-view [options] <serveur>      # sremfb-view --help pour tout le détail
```

- Conçue pour la **latence** : un thread réseau reçoit et décompresse
  (LZ4) dès l'arrivée des octets et renvoie les PING ; le thread
  d'affichage n'envoie au GPU que les rectangles modifiés et présente
  toujours l'image la plus récente — aucune file de frames. VSync coupée
  par défaut (`--vsync` pour l'activer). **Jamais de H.264** : la
  capacité n'est pas annoncée, le flux reste en rectangles RAW/LZ4.
- Géométrie `--size WxH` (défaut 1920x1080), XRGB8888 ou `--rgb565`
  (moitié du débit, tramé par le serveur).
- Identité : MAC **localement administrée** dérivée de
  `/etc/machine-id` (stable, jamais celle d'une vraie carte ; `--mac`
  pour forcer), modèle « sremfb-view » (`--model`). GNOME mémorise donc
  la position de cet écran comme pour un SBC.
- **Clavier, souris et manette** passent au bureau distant quand son
  serveur l'autorise (`SREMFB_INPUT=1`, désactivé par défaut ; voir
  [PROTOCOL.fr.md](PROTOCOL.fr.md#entrées)), rejoués là-bas sur des
  périphériques uinput — touches par position physique (la disposition
  du serveur s'applique), manette vue comme une Xbox 360 par Steam et
  SDL. Rien ne reste enfoncé : perte de focus, déconnexion ou lien
  perdu relâchent tout côté distant.
- Souris **absolue** par défaut (usage bureau/KVM : le curseur distant
  suit le vôtre). **Appui bref sur Ctrl droit** pour la **capturer** en
  jeu : souris relative (brute, pointeur verrouillé) et clavier saisi,
  donc Super, Alt+Tab… partent aussi côté distant (GNOME demande une
  fois l'autorisation d'inhiber ses raccourcis). Nouvel appui sur Ctrl
  droit pour libérer ; la perte de focus libère aussi. Ctrl droit n'est
  jamais transmis ; **Ctrl droit+F** plein écran, **Ctrl droit+Q**
  quitter. Le titre affiche le mode.
- Sans entrées (ancien serveur, `SREMFB_INPUT` absent, `--no-input`) :
  lecture seule, **F11** plein écran, **Échap maintenue 1 s** pour
  quitter. Fermer la fenêtre quitte toujours. Fenêtre redimensionnable
  (ratio conservé, bandes noires).
- `--latency-test N` mesure entrée → rendu distant → capture → réseau →
  décodé ici, sur la seule horloge de la visionneuse, face à
  `sremfb-latency-probe` (outil du paquet, python3-gi + GTK 4) lancé en
  plein écran sur l'écran virtuel : min/médiane/p95/max sur N essais,
  pour une touche (`--latency-input key`), le pointeur absolu (`abs`),
  la souris relative (`rel`) — la réaction de la fenêtre sonde — ou le
  curseur distant lui-même atteignant le pixel sonde (`cursor`, sonde
  lancée avec `--ignore-motion`). « rx » = pixel décodé, « shown » =
  après le `SDL_RenderPresent` local ; le compositeur local, le scanout
  et le retard propre de l'écran ne sont pas comptés.
- **Son** du bureau distant (serveur ≥ 1.4.1+holo3, PipeWire) : tant
  que la visionneuse est connectée, le serveur crée une sortie
  « sRemFB <modèle> », en fait la **sortie par défaut** (jeux, Steam,
  bureau y basculent) et remet l'ancienne au départ. Le son arrive en
  **PCM brut** 48 kHz stéréo (aucun codec, aucun délai d'encodage) sur
  un flux UDP à part (même numéro de port, ouvert par la visionneuse :
  rien à ouvrir dans un pare-feu côté client), paquets de 2,7 ms, et part
  vers la sortie son par défaut du poste via SDL3. Tampon **court** :
  cible 10 ms (`--audio-buffer MS`), période de la carte 128 trames
  (`--audio-frames`) ; au-delà de cible + 10 ms, des paquets sont
  **jetés** pour y revenir — la latence ne dérive jamais ; paquet perdu
  = silence de même durée ; la dérive entre les deux horloges son est
  compensée en douceur (±0,3 % max). Mesuré sur un lien 2,5 GbE :
  environ 15 ms de l'événement d'entrée au son sortant de la carte
  (estimation, voir `--latency-test`). `--no-audio` pour s'en passer.
- Pas encore de vibrations de manette (retour de force non transmis).
- Déconnexions comme le client SBC : battement de cœur de 6 s,
  reconnexion automatique avec backoff.
- `--stats` : avec le son, une ligne `audio:` en plus — paquets/s,
  perdus, en retard, jetés, sous-alimentations, profondeur du tampon,
  délai réseau (horloges recalées par les ECHO UDP), correction de
  dérive, latence estimée (réseau + tampon + un paquet + période de la
  carte ; serveur son et DAC locaux non comptés). `--latency-test` avec
  `sremfb-latency-probe --click` : chaque événement produit aussi un
  clic dans la sortie par défaut du serveur, et le test donne en plus
  « snd rx » (clic reçu) et « snd out » (clic sortant de la carte,
  estimé).
- `--stats` : toutes les 5 s sur stderr, fps reçus et présentés, Mo/s,
  temps LZ4, temps upload+présentation, délai de file estimé depuis les
  PING. `--dump N` : sauve N frames en PPM (validation sans écran).

## Jouer depuis la visionneuse

Pour jouer aux jeux d'un PC GNOME (Steam…) depuis un autre poste :
serveur avec `SREMFB_INPUT=1` (et le son, actif par défaut), puis
`sremfb-view <serveur>` sur le poste. Pour que l'écran virtuel soit
**le seul écran** pendant la session de jeu — Steam et les jeux s'y
ouvrent, l'écran physique du serveur s'éteint — et que tout revienne à
la déconnexion :

1. connecter la visionneuse (son identité doit rester la même : ne pas
   changer `--mac`/`--model` ensuite) ;
2. **une seule fois**, sur l'écran virtuel : Réglages → Affichage →
   « Un seul écran » → l'écran sRemFB (fréquence au choix, 120 Hz
   possible) → Appliquer → Conserver. mutter mémorise cette
   configuration pour l'ensemble {écran physique + cet écran virtuel} ;
3. c'est tout : à chaque connexion l'écran virtuel devient seul et
   principal (le serveur la réapplique si le connecteur a changé, voir
   les notes), et à la déconnexion mutter revient à la configuration de
   l'écran physique seul, qui se rallume en ~3 s.

Sans accès à Réglages (à distance), la même chose en D-Bus :
`ApplyMonitorsConfig` avec la méthode 2 (persistante) et un seul
moniteur logique contenant le connecteur de l'écran sRemFB (voir
`GetCurrentState`). Comme depuis Réglages, gnome-shell demande alors
« Conserver cette configuration ? » sur l'écran sRemFB : cliquer
**Conserver les modifications** dans les 20 s, sinon mutter revient à
l'ancienne configuration et **n'enregistre rien**. Revenir en arrière : même procédure avec
« Joindre les écrans » ou « Miroir ».

Si la visionneuse disparaît brutalement (lien coupé), le serveur
débranche l'écran virtuel au bout de ~6 s et l'écran physique revient ;
un arrêt du serveur (`systemctl --user stop sremfb-server`, changement
de mode) le débranche aussi. Ne jamais supprimer `monitors.xml` pendant
que seul l'écran virtuel est actif.

## Notes

- La position de chaque écran se règle **une seule fois** dans Réglages →
  Affichage ; GNOME la mémorise dans `~/.config/monitors.xml`, indexée
  sur l'identité EDID (vendor `RFB` / modèle du panneau / série = MAC).
  Changer le panneau branché au SBC change le modèle, donc l'identité —
  comme un vrai changement de moniteur. Piège : mutter indexe aussi
  chaque configuration sur le **nom du connecteur** (`DVI-I-13`), et
  celui-ci change dès que les cartes evdi sont recréées (changement de
  mode, reset) ; mutter ne retrouverait plus rien. Le serveur rattrape
  ce cas : quand l'écran d'un client s'allume, s'il existe dans
  `monitors.xml` une configuration des mêmes moniteurs où seul le
  connecteur de ce client diffère (et pas de correspondance exacte), il
  la réapplique avec le nouveau nom (`ApplyMonitorsConfig`,
  **temporaire** : une application persistante ferait afficher à
  gnome-shell « Conserver cette configuration ? » et revenir en arrière
  au bout de 20 s sans réponse) — de préférence la plus récente
  (`~/.local/state/sremfb/last-connectors`).
  `SREMFB_LAYOUT=0` pour s'en passer.
- GNOME compose l'étiquette des Réglages comme « vendor + diagonale ».
  La hwdb udev (`61-sremfb-display-vendor.hwdb`) enregistre le vendor
  EDID `RFB` sous le nom **« 462eng sRemFB »** → « 462eng sRemFB 24" »
  (après une reconnexion de session, gnome-shell gardant l'ancienne
  table en mémoire). La diagonale annoncée est fixe (24").
- `install-server`/le paquet déposent `/etc/modules-load.d/sremfb.conf`
  et `/etc/modprobe.d/sremfb.conf` (`evdi initial_device_count=2`). Les
  devices apparaissent comme `cardN` avec un connecteur `DVI-I-N`
  « disconnected ».
- Validé sur GNOME 48 Wayland (mutter pilote les cartes EVDI en GPU
  secondaire, chemin DisplayLink standard). Sous X11 il faudrait
  déclarer les devices dans xorg.conf — non testé.
- Débit : à 1080p/32bpp une frame pleine fait ~8,3 Mo, mais grâce au
  damage seuls les rectangles modifiés partent sur le fil. En RGB565
  c'est moitié moins. Contenu statique : zéro trafic.
- Un client lent ne retarde jamais les autres : les envois sont **non
  bloquants**, chaque client a sa propre file. Le damage s'y accumule en
  région sale et la frame suivante n'est construite (conversion, LZ4 ou
  encodage) que quand sa socket peut la prendre, **depuis les pixels les
  plus frais** — un client à la traîne reçoit moins de frames, jamais
  des frames périmées, et les autres ne voient rien.

## Licence

[MIT](LICENSE) — © 2026 Jonathan Roth.
