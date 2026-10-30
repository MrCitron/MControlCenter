# analysis — investigation history for the MS-1591 fan curve work

This directory keeps the full investigative trace that led to the WMAM fan
curve implementation: the Windows-side reconnaissance, the read-only
evidence campaigns on Linux, the methodology scripts, and the low-level
disassembly. Everything here was collected **read-only** (no EC write) or
explicitly consented (the single `SET_FAN` trial), and was sanitized before
being committed (serial number, hostname and absolute personal paths
redacted).

## Contents

| Item | Description |
|---|---|
| `ec-protocol-analysis.md` | First static analysis (26 Oct): how Windows drives the fans (WMI-ACPI vs FanTool64), why the raw EC writes of upstream MControlCenter hard-reset the machine, and the original action plan. |
| `windows/` | The Windows investigation history: methodology (read-only), capture session results (One Dragon Center registry layout), the step-by-step investigation report, the DSDT `WMAM` command table, the first raw WMI dump and the low-level disassembly traces (FanTool64 port protocol, .NET IL trace). |
| `wmi-evidence/` | The three Linux evidence campaigns, each state containing the WMAM window dump + the `ec_sys` page-0 ECRAM dump + hwmon/msi-ec state, plus the delta summaries and campaign logs. `20261029-172118` is the campaign that validated the byte-exact WMAM ↔ EC RAM mapping. |
| `set-fan-trial-20261029-173613.log` | The single controlled `SET_FAN` curve-write trial (explicit consent): write accepted, live duty recomputed by the EC from the stored curve even in auto mode, clean restoration. |
| `scripts/` | The collection scripts: `dump_wmi_readonly.sh` (one-shot all-WMAM dump), `wmi_probe_full.sh` (4-state differential campaign producing `wmi-evidence/`), `set-fan-trial.sh` (the consent-gated curve-write trial). |

## Privacy note

- `windows/system-and-bios.txt`: the machine serial number and hostname are
  redacted (`REDACTED`).
- All logs referencing absolute personal paths were normalized to
  repository-relative paths.
- The registry captures (`registry-scenario.txt`,
  `regwatch-curve-trials.txt`) contain only MSI One Dragon Center values
  (fan curve shelves, EC version string) — no personal data.

## Language note

All documentation is in English. The raw machine outputs (`campaign.log`
files, `summary-deltas.txt`, `set-fan-trial-*.log`) are kept **as originally
produced** (some of them in French) to preserve the integrity of the
evidence records.
