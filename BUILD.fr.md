# Compilation et déploiement

[English](BUILD.md) · **Français**

Deux binaires C, sans build system exotique : de simples `Makefile`.

- `server/sremfb-server` — sur le PC (x86-64, GNOME/Wayland).
- `client/sremfb-client` — sur le SBC (ARM) ou en mode test sur le PC.

`make` à la racine construit les deux. Chaque sous-répertoire a aussi son
propre `Makefile` si on ne veut qu'un côté.

## Dépendances

### Serveur (PC)

```sh
sudo apt install build-essential libglib2.0-dev liblz4-dev libx264-dev \
                 evdi-dkms libevdi-dev
```

- `glib-2.0` — boucle d'événements, sources, utilitaires.
- `liblz4` — compression des rectangles.
- `libx264` — encodage H.264 du chemin de compression adaptative
  (enclenché par client, sous congestion mesurée).
- `libevdi` + `evdi-dkms` — le module noyau EVDI (pilote DisplayLink) et
  sa bibliothèque. `evdi-dkms` compile le module pour le noyau courant ;
  un `dkms` fonctionnel et les en-têtes noyau sont donc requis.
- `-lm` (libc math) pour le dithering.

Testé avec libevdi/evdi-dkms **1.14.8** (Debian trixie).

### Client (SBC)

Seule dépendance de build : `liblz4-dev`.

```sh
sudo apt install build-essential liblz4-dev
```

Le client est volontairement du C pur : pas de GLib, pas de DRM, rien
d'autre que la libc et lz4 — le décodage H.264 matériel optionnel passe
par de purs ioctls V4L2 contre les en-têtes UAPI du noyau. Il compile
tel quel sur Debian, Raspberry Pi OS et Armbian.

## Build local

```sh
make                 # server/sremfb-server + client/sremfb-client
make -C server       # serveur seul
make -C client       # client seul
make clean
```

Flags par défaut : `-O2 -g -Wall -Wextra`. Surchargeables via `CFLAGS`.

## Installation locale (sans paquet)

### Serveur (sur le PC, en root)

```sh
sudo make install-server
```

Installe :

- le binaire dans `/usr/local/bin/` ;
- l'unité systemd **user** dans `/etc/systemd/user/` (le serveur tourne
  dans la session graphique, pas en service système) ;
- `/etc/modules-load.d/sremfb.conf` et `/etc/modprobe.d/sremfb.conf`
  (charge `evdi` au boot avec `initial_device_count=2`) ;
- la hwdb `61-sremfb-display-vendor.hwdb` (étiquette du vendor EDID) ;
- la règle udev `60-sremfb-evdi.rules` (droits groupe `video` sur la
  création des devices EVDI — voir [README.fr.md](README.fr.md), section
  « Plusieurs écrans ») ;
- `/etc/sremfb-server.conf` (s'il n'existe pas déjà).

Puis, la première fois :

```sh
sudo modprobe evdi     # auto au boot ensuite
systemctl --user daemon-reload
systemctl --user enable --now sremfb-server
```

L'utilisateur de session doit appartenir au groupe `video` pour que le
serveur puisse régénérer les devices EVDI à son démarrage
(`id -nG | grep video` ; sinon `sudo usermod -aG video $USER` puis
relogin).

### Client (sur le SBC, en root)

```sh
sudo make install-client
sudo nano /etc/sremfb.conf         # SREMFB_SERVER=<ip du PC>
sudo systemctl daemon-reload
sudo systemctl enable --now sremfb-client
```

`PREFIX` (défaut `/usr/local`) et `DESTDIR` sont respectés par les deux
cibles d'installation.

## Paquets Debian

`pkg/build-debs.sh` produit trois `.deb` dans `dist/` :

| Paquet | Arch | Cible |
|---|---|---|
| `sremfb-server` | amd64 | PC GNOME/Wayland |
| `sremfb-client` | arm64 | SBC 64 bits (Pi 3/4/5/500, etc.) |
| `sremfb-client` | armhf | SBC ARMv7 (Banana Pi M1+, Pi 2, etc.) |

```sh
./pkg/build-debs.sh              # version 1.1.0 par défaut
./pkg/build-debs.sh 3.1.0        # version explicite
```

Détails :

- **Client lié statiquement à lz4.** Le script télécharge `liblz4-dev`
  pour chaque architecture cible (`apt-get download`, mis en cache dans
  `pkg/sysroot/`) et lie `liblz4.a` en dur. Le paquet client ne dépend
  donc que de `libc6` — aucune contrainte de version lz4 sur la cible.
- **Cross-compilation** du client : nécessite les toolchains
  `gcc-aarch64-linux-gnu` et `gcc-arm-linux-gnueabihf`, et les
  architectures `arm64`/`armhf` activées dans dpkg pour `apt-get
  download` :

  ```sh
  sudo apt install gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf
  sudo dpkg --add-architecture arm64
  sudo dpkg --add-architecture armhf
  sudo apt update
  ```

- **Le module noyau n'est pas repackagé** : `evdi-dkms` existe dans
  Debian et n'est requis que côté serveur (déclaré en `Depends`).
- Les paquets **remplacent** les anciens `rfb-server`/`rfb-client`
  (`Conflicts`/`Replaces`). Le postinst du client migre automatiquement
  `/etc/rfb.conf` → `/etc/sremfb.conf` (variables `RFB_*` → `SREMFB_*`)
  si la conf n'a pas encore été touchée depuis l'installation.
- Le postinst du serveur met à jour la hwdb, recharge udev, charge
  `evdi` et applique tout de suite les droits groupe `video` sur
  `/sys/devices/evdi/{add,remove_all}`.

Le champ mainteneur du paquet vaut par défaut `Jonathan Roth <jr@462eng.fr>` ;
surchargez-le via la variable d'environnement `MAINT`
(`MAINT="Nom <vous@example.com>" ./pkg/build-debs.sh`).

### Installation d'un paquet

```sh
# serveur
sudo apt install ./dist/sremfb-server_1.1.0_amd64.deb
# client (sur le SBC)
sudo apt install ./dist/sremfb-client_1.1.0_arm64.deb
```

Sur une conf déjà modifiée, `dpkg -i --force-confold` conserve le fichier
existant.

## Publication sur un dépôt APT (optionnel)

Pour distribuer les paquets par un dépôt APT géré avec `reprepro`
(adaptez l'hôte, le codename et le composant à votre infrastructure) :

```sh
scp dist/*.deb REPO_HOST:/tmp/
ssh REPO_HOST 'reprepro -b /srv/debian -C main includedeb CODENAME /tmp/sremfb-*.deb'
```

Le dépôt doit déclarer les architectures `amd64 arm64 armhf` et peut être
signé avec la clé GPG de votre choix (`SignWith` dans `conf/distributions`).

## Test sans matériel

Le client tourne en mode test sur le PC lui-même (pas de framebuffer, pas
de console prise en main) :

```sh
./server/sremfb-server &
./client/sremfb-client --test 1920x1080 localhost
```

Un écran 1920×1080 apparaît dans **Réglages → Affichage**. Le client
enregistre une frame sur 30 dans `sremfb-test-NNNN.ppm` (vérification des
couleurs, du curseur incrusté, du damage). Deuxième écran de test avec un
autre MAC :

```sh
SREMFB_MAC=02:00:00:00:00:02 ./client/sremfb-client --test 1920x1080 localhost
```

Ctrl-C sur le client → l'écran « se débranche » côté PC.
