# Protocole réseau

[English](PROTOCOL.md) · **Français**

Protocole applicatif de sRemFB, partagé mot pour mot par le serveur et le
client dans [`protocol.h`](protocol.h). Version décrite ici : **v2**
(`SREMFB_PROTO_VER = 2`).

## Transport

- **TCP**, un port unique (défaut **4629**), plusieurs clients simultanés
  distingués par leur adresse MAC (voir [README.fr.md](README.fr.md)).
- `TCP_NODELAY`, `SO_KEEPALIVE` (idle 10 s / intvl 5 s / cnt 3 — détecte
  un SBC débranché même sans trafic), `SO_SNDTIMEO` 20 s, `SO_RCVTIMEO`
  5 s. `SIGPIPE` ignoré.
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
```

## Messages

### `sremfb_client_hello` — 48 octets, client → serveur

Envoyé une seule fois, juste après la connexion.

| Champ | Type | Rôle |
|---|---|---|
| `magic` | `u32` | `SREMFB_MAGIC` |
| `proto_ver` | `u16` | `SREMFB_PROTO_VER` (2) |
| `flags` | `u16` | bit 0 `SREMFB_HELLO_FLAG_LZ4` = le client accepte LZ4 |
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
| `reserved[3]` | `u8` | réservé |

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
| `reserved[3]` | `u8` | réservé |
| `x`, `y`, `w`, `h` | `u16` | rectangle destination, en coordonnées flux |
| `payload_len` | `u32` | RAW : `w*h*bytespp` ; LZ4 : taille du bloc ; BLANK/UNBLANK : 0 |

Encodages (`enum sremfb_encoding`) :

| Valeur | Nom | Payload |
|---|---|---|
| 0 | `RAW` | `w*h*bytespp` pixels bruts, sans padding |
| 1 | `LZ4` | un bloc LZ4 de ces mêmes pixels bruts |
| 2 | `BLANK` | aucun : éteindre la dalle (DPMS off) |
| 3 | `UNBLANK` | aucun : rallumer la dalle |

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
partent. Écran statique = zéro trafic. Chaque rectangle est compressé en
LZ4 (repli RAW si le client n'a pas mis le flag, ou si LZ4 ne gagne rien).

## Compatibilité de version

Le magic v1 est conservé, mais `proto_ver` est vérifié à la réception du
hello : un client v1 (24 o) est rejeté par le serveur v2 avec
`BAD_HELLO`. Les champs `reserved[]` permettent d'étendre les structs sans
casser la taille tant qu'ils restent à zéro côté ancien pair.
