# Protocole réseau

[English](PROTOCOL.md) · **Français**

Protocole applicatif de sRemFB, partagé mot pour mot par le serveur et le
client dans [`protocol.h`](protocol.h). Version décrite ici : **v2**
(`SREMFB_PROTO_VER = 2`), avec ses **bits de fonctionnalité** (mesure du
délai, H.264 adaptatif, entrées clavier/souris/manette, son, négociés par
les hellos sans changer de version — toutes les combinaisons
ancien/nouveau restent compatibles).

## Transport

- **TCP**, un port unique (défaut **4629**), plusieurs clients simultanés
  distingués par leur adresse MAC (voir [README.fr.md](README.fr.md)).
- **UDP** sur le même numéro de port, uniquement pour le son quand il est
  négocié (voir « Son »).
- `TCP_NODELAY`, `SO_KEEPALIVE` (idle 10 s / intvl 5 s / cnt 3),
  `TCP_USER_TIMEOUT` 6 s (données non-ACKées ⇒ la connexion meurt même
  en plein envoi), `SO_SNDTIMEO` 20 s, `SO_RCVTIMEO` 5 s. `SIGPIPE`
  ignoré.
- **Vivacité** (quand la fonctionnalité PING est négociée) : le serveur
  maintient un PING de battement de cœur au moins toutes les ~2 s même
  écran statique, et chaque côté déclare l'autre mort après ~6 s de
  silence — l'écran virtuel se débranche et la dalle passe en « no
  signal » en ~6-7 s sur une coupure réseau. Les anciens pairs retombent
  sur les timers TCP (~20-25 s).
- Le serveur vérifie l'adresse contre l'allowlist CIDR (`SREMFB_ALLOW`)
  **à l'accept**, avant même de lire le hello.
- **Aucun chiffrement, aucune authentification.** À réserver à un LAN
  dédié de confiance.

## Endianness

Tous les entiers sont **little-endian** sur le fil. Les deux cibles
supportées (serveur x86-64, clients ARM) sont little-endian ; les hôtes
big-endian ne sont pas pris en charge. Les structs sont packées et
vérifiées par `_Static_assert` sur leur taille.

## Constantes

```c
#define SREMFB_MAGIC        0x30624672u   /* octets 'r','F','b','0' (héritage v1) */
#define SREMFB_PROTO_VER    2
#define SREMFB_DEFAULT_PORT 4629
```

Le magic `rFb0` est conservé depuis la v1 : c'est un garde de resync placé
en tête de chaque message.

## Séquence

```
client → serveur   :  connexion TCP
client → serveur   :  sremfb_client_hello   (48 o, une fois)
                      ─ le serveur choisit un device EVDI, construit
                        l'EDID et le « branche » ; le compositeur fixe
                        un mode ─
serveur → client   :  sremfb_server_hello   (16 o, une fois)
                        status != 0  ⇒  le serveur ferme la connexion
serveur → client   :  sremfb_frame_hdr + payload   (répété, sur damage)
                      sremfb_frame_hdr BLANK / UNBLANK   (sans payload)
                      sremfb_frame_hdr PING + u64        (si négocié)
client → serveur   :  sremfb_client_msg PONG (16 o, écho de chaque PING)
client → serveur   :  sremfb_input_msg INPUT (16 o, événements d'entrée,
                      si négocié)
serveur → client   :  sremfb_frame_hdr H264 + access unit   (sous
                      congestion mesurée, si négocié ; H264_EOS clôt
                      l'épisode)

UDP, si le son est négocié :
client → serveur   :  sremfb_udp_hello AUDIO_HELLO (16 o, ≥ 1/s)
serveur → client   :  sremfb_udp_echo ECHO (24 o, une par hello)
serveur → client   :  sremfb_audio_hdr + PCM (≈ 1 paquet / 2,7 ms tant
                      que quelque chose joue)
```

## Messages

### `sremfb_client_hello` — 48 octets, client → serveur

Envoyé une seule fois, juste après la connexion.

