# msi-wmi-platform + fan curves (out-of-tree, MS-1591)

Upstream `msi-wmi-platform` kernel driver (7.0) + fan-curve sysfs interface for
the MSI Summit E16 Flip A11UCT (MS-1591). See `../docs/FAN_CURVE_MAPPING.md` for
the validated WMAM ↔ EC RAM mapping (read-only).

## Added sysfs attributes (under `/sys/bus/wmi/devices/ABBC0F6E-8EA1-11D1-00A0-C90629100000/`)

| File | Access | Content |
|---|---|---|
| `fan1_duty_curve` | RW (root) | 7 fan1 duty points (EC 0x72–0x78). EC default: `45 55 60 70 90 110 130` |
| `fan2_duty_curve` | RW (root) | 7 fan2 duty points (EC 0x8A–0x90). EC default: `45 70 90 100 110 110 110` |
| `fan1_live_duty` | RO | fan1 live duty (EC 0x71) |
| `fan2_live_duty` | RO | fan2 live duty (EC 0x89) |
| `fan1_temp_curve` | RO | EC 0x69–0x70 sorted: enable + 5 curve points + 2 alarms (`0 60 70 80 85 90 95 100`) |
| `fan2_temp_curve` | RO | same for fan2 (`0 55 65 75 85 89 90 89`) |

The RPMs remain in the existing hwmon (`hwmon*/fan1_input`, `fan2_input`).

## Safety guards

1. **`allow_curve_writes` (module parameter, default `false`, 0644)**:
   every write returns `-EPERM` until the module is loaded with
   `allow_curve_writes=1`. Reads always work.
2. DMI gate `MSI / MS-1591`: on any other model, no attribute appears.
3. One write = one full WMAM SET_FAN window write (8-byte window, the live-duty
   byte taken from a fresh read, never byte-by-byte),
   then a **readback**: if the 7 values do not match, the write returns
   `-EIO` and the event is logged (`dmesg`).
4. The advanced fan mode remains the responsibility of **msi-ec** (`fan_mode`),
   the already validated channel.

## Build / install (user steps, sudo required)

```bash
cd wmi-fan-curve
make

# replace the in-tree module (it is already loaded)
sudo rmmod msi_wmi_platform
sudo insmod msi-wmi-platform.ko          # NO EC write at load time

# write-free checks:
ls /sys/bus/wmi/devices/ABBC0F6E-8EA1-11D1-00A0-C90629100000/
cat /sys/bus/wmi/devices/ABBC0F6E-8EA1-11D1-00A0-C90629100000/fan1_duty_curve
cat /sys/bus/wmi/devices/ABBC0F6E-8EA1-11D1-00A0-C90629100000/fan1_live_duty
dmesg | tail -20
```

Return to the stock module: `sudo rmmod msi_wmi_platform && sudo modprobe msi_wmi_platform`.

Blacklist (optional, to exclude the in-tree module at boot):

```bash
echo "blacklist msi_wmi_platform" | sudo tee /etc/modprobe.d/msi-curve-test.conf
sudo update-initramfs -u   # only if the in-tree module is included in the initramfs
```

## Controlled curve-write trial (explicit consent)

```bash
sudo rmmod msi_wmi_platform
sudo insmod msi-wmi-platform.ko allow_curve_writes=1
sudo ../analysis/scripts/set-fan-trial.sh   # trial script, see analysis/
```

The script:

1. refuses to run without `allow_curve_writes=Y` (double safety guard);
2. prints the full state before (curves, live duty, RPMs, temperatures, fan_mode);
3. asks for an explicit confirmation;
4. modifies ONE point (+3 on the point 1 of fan1, low-temperature zone),
   monitors for 60 s with a per-second log (CPU temp / RPM / live duty),
   then RESTORES the original values and verifies with a readback;
5. on Ctrl-C: immediate restoration too (trap).

Even in auto mode, the EC may overwrite the live duty: the trial mainly measures
the persistence of the stored curve; the real RPM effects of the `advanced`
mode (msi-ec `fan_mode`) — to be enabled manually and afterwards.
