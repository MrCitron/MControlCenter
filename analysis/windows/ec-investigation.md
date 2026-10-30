# Windows EC fan curve investigation

Target computer: **MSI Summit E16 Flip A11UCT**
Board: **MS-1591**
BIOS: **E1591IMS.310**
EC firmware: **1591EMS1.108**

This procedure aims at identifying the addresses and the write sequence of the
fan curves used by MSI Center. It must be performed read-only as much as
possible.

## Safety rules

- Do not use the write functions of any EC/ACPI tool.
- Do not modify the BIOS or the EC during the investigation.
- Do not use "Cooler Boost" as a test.
- Keep the charger plugged in and back up important data.
- Do not test an aggressive curve: use only 1-percentage-point changes,
  for example 30 % to 31 %.
- If a tool warns that it may write to the EC, cancel any write operation.

## 1. Record the system identity

In PowerShell, run:

```powershell
Get-CimInstance Win32_ComputerSystem | Format-List Manufacturer,Model
Get-CimInstance Win32_BIOS | Format-List SMBIOSBIOSVersion,ReleaseDate
Get-CimInstance Win32_PnPSignedDriver |
  Where-Object {$_.DeviceName -match 'MSI|System Interface|WMI'} |
  Select-Object DeviceName,DriverVersion,DriverDate,InfName
```

Also note:

- the Windows version;
- the MSI Center version;
- the MSI Center component versions;
- the presence of an MSI System Control Manager driver or MSI Center SDK.

Export the information:

```powershell
New-Item -ItemType Directory -Force .\ec-investigation | Out-Null
Get-CimInstance Win32_ComputerSystem |
  Out-File .\ec-investigation\system.txt
Get-CimInstance Win32_BIOS |
  Out-File .\ec-investigation\bios.txt
Get-CimInstance Win32_PnPSignedDriver |
  Where-Object {$_.DeviceName -match 'MSI|System Interface|WMI'} |
  Format-List * |
  Out-File .\ec-investigation\msi-drivers.txt
```

## 2. Back up the MSI files

Identify the MSI executables, DLLs and drivers without running them:

```powershell
Get-ChildItem 'C:\Program Files\MSI' -Recurse -File -ErrorAction SilentlyContinue |
  Where-Object {$_.Extension -in '.exe','.dll','.sys'} |
  Select-Object FullName,Length,LastWriteTime |
  Out-File .\ec-investigation\msi-files.txt
```

For each interesting file, compute a hash:

```powershell
Get-ChildItem 'C:\Program Files\MSI' -Recurse -File -ErrorAction SilentlyContinue |
  Where-Object {$_.Extension -in '.exe','.dll','.sys'} |
  Get-FileHash -Algorithm SHA256 |
  Export-Csv .\ec-investigation\msi-hashes.csv -NoTypeInformation
```

The `.sys` files are the priority: the GUI may delegate the EC accesses to
the Windows driver.

## 3. Search the constants statically

Copy the binaries into `ec-investigation\binaries` and work on the copies.
Never modify system files.

With **Ghidra** or an equivalent tool, search in the `.sys`, `.dll` and
`.exe` files:

- `0x62`, `0x66` (classic ACPI EC ports);
- `0xD2`, `0xD4` (modes known for this EC);
- `0x72` to `0x78`;
- `0x82` to `0x90`;
- the strings `fan`, `curve`, `cooler`, `performance`, `silent`;
- WMI, ACPI or IOCTL calls.

The presence of a single constant is not enough: it has to be confirmed that
it is used by an EC read/write function and that it corresponds to a curve
modification.

## 4. Controlled state comparison

This step requires a tool able to **read** the EC memory. The tool must be
checked and used read-only. If the read-only mode is not guaranteed, do not
use it.

Create one directory per trial:

```powershell
New-Item -ItemType Directory -Force .\ec-investigation\trial-01 | Out-Null
```

For each trial:

1. Start Windows and wait for 2 minutes without starting MSI Center.
2. Do a read-only EC dump: `before.bin`.
3. Start MSI Center.
4. Modify **a single point** of a single curve, from 30 % to 31 %.
5. Wait 10 seconds without changing anything else.
6. Do a second dump: `after.bin`.
7. Restore the original setting in MSI Center.
8. Do a third dump: `restored.bin`.
9. Record the time, the used profile, the modified curve and the result.

Do not test several points at once: that would make the result impossible
to interpret.

## 5. Compare the dumps

From Linux, copy `ec-investigation` into this analysis folder then run:

```bash
cmp -l trial-01/before.bin trial-01/after.bin
cmp -l trial-01/after.bin trial-01/restored.bin
```

Or, to display the offsets in hexadecimal:

```bash
xxd -g 1 trial-01/before.bin > trial-01/before.hex
xxd -g 1 trial-01/after.bin  > trial-01/after.hex
diff -u trial-01/before.hex trial-01/after.hex
```

A byte that always changes at the same offset when a single point is
modified is a candidate. The test must be repeated for every CPU and GPU
point, then the offsets must be verified to change regularly.

## 6. Verify the sequence, not only the addresses

A curve may require several writes:

1. select the advanced mode;
2. write the temperatures;
3. write the speeds;
4. commit the curve with a command byte;
5. return to the automatic or advanced mode.

So do not only replay the found offsets. Also record the order of the
observed changes and the fan state.

## 7. Report to bring back into this repository

Copy only the required results:

```text
ec-investigation/
  system.txt
  bios.txt
  msi-drivers.txt
  msi-files.txt
  msi-hashes.csv
  trial-01/
    notes.md
    before.bin
    after.bin
    restored.bin
```

In `notes.md`, state:

- date and time;
- the used MSI profile;
- the CPU/GPU point modified;
- the before/after values;
- the observed temperature and speed;
- any crash or reboot.

Do not publish serial numbers, Windows identifiers or personal files.

## 8. Next Linux step

After the offsets are confirmed, compare the results with:

- `msi-ec/msi-ec.c`;
- `mcontrolcenter/src/operate.cpp`;
- `mcontrolcenter/src/helper/readwrite.cpp`.

The Linux changes must first be implemented on a local branch, with a guard
that disables the curves unless the firmware is explicitly validated. No
write test must be performed on the laptop before that validation.
