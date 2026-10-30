# Linux resume — fan control repair (MSI Summit E16 Flip A11UCT)

Machine: MS-1591, BIOS `E1591IMS.310`, EC `1591EMS1.108`.
Windows: analysis completed, 100 % read-only (see `investigation-report.md` below/in this folder).

## 1. Summary of what to keep from the Windows side

- Fan control goes through EC RAM. The relevant addresses:
  - `0x33/0x34`: full-speed forcing (bit 0 of 0x33 — target of FanTool64.exe, **do not use**)
  - `0x68` CPU temp · `0x80` GPU temp
  - `0x71–0x78`: fan1 (CPU) — duty = `0x71`, RPM = `0x72/0x73`
  - `0x89–0x90`: fan2 (GPU) — duty = `0x89`, RPM = `0x8A/0x8B`
  - `0x79–0x7F`, `0x91–0x9E`, `0x9F`, `0xC8–0xCF`: secondary blocks (min-duty, mode, thresholds)
- The WMI/SCM0 commands (`WMAM`) pair these addresses read/write:
  fan read = 0x0D, write = 0x0E; temperatures 0x09/0x0A; duty blocks 0x11/0x12.
- One Dragon Center is probably the agent of the problem: the `Fan.txt` of
  nbFoundation does not reference any "Summit E16Flip" model (profile
  misapplied/overwritten).

## 2. Steps to run on Linux (read-only first)

```bash
# 0) Verify msi-ec (or ec_sys) is loaded + current state
lsmod | grep -E 'msi|ec_sys'
cat /sys/firmware/dmi/id/product_name; sudo dmidecode -s bios-version

# 1) Read-only EC dump (if ec_sys/debugfs available)
sudo modprobe ec_sys write_support=0
sudo hexdump -C /sys/kernel/debug/ec/ec0/io

# 2) Three dumps per scenario (quiet / balanced / performance),
#    in parallel: cat /proc/acpi/... + sensors; comparison with the §5 table of investigation-report.md
sudo hexdump -C /sys/kernel/debug/ec/ec0/io | sed -n '3,17p;19,22p'   # 0x28-0x9F, roughly
sudo hexdump -C /sys/kernel/debug/ec/ec0/io | awk 'NR>=1&&NR<=16'
#    keep everything: dump-base-x3.txt, dump-quiet-x3.txt, dump-perf-x3.txt

# 3) Cross-check with msi-ec
git grep -n "0x71\|0x89\|0x68\|0x80\|0x33" -- *.c *.h   # msi-ec repository

# 4) Only after a validated dump backup:
#    controlled write test through the standard msi-ec channel (never raw port I/O),
#    write a bounded duty value then re-dump + measure the RPMs.
```

## 3. What NOT to do

- Do not run `FanTool64.exe` (HW Diagnosis): it performs an RMW on EC 0x33 bit 0.
- Do not trigger "Cooler Boost" again until the EC has a stable profile.
- Do not flash the BIOS/EC.

## 5. Fan curves — results of the "protocol capture" session (29/10)

The One Dragon Center advanced curves are persisted in **`High_Fan`**
(HKLM registry): 12 values = `[duty D1..D6 fan1] + [duty D1..D6 fan2]`, scale 0–110,
point → index n−1 (fan2 block offset +6), temperatures not editable
(`Default_Temp`).
Scenarios: `OC` = active index, `PB` = perf-bias (40/50/74/100), advanced-curve flag
per scenario `x_Scenario\Fan`. See `windows/capture-results.md` +
`windows/regwatch-curve-trials.txt`.

## 6. Restoration (Windows side, after the Linux verification phase)

- Repair/reinstall MSI Center or reset the One Dragon Center profiles
  (`Data/DuetDefaultProfile.xml`, fan profiles), then power off completely
  (cold shutdown ≥ 30 s, unplug power + power drain) to re-initialize the EC.
- The missing model in `Fan.txt` ⇒ consider patching `Fan.txt` (back it up first)
  so nbFoundation recognizes the Summit E16Flip (2-fan type).
