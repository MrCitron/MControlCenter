# Windows EC Investigation Report — Fan Control on MSI Summit E16 Flip A11UCT

**Machine**: MSI Summit E16 Flip A11UCT (MS-1591), BIOS `E1591IMS.310`, EC firmware `1591EMS1.108`, Windows 11 Pro (build 26300).
**Scope**: strictly read-only investigation. No EC writes, no Cooler Boost, no BIOS/EC flashing, no aggressive fan tests were performed. Every artifact in `ec-investigation/` was produced by static analysis (file copy, hashes, strings, disassembly, ACPI table dump/decompilation, IL tracing) plus reading of service log files.
**Purpose**: provide the Windows-side map needed to resume the fan-curve repair (broken by One Dragon Center) on the Linux session, using msi-ec / m-control-center.

---

## 1. Executive summary

Three independent paths exist from Windows software to the Embedded Controller:

| # | Path | Implementation | Used by | Notes |
|---|------|----------------|---------|-------|
| A | **ACPI/WMI "SCM0"** | ASL method `WMAM` on device `\_SB.PC00.LPCB.EC.SCM0` (PNP0C14); reads/writes an index/data window at **I/O 0x360/0x361** | `MSIAPService.exe` via `MSIWMIACPI2.dll` (WmiAcpiLib2) | The official "System Control Manager" interface; full command table decoded (§3) |
| B | **Direct EC port I/O via WinIO** | Driver `KernCoreLib64.Sys` (= open-source **WinIO**, PDB `WinIo_KernCoreLib_2017`), user-mode `KernCoreLib64.dll` exports port access; IOCTLs `0x221C0C` (read port) / `0x221C10` (write port) | `FanTool64.exe` (HW Diagnosis fan tool), `MSIAPService.exe` (`loadWinIO`, `WinIO.ReadMemory`) | EC RAM via ports: 0x66 = command (`0x80`/`0x81` status-check, `0x90` read, `0x91` write), 0x62 = data (§4) |
| C | **EC SMBus master** | EC fields `SMPR/SMST/SMAD/SMCM/SMD0/SMAA` at EC RAM 0x00–0x06 (DSDT line 70515 ff.) + WMAM commands 0x07/0x08 (window SCM 0x02–0x26) | `MSIWMIACPI2.dll` (`Get_SMBUS`/`Set_SMBUS`) | For onboard sensor chips (F718XX/IT87XX/NCT677X code exists in `Sendevsvc.exe`) |

Fan curve state on this machine lives in EC RAM; the readable/writable **fan-relevant EC addresses** are 0x33/0x34, 0x68–0x70, 0x79–0x7F, 0x81–0x90, 0x9F, 0xC8–0xCF (§5). The .NET service names the 6 curve points `FanCPUT1Percentage`…`FanCPUT6Percentage` and commands are transported over a **named pipe** to the service as ASCII `WMI2:GFAN_REQ:…` / `WMI2:SFAN_REQ:…` (§6).

---

## 2. Inventory of installed MSI software (supported paths)

