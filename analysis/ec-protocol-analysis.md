# EC protocol analysis — MSI Summit E16 Flip A11UCT (MS-1591, EC 1591EMS1.108)

Date: 2026-10-26
Status: static analysis only (no EC write tested at this point)
Sources: Windows artifacts (`windows/disasm/`), MControlCenter code, `msi-ec` code.

---

## 1. Summary of conclusions

| Question | Answer |
|---|---|
| Why the hard resets? | At application startup, MControlCenter restores `fanModeAdvanced=true` and **rewrites the fan curves with raw EC writes** (`ec_sys`) to the generic addresses 0x72–0x78, 0x8A–0x90, 0x6A, 0x82, then forces 0xD4=0x8D. On this model, these raw writes, outside any validated sequence, trigger a platform hard reset. |
| How does Windows drive the fans? | **Two distinct paths**: (1) `FanTool64.exe` — a *hardware diagnosis* tool only; (2) `MSIAPService.exe` — the real day-to-day engine, through **WMI-ACPI** (`root\WMI`, class `MSI_AP`), so the work is done **by the BIOS/EC itself**, never by a raw write from the OS. |
| Is `0x33` a curve address? | **No (probably not).** In FanTool, `0x33` is the *index* of a vendor EC command (`0x90` read / `0x91` write) sent through the 0x66/0x62 ports — a command protocol, not the direct EC RAM space that `msi-ec`/`ec_sys` read (standard commands 0x80/0x81). Bit 0 of that index enables the manual fan mode. |
| Is advanced fan mode with a custom curve possible under Linux? | Yes, eventually, but first the addresses/format must be confirmed (Windows differential dumps + MSIAPService logs), then it must be added kernel-side. MControlCenter CANNOT do it safely today. |

---

## 2. Path A — FanTool64.exe (command protocol through ports 0x66/0x62)

### 2.1 Evidence that FanTool64 is a diagnostic tool, not the curve engine

In `windows/disasm/dotnet-il-trace.txt`, the OneDragonCenter DLL `API_HW Diagnosis.dll`:

```
[Fan::Fan_Test]  rva=0x33cc
    ... Process::GetProcessesByName ...
    ... "HW Diagnosis\Fan\FanTool64.exe"
    ProcessStartInfo::set_FileName
    ProcessStartInfo::set_Arguments  -> "/s"
    Process::Start
```

`FanTool64.exe` is launched only with the `/s` argument by the **HW Diagnosis** module of MSI Center (fan testing, producing `FanRPM_Fan1.txt` / `fanRPM_Fan2.txt` / `feedback.txt`). Nothing (so far) shows it being called when a curve is edited in the MSI Center UI. This matches the `FT_ReadByteDelegate`, `FT_GetStatusDelegate` delegates: it is a self-contained fan specification/diagnostic binary.

### 2.2 The exact routine (annotated, from `windows/disasm/disasm-fantool2-ports.txt`)

All of the code runs in user space (`FanTool64.exe`); the `KernCoreLib64.Sys` / `NTIOLib.sys` drivers are mere `GetPortVal`/`SetPortVal` conduits (IOCTLs `0x221c0c`/`0x221c10`), without any EC logic. The `0x66`/`0x62` port constant is initialized at load time (`.text 0x4354`).

Sequencencing at `.text 0x3772–0x3975` — **read then write of index byte `0x33`, with bit 0 set to 1**:

1. **Wait**: read status port `0x66` until bit 1 (IBF, input buffer full) is 0 — max 10 attempts spaced with `Sleep(100 ms)` (`mov ecx, 0x64` → 100 ms; `cmp edi, 0xa` → 10 tries).
2. **Read command**: write `0x90` to port `0x66` (vendor EC command byte).
3. **Index**: wait for IBF=0 again, then write `0x33` to port `0x62`.
4. **Wait** OBF (bit 0 of `0x66` = 1), then **read the data** from `0x62` → `esi`.
5. `or sil, 1`: force bit 0 of the read value to 1 (enabling the manual/advanced fan mode EC-side).
6. **Write command**: write `0x91` to `0x66`.
7. **Index**: write `0x33` to `0x62`.
8. **Data**: write the modified value (`sil`) to `0x62`.

### 2.3 Interpretation

