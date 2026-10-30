# WMAM (SCM0) ↔ EC RAM mapping — MSI Summit E16 Flip A11UCT (MS-1591)

Date: 2026-10-29 · Source: campaigns `analysis/wmi-evidence/20261029-165150/` and the **ECRAM cross-check
campaign `analysis/wmi-evidence/20261029-172118/` (with `ec_sys` in read-only mode)**
Status: **1:1 MAPPING VALIDATED — WMAM ↔ EC RAM confirmed byte-exact**, with no EC write at
any point of the study.

## 0. Mechanics

- ACPI method `WMAM` (`\_SB.PC00.LPCB.EC.SCM0`), WMI GUID
  `ABBC0F6E-8EA1-11D1-00A0-C90629100000`, object `AM`, 1 instance. Index/data window on
  I/O `0x360/0x361` (the "SCM indices" of the DSDT).
- WMI call: 32-byte buffer. `buffer[0]` = IPAR (sub-group index), `buffer[1..31]` = BS01..BS31.
  Response: `output[0]` = status (must be ≠ 0), `output[1..31]` = data.
- Pairing: read = odd command, write = odd+1.
- The kernel driver `msi_wmi_platform` (already in-tree) exposes one debugfs file per
  command: writing 32 bytes into it invokes the command; reading it back returns the
  response buffer.
  → `get_fan` = cmd `0x11`, `set_fan` = `0x12`, `get_temperature` = `0x0D`,
  `set_temperature` = `0x0E`, `get_thermal` = `0x0F`, `set_thermal` = `0x10`
  (file names are the kernel's; the command number is the source of truth).

## 1. Final table: cmd / IPAR → EC addresses

⚠ Some windows use NON-sequential orders (validated by live data): the order given is the
`output[1..N]` ↔ EC indices replacement order.

| kernel cmd | hex | IPAR | EC indices (window order) | Observed live content |
|---|---|---|---|---|
| get_ec | 0x03 | 0 | (special page) | `1591EMS1.108 ...` — EC RAM version. Verified: page switching (flag bit 6) |
| get_temperature | 0x0D | 0 | 0x68, 0x80, 0x9F | EC[0x68] = **live CPU temp** (0x23=35 °C idle, 0x4D=77 °C load) ✓ |
| get_temperature | 0x0D | 1 | 0x69, 0x6F, 0x70, 0x6A-0x6E | see §2 fan1 |
| get_temperature | 0x0D | 2 | 0x81, 0x87, 0x88, 0x82-0x86 | see §3 fan2 |
| get_thermal | 0x0F | 0 | 0x33, 0x34 | EC[0x33][0x34] = mode: `0D 01` comfort / `05 00` eco ✓ (consistent with shift_mode) |
| get_thermal | 0x0F | 1 | 0x79-0x7F | fan1 configuration (static): `00 0a 08 08 03 03 03` |
| get_thermal | 0x0F | 2 | 0x91-0x97 | fan2 configuration (static): `00 05 03 03 08 07 03` |
| get_thermal | 0x0F | 3 | 0x98-0x9E | EC[0x98]=0x02 (cooler boost off — consistent with MControlCenter coolerBoostAddress=0x98), 0x9A/0x9D=0x7d, EC[0x9E] = **live temp #2** (0x23→0x32 under load) |
| get_fan | 0x11 | 0 | 0xC8-0xCF | **RPM**: fan1 = BE16(EC[0xC8..0xC9]) (162→2963 rpm ✓), fan2 = BE16(EC[0xCA..0xCB]) (177→2712 ✓). Driver formula: `480000/rpm_code` |
| get_fan | 0x11 | 1 | 0x71-0x78 (sequential) | fan1: EC[0x71] = **live duty** (0x2D→0x46 under load ✓), EC[0x72-0x78] = **7 duty points** (static) |
| get_fan | 0x11 | 2 | 0x89-0x90 (sequential) | fan2: EC[0x89] = **live duty**, EC[0x8A-0x90] = **7 duty points** (static) |

Mirror write commands (next odd+1 command, SAME window, never invoked during the study):
- `set_fan` (0x12) IPAR 1: writes the same `0x71-0x78` (in `buffer[1..8]`).
- `set_fan` (0x12) IPAR 2: writes `0x89-0x90`.
- `set_thermal` (0x10) IPAR 0: writes {0x33,0x34} (mode); IPAR 1/2/3: 0x79-0x7F / 0x91-0x97 / 0x98-0x9E.
- `set_temperature` (0x0E) IPAR 1/2: writes the temperature windows (⚠ non-sequential order — see §2).

## 2. Fan1 (CPU) block — EC layout cross-checked with One Dragon Center registers

### Live duty points stairway (invariant over the whole campaign):

| EC addr | 0x71 | 0x72 | 0x73 | 0x74 | 0x75 | 0x76 | 0x77 | 0x78 |
|---|---|---|---|---|---|---|---|---|
| value | live duty | 45 | 55 | 60 | 70 | 90 | 110 | 130 |
| evidence | 0x2D→0x46 under load | static | static | static | static | static | static | static |

### Fan1 temperature points (via get_temperature IPAR1, window order de-shuffled):

| EC addr | 0x69 | 0x6A | 0x6B | 0x6C | 0x6D | 0x6E | 0x6F | 0x70 |
|---|---|---|---|---|---|---|---|---|
| value | 0 (enable) | 60 | 70 | 80 | 85 | 90 | 95 | 100 |
| DC Default_Temp fan1 | — | idx1 | idx2 | idx3 | idx4 | idx5 | (alarm) | (alarm) |

Consistency with MControlCenter (`operate.cpp`, the generic MSI curve model):
`fan1TempSettingStartAddress = 0x6A, count 6`; `fan1SpeedSettingStartAddress = 0x72, count 7`;
`fan1Address = 0xC9` (RPM modulo the BE16: 0xC8-0xC9 family). **The MControlCenter code encodes
exactly what the campaign observed.**

## 3. Fan2 (GPU) block — same idea

| EC addr | 0x89 | 0x8A | 0x8B | 0x8C | 0x8D | 0x8E | 0x8F | 0x90 |
|---|---|---|---|---|---|---|---|---|
| value | live duty | 45 | 70 | 90 | 100 | 110 | 110 | 110 |

| EC addr | 0x81 | 0x82 | 0x83 | 0x84 | 0x85 | 0x86 | 0x87 | 0x88 |
|---|---|---|---|---|---|---|---|---|
| value | 0 (enable) | 55 | 65 | 75 | 85 | 89 | 90 | 89 |
| DC Default_Temp fan2 | — | idx1 | idx2 | idx3 | idx4 | idx5 | (alarm) | (alarm) |

## 4. Conversions / semantics

- **RPM**: `480000 / BE16(code)`. code 0 = stopped fan.
- **Duty**: raw EC scale, observed ceiling 130 (= the default of the last point). The Windows
  UI 0–110 is a *user* ceiling within the extended EC scale (Windows `High_Fan` never exceeds
  110; the firmware accepts 130).
- **Temperatures**: no change across the 7 campaign states → not OS-editable on this firmware
  (DC greys them out too).
- **Duty writes**: advanced mode = msi-ec fan_mode (EC 0xD4 = 0x8D) + EC duty points.
  In auto mode (0x0D at EC[0x33]) the EC recomputes the live duty itself, but **the EC keeps
  using the stored curve whatever the fan mode is** — custom curves were verified to apply
  in auto mode too. Writing the firmware defaults back is the only real way to deactivate
  an OS-written curve.
  **Recommended precaution**: always write a whole 8-byte window, re-taking EC[0x71] from a
  fresh read.

## 5. Kernel patch plan (shipped: `wmi-fan-curve/`)

- `fan1_duty_curve` / `fan2_duty_curve` (sysfs, RW, root): GET_FAN ipar1/2 → 7 values →
  SET_FAN ipar1/2 (preserving the live-duty `buffer[1]` byte) + readback verification.
- `fan1_live_duty` / `fan2_live_duty` (RO): EC 0x71 / 0x89.
- `fan1_temp_curve` / `fan2_temp_curve` (RO): the de-shuffled temperature windows
  (8 values, ascending EC addresses: enable, curve points, alarms).
- **`allow_curve_writes` module parameter (default false)**: no write without an explicit opt-in.
- DMI gate `MS-1591` for the curve attributes (the rest of the driver unchanged).
- All GET commands unchanged; writes share the `wmi_lock` mutex.

## 6. Opened questions — status

1. ~~Final cross-check against the raw EC layer (direct page 0)~~ → **RESOLVED**: the
   2026-10-29 17:21 campaign (`ec_sys` read-only) showed the 8 WMAM windows matching
   the ECRAM page-0 dump **byte for byte** at the same instant:
   - `get_fan ipar0` ↔ EC[0xC8..0xCB] = `00 a7 00 b1` (2874/2697 rpm = hwmon ✓)
   - `get_fan ipar1` ↔ EC[0x71..0x78] = `2d 2d 37 3c 46 5a 6e 82` ✓
   - `get_fan ipar2` ↔ EC[0x89..0x90] = `2d 2d 46 5a 64 6e 6e 6e` ✓
   - `get_temperature ipar0` ↔ EC[0x68]=0x20 (CPU temp 32 °C = sysfs msi-ec ✓)
   - `get_temperature ipar1/2` ↔ de-shuffled windows EC[0x69-0x70] / [0x81-0x88] ✓
     (the EC-ascending stairway `00 3c 46 50 55 5a 5f 64` is directly visible in the dump)
   - `get_thermal ipar0..3` ↔ EC[0x33-0x34], [0x79-0x7F], [0x91-0x97], [0x98-0x9E] ✓
   - System coherence points: EC[0xD2]=0xC1/EC[0xD4]=0x0D (the msi-ec-side shift/fan station),
     EC[0x9E] = second live temperature sensor (0x20 idle → 0x30 load),
     EC[0xA0-0xBC] = the EC version string `1591EMS1.108 …`.
     The live duty follows the load (45 % at 32 °C, 60 % at 75 °C, 70 % at 77–78 °C);
     the EC's internal hysteresis is not an OS concern — only the stored-curve write matters.
2. SET_FAN semantics in auto mode → **TESTED** (2026-10-29 17:36 trial,
   `analysis/set-fan-trial-20261029-173613.log`, explicit consent):
   - writing one point (45 → 48, EC 0x72) via `SET_FAN` (0x12) IPAR1: **accepted by the EC
     without error** (immediate readback = 48, no raw write, no instability, ~1 min of
     monitoring);
   - the "preserved" live-duty byte (EC[0x71] written as 45 in the window) was
     **recomputed by the EC from the stored curve**: live duty = 48 over the whole
     60 s of monitoring — the EC automatically re-evaluates the duty from the stored
     points, *even in auto mode*;
   - the external RPMs did not move perceptibly (+3 % duty, within rotor noise);
   - restoration (45) read back fine, EC state identical.
   → Conclusions: (a) SET_FAN is the valid and stable write path; (b) the EC firmware
     applies the stored curve by itself — the OS only needs to write the 7 points;
     (c) pre-reboot persistence still unverified (MControlCenter re-applies anyway).

## 7. Reference files of this study

- Campaigns: `analysis/wmi-evidence/20261029-165150/` and `analysis/wmi-evidence/20261029-172118/`
  (7 states each: `wmi.txt` + `ecram.txt` + `state.txt`)
- DSDT WMAM table: `analysis/windows/dsdt-wmam-commands.txt` (manual ADDENDUM section)
- Cross-referenced kernel code: the in-tree `msi-wmi-platform` driver (7.0)
- MControlCenter code: `src/operate.cpp` (fan addresses, forks commits `b078181..`)
- Live set_fan trial: `analysis/set-fan-trial-20261029-173613.log`