- `C:\Program Files (x86)\MSI\MSI NBFoundation Service\` — running services
  - `MSIAPService.exe` (.NET, x64, "MSI Foundation Service") — WMI2 named-pipe server, `FanTemperature1–6`, `FanCPUT1–6Percentage`, `CoolerBoost`, `Get_Fan`/`Set_Fan`, WinIO loader.
  - `Sendevsvc.exe` (.NET, x86, "Sensor dev service") — OLS IOCTLs, sensor chip drivers F718XX/IT87XX/NCT677X, NVIDIA/NvApi (`SetCoolerLevels`, `GetCoolerSettings`, `SaveDefaultFanSpeed`/`RestoreDefaultFanSpeed`).
  - `MSIWMIACPI2.dll` — "MSI WMI ACPI Library" (WmiAcpiLib2), method `InvokeWmiMethod` wrapper over `ManagementClass.InvokeMethod`.
  - `KernCoreLib64.Sys/.dll` — WinIO redirector (also present under `One Dragon Center\HW Diagnosis\Fan\`).
- `C:\Program Files (x86)\MSI\One Dragon Center\` (GUI, not started as service) — successor of MSI Center on this machine; contains `HW Diagnosis\Fan\FanTool64.exe`, `System Monitor\API_NB_System Monitor.dll` (`FAN_PERCENTAGE`, `FAN_RPM`, `GPU_COOLER…`), `User Scenario\API_NB_User Scenario.dll`, `Data\FanRPMLimit…` support files, and `Data\DuetDefaultProfile.xml`.
- Kernel drivers: `KernCoreLib64.Sys` (WinIO), `NTIOLib.sys`/`NTIOLib_X64.sys` (MSI NTIOLib, also has `HalGet/SetBusDataByOffset`), `msisadrv.sys` (ACPI saddle driver, no direct I/O).
- ⚠️ `Fan.txt` in NBFoundation lists model→fan-type mappings; **"Summit E16Flip" is NOT in the list** (it keys on names like `Summit E16Flip A11UCT`… none present). This is likely part of why One Dragon Center misconfigured fan behavior.

Full inventory + SHA-256: `ec-investigation/msi-files.txt`, `msi-hashes.csv`. Binary copies for analysis: `ec-investigation/binaries/`.

---

## 3. Path A — WMI/SCM0 decoded (the main control plane)

### 3.1 Device

`\_SB.PC00.LPCB.EC.SCM0`, `_HID PNP0C14`, `TDVR=0x14`, `MAJR=0x02`, `MINR=0x08` (WMI 2.8).
`_WDG` (DSDT line 71905):

| GUID | Object | Kind |
|------|--------|------|
| `{ABBC0F6A-8EA1-11D1-00A0-C90629100000}` | AK | data |
| `{ABBC0F6B-8EA1-11D1-00A0-C90629100000}` | AL | data |
| `{ABBC0F6D-8EA1-11D1-00A0-C90629100000}` | AJ | method |
| `{ABBC0F6E-8EA1-11D1-00A0-C90629100000}` | AM | method |
| `{5B3CC38A-40D9-7245-8AE6-1145B751BE3F}` | C0 | MOF |

Method `WMAM(3)` (line 72130) = the dispatcher ("WM"+"AM" per ACPI convention; the `AJ↔WMAJ` counterpart is absent in the decompiled DSDT — exact GUID→method binding should be confirmed once on Linux with `wmidump`/`barydump`, non-destructive).

### 3.2 Transport mechanics

```
OperationRegion (WMIO, SystemIO, 0x0360, 0x02)
Method WMRD(i)  { I360 = i ; return D361 }   // out 0x360=index, in 0x361=data
Method WMWT(i,v){ I360 = i ; D361  = v }     // out 0x360=index, out 0x361=value
```

`WMAM(Arg0, Arg1, Arg2)` where `Arg1` = command, `Arg2` is a 32-byte-ish buffer whose byte 0 is `IPAR` (sub-index) and bytes 1.. are data. Every read (odd command) has a write twin (successor even command) addressing **the same SCM windows**. The SCM window indices line up with conventional EC RAM addresses (battery region 0xE0–0xFF, SMBus fields 0x00–0x26 confirmed by the parallel `Field (EC, EmbeddedControl)` declarations).

### 3.3 Verified command table

| Read | Write | Data groups (SCM/EC addresses) | Function |
|------|-------|-------------------------------|----------|
| 0x03 | 0x04 (=ack no-op) | 0x37 + 0xA0–0xBB | BIOS/EC identifiers |
| 0x05 | 0x06 | 0xE0–0xFF (battery 1/2 blocks) | battery info (GMBAT/SMBAT) |
| 0x07 | 0x08 | 0x02–0x26 (and unspecified sub-group) | EC SMBus master window |
| 0x09 | 0x0A | {0x31, 0xBC}, 0x38–0x3F, 0x40–0x47, 0x48–0x4F | temperatures / thresholds |
| 0x0B | 0x0C | {0x32, 0xBD}, 0x50–0x57, 0x58–0x5F, 0x60–0x67 | (therm group 2) |
| **0x0D** | **0x0E** | **{0x68, 0x80, 0x9F}, 0x69/0x6F/0x70 + 0x6A–0x6E, 0x81/0x87/0x88 + 0x82–0x86** | **fans (see §5)** |
| 0x0F | 0x10 | **{0x33, 0x34}**, 0x79–0x7F, 0x91–0x97, 0x98–0x9E | fan/mode block incl. Cooler-Boost-ish byte 0x33 |
| 0x11 | 0x12 | 0xC8–0xCF, **0x71–0x78 (fan1/CPU)**, **0x89–0x90 (fan2/GPU)** | fan duty/RPM blocks |
| 0x13 | 0x14 | {0x28,0x29}, {0x2E,0x2F,0x2C,0x2D,0x2A,0x2B} | misc |
| 0x15 | 0x16 | ~{0x30, 0xBF, …} | misc |
| 0x17 | 0x18 | 0xC0–0xC7 + {0x35, 0x36} | misc |
| 0x19 | 0x1A | {0xD0–0xDB}, {0xD4–0xD6}, {0xD8–0xDF} | battery/power external |

### 3.4 .NET ↔ WMAM binding (from IL trace + constants)

`WmiAcpiLib2::InvokeWmiMethod(A, B, N)` — observed reads pass the **even** value:

- `Get_SMBus` → `InvokeWmiMethod(2, 0, 4)` ⇒ WMAM cmd **0x03** (read)
- `Get_Temperature` → `(2, 0, 10)` ⇒ **0x09** (read)
- `Get_Fan` → `(2, 0, 14)` ⇒ **0x0D** (read fan)

So reads use `N = 2·i` (data block index `i`), WMAM command `2i−1`; writes (`Set_WMIACPI2(iDataBlockIndex)` with `Set_SMBus=2`, `Set_Temperature=5`, `Set_Fan=7`) dispatch to WMAM command `2i`. Both directions confirmed consistent: `i=7` → read 0x0D / write 0x0E; `i=5` → 0x09/0x0A; `i=2` → 0x03/0x04.
Inside `Set_WMIACPI2` the IL chain shows `InvokeWmiMethod` calls carrying the odd constants 3,5,7,9,11,13,15,17,19,21,23,25 (the read twins).

`InvokeWmiMethod` → `ExecuteWmiMethod` → `System.Management` (`ManagementBaseObject["Data"]`, `"Bytes"`) — hence the log strings `Data`, `Bytes`, and results `results.Flag`/`results.Data`.

---

## 4. Path B — FanTool64.exe direct EC access (disassembly facts, read-only evidence)

From `ec-investigation/disasm-fantool-ec.txt` + `disasm-fantool2-ports.txt`:

- Port access is via `KernCoreLib64.dll` (WinIO) IOCTL **`0x221C0C`** = `ReadPort`, **`0x221C10`** = `WritePort` (functions at RVA 0x3580 read, 0x3680 write).
- EC RAM read sequence (RVA 0x3760 ff.): poll port 0x66 until ready (bit 2, `test al,2`), up to **10 retries × 100 ms** loop (`mov ecx,0x64` + `GetTickCount`/`Sleep`), then `write port 0x66 = 0x90` (EC read command)... writes via command **`0x91`**, data through port **0x62**. (Details per disassembly in the artifacts.)
- The single EC-modifying function in FanTool64 is a **read-modify-write of EC address 0x33, bit 0** — used by the "set fan full speed" feature (`set fan full speed error!` string). ⇒ EC RAM 0x33 bit0 = force-full-speed / cooler-boost style toggle (matches WMAM 0x0F/0x10 block `{0x33,0x34}`).
- Strings: `FanRPMLimit.ini` parsing with `MODE(0/1/2)`, `MIN`, `MAX`, `ALLLIMIT`, `TIME`, `FAN_NUM(1–4)`, `DUTY%d`, `DUTY%dRPMLOWUP`, output files `FanRPM_Fan1..4.txt`.
- `MSIAPService.exe` strings confirm service-side WinIO use: `loadWinIO`, `FreeWinIO`, `InitializeWinIo`, `WinIO.ReadMemory` (observed live in `MSIAPService_Log.txt`).

**Do not** run FanTool64/one-dragon "HW Diagnosis" while EC state is broken: it writes EC 0x33.

---

## 5. Fan-relevant EC RAM map (working hypothesis, to re-verify read-only on Linux)

Consistent with the public **msi-ec** memory map for recent Summit ECs (159x/TglU family):

| EC addr | Meaning (hypothesis) | Evidence |
|---------|----------------------|----------|
| 0x33 / 0x34 | fan force-full-speed bit (bit0) + companion byte | FanTool64 RMW(0x33, bit0); WMAM 0x0F/0x10 IPAR0 |
| 0x68 | CPU temperature | read in cmd 0x0D IPAR0; msi-ec `cpu_temp=0x68` |
| 0x69–0x70 | fan1 group (order in buffer: 0x69,0x6F,0x70,0x6A–0x6E) | WMAM 0x0D/0x0E IPAR1 |
| 0x71–0x78 | **fan1 (CPU): duty at 0x71, RPM at 0x72–0x73**, rest = curve data | WMAM 0x11/0x12 IPAR1; msi-ec `fan1_duty=0x71`, `fan1_rpm=0x72` |
| 0x79–0x7F | fan1 secondary block (7 bytes, e.g. min duty/thresholds) | WMAM 0x0F/0x10 IPAR1 |
| 0x80 | GPU temperature | cmd 0x0D IPAR0; msi-ec `gpu_temp=0x80` |
| 0x81–0x88 | fan2 group (0x81,0x87,0x88,0x82–0x86) | WMAM 0x0D/0x0E IPAR2 |
| 0x89–0x90 | **fan2 (GPU): duty 0x89, RPM 0x8A–0x8B**, rest = curve | WMAM 0x11/0x12 IPAR2; msi-ec `fan2_duty=0x89` |
| 0x91–0x97 / 0x98–0x9E | fan2 secondary blocks | WMAM 0x0F/0x10 IPAR2/3 |
| 0x9F | fan mode / status byte | WMAM 0x0D IPAR0 third byte |
| 0xC8–0xCF | power/extra block | WMAM 0x11/0x12 IPAR0 |

The 6-point curve (`FanCPUT1Percentage`…`FanCPUT6Percentage` in MSIAPService) is carried in the 8-byte groups; exact semiotic layout must be pinned by diffing **read-only dumps** on Linux (see §8), not by writing.

---

## 6. Service plumbing (useful for log-based testing)

- UI ⇄ service: named pipe, ASCII protocol `WMI2:<REQ>_xx:<1;…>` — request names seen: `GWMIVER,GEC,GBIOS/SBIOS,GSMBUS/SSMBUS,GMBAT/SMBAT,GSBAT/SSBAT,GTEMP/STEMP,GTHER/STHER,GFAN/SFAN,GDEV/SDEV,GPWR/SPWR,GDBG/SDBG,GAP/SAP,GDATA/SDATA` and response forms `WMI2:GFAN_RST:…` / `GDATA_RST` / `REG:WRST`.
- `MSIAPService_Log.txt` shows past activity incl. `startNamedPipeProcessClient`, `loadWinIO`, `WinIO.ReadMemory` (plaintext segments; rest encrypted).
- One Dragon Center logs: `Log\One Dragon Center\DC_*.txt` (`Load component User Scenario …`), `Log\MSI_Central_Service\*`. **Log-diff testing** (start/stop One Dragon Center, trigger "User Scenario" / fan settings, diff logs + read-only EC dumps) is the safe replacement for gist's raw-EC-write probing.

---

## 7. ACPI-side extras

- DSDT 368 677 bytes recovered via `GetSystemFirmwareTable('ACPI','DSDT')` (not listed by `EnumSystemFirmwareTables`!) → `ec-investigation/acpi/DSDT_...dsl` (2.65 MB decompiled).
- Registry also holds SSD1–SSDC tables incl. **SSD5_MSI_NB_TglU_Rvp** (27 974 B) and **SSD6_MSI_NB_PtidDevc** (intel PTID: `ECRD(_SB.PC00.LPCB.H_EC.TSR2)` temperatures feeding `TMPX[6]`, strings `"CPU Fan Duty Cycle"`, `"CPU Fan #1 Speed"`).
- 13 duplicated "SSDT" tables from the user API are all identical `CpuSsdt` — ignore.
- `H_EC` device: `OperationRegion (EC, EmbeddedControl, 0, 0x100)`; its `_CRS` = I/O ports **0x62/0x66** (line 70509–70513).

---

## 8. Recommended next steps (all on the Linux session)

1. **Read-only mapping first**: with `msi-ec`/`m-control-center` running (or `ec_sys` debugfs if built-in), dump EC bytes 0x28–0x9F, 0xC0–0xDF at baseline (silent, balanced, game scenario) — 3 dumps each, no writes.
2. **Correlate** the dump bytes with §5 hypotheses (`0x71` ≈ duty CPU, `0x89` ≈ duty GPU, `0x72/0x8A` ≈ RPM) and with `Fan.txt` absence for `Summit E16Flip`.
3. **Log-diff test** (safe complement): if a Windows UI action is needed, obtain explicit user authorization first, trigger it once, and note the delta in `MSIAPService_Log.txt`/`DC_*.txt` next to the before/after read-only EC dumps.
4. **Restore**: reinstall/repair MSI Center (or clean One Dragon Center leftovers), then let a *known-good* MSI stack reinitialize EC defaults; avoid FanTool64 in HW Diagnosis (writes EC 0x33).
5. Only after a verified backup dump: consider controlled write tests in `msi-ec` (never raw port I/O) with reversible profile re-application.

## 9. Safety appendix

- Everything done here was read-only. No EC writes, no Cooler Boost, no firmware flashes. The only nominally "concurrent" runtime activity was reading MSI's own log files.
- Do not use the WMI command table or FanTool64 sequences to write EC values from this report without the byte-level protocol (write-back window semantics may require `WMI2`/SMM helpers).
- EC flash/BIOS update n/a; keep to 1591EMS1.108.

