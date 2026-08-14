# Bug : grippage EBUSY de mutter après churn des devices EVDI

Diagnostiqué le 2026-07-20 sur le PoC Proxmox (VM `debian`, Debian 13,
GNOME/mutter 48.7, evdi 1.14.8, kernel 6.12.95, GPU VirtIO sans VirGL).

## Symptôme

Côté client, refus systématique au hello :

    sremfb-client: server refused: status 2

Côté serveur, chaque connexion suit le même cycle :

    [MAC] using evdi device /dev/dri/card7
    [MAC] connector plugged at 1920x1080 (serial 0x07c2b535), waiting for the compositor
    [MAC] compositor did not light up the connector within 10s (is a Wayland/GNOME session running?)
    [MAC] quarantining /dev/dri/card7
    [MAC] connector unplugged

alors que la session GNOME Wayland tourne normalement. Dans le journal de
gnome-shell, au moment de chaque plug :

    gnome-shell: Added device '/dev/dri/card8' (evdi) using atomic mode setting.
    gnome-shell: Failed to reopen '/dev/dri/card7': GDBus.Error:System.Error.EBUSY: Device or resource busy

## Cause

Quand des tentatives de connexion échouent en série (dans le cas du PoC :
le client joignait le serveur depuis un réseau où le retour streaming ne
passait pas), la boucle quarantaine → self-heal recrée un device EVDI
frais à chaque essai. Les numéros de cartes montent (card1 → card12…)
**pendant que gnome-shell tourne**.

mutter 48 ne survit pas à ce turnover : au bout de quelques cycles
add/remove, toute réouverture d'un device evdi — y compris un device
*neuf* jamais vu — échoue en `EBUSY` via logind, définitivement. Le
connecteur n'est alors jamais allumé, le serveur répond
`SREMFB_STATUS_SERVER_FAIL` (status 2), quarantaine le device et le
self-heal en recrée un… ce qui entretient le grippage. L'état est
auto-aggravant : une simple période de connexions ratées suffit à wedger
la session pour de bon.

C'est le même grippage que celui mentionné dans le commentaire de
`sremfb-evdi-perms.service` (reset anti-grippage au démarrage du
serveur), mais déclenché *en cours de session* : le reset au démarrage ne
protège pas de celui-là, et **redémarrer sremfb-server ne suffit pas**
(vérifié : devices tout frais card11/card12, EBUSY immédiat quand même).

## Contournement

Recycler la session compositor complète :

    systemctl restart gdm3

Indolore quand l'auto-login GDM est actif (la session et le serveur
reviennent seuls en ~20 s). C'est le seul remède constaté une fois le
grippage installé.

## Pistes de correction côté serveur

- **Replug plutôt que churn** : en cas de `compositor did not light up`,
  re-tenter un plug sur le *même* device au lieu de le mettre en
  quarantaine et d'en créer un neuf — le churn est le déclencheur du
  grippage, pas sa victime.
- **Plafonner le self-heal** plus agressivement quand les échecs
  s'enchaînent sur des devices pourtant frais (signature du grippage
  mutter, par opposition à un device individuellement wedgé).
- **Détecter le grippage** : plusieurs devices neufs consécutifs jamais
  allumés ⇒ log explicite suggérant le restart de la session, plutôt que
  de boucler en silence.

## Problèmes voisins rencontrés le même jour (environnement, pas sremfb)

- **VirGL casse la capture** (VM libvirt/QEMU, `accel3d=yes`) : le
  « secondary GPU copy » de mutter ne peint qu'un des deux buffers de
  flip de la sortie EVDI → le flux alterne frame de contenu / frame 100 %
  noire (+ artefact curseur noir 114x102 en 0,0). Sur le panneau : noir
  sauf zones re-damagées, zones figées. Toujours désactiver VirGL/3D
  accel sur un serveur en VM (la copie CPU/llvmpipe est correcte).
- **GDM force Xorg dans les VM** : la règle
  `/usr/lib/udev/rules.d/61-gdm.rules` tague les GPU virtuels
  (virtio 1af4:1050, bochs 1234:1111, qxl, cirrus) et GDM préfère alors
  Xorg — `WaylandEnable=true` ne suffit pas. Contournement : copie de la
  règle dans `/etc/udev/rules.d/` sans ces lignes, **reboot obligatoire**
  (la propriété udev reste attachée au device jusque-là).
- **Greeter Wayland vs devices EVDI au boot** : avec
  `initial_device_count=2`, le gnome-shell du greeter peut boucler sur
  `gbm_surface_lock_front_buffer failed` (login impossible). Contournement
  déployé sur le PoC : drop-in gdm3
  `ExecStartPre=-/bin/sh -c "echo 1 > /sys/devices/evdi/remove_all"` —
  sans effet de bord, le serveur recrée ses devices à son démarrage.
