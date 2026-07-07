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
  RAW/LZ4. Aucune limite à configurer — la capacité du lien s'apprend
  par la mesure. Les mêmes PING servent de battement de cœur : une
  coupure réseau débranche l'écran virtuel et passe la dalle en « no
  signal » en ~6 s (repli TCP ~20-25 s avec un ancien pair).
- **Hotplug de la dalle répercuté** : si la dalle du SBC est débranchée,
  le client se déconnecte et l'écran virtuel disparaît du bureau —
  exactement comme un câble tiré sur un vrai moniteur. La rebrancher
  fait revenir l'écran (surveillance du statut du connecteur DRM, ~2 s).
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
> autre) ; et quand plus aucun device sain ne reste, le serveur
> **s'auto-guérit** en créant un device neuf pour ce retry (borné — budget
> épuisé, une reconnexion de session remet mutter d'aplomb).

## Notes

- La position de chaque écran se règle **une seule fois** dans Réglages →
  Affichage ; GNOME la mémorise dans `~/.config/monitors.xml`, indexée
  sur l'identité EDID (vendor `RFB` / modèle du panneau / série = MAC).
  Changer le panneau branché au SBC change le modèle, donc l'identité —
  comme un vrai changement de moniteur.
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
- Limite connue : les envois sont bloquants, donc un client congestionné
  ou mort peut retarder les autres jusqu'au `SO_SNDTIMEO` (20 s) —
  *head-of-line blocking*. Correctif possible (non implémenté) : sockets
  non bloquantes + buffer « dernière frame » par client. Le H.264
  adaptatif réduit d'un ordre de grandeur les envois des clients qui en
  ont besoin, ce qui atténue le problème en pratique.

## Licence

[MIT](LICENSE) — © 2026 Jonathan Roth.
