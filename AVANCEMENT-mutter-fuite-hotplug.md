# Fuite mémoire mutter au hotplug evdi pendant la veille — avancement

Branche `mutter-fuite-hotplug` (depuis main). Incident : nuit du 05 au 06/11/2026
(1 368 cycles plug/unplug de card1 pendant `PowerSaveMode` = 3, ~31 Mo par cycle,
OOM à 05:37).

## État (06/11/2026, ~11:40)

- [x] Mécanisme trouvé dans le code (voir plus bas), **prouvé par lecture du code**.
- [x] Correctif amont trouvé : 3 commits GNOME 49 (septembre 2026), absents de
      la branche gnome-48 à ce jour. Rétroportés, pas de patch maison.
- [x] Patches dans `pkg/mutter/patches/`, script `pkg/mutter/build-mutter-sremfb.sh`.
- [ ] Paquets `48.7-0+deb13u1+sremfb2` : construction lancée sur Riza (dans le
      scratchpad local, pas le ~ NFS). Résultat attendu dans
      `dist/mutter-48.7-sremfb2/` (dist/ n'est pas suivi par git).
- [ ] Reproduction en VM : non faite, protocole ci-dessous.
- [ ] Validation en réel : à faire par Jonathan (installation non faite, interdite ici).

⚠ Constat : `dpkg -l` montre que Riza tourne sur le mutter Debian **d'origine**
(`48.7-0+deb13u1`), pas sur sremfb1. L'incident a donc eu lieu sans le patch
sremfb1. sremfb2 contient sremfb1 + les 3 rétroportages.

## Mécanisme (mutter 48.7, src/backends/native/meta-renderer-native.c)

1. Chaque hotplug (branchement **ou** débranchement d'un connecteur) aboutit à
   `meta_renderer_native_rebuild_views()` (l. 1604). Celui-ci appelle
   `detach_onscreens()` (l. 1562) : **tous** les onscreens de toutes les vues
   (écran principal amdgpu compris, pas seulement l'evdi) sont ajoutés à
   `renderer_native->detached_onscreens` avec un `g_object_ref()`.
2. Cette liste n'est vidée que par `clear_detached_onscreens()` (l. 862), appelée
   seulement depuis `meta_renderer_native_post_mode_set_updates()` (l. 945) et
   `dispose` (l. 2557, arrêt de mutter). `post_mode_set_updates` n'est atteint que
   depuis `maybe_post_next_frame` de meta-onscreen-native.c (l. 1723, 1738), c'est-à-dire
   quand une frame est postée avec un modeset en attente. En veille, rien n'est posté
   (`pop_pending_mode_set` renvoie FALSE tant que le mode n'est pas ON, l. 1283).
3. Donc pendant la veille, chaque `rebuild_views` empile les onscreens précédents et
   rien ne les libère. Il n'y a pas de fuite au sens strict : tout serait libéré à la
   première frame après le réveil. Mais l'accumulation n'a pas de borne.
4. Ce que retient un onscreen evdi : en mode copie CPU / copie par le GPU principal
   (evdi n'a pas de rendu, d'où « kmsro: driver missing » et « Failed to initialize
   accelerated iGPU/dGPU framebuffer sharing »), `init_secondary_gpu_state_cpu_copy_mode()`
   (meta-onscreen-native.c l. 2820, boucle l. 2860-2875) alloue **3 dumb buffers** de la taille du mode
   sur la carte evdi, dès la création de l'onscreen. Côté evdi, ce sont des GEM shmem
   (`evdi_gem_create` → `drm_gem_get_pages`, evdi_gem.c l. 242-258, `.free` =
   `evdi_gem_free_object` l. 450). Ils ne sont libérés que quand mutter lâche l'objet,
   ce qui n'arrive pas ici. Pas de défaut evdi dans ce mécanisme.
   3 × 1920×1080×4 = 24,9 Mo par onscreen evdi accumulé. Le reste des ~31 Mo/cycle
   est **supposé** : gbm_surface du GPU principal pour la vue evdi et pour l'écran
   principal (amdgpu, VRAM/GTT, pas forcément compté en Shmem), mmap des dumb buffers
   et textures importées.
   Certitude : forte pour l'accumulation et les 3 dumb buffers (code), moyenne pour la
   ventilation exacte des 31 Mo (pas mesurée).

## Correctif amont (rétroporté)

| Commit gnome-49 | MR | Rôle |
|---|---|---|
| fd3c3b5f20e3 (cherry-pick de 82ad6279) | !5197 | ne pas mettre les offscreens (CRTC virtuels) dans la liste |
| d4a0aa088e77 (cherry-pick de af926cff), Michel Dänzer | !5285 | **le correctif** : `clear_detached_onscreens()` à l'entrée en veille + pas d'empilement en veille. Closes work_items 4101 et 5008 |
| a13f465e6934 (cherry-pick de 8e3870f4), Jonas Ådahl | !5324 | toujours appeler `meta_onscreen_native_detach()` (évite un use-after-free de vue par les signaux gamma ou privacy-screen après hotplug en veille, work_item 5042) |

Le 3e est indispensable avec le 2e (un hotplug en veille faisait planter sans lui).
Pour le 3e, le cas de test (`src/tests/native-kms-device.c`) n'est pas repris : les
en-têtes et utilitaires de test de 48.7 diffèrent.

## Hotplug pendant la veille (priorité 2) : pas de bug trouvé dans le code

- Le hotplug n'est pas filtré pendant la veille (aucun test de power_save dans
  meta-monitor-manager.c sur ce chemin). La config est calculée et appliquée, et les
  vues sont reconstruites tout de suite, connecteur evdi compris.
- Au réveil : `on_power_save_mode_changed` (ON, MODE_CHANGE) →
  `meta_renderer_native_queue_modes_reset()` met **toutes** les vues courantes dans
  `pending_mode_set_views`. La première frame fait donc le modeset de chaque CRTC,
  y compris celui du connecteur branché pendant la veille.
- Un GPU evdi ajouté pendant la veille n'a pas reçu `meta_kms_device_disable()`
  (c'est fait seulement à l'entrée en veille, pour les GPU présents). Sans
  conséquence, il n'était pas allumé.
- Conclusion : il faut seulement vérifier en réel qu'un evdi branché pendant la
  veille s'allume au réveil. Le code ne montre pas de bug.

## Protocole de reproduction (VM jetable, pas la session de Jonathan)

1. VM KVM Debian 13 + GNOME (gdm, session Wayland), 4 Go RAM, virtio-gpu. Installer
   `evdi-dkms` 1.15.1 et `sremfb-server` (ou un petit client libevdi qui fait
   `evdi_connect` avec un EDID 1920×1080 puis `evdi_disconnect`).
2. Ouvrir une session, puis forcer la veille :
   `gdbus call --session -d org.gnome.Mutter.DisplayConfig -o /org/gnome/Mutter/DisplayConfig -m org.freedesktop.DBus.Properties.Set org.gnome.Mutter.DisplayConfig PowerSaveMode '<3>'`
   (ou `busctl --user set-property … PowerSaveMode i 3`).
3. Boucle de 50 cycles : connect, 1 s, disconnect, 15 s. À chaque cycle, relever
   `Shmem`, `Unevictable` dans /proc/meminfo, et le nombre de lignes de
   `/sys/kernel/debug/dri/<evdi>/gem_names` ou `clients` si disponible.
4. Attendu avec 48.7-0+deb13u1 : Shmem croît de ~25-31 Mo par cycle, et retombe au
   réveil (PowerSaveMode 0 + mouvement souris). Attendu avec sremfb2 : Shmem plat.
5. Contrôle priorité 2 : brancher pendant la veille, réveiller. Le connecteur evdi
   doit afficher (journal `MUTTER_DEBUG=kms` : « Queue mode set » puis modeset du CRTC evdi).
6. Contrôle du 3e patch : pendant la veille, hotplug puis changement de température
   de couleur (Night Light) avant le réveil. Pas de crash attendu.

## Reprise

- Sources préparées (patches Debian + sremfb appliqués, puis quilt pop) :
  scratchpad de la session, `src/mutter-48.7`. Log : `src/build-sremfb2.log`.
- Sinon : `WORK=/var/tmp/x pkg/mutter/build-mutter-sremfb.sh 2` reconstruit tout.