| Champ | Type | Rôle |
|---|---|---|
| `magic` | `u32` | `SREMFB_MAGIC` |
| `proto_ver` | `u16` | `SREMFB_PROTO_VER` (2) |
| `flags` | `u16` | bit 0 `SREMFB_HELLO_FLAG_LZ4` = le client accepte LZ4 · bit 1 `SREMFB_HELLO_FLAG_FEEDBACK` = le client renvoie les PING en PONG · bit 2 `SREMFB_HELLO_FLAG_H264` = le client décode le H.264 (4:2:0, Annex B) à sa résolution · bit 3 `SREMFB_HELLO_FLAG_USB` = le client exporte ses périphériques USB par usbip (voir « Téléport USB ») · bit 4 `SREMFB_HELLO_FLAG_INPUT` = le client voudrait envoyer ses entrées (voir « Entrées ») · bit 5 `SREMFB_HELLO_FLAG_AUDIO` = le client joue le son du serveur (voir « Son ») |
| `xres`, `yres` | `u16` | résolution visible du framebuffer |
| `bpp` | `u8` | bits par pixel du fb : 16 ou 32 |
| `pixfmt` | `u8` | `enum sremfb_pixfmt` |
| `red_off`, `red_len` | `u8` | disposition du canal rouge (informatif) |
| `green_off`, `green_len` | `u8` | canal vert |
| `blue_off`, `blue_len` | `u8` | canal bleu |
| `mac[6]` | `u8` | MAC du client (tout-à-zéro = inconnu) → série EDID |
| `model[13]` | `char` | « vendor modèle » du panneau branché au SBC (lu dans son EDID), complété par espaces/NUL ; vide = inconnu → nom de modèle du moniteur virtuel |
| `reserved[9]` | `u8` | réservé |

### `sremfb_server_hello` — 16 octets, serveur → client

Envoyé une fois, après que le compositeur a fixé un mode sur le connecteur.

| Champ | Type | Rôle |
|---|---|---|
| `magic` | `u32` | `SREMFB_MAGIC` |
| `proto_ver` | `u16` | `SREMFB_PROTO_VER` |
| `status` | `u16` | `enum sremfb_status` ; non nul ⇒ le client ferme |
| `width`, `height` | `u16` | taille négociée du flux (normalement `xres`,`yres`) |
| `pixfmt` | `u8` | format des pixels des frames qui suivent |
| `flags` | `u8` | bit 0 `SREMFB_SRV_FLAG_PING` = des PING peuvent arriver · bit 1 `SREMFB_SRV_FLAG_H264` = le flux peut basculer en H.264 · bit 2 `SREMFB_SRV_FLAG_INPUT` = les messages INPUT sont acceptés (périphériques créés) · bit 3 `SREMFB_SRV_FLAG_AUDIO` = la sortie son du client existe, ouvrir le flux UDP. Le serveur ne pose un bit que si le client a annoncé la capacité correspondante ; les anciens serveurs envoient toujours 0 ici |
| `audio_token` | `u16` | avec `SREMFB_SRV_FLAG_AUDIO` : jeton à citer dans les AUDIO_HELLO UDP (ancien `reserved[2]`, toujours 0 chez les anciens serveurs) |

Codes de statut (`enum sremfb_status`) :

| Valeur | Nom | Sens |
|---|---|---|
| 0 | `OK` | flux à venir |
| 1 | `BAD_HELLO` | hello client incompatible ou malformé |
| 2 | `SERVER_FAIL` | le compositeur n'a jamais allumé le connecteur |
| 3 | `NO_DEVICE` | aucun device EVDI libre pour ce client |

### `sremfb_frame_hdr` — 20 octets, serveur → client

Un en-tête par message, suivi de `payload_len` octets. Les messages de
contrôle BLANK/UNBLANK n'ont pas de payload et portent un rectangle 0×0.

