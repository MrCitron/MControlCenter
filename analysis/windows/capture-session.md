# Windows session — fan curve protocol capture

Target machine: **MSI Summit E16 Flip A11UCT**
Goal: identify the exact parameter set the Windows application sends to the
BIOS/EC when a fan curve is modified, in order to replicate that mechanism on
Linux (`msi-ec` + patched MControlCenter).

Why this session differs from the previous one: we now know that the Windows
OS never writes anything directly to the EC. Everything goes through
`MSIAPService.exe` → WMI-ACPI (`root\WMI`, class `MSI_AP`) → BIOS ACPI
method. And **MSIAPService logs every WMI exchange in hexadecimal**
(function `ExecuteWmiMethod`, line `Return value of "Set_Fan(...)" = ...`).
So the protocol can be observed by driving only the MSI Center UI, without
any EC access tool.

---

## 1. Safety rules

- Do not call the WMI methods (`Set_Fan`, `Set_Thermal`, …) manually from
  PowerShell or anything else: we observe MSI Center, we drive nothing.
- Do not use RWEverything or any raw EC access tool.
- Modify **a single setting at a time** in the MSI Center UI.
- Keep the charger plugged in during the whole session.
- Boot onto a fresh Windows session (reboot) before the capture.

## 2. Preparation (10 minutes)

Create a working directory:

```powershell
New-Item -ItemType Directory -Force .\ec-investigation | Out-Null
```

### 2.1 Where is MSIAPService?

```powershell
Get-CimInstance Win32_Service |
  Where-Object { $_.Name -like '*MSI*' -or $_.DisplayName -like '*MSI*' } |
  Format-List Name, DisplayName, State, PathName |
  Out-File .\ec-investigation\msi-services.txt
```

Note the path (`PathName`). MSIAPService logs through a
`writeLog("MSIAPService", "Log", …)` function: the log file is generally in
the MSI Center installation directory, or under `C:\ProgramData`.

### 2.2 Locate the MSIAPService log file(s)

The most reliable way: **Process Monitor** (Sysinternals).

1. Start Procmon (`procmon64.exe`).
2. Filter: `Process Name` is `MSIAPService.exe` → Include.
3. Highlight: `Operation` is `WriteFile`.
4. Restart the MSI service (exact name from step 2.1):

```powershell
Restart-Service "MSIAPService" -Force
```

5. Look which files MSIAPService writes → note those paths.
6. Copy all existing log files into `ec-investigation\logs-baseline`.

Alternative without Procmon:

```powershell
Get-ChildItem 'C:\ProgramData' -Recurse -Directory -ErrorAction SilentlyContinue |
  Where-Object { $_.FullName -match 'MSI' } | Select-Object FullName

Get-ChildItem "$env:ProgramFiles\MSI" -Recurse -Include '*log*' -File -ErrorAction SilentlyContinue |
  Select-Object FullName, Length, LastWriteTime
```

Also check the Event Viewer: Applications and Services Logs (and search for
an "MSIAPService" source).

### 2.3 Back up the MSI Center user settings

MSI Center persists its settings (settings.json / INI / registry). Those
files contain the previously defined curve: very useful to build the
expected format.

```powershell
Get-ChildItem "$env:ProgramData\MSI" -Recurse -File -ErrorAction SilentlyContinue |
  Where-Object { $_.FullName -match 'settings|config|\.json|\.ini' } |
  Copy-Item -Destination .\ec-investigation\msicenter-config -Force

Get-ChildItem "$env:APPDATA" -Directory | Where-Object { $_.Name -match 'MSI' }
```

MSI registry branches:

```powershell
reg query HKCU\Software /s /f MSI > ec-investigation\registry-msi.txt
reg query HKLM\Software /s /f "MSI Center" >> ec-investigation\registry-msi.txt
```

Do not publish that whole folder (potential personal data); only the keys
related to the fans are needed for the analysis.

### 2.4 Confirm the WMI class present on this machine

```powershell
Get-CimInstance -Namespace root\WMI -ClassName MSI_AP | Select-Object -Property * |
  Out-File ec-investigation\msi-ap-instance.txt

Get-WmiObject -Namespace root\WMI -List Msi* |
  Out-File ec-investigation\msi-wmi-classes.txt
```

Enumeration/reading is harmless (MSIAPService calls it continuously).
**Do not invoke** `Set_Fan` manually.

## 3. Capture protocol per trial

One trial = one single modification in the MSI Center UI.

1. Stream the log file in a PowerShell window:

```powershell
Get-Content 'log-path' -Wait -Tail 50
```

2. Record the "before":
   - exact timestamp (`Get-Date -Format o`);
   - temperatures and speeds displayed in MSI Center.
3. In MSI Center → Fans / Fan Speed tab:
   - choose **a single curve point** (e.g. point 1 of Fan1);
   - modify it by **a single step** (30 % → 31 %, or 1 °C).
4. Wait 5 seconds, record: timestamp; displayed speed before/after.
5. Restore the original value in MSI Center (second event), record.
6. Copy the log excerpt corresponding to the trial (between the timestamps).

### 3.1 Recommended trial table (6 trials)

| # | Setting | Modification | What it teaches |
|---|---------|--------------|------------------|
| 1 | Fan1 — speed, point 1 | 30 % → 31 % | the full Set_Fan payload |
| 2 | Fan1 — speed, point 4 | 30 % → 31 % | position of the modified point in the payload |
| 3 | Fan1 — temperature, point 2 | +1 °C | does the payload contain both temp and speed? |
| 4 | Fan2 — speed, point 1 | 30 % → 31 % | GPU mapping: identical or shifted? |
| 5 | Auto → Silent → Auto | MSI Center selector | which data block drives the modes (Set_Thermal?) |
| 6 | Fan mode Advanced ↔ Auto | MSI Center checkbox | advanced-mode transition without a reboot |

If a slider modification triggers several WMI events in the log (e.g. the
whole curve sent at once), **keep the whole log**: that is exactly what we
want to understand.

### 3.2 What I analyzed in the logs, Linux-side

1. the size and shape of the `Set_Fan` payload (7 temperature/speed pairs
   expected for 7 curve points);
2. the position of the modified point (trial 1 vs 2 vs 3);
3. the Fan1/Fan2 comparison (trial 4);
4. the restore-to-original (2nd event of each trial) acts as a control, to
   validate the mapping between the payload bytes and the points displayed
   in MSI Center.

## 4. Optional — differential EC dumps

Already documented in `windows/ec-investigation.md`. It is **not**
mandatory: the full `Set_Fan` payload is better information than an EC
address diff. Only do it if the MSIAPService logs do not exist or are
unusable.

## 5. Deliverable, Linux side

1. Create a secure local drop containing:
   - `msiapservice-log-trial1..6.txt` (log excerpts around the trials);
   - `notes.md` (table 3.1 filled in, timestamps, temperatures);
   - `msicenter-config/` (non-personal settings/config files);
   - if no log source found: the explored path + a note about what Procmon
     showed as MSIAPService writes.
2. Hand the URL here. On the Linux side, this enabled:
   - the payload analysis (cross-check with the FanTool constants
     0x90/0x91/0x33 of the previous session);
   - the specification of the curve support in the early `msi-ec` design
     (`fan_curve` sysfs attributes or the like, depending on the actual
     protocol);
   - the adaptation of the patched MControlCenter to write the curves
     through sysfs.
