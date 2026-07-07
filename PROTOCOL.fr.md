# Protocole réseau

[English](PROTOCOL.md) · **Français**

Protocole applicatif de sRemFB, partagé mot pour mot par le serveur et le
client dans [`protocol.h`](protocol.h). Version décrite ici : **v2**
(`SREMFB_PROTO_VER = 2`), avec ses **bits de fonctionnalité** (mesure du
délai et H.264 adaptatif, négociés par les hellos sans changer de
version — toutes les combinaisons ancien/nouveau restent compatibles).

## Transport

- **TCP**, un port unique (défaut **4629**), plusieurs clients simultanés
  distingués par leur adresse MAC (voir [README.fr.md](README.fr.md)).
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
serveur → client   :  sremfb_frame_hdr H264 + access unit   (sous
                      congestion mesurée, si négocié ; H264_EOS clôt
                      l'épisode)
```

## Messages

### `sremfb_client_hello` — 48 octets, client → serveur

Envoyé une seule fois, juste après la connexion.

| Champ | Type | Rôle |
|---|---|---|
| `magic` | `u32` | `SREMFB_MAGIC` |
| `proto_ver` | `u16` | `SREMFB_PROTO_VER` (2) |
| `flags` | `u16` | bit 0 `SREMFB_HELLO_FLAG_LZ4` = le client accepte LZ4 · bit 1 `SREMFB_HELLO_FLAG_FEEDBACK` = le client renvoie les PING en PONG · bit 2 `SREMFB_HELLO_FLAG_H264` = le client décode le H.264 (4:2:0, Annex B) à sa résolution · bit 3 `SREMFB_HELLO_FLAG_USB` = le client exporte ses périphériques USB par usbip (voir « Téléport USB ») |
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
| `flags` | `u8` | bit 0 `SREMFB_SRV_FLAG_PING` = des PING peuvent arriver · bit 1 `SREMFB_SRV_FLAG_H264` = le flux peut basculer en H.264. Le serveur ne pose un bit que si le client a annoncé la capacité correspondante ; les anciens serveurs envoient toujours 0 ici |
| `reserved[2]` | `u8` | réservé |

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

Le seul message montant après le hello. Émis uniquement quand le hello
serveur a annoncé `SREMFB_SRV_FLAG_PING` (les anciens serveurs lisent et
ignorent les octets montants : un client qui se tromperait ne casse
rien).

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

## Téléport USB

Hors-bande, aucun message sRemFB : quand le client met
`SREMFB_HELLO_FLAG_USB`, il promet qu'un `usbipd` standard écoute sur
son port TCP 3240 avec les périphériques éligibles liés à `usbip-host`.
Le serveur les attache (vhci-hcd) tant que le client streame et les
détache à sa déconnexion — la même vivacité de 6 s qui débranche
l'écran virtuel libère aussi les périphériques USB. Tout passe par le
protocole usbip standard ; sRemFB n'orchestre que le cycle de vie.
