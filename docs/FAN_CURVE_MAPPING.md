# Correspondance WMAM (SCM0) ↔ EC RAM — MSI Summit E16 Flip A11UCT (MS-1591)

Date : 2026-10-29 · Source : campagne `wmi-evidence/20261029-165150/` + **campagne ECRAM croisée `wmi-evidence/20261029-172118/` (avec `ec_sys` en lecture seule)**
Statut : **VALIDÉ 1:1 — correspondance octet-exacte WMAM ↔ EC RAM confirmée** sans aucune écriture EC.

## 0. Mécanique

- Méthode ACPI `WMAM` (`\_SB.PC00.LPCB.EC.SCM0`), GUID WMI `ABBC0F6E-8EA1-11D1-00A0-C90629100000`,
  objet `AM`, 1 instance. Fenêtre index/données sur I/O `0x360/0x361` (= « indices SCM » du DSDT).
- Appel WMI : tampon de 32 octets. `buffer[0]` = IPAR (sous-index de groupe), `buffer[1..31]` = BS01..BS31.
  Réponse : `output[0]` = statut (doit être ≠ 0), `output[1..31]` = données.
- Pairage : lecture = cmd impair, écriture = cmd impair+1.
- Le pilote noyau `msi_wmi_platform` (déjà dans l'arbre) expose un fichier debugfs par commande :
  y écrire 32 octets = invoquer la commande ; relire = obtenir le tampon réponse.
  → `get_fan` = cmd `0x11`, `set_fan` = `0x12`, `get_temperature` = `0x0D`, `set_temperature` = `0x0E`,
  `get_thermal` = `0x0F`, `set_thermal` = `0x10` (les noms de fichiers noyau, la cmd est la source de vérité).

## 1. Table finale : cmd / IPAR → adresses EC

⚠ Ordres NON séquentiels dans certaines fenêtres (validé par les données live) :
l'ordre indiqué est celui de remplissage `output[1..N]` ↔ indices EC.

| cmd noyau | hex | IPAR | indices EC (ordre de la fenêtre) | Contenu live observé |
|---|---|---|---|---|
| get_ec | 0x03 | 0 | (page spéciale) | `1591EMS1.108 ...` — version EC RAM. VERIFIED : échange de page (flag bit 6) |
| get_temperature | 0x0D | 0 | 0x68, 0x80, 0x9F | EC[0x68] = **temp CPU live** (0x23=35 °C idle, 0x4D=77 °C load) ✓ |
| get_temperature | 0x0D | 1 | 0x69, 0x6F, 0x70, 0x6A-0x6E | voir §2 fan1 |
| get_temperature | 0x0D | 2 | 0x81, 0x87, 0x88, 0x82-0x86 | voir §3 fan2 |
| get_thermal | 0x0F | 0 | 0x33, 0x34 | EC[0x33][0x34] = mode : `0D 01` comfort / `05 00` eco ✓ (cohérent shift_mode) |
| get_thermal | 0x0F | 1 | 0x79-0x7F | config fan1 (static) : `00 0a 08 08 03 03 03` |
| get_thermal | 0x0F | 2 | 0x91-0x97 | config fan2 (static) : `00 05 03 03 08 07 03` |
| get_thermal | 0x0F | 3 | 0x98-0x9E | EC[0x98]=0x02 (cooler boost off — cohérent MControlCenter coolerBoostAddress=0x98), 0x9A/0x9D=0x7d, EC[0x9E] = **temp live #2** (0x23→0x32 sous charge) |
| get_fan | 0x11 | 0 | 0xC8-0xCF | **RPM** : fan1 = BE16(EC[0xC8..0xC9]) (162→2963 tr/min ✓), fan2 = BE16(EC[0xCA..0xCB]) (177→2712 ✓). Formule pilote : `480000/rpm_code` |
| get_fan | 0x11 | 1 | 0x71-0x78 (séquentiel) | fan1 : EC[0x71] = **duty live** (0x2D→0x46 sous charge ✓), EC[0x72-0x78] = **7 duty points** (statics) |
| get_fan | 0x11 | 2 | 0x89-0x90 (séquentiel) | fan2 : EC[0x89] = **duty live**, EC[0x8A-0x90] = **7 duty points** (statics) |

Écritures miroir (cmd pair suivant, MÊME fenêtre, **pas encore invoquées**) :
- `set_fan` (0x12) IPAR 1 : écrit les mêmes `0x71-0x78` (tampon `buffer[1..8]`).
- `set_fan` (0x12) IPAR 2 : écrit `0x89-0x90`.
- `set_thermal` (0x10) IPAR 0 : écrit {0x33,0x34} (mode) ; IPAR 1/2/3 : 0x79-0x7F / 0x91-0x97 / 0x98-0x9E.
- `set_temperature` (0x0E) IPAR 1/2 : écrit les fenêtres de température (⚠ ordre non séquentiel — cf. §2).

## 2. Bloc fan1 (CPU) — croisement EC brut + registres One Dragon Center

### Escalier live (invariant sur toute la campagne) des duty points :

| EC addr | 0x71 | 0x72 | 0x73 | 0x74 | 0x75 | 0x76 | 0x77 | 0x78 |
|---|---|---|---|---|---|---|---|---|
| valeur | duty live | 45 | 55 | 60 | 70 | 90 | 110 | 130 |
| évidence | 0x2D→0x46 sous charge | static | static | static | static | static | static | static |

### Temp points fan1 (via get_temperature IPAR1, dé-shuffling de l'ordre de la fenêtre) :

| EC addr | 0x69 | 0x6A | 0x6B | 0x6C | 0x6D | 0x6E | 0x6F | 0x70 |
|---|---|---|---|---|---|---|---|---|
| valeur | 0 (enable) | 60 | 70 | 80 | 85 | 90 | 95 | 100 |
| DC Default_Temp fan1 | — | idx1 | idx2 | idx3 | idx4 | idx5 | (alarme) | (alarme) |

Cohérence avec MControlCenter (`operate.cpp`, modèle générique MSI pour les courbes) :
`fan1TempSettingStartAddress = 0x6A, count 6` ; `fan1SpeedSettingStartAddress = 0x72, count 7` ;
`fan1Address = 0xC9` (RPM, modulo BE16 : famille 0xC8-0xC9). **Le code MControlCenter encode
exactement ce que la campagne observe.**

## 3. Bloc fan2 (GPU) — idem

| EC addr | 0x89 | 0x8A | 0x8B | 0x8C | 0x8D | 0x8E | 0x8F | 0x90 |
|---|---|---|---|---|---|---|---|---|
| valeur | duty live | 45 | 70 | 90 | 100 | 110 | 110 | 110 |

| EC addr | 0x81 | 0x82 | 0x83 | 0x84 | 0x85 | 0x86 | 0x87 | 0x88 |
|---|---|---|---|---|---|---|---|---|
| valeur | 0 (enable) | 55 | 65 | 75 | 85 | 89 | 90 | 89 |
| DC Default_Temp fan2 | — | idx1 | idx2 | idx3 | idx4 | idx5 | (alarme) | (alarme) |

## 4. Conversions / sémantique

- **RPM** : `480000 / BE16(code)`. code 0 = ventilateur arrêté (eco).
- **Duty** : échelle brute EC, plafond observé 130 (= valeur par défaut du dernier point).
  L'UI Windows 0-110 = plafond *utilisateur* au sein de l'échelle EC étendue
  (Windows `High_Fan` n'excède pas 110 ; le firmware tolère 130).
- **Temps** : aucun changement entre les 7 états de la campagne → non éditables côté « OS » du
  firmware (DC non plus : zone grisée dans l'UI).
- **Écriture duty** : mode avancé = fan_mode msi-ec (EC 0xD4 = 0x8D) + duty points EC.
  En mode auto (0x0D à EC[0x33]), l'EC écrase EC[0x71] lui-même ; les écritures de courbe
  n'ont de sens qu'en mode avancé.
  **Précaution recommandée : toujours écrire un bloc complet en reprenant EC[0x71] de la lecture.**

## 5. Plan de patch noyau (livré : `wmi-fan-curve/`)

- `fan1_duty_curve` / `fan2_duty_curve` (sysfs, RW, root) : GET_FAN ipar1/2 → 7 valeurs →
  SET_FAN ipar1/2 (en conservant le duty live `byte[1]`) + vérification par relecture (readback).
- `fan1_live_duty` / `fan2_live_duty` (RO) : EC 0x71 / 0x89.
- `fan1_temp_curve` / `fan2_temp_curve` (RO) : blocs de température dé-shufflés
  (8 valeurs, adresses EC croissantes : enable, points de courbe, alarmes).
- **Param module `allow_curve_writes` (défaut false)** : aucune écriture sans opt-in explicite.
- Gate DMI `MS-1591` pour les attributs courbe (le reste du pilote inchangé).
- Toutes les cmd GET sont inchangées ; les écritures partagent le mutex `wmi_lock`.

## 6. Points laissés ouverts (aucun test EC nécessaires à ce stade)

1. ~~Confirmation finale contre la couche EC brute (page 0 directe)~~ → **RÉSOLU** :
   campagne du 2026-10-29 17:21 (`ec_sys` en lecture seule) — les 8 fenêtres WMAM
   correspondent **octet pour octet** au dump ECRAM page 0 au même instant :
   - `get_fan ipar0` ↔ EC[0xC8..0xCB] = `00 a7 00 b1` (2874/2697 tr/min = hwmon ✓)
   - `get_fan ipar1` ↔ EC[0x71..0x78] = `2d 2d 37 3c 46 5a 6e 82` ✓
   - `get_fan ipar2` ↔ EC[0x89..0x90] = `2d 2d 46 5a 64 6e 6e 6e` ✓
   - `get_temperature ipar0` ↔ EC[0x68]=0x20 (temp CPU 32 °C = sysfs msi-ec ✓)
   - `get_temperature ipar1/2` ↔ fenêtres dé-shufflées EC[0x69-0x70] / [0x81-0x88] ✓
     (l'escalier EC croissant `00 3c 46 50 55 5a 5f 64` visible directement dans le dump)
   - `get_thermal ipar0..3` ↔ EC[0x33-0x34], [0x79-0x7F], [0x91-0x97], [0x98-0x9E] ✓
   - Cohérences système : EC[0xD2]=0xC1/EC[0xD4]=0x0D (station msi-ec shift/fan),
     EC[0x9E] = 2ᵉ capteur de température live (0x20 idle → 0x30 load),
     EC[0xA0-0xBC] = chaîne version EC `1591EMS1.108 …`.
     Le duty live suit la charge (45 % @ 32 °C, 60 % @ 75 °C, 70 % @ 77-78 °C),
     coïncide avec les plages inter-points ; l'hystérésis interne de l'EC n'est
     pas à modéliser côté OS — seul l'écrit de la courbe stockée compte.
2. Sémantique SET_FAN en mode auto → **TESTÉ** (essai du 2026-10-29 17:36, `wmi-fan-curve/essai_20261029-173613.log`, consentement explicite) :
   - écriture d'un point (45 → 48, EC 0x72) via `SET_FAN` (0x12) IPAR1 : **acceptée par l'EC sans erreur**
     (readback immédiat = 48, aucune écriture/raw, aucune instabilité, ~1 min de surveillance) ;
   - l'octet duty live « préservé » (EC[0x71] écrit 45 dans la fenêtre) a été **recalculé par l'EC
     à partir de la courbe stockée** : live duty = 48 en 60 s de surveillance constante — l'EC
     réévalue automatiquement la duty depuis les points stockés, *même en mode auto* ;
   - les RPM externes n'ont pas bougé perceptiblement (+3 % duty, hors bruit du rotor) ;
   - restauration (45) relue ok, état EC identique.
   → Conclusions : (a) SET_FAN est la voie d'écriture valide et stable ; (b) le moteur EC
     applique la courbe stockée tout seul — le pilote/OUI n'a qu'à **écrire les 7 points** ; (c)
     la persistance avant reboot reste à vérifier (MControlCenter réappliquera de toute façon).

## 7. Fichiers de référence de cette étude

- Campagne : `wmi-evidence/20261029-165150/` (7 états, `wmi.txt` + `ecram.txt` + `state.txt`)
- Table WMAM DSDT : `gist/results/wmam-commands.txt` (§ ADDENDUM manuel)
- Code noyau mis en correspondance : `/tmp/opencode/msi-wmi-platform.c` (pilote 7.0.0 upstream)
- Code MControlCenter : `mcontrolcenter/src/operate.cpp` (adresses fan, patch b078181)
