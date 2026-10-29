# MControlCenter — édition MSI Summit E16 Flip A11UCT (MS-1591)

Fork de [dmitry-s93/MControlCenter](https://github.com/dmitry-s93/MControlCenter) dédié à un
**unique modèle** : *MSI Summit E16 Flip A11UCT*, carte **MS-1591**, firmware EC `1591EMS1.108`.

Le dépôt contient **tout le système** :

| Dossier | Contenu |
|---|---|
| `src/` | l'application Qt6 (fork de MControlCenter) |
| `src/helper/` | le démon D-Bus privilégié (`mcontrolcenter-helper`) |
| `wmi-fan-curve/` | le module noyau `msi-wmi-platform` patché (courbes ventilateur via WMAM) |
| `docs/FAN_CURVE_MAPPING.md` | la correspondance WMAM ↔ EC RAM validée (lire en premier !) |
| `install_mcontrolcenter.sh` | l'installation complète (app + démon + autostart + module) |

**Ce que ça permet** : contrôle avancé des ventilateurs (courbes personnalisées 7 points)
réappliqué à chaque démarrage, silencieux à faible charge, sans la moindre écriture EC brute —
les écritures passent par l'interface WMI-ACPI du firmware (`WMAM`/`SET_FAN`), la même que
celle du logiciel MSI sous Windows.

## Pourquoi ce fork (l'écart avec l'original)

Sur ce modèle, les écritures écrites EC brutes des courbes du logiciel original ont causé des
redémarrages matériels durs (l'EC coupe l'alimentation), et l'original n'expose aucune courbe
sysfs. Le fork remplace le transport des courbes par la voie WMI validée et applique un
invariant de sécurité observé : *courbes avancées uniquement sur le profil « High performance »*.

### Liste des changements vis-à-vis de l'original

1. **Aucune écriture EC brute des courbes** tant que le pilote `msi-ec` possède l'EC
   (`setFan{1,2}Speed/TempSettings` refusés par le canal raw).
2. **Lecture/écriture des courbes via sysfs** (`fan1_duty_curve` / `fan2_duty_curve`
   du module noyau patché) : une écriture = une commande WMAM `SET_FAN` de fenêtre
   complète (8 octets, duty live préservé), vérifiée par le pilote par relecture.
3. **Écritures root via le démon** : les attributs sysfs sont root-owned ; ajout d'une
   méthode D-Bus `setWmiFanCurve` strictement validée (liste blanche d'attributs,
   7 valeurs 0–255) dans `mcontrolcenter-helper`, avec constantes partagées
   `src/helper/wmifancurve.h`.
4. **Désactivation réelle** : quitter le mode « Advanced » (ou basculer le profil système)
   **réécrit les courbes par défaut du firmware** (`45 55 60 70 90 110 130` /
   `45 70 90 100 110 110 110`) — l'EC garde la courbe stockée quel que soit le mode fan,
   donc réécrire est la seule vraie façon de « reprendre le contrôle auto ».
5. **Invariant advanced ⇔ High performance** (crashs multiples observés sur le combo
   « profil ≠ High performance » + « advanced ») :
   - la case « Enable advanced fan control » n'est cliquable qu'en High performance ;
   - tout autre profil la décoche, repasse le mode fan en `auto` via msi-ec et restaure
     les courbes usine ;
   - un avertissement inline rappelle la règle.
6. **Start-up et veille** : le mode fan est d'abord appliqué, les courbes personnalisées ne
   sont restaurées que si l'état résultant est sûr ; sinon courbes usine. Wake inclus.
7. **UI Fan Control repensée** :
   - boutons **Cancel** (ex-Reset) et **Apply** actifs seulement si un éditeur diffère
     du dernier jeu appliqué (snapshot) ;
   - Cancel ramène les valeurs de référence sans relire l'EC ;
   - température read-only (l'EC les possède — comme l'UI MSI sous Windows).
8. **Identification de la build** : la page *About* affiche version + `commit` git +
   horodatage de build automatiques.

Côté **module noyau** (`wmi-fan-curve/`) : sysfs `fan{1,2}_duty_curve` (RW),
`fan{1,2}_live_duty` (RO), `fan{1,2}_temp_curve` (RO) ; double garde-fou — paramètre de
module **`allow_curve_writes`** (défaut *false* : `-EPERM` sans opt-in explicite) et
**readback** après chaque écriture.

## Installation

Dépendances : Qt6 ≥ 6.4 (`qt6-base-dev`, `qt6-tools-dev`), `cmake`, `build-essential`.

```bash
git clone <ce dépôt>
cd mcontrolcenter

# application + démon
cmake -B build -S . && cmake --build build -j8

# module noyau patché
make -C wmi-fan-curve

# installation complète (app + démon D-Bus + autostart + module + depmod)
sudo ./install_mcontrolcenter.sh
```

Activer sans reboot : `sudo rmmod msi_wmi_platform && sudo modprobe msi_wmi_platform`.

Désinstaller :

```bash
sudo rm /usr/local/bin/mcontrolcenter /usr/libexec/mcontrolcenter-helper \
        /usr/share/dbus-1/system-services/mcontrolcenter.helper.service \
        /usr/share/dbus-1/system.d/mcontrolcenter-helper.conf \
        /usr/share/applications/mcontrolcenter.desktop /usr/share/pixmaps/mcontrolcenter.png \
        /etc/xdg/autostart/mcontrolcenter.desktop /etc/modprobe.d/msi-fan-curve-writes.conf
sudo rm -rf /lib/modules/$(uname -r)/updates/msi-wmi-platform
sudo depmod -a
```

## Prérequis pilotes

- `msi-ec` doit être chargé (mode ventilateur, shift mode…).
  Vérifier : `cat /sys/devices/platform/msi-ec/shift_mode`.
- `ec_sys` **sans** `write_support=1` est suffisant pour les lectures de repli ;
  `modprobe ec_sys` (lecture seule). Le module original chargeait
  `ec_sys write_support=1` → inutile ici.

## Garde-fous & sécurité

- Les courbes ne s'écrivent que si le module noyau a été chargé avec
  `allow_curve_writes=1` — consentement explicite, persisté via
  `/etc/modprobe.d/msi-fan-curve-writes.conf`.
- Chaque écriture de courbe est **vérifiée par relecture** (readback) : refus visible côté app
  (`qWarning`) si l'EC ne mémorise pas les valeurs.
- Le combo « advanced » + profil non-High-performance est **impossible par construction**.
- En cas de doute : repasser en `balanced` — mode fan `auto` + courbes usine, comportement
  équivalent à la machine d'origine.

## Maintenance

- **Après une mise à jour du noyau Ubuntu** : `make -C wmi-fan-curve`
  puis `sudo ./install_mcontrolcenter.sh` (~1 min).
- Le diagnostic des bascules de profils système est visible au lancement de l'app depuis un
  terminal (`PowerMonitor: PropertiesChanged …`).

## Crédits

- [dmitry-s93/MControlCenter](https://github.com/dmitry-s93/MControlCenter) — l'application
  d'origine (GPLv3), par Dmitry Serov et la communauté.
- [BeardOverflow/msi-ec](https://github.com/BeardOverflow/msi-ec) — le pilote EC noyau.
- [Armin Wolf](https://lore.kernel.org/all/?q=msi-wmi-platform) — le pilote noyau
  `msi-wmi-platform`, base du module de ce dépôt.
- Licence : GPLv3 (voir `LICENSE`).