- `0x90`/`0x91` are **not** the standard ACPI EC RAM read/write commands (`0x80`/`0x81`), the ones `ec_sys` and `msi-ec` use. They are **proprietary EC commands**: an index (`0x33`) is passed first, then data flows through port 0x62.
- Important consequence: we CANNOT conclude that "writing address 0x33 via `ec_write`" is equivalent to what FanTool does. Bit 0 of index 0x33 of the command protocol may target a protected/banked register different from the direct 0x00–0xFF space.
- Possible read-only disambiguation check under Windows: compare, at the same instant, the result of a direct read of byte 0x33 (standard command `0x80` via an EC tool) with the value read by FanTool (index 0x33 through command 0x90). Different values → different spaces; identical values → same space.

---

## 3. Path B — MSIAPService.exe / MSIWMIACPI2.dll (the real day-to-day path)

From `windows/disasm/dotnet-il-trace.txt`:

```
[NativeNamedPipeServer::Get_Fan]  rva=0x6458
    ldc.i4.s 14 (0xe)              <- WMI data block index
    callvirt WmiAcpiLib2::InvokeWmiMethod

[NativeNamedPipeServer::Set_Fan]  rva=0x6c90
    ldc.i4 7                       <- Set_WMIACPI2(dataBlockIndex = 7)
    call local:Set_WMIACPI2

[NativeNamedPipeServer::Set_WMIACPI2]  rva=0x6d38
    ... InvokeWmiMethod with indices 3, 5, 7, 9, 0xB, 0xD, 0xF ... (switch on the requested index)
```

Key points:

- Transport: **named pipe** (`\\.\pipe\simple`) + text protocol (`WMI2:SFAN_RST:`…) — everything the GUI does is transmit *parameters* to the service; it is the service that queries WMI.
- Target: `root\WMI`, classes **`MSI_AP`** and **`MSI_Event`** ("WMI 2", `WmiMajorVersion >= 2`, library `MSIWMIACPI2.dll`).
- Data block index for `Get_Fan` = **0xE (14)**; for `Set_Fan` = **7**; `Get_EC` handled separately in `ExecuteWmiMethod`.
- **Everything is executed by a BIOS ACPI method** (`InvokeWmiMethod` → `_WQ`/`_WS` ACPI in the DSDT). The operating system never touches the 0x62/0x66 ports itself.

### Crucial consequence

Fine-grained fan control (curves included) under Windows is probably fully encoded **in the EC/BIOS firmware** through these WMI-ACPI methods. The "addresses" we look for on Linux may not be simple bytes of the direct EC RAM: they are the parameters of an ACPI method → an EC command validated by the firmware. This is exactly what MControlCenter bypassed by writing "blindly" into the direct EC RAM.

### Very useful lead for the next steps: the MSIAPService logs

`ExecuteWmiMethod` logs **every exchange**:

```
"Return value of "Get_Fan(...)" = ..., 0x{payload in hex}"   (read)
"Set ..."                                                     (write)
```

Under Windows, it is enough to:

1. enable/collect the `MSIAPService` logs (log file / Event Viewer, see `writeLog`);
2. start MSI Center;
3. modify **one** curve point (e.g. 30 % → 31 %);
4. look at the log: the exact payload of `Set_Fan` (data block 7) is written in hexadecimal.

This is **passive collection**: no extra EC tool required, and the true payload semantics is revealed without firmware reverse engineering.

---

## 4. Cross-check with msi-ec and MControlCenter

### 4.1 msi-ec v0.12 — `CONF_G2_1` (matches 1591EMS1.108, ~line 1182)

```
charge_control       : 0xD7
webcam               : 0x2E / block 0x2F, bit 1
fn_win_swap          : 0xE8, bit 4 (inverted)
cooler_boost         : 0x98, bit 7
shift_mode (0xD2)    : turbo 0xC4, eco 0xC2, comfort 0xC1
super_battery        : 0xEB, mask 0x0F
fan_mode   (0xD4)    : auto 0x0D, silent 0x1D, advanced 0x8D
cpu  : rt_temp 0x68, rt_fan 0x71
gpu  : rt_temp 0x80, rt_fan 0x89
leds : micmute 0x2C, mute 0x2D, bit 1
kbd_bl : mode 0x2C, state 0xD3
```

There is **no fan curve address** in this configuration. That is consistent: on this model, `msi-ec` handles the modes correctly, but curve editing requires the mechanisms described above (command protocol / WMI-ACPI), which are **not modelled in msi-ec so far**.

### 4.2 MControlCenter — the crashing flow

In `src/operate.cpp`:

```
const int fan1SpeedSettingStartAddress = 0x72;      // line 59
const int fan2SpeedSettingStartAddress = 0x8A;      // line 60
const int fan1TempSettingStartAddress  = 0x6A;      // line 62
const int fan2TempSettingStartAddress  = 0x82;      // line 63
const int fanModeAdvanced = 0x8D;                   // line 72
```

