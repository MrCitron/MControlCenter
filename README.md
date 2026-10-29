# MControlCenter — MSI Summit E16 Flip A11UCT (MS-1591) edition

A fork of [dmitry-s93/MControlCenter](https://github.com/dmitry-s93/MControlCenter) dedicated to a
**single model**: *MSI Summit E16 Flip A11UCT*, board **MS-1591**, EC firmware `1591EMS1.108`.

This repository is **self-contained**:

| Directory | Content |
|---|---|
| `src/` | the Qt6 application (MControlCenter fork) |
| `src/helper/` | the privileged D-Bus daemon (`mcontrolcenter-helper`) |
| `wmi-fan-curve/` | the patched `msi-wmi-platform` kernel module (fan curves via WMAM) |
| `docs/FAN_CURVE_MAPPING.md` | the validated WMAM ↔ EC RAM mapping (read this first!) |
| `install_mcontrolcenter.sh` | the complete installer (app + daemon + autostart + module) |

**What it makes possible**: advanced fan control (custom 7-point curves) re-applied at every
startup, a fully silent machine at low load, with **no raw EC write at all** — curve writes go
through the firmware's own WMI-ACPI interface (`WMAM`/`SET_FAN`), the same one the MSI software
uses under Windows.

## Why this fork exists (the delta with upstream)

On this model, the raw EC curve writes of the original application caused hard machine
resets (the EC cutting the power), and upstream exposes no sysfs curve interface. The fork
replaces the curve transport with the validated WMI path, and enforces a safety invariant
observed in the field: *custom curves only with the "High performance" system profile*.

### Full changelog vs upstream

1. **No raw EC fan writes** while the `msi-ec` kernel driver owns the EC
   (`setFan{1,2}Speed/TempSettings` refuse the raw channel).
2. **Curves read/written through sysfs** (`fan1_duty_curve` / `fan2_duty_curve`
   attributes of the patched kernel module): one write = one WMAM `SET_FAN`
   window write (8 bytes, live-duty byte preserved), verified by the driver
   with a readback.
3. **Root writes through the daemon**: the sysfs attributes are root-owned; a strictly
   validated `setWmiFanCurve` D-Bus method (attribute whitelist, 7 values in 0–255)
   was added to `mcontrolcenter-helper`, with shared constants in
   `src/helper/wmifancurve.h`.
4. **Real curve deactivation**: leaving the "Advanced" mode (or switching the system
   profile) **writes the firmware default curves back** (`45 55 60 70 90 110 130` /
   `45 70 90 100 110 110 110`) — the EC keeps using the stored duty points whatever
   the fan mode is, so rewriting is the only real way to hand the fans back to "auto".
5. **Advanced ⇔ High performance invariant** (multiple hard resets observed with
   "profile ≠ High performance" + "advanced"):
   - the "Enable advanced fan control" checkbox is only clickable in High performance;
   - any other profile unchecks it, switches the fan mode back to `auto` via msi-ec
     and restores the firmware default curves;
   - an inline warning states the rule.
6. **Startup and wake**: the fan mode is applied first, and the custom curves are only
   restored when the resulting state is safe; otherwise the firmware defaults are
   written back. Wake-from-sleep included.
7. **Rewritten Fan Control tab**:
   - **Cancel** (formerly Reset) and **Apply** buttons are enabled only while at least
     one editor differs from the last loaded/applied snapshot;
   - Cancel restores the reference values without a new EC read;
   - temperature points are read-only (the EC firmware owns them — like MSI's own
     UI under Windows).
8. **Build identification**: the *About* page shows version + git `commit` +
   automatic build timestamp.

On the **kernel module** side (`wmi-fan-curve/`): sysfs `fan{1,2}_duty_curve` (RW),
`fan{1,2}_live_duty` (RO), `fan{1,2}_temp_curve` (RO); double safety guard — the
**`allow_curve_writes`** module parameter (default *false*: `-EPERM` until explicit
opt-in) and a **readback** after every write.

## Installation

Dependencies: Qt6 ≥ 6.4 (`qt6-base-dev`, `qt6-tools-dev`), `cmake`, `build-essential`.

```bash
git clone <this repository>
cd mcontrolcenter

# application + daemon
cmake -B build -S . && cmake --build build -j8

# patched kernel module
make -C wmi-fan-curve

# complete installation (app + D-Bus daemon + autostart + module + depmap)
sudo ./install_mcontrolcenter.sh
```

Activate without a reboot: `sudo rmmod msi_wmi_platform && sudo modprobe msi_wmi_platform`.

Uninstalling:

```bash
sudo rm /usr/local/bin/mcontrolcenter /usr/libexec/mcontrolcenter-helper \
        /usr/share/dbus-1/system-services/mcontrolcenter.helper.service \
        /usr/share/dbus-1/system.d/mcontrolcenter-helper.conf \
        /usr/share/applications/mcontrolcenter.desktop /usr/share/pixmaps/mcontrolcenter.png \
        /etc/xdg/autostart/mcontrolcenter.desktop /etc/modprobe.d/msi-fan-curve-writes.conf
sudo rm -rf /lib/modules/$(uname -r)/updates/msi-wmi-platform
sudo depmod -a
```

## Driver prerequisites

- `msi-ec` must be loaded (fan mode, shift mode...).
  Check with: `cat /sys/devices/platform/msi-ec/shift_mode`.
- `ec_sys` **without** `write_support=1` is enough for the read-only fallback;
  `modprobe ec_sys` (read-only). The upstream application used to load
  `ec_sys write_support=1` → not needed here.

## Safety guards

- Curves can only be written when the kernel module was loaded with
  `allow_curve_writes=1` — explicit consent, persisted through
  `/etc/modprobe.d/msi-fan-curve-writes.conf`.
- Every curve write is **verified with a readback**: a rejection is visible on the
  application side (`qWarning`) if the EC does not memorize the values.
- The "advanced" + non-High-performance-profile combination is
  **impossible by construction**.
- When in doubt: switch to `balanced` — fan mode `auto` + firmware default curves,
  equivalent to the stock machine.

## Maintenance

- **After an Ubuntu kernel update**: `make -C wmi-fan-curve`
  then `sudo ./install_mcontrolcenter.sh` (~1 min).
- System power-profile switching diagnostics are visible when the app is
  started from a terminal (`PowerMonitor: PropertiesChanged ...`).

## Credits

- [dmitry-s93/MControlCenter](https://github.com/dmitry-s93/MControlCenter) — the
  original application (GPLv3), by Dmitry Serov and the community.
- [BeardOverflow/msi-ec](https://github.com/BeardOverflow/msi-ec) — the kernel EC driver.
- [Armin Wolf](https://lore.kernel.org/all/?q=msi-wmi-platform) — the kernel
  `msi-wmi-platform` driver, base of the module in this repository.
- License: GPLv3 (see `LICENSE`).