| Champ | Type | Rôle |
|---|---|---|
| `magic` | `u32` | `SREMFB_MAGIC` (resync/anti-corruption) |
| `encoding` | `u8` | `enum sremfb_encoding` |
| `reserved[3]` | `u8` | H264 : `reserved[0]` bit 0 = `SREMFB_H264_FLAG_IDR` (informatif) ; sinon réservé |
| `x`, `y`, `w`, `h` | `u16` | rectangle destination, en coordonnées flux |
| `payload_len` | `u32` | RAW : `w*h*bytespp` ; LZ4 : taille du bloc ; BLANK/UNBLANK/H264_EOS : 0 ; PING : 8 ; H264 : taille de l'access unit |

Encodages (`enum sremfb_encoding`) :

| Valeur | Nom | Payload |
|---|---|---|
| 0 | `RAW` | `w*h*bytespp` pixels bruts, sans padding |
| 1 | `LZ4` | un bloc LZ4 de ces mêmes pixels bruts |
| 2 | `BLANK` | aucun : éteindre la dalle (DPMS off) |
| 3 | `UNBLANK` | aucun : rallumer la dalle |
| 4 | `PING` | 8 o : `u64` horloge monotone du serveur (µs), à renvoyer telle quelle dans un PONG. Rectangle 0×0 |
| 5 | `H264` | une access unit H.264 Annex B (4:2:0, sans B-frames, ordre de décodage = ordre d'affichage) ; le rectangle est toujours le flux entier |
| 6 | `H264_EOS` | aucun : l'épisode H.264 est clos — drainer le décodeur, tout afficher, puis reprendre la lecture |

### `sremfb_client_msg` — 16 octets, client → serveur

Tous les messages montants après le hello font **16 octets** et
commencent par `magic` + `type` ; le serveur ignore les types qu'il ne
connaît pas (et saute octet par octet jusqu'au magic suivant en cas de
corruption, en coupant au-delà de 256 octets de déchets). Deux types :

- `SREMFB_CMSG_PONG` (1), émis uniquement quand le hello serveur a
  annoncé `SREMFB_SRV_FLAG_PING` ;
- `SREMFB_CMSG_INPUT` (2), émis uniquement quand le hello serveur a
  annoncé `SREMFB_SRV_FLAG_INPUT` (struct `sremfb_input_msg`, voir
  « Entrées »).

| Champ | Type | Rôle |
|---|---|---|
| `magic` | `u32` | `SREMFB_MAGIC` |
| `type` | `u8` | 1 = `SREMFB_CMSG_PONG` |
| `reserved[3]` | `u8` | réservé |
| `t_echo_us` | `u64` | le payload du PING, tel quel |

Le client émet le PONG **à sa position dans le flux de réception**,
après avoir appliqué toutes les frames qui précédaient le PING. TCP
étant ordonné, le `maintenant − t_echo_us` côté serveur mesure donc le
délai de bout en bout de tout ce qui était en file devant — buffer
d'envoi noyau, files réseau et traitement client. Ce délai est le
signal de congestion qui pilote l'encodeur adaptatif (voir plus bas) ;
seule l'horloge du serveur intervient, aucune synchronisation n'est
nécessaire.

### `sremfb_input_msg` — 16 octets, client → serveur

Un événement Linux evdev (valeurs de `linux/input-event-codes.h`), même
taille et même en-tête que `sremfb_client_msg`.

| Champ | Type | Rôle |
|---|---|---|
| `magic` | `u32` | `SREMFB_MAGIC` |
| `type` | `u8` | 2 = `SREMFB_CMSG_INPUT` |
| `dev` | `u8` | `enum sremfb_indev` : périphérique cible |
| `reserved[2]` | `u8` | réservé (0) |
| `ev_type` | `u16` | `EV_SYN`, `EV_KEY`, `EV_REL` ou `EV_ABS` |
| `ev_code` | `u16` | code evdev (`KEY_A`, `BTN_LEFT`, `REL_X`, `ABS_X`…) |
| `ev_value` | `i32` | valeur evdev |

Périphériques (`enum sremfb_indev`), un périphérique uinput distinct
chacun côté serveur :

| Valeur | Nom | Événements |
|---|---|---|
| 0 | `ALL` | uniquement avec `ev_type` 0 : relâcher toutes les touches et tous les boutons tenus sur tous les périphériques (perte de focus) ; la manette revient au neutre |
| 1 | `KEYBOARD` | `EV_KEY` `KEY_*` (positions physiques ; la disposition du serveur donne les caractères) |
| 2 | `MOUSE` | relative : `REL_X`/`REL_Y`, `REL_WHEEL`/`REL_HWHEEL` (+ `_HI_RES`, 1/120 de cran), `BTN_LEFT/RIGHT/MIDDLE/SIDE/EXTRA` |
| 3 | `POINTER` | absolue : `ABS_X`/`ABS_Y` en **pixels du flux** (0..largeur−1, 0..hauteur−1) ; le serveur les place sur l'écran virtuel du client ; boutons et molettes comme `MOUSE` |
| 4 | `GAMEPAD` | disposition Xbox 360 (xpad) : `BTN_A/B/X/Y`, `BTN_TL/TR`, `BTN_SELECT/START/MODE`, `BTN_THUMBL/R` ; `ABS_X/Y/RX/RY` −32768..32767 (+Y vers le bas), gâchettes `ABS_Z/RZ` 0..255, croix `ABS_HAT0X/Y` −1..1 |

## Entrées

Négociation, dans les deux sens :

1. le client met `SREMFB_HELLO_FLAG_INPUT` dans son hello ;
2. le serveur crée les périphériques uinput du client **avant** d'envoyer
   son hello, et n'y met `SREMFB_SRV_FLAG_INPUT` que s'ils existent et
   que l'administrateur l'a permis (`SREMFB_INPUT=1`, désactivé par
   défaut, en plus de l'allowlist CIDR) ;
3. le client n'envoie **aucun** message INPUT tant que ce bit n'est pas
   reçu, et le serveur ignore les INPUT d'un client pour qui il ne l'a
   pas posé.

Chaque périphérique applique ses événements au `EV_SYN`/`SYN_REPORT`
que le client envoie explicitement, comme un pilote noyau (typiquement
les événements d'une action + un SYN dans un seul `write`). Pas de
répétition automatique : c'est le compositeur du serveur qui répète,
comme avec un clavier USB. Le serveur filtre ce qui n'a pas été déclaré
(codes, plages) et les appuis doublés / relâchements de touches non
enfoncées.

**Aucune touche coincée** : à la déconnexion ou à la perte du client
(dont le chien de garde de 6 s), le serveur relâche tout ce qui est
encore tenu, recentre la manette, puis détruit les périphériques ; le
client envoie `ALL` quand sa fenêtre perd le focus.

Pointeur absolu : le serveur lit la disposition des écrans du
compositeur (`org.gnome.Mutter.DisplayConfig.GetCurrentState`, relue à
chaque `MonitorsChanged`), retrouve l'écran virtuel du client par son
connecteur (`DVI-I-N` de la carte EVDI), et convertit les pixels du flux
en coordonnées 0..32767 sur l'étendue globale — ce que libinput attend
d'un pointeur absolu façon tablette USB de QEMU. Tant que la disposition
ne connaît pas encore le connecteur, les événements absolus sont
ignorés.

Manette : « Microsoft X-Box 360 pad », USB `045e:028e`, pour que Steam
et SDL appliquent leur correspondance standard. Pas de retour de force
(vibrations) pour l'instant.

## Son

Négociation, dans les deux sens, comme les entrées :

1. le client met `SREMFB_HELLO_FLAG_AUDIO` dans son hello ;
2. le serveur crée la sortie son du client **avant** d'envoyer son hello
   (une sortie PipeWire « sRemFB <modèle> », classe `Audio/Sink`, qui
   devient la sortie par défaut du bureau tant que le client est là) et
   n'y met `SREMFB_SRV_FLAG_AUDIO` + `audio_token` que si elle existe et
   que l'administrateur ne l'a pas coupé (`SREMFB_AUDIO=0`) ;
3. le client n'ouvre le flux UDP que si ce bit est reçu.

Le flux UDP (même numéro de port que le TCP) est ouvert **par le
client** : il envoie depuis la socket sur laquelle il écoute des
`AUDIO_HELLO` portant le jeton, toutes les 250 ms jusqu'au premier
paquet puis chaque seconde (c'est aussi le keepalive ; sans hello
pendant 5 s le serveur cesse d'envoyer). Le serveur n'accepte un hello
que depuis l'adresse IP du pair TCP avec le bon jeton, répond à chacun
par un `ECHO` et envoie le son à l'adresse source. Pare-feux et NAT ne
voient qu'un flux sortant du client. Chaque datagramme commence par
`magic` + `type` (`enum sremfb_udp_type`).

### `sremfb_udp_hello` — 16 octets, client → serveur (UDP)

| Champ | Type | Rôle |
|---|---|---|
| `magic` | `u32` | `SREMFB_MAGIC` |
| `type` | `u8` | 2 = `SREMFB_UDP_AUDIO_HELLO` |
| `reserved` | `u8` | 0 |
| `token` | `u16` | `server_hello.audio_token` |
| `t_client_us` | `u64` | horloge monotone du client (µs), renvoyée dans l'ECHO |

### `sremfb_udp_echo` — 24 octets, serveur → client (UDP)

| Champ | Type | Rôle |
|---|---|---|
| `magic` | `u32` | `SREMFB_MAGIC` |
| `type` | `u8` | 3 = `SREMFB_UDP_ECHO` |
| `reserved[3]` | `u8` | 0 |
| `t_client_us` | `u64` | celui du hello, tel quel |
| `t_server_us` | `u64` | horloge monotone du serveur (µs) à la réception |

Avec t1 = `t_client_us`, t2 = `t_server_us`, t3 = l'arrivée de l'ECHO :
décalage d'horloge ≈ t2 − (t1 + t3)/2, à ± RTT/2 près (le client garde
l'échantillon au plus petit RTT des derniers). Il ne sert qu'aux
statistiques de latence.

### `sremfb_audio_hdr` — 24 octets + PCM, serveur → client (UDP)

| Champ | Type | Rôle |
|---|---|---|
| `magic` | `u32` | `SREMFB_MAGIC` |
| `type` | `u8` | 1 = `SREMFB_UDP_AUDIO` |
| `format` | `u8` | 0 = `SREMFB_AUDIO_S16LE_48K_STEREO` (seul format) |
| `frames` | `u16` | trames qui suivent (1..240) |
| `seq` | `u32` | +1 à chaque paquet ; un trou = paquet perdu |
| `reserved` | `u32` | 0 |
| `t_us` | `u64` | horloge monotone du serveur (µs) quand la première trame a quitté le graphe audio |

Suivi de `frames × 4` octets de PCM entrelacé S16LE stéréo à 48 kHz,
**sans codec**. Le serveur envoie un paquet par cycle du graphe PipeWire
(il demande un quantum de 128 trames, 2,7 ms ; au plus 240 trames = 5 ms
par paquet, DSCP EF), rien quand rien ne joue (sortie suspendue). Pas
de retransmission. Le client gère la gigue : tampon cible court,
paquets perdus remplacés par un silence de même durée, paquets en
retard ignorés, et **jamais** de latence qui dérive (au-delà de la cible
+ 10 ms, il jette des paquets jusqu'à revenir à la cible) — voir
[README.fr.md](README.fr.md#visionneuse-fenêtrée).

Débit : 192 ko/s de PCM (≈ 1,6 Mbit/s en-têtes compris). Un codec (Opus)
pour le Wi-Fi ou les SBC est une piste pour plus tard ; il prendrait un
autre `format`.

## Pixels

`enum sremfb_pixfmt` :

- `SREMFB_PIX_XRGB8888` (0) — 4 o/px, ordre mémoire B,G,R,X (DRM XRGB8888
  little-endian). Passthrough vers un fb 32bpp.
- `SREMFB_PIX_RGB565` (1) — 2 o/px, `u16` LE, `r<<11 | g<<5 | b`. Le
  serveur convertit depuis le BGRx d'EVDI, avec dithering ordonné (Bayer)
  aligné sur l'écran (`SREMFB_NO_DITHER=1` pour couper).

Le format effectif des frames est celui annoncé dans `server_hello.pixfmt`.

## Damage

Le serveur n'émet un `frame_hdr` que sur damage : le noyau EVDI fusionne
les zones modifiées en 16 rectangles maximum et seuls ces rectangles
partent. Écran statique = zéro trafic de pixels (juste le battement de
cœur de 28 octets toutes les ~2 s quand il est négocié). Chaque
rectangle est compressé en LZ4 (repli RAW si le client n'a pas mis le
flag, ou si LZ4 ne gagne rien).

## H.264 adaptatif

Quand les deux côtés l'ont annoncé, le serveur bascule le flux en H.264
sous congestion **mesurée** (délai d'écho trop haut, ou cadence livrée
sous ~15 fps alors que le damage continue) et revient quand elle
retombe — rien n'est configuré : la capacité du lien s'apprend en
observant ce qui s'écoule réellement pendant que le délai dit que le
lien sature. Le flux reste piloté par le damage en mode H.264 : une
access unit plein écran par événement de damage (les zones inchangées ne
coûtent rien grâce aux blocs skip), écran statique = toujours zéro
trafic.

Les règles d'ordre qui rendent les bascules invisibles :

- **RAW → H264** : la première access unit est un IDR (avec SPS/PPS),
  elle repeint la frame entière ; pas de trou.
- **H264 → RAW** : le serveur envoie `H264_EOS`, puis un **repaint
  plein écran RAW/LZ4**, puis les rects normaux. Le client doit finir de
  drainer et d'afficher la sortie de son décodeur *avant* de continuer à
  lire : le repaint arrive donc toujours en dernier et l'écran finit
  exact au pixel près.
- Un nouvel épisode redémarre toujours par un IDR.
- `PING` peut apparaître n'importe où dans le flux, dans les deux modes.

Le flux encodé est en 4:2:0 (profil High au plus), BT.601 plage
limitée, sans B-frames — décodable dans l'ordre par les décodeurs
matériels V4L2 stateful des SBC courants (ex. Raspberry Pi ≤ 3).

## Compatibilité de version

Le magic v1 est conservé, mais `proto_ver` est vérifié à la réception du
hello : un client v1 (24 o) est rejeté par le serveur v2 avec
`BAD_HELLO`. Les champs `reserved[]` permettent d'étendre les structs sans
casser la taille tant qu'ils restent à zéro côté ancien pair — c'est
exactement ainsi que les bits de fonctionnalité v2 ont été ajoutés : un
ancien client laisse les bits 1-2 à zéro (le serveur ne pingue ni
n'encode jamais), un ancien serveur envoie un octet `flags` nul (le
client n'écrit jamais en montant), et chaque combinaison conserve le
comportement v2 de base.

Même chose pour les entrées : un ancien serveur (≤ 1.4.1) ignore le bit
4 du hello client et n'annonce jamais `SREMFB_SRV_FLAG_INPUT`, donc un
nouveau client reste en lecture seule ; et même s'il envoyait des INPUT,
ce sont des messages de 16 octets avec magic, que l'ancien serveur
ignore sans perdre le cadrage (type inconnu). Un ancien client ne pose
jamais le bit 4 : le serveur ne crée rien.

Et pour le son : un ancien serveur ignore le bit 5 et envoie `flags`
bit 3 et `audio_token` à zéro — le nouveau client n'ouvre aucun flux
UDP ; un ancien client ne pose pas le bit 5 et ignore les octets
`audio_token` (ex-`reserved`) — le serveur ne crée aucune sortie son.
Rien ne change sur la connexion TCP.

## Téléport USB

Hors-bande, aucun message sRemFB : quand le client met
`SREMFB_HELLO_FLAG_USB`, il promet qu'un `usbipd` standard écoute sur
son port TCP 3240 avec les périphériques éligibles liés à `usbip-host`.
Le serveur les attache (vhci-hcd) tant que le client streame et les
détache à sa déconnexion — la même vivacité de 6 s qui débranche
l'écran virtuel libère aussi les périphériques USB. Tout passe par le
protocole usbip standard ; sRemFB n'orchestre que le cycle de vie.
