# Windows session — fan curve protocol capture (results)

Machine: **MSI Summit E16 Flip A11UCT** (MS-1591, BIOS/EC `1591EMS1.108`)
Observed application: **One Dragon Center** (MSI Center Pro Store 2.1.42.0, DCv2.exe 2.0.129.1, SDK 3.2023.0324.02)
Date: 2026-10-29 · Method: pure observation (no WMI call, no EC write, no EC access tool)

## 1. The observables actually available

| Channel | Usefulness here | Verdict |
|---|---|---|
| `MSIAPService_Log.txt` (logged pipe) | records the registry operations (`REG:RRST/WRST`) + old `WMI2:GFAN…` payloads | curve dialogs do **not** go through it (no `WMI2:*` line during the trials) |
| ETW log `Microsoft-Windows-WMI-Activity/Operational` | WMI failures only (ev 5858) | useless to trace successful calls |
| **HKLM registry** ✓ | **the entire fan curve is persisted there** | already covered by the trials |
| UI logs (DCv2/DC_*/UI_*) | component lifecycle (`CurFeature=Performances Optimizer`) | no curve content |

## 2. THE CURVE MODEL (central result)

Key: `HKLM\Software\WOW6432Node\MSI\One Dragon Center\Component\Base Module\Scenario`

```
Default_Temp = "0;60;70;80;85;90;0;55;65;75;85;89"
Default_Fan  = "0;0;0;70;90;110;0;0;0;20;100;110"
User_Fan     = "0;0;0;70;90;110;0;0;0;20;100;110"
High_Fan     = "0;0;0;0;24;110;0;0;0;0;100;110"
```

Format of the 4 arrays: **6 values for fan1 (CPU) + 6 values for fan2 (GPU)**.
* `*_Fan[i]` = duty (MSI scale 0…110) of point i
* `Default_Temp[i]` = setpoint temperature of point i (first value `0` = placeholder, points 60-90 °C) — **not editable** in the UI
* UI-point ↔ index correlation (rule: point n → index n−1 of the fan block):
  * trial 2bis: fan1 point 4 → 50 % ⇒ `High_Fan[3]=50` ✓
  * trial 4: fan2 point 1 → 50 % ⇒ `High_Fan[6]=50` ✓

Key semantics:
* `High_Fan` = the working array **of the currently edited advanced curve** (the UI changes the index during drag/release, restores at redefault). `Default_Fan` = default. **Observed behaviour: instant point modifications go only into `High_Fan`** — `User_Fan`/`Default_Fan` never moved during the trials.
* Point change = 1 registry value modified per point (no full payload re-written).

## 3. SCENARIO SELECTOR (User Scenario)

Keys `...\Component\Base Module\{0..4}_Scenario`, each scenario:
`Performance` (1..3), `Fan` (0/1 flag of the active advanced curve), `KB` (keyboard flag), `PB` (integer 40..100)

Root key `...\Scenario`: `OC` = active scenario index (0..4), `PB` = PB of the active scenario (synced ~0.2-0.5 s after OC).

Trial 5 correlations (Auto→Silent→HighPerf→Auto):
* OC `1`→`3` (Silent: PB 0x32→0x64=100) → return: OC `1`, PB `0x32`
* On fan selector "Auto": `1_Scenario\Fan` 1→0 (it was 1 = advanced mode enabled at trial 1)
* `GeneralSetting\Fan=2` constant (hypothesis = number of fans)
* `Record\1a` / `Record\4a` = action counters (0x2a→0x2c / 0x10→0x12)

## 4. TRIAL TIMELINE (reg-watch excerpt)

```
16:03:14 High_Fan[0] 0→1   trial 1: fan1 pt1 modified (value = real % of the point)
16:03:49 High_Fan[0] 1→0   trial 1 restoration
16:07:44 High_Fan[3] 0→1   trial 2: fan1 pt4 0%→1%
16:08:10 High_Fan[3] 1→0   restoration
16:11:14 High_Fan[3] =50   trial 2bis: fan1 pt4 0→50 %     ← proof of the raw-% mapping
16:12:01 High_Fan[6] =50   trial 4: fan2 pt1 0→50 %       ← GPU shares the same array idx 6-11
16:12:52 High_Fan reset    restorations
16:14:00 OC 1→3, PB 0x32→0x64   trial 5: Auto→Silent (OC first, PB ~0.5 s later)
16:18:35 OC 3→1, PB 0x64→0x32   back to HighPerf (OC=1, PB=0x32) then Auto
16:18:47 1_Scenario\Fan 1→0    fan selector Auto → flag cleared
16:18:47-48 REG:WRST:1 ×4 pipe → matches Record\4a 0x10→0x12
```

Trial 3 (temperature) impossible: temp points are not editable.

## 5. Hypotheses to validate Linux-side (msi-ec)

1. The number of levels (6 / fan) + duty 0-110 must match the known EC registers
   (`0x71-0x78` fan1, `0x89-0x90` fan2 of investigation-report.md §5): verify that `110` = the "max boost"
   value accepted by the EC (or else a 0-100 scale with 110 marking 100 % boosted).
2. `Default_Temp` (60-90 °C fan1, 55-89 °C fan2) vs the EC thresholds `0x79-0x7F` / `0x91-0x9E`.
3. The persistence of `User_Fan` (vs `Default_Fan`): not observed — probably triggered by an
   explicit profile save in the UI, which we did not perform.
4. The actual EC write (Windows side) does not go through the logged MSIAPService pipe:
   an in-process DCv2 path (WMILib/MSIWMIACPI2 heritage → `MSI_ACPI` class root\WMI). To be
   reconstructed via the already-done disassembly (see the disasm artifacts) and, Linux-side,
   to compare the EC dumps before/after an editing-point change.

## 6. Files produced Windows-side

* `logs-trials/reg-watch.txt` — full registry watcher (all timestamped changes, basis of the trials)
* `logs-trials/live-capture.txt` — tail of the MSIAPService log during the session
* `logs-baseline/` — reference copies before the trials
* `msicenter-config/` — user.config, DuetDefaultProfile.xml, Define_Base*.dat, Define_Data.NB,
  Device*.dat, OC_Data.dat + 4 HKLM/HKCU registry exports (before / after / final)
* `wdg-classes.txt` — _WDG decoding of the 3 WMI-ACPI providers (WFDE/WFTE/SCM0), complementary to investigation-report.md