`Operate::loadSettings()` (lines 522–552) rewrites **at every application startup**:

1. user mode → `msi-ec` sysfs (correct);
2. curves: `setFan1SpeedSettings` → raw writes 0x72…, `setFan2SpeedSettings` → 0x8A…, same for temperatures 0x6A/0x82;
3. `setFanModeAdvanced(true)` → raw write `0xD4 = 0x8D`.

All writes go through `helper.putValue` → `/sys/kernel/debug/ec/ec0/io` (raw access, `ec_sys` loaded with `write_support=1` by the D-Bus helper).

### 4.3 Three hypotheses for the hard reset (ordered by likelihood)

1. **Concurrency / broken sequencing**: raw writes through `ec_sys` bypass any arbitration; they interleave with the reads/writes of `msi-ec` and with the periodic BIOS activity on the same EC. A bad interleaving in the middle of an EC transaction may put the EC in an invalid state → fail-safe/hardware fuse → hard reset (no kernel trace, consistent with our logs).
2. **Generic curve addresses invalid for this model**: 0x72–0x78 / 0x8A–0x90 come from other MSI models; here they may target something else (our data partially overlaps with `msi-ec`: CPU temp 0x68 ✓, GPU temp 0x80 ✓, CPU fan 0x71 ✓, GPU fan 0x89 ✓ — but the curve blocks/sequencencing are not validated).
3. **Advanced mode engaged without a consistent curve**: forcing `0xD4=0x8D` while writing curve values in the format/order of another model may drive the EC into an invalid state (aberrant PWM/setpoint).

In all three cases, the identified trigger is the same: raw writes with unvalidated addresses.

---

## 5. Action plan

### Step 1 — Put the environment back to safety (no EC)

In `~/.config/MControlCenter.conf`:
- `fanModeAdvanced=false`;
- remove/empty the `fan1SpeedSettings`, `fan2SpeedSettings`, `fan1TempSettings`, `fan2TempSettings` keys.

Do it **with the application closed** (MControlCenter rewrites the file on exit).

### Step 2 — Safe MControlCenter fix (Linux side)

1. `Operate::loadSettings()` / `handleWakeEvent()`: **never** restore curves nor advanced mode by raw write while `msiEcHelper.hasFanMode()` returns true — in that case use the `msi-ec` sysfs interface (`setFanMode`) or skip the step.
2. Change the "advanced mode supported" condition to a **firmware whitelist** (as msi-ec does), not a default value.
3. On Linux, do not load `ec_sys` with `write_support=1` when `msi-ec` already exposes every sysfs file the application needs.
4. `Operate::setValue`: do not allow addresses outside a whitelist (0xD2, 0xD4, …).

Patches we can prepare without risking the EC, then propose upstream (issues/PR of the projects).

### Step 3 — Advanced fine curves (conditioned on validation)

1. **Under Windows (passive)**: collect the `MSIAPService` logs while driving the MSI Center UI → exact payload of `Set_Fan` (data block 7) and `Get_Fan` (0xE).
2. **Under Windows (differential)**: direct EC dump before/after a one-slider change (procedure in `windows/ec-investigation.md`) + the 0x33 disambiguation test (§2.3).
3. **Under Linux (implementation)**: depending on the confirmation:
   - if the curves live in direct EC RAM → propose a basic kernel patch in `msi-ec` (sysfs `fan_curve`);
   - if they go through the command protocol or WMIACPI → kernel implementation of a command channel, heavier, to be done first `msi-ec`-side (issues + discussion).

Until step 3.1/3.2 is done, **no EC curve write must be attempted under Linux**.

---

## 6. Annex — answers to the specific questions

| Question | Status |
|---|---|
| What caused the resets? | raw writes at MControlCenter startup (fanModeAdvanced=true), outside any validated sequence |
| Windows advanced mode vs Linux? | Windows goes through a WMI ACPI method (`MSI_AP`, Set_Fan=7) + the command EC protocol in FanTool; Linux today only has `0xD4=0x8D`, without any curve mechanism |
| `0x33`? | index of the FanTool command protocol (commands 0x90/0x91), NOT an EC RAM address to write via `ec_write` |
| EC firmware newer than 1591EMS1.108? | not identified; BIOS E1591IMS.310 (2023-07) appears to be the last release for MS-1591 |
| Quick fix? | yes: configuration (`fanModeAdvanced=false`) |
| Proper fix? | MControlCenter patch (raw-write restrictions) + msi-ec iteration after the Windows dumps |
