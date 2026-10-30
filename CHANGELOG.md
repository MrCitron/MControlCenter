# Changelog

All notable changes to this project will be documented in this file.

## Unreleased
- Do not perform raw EC fan writes when the msi-ec kernel driver is loaded:
  - fan curve (speed/temperature) writes are disabled (raw addresses are
    model-specific and not validated on all models, e.g. MSI Summit E16 Flip
    A11UCT where such writes can hard reset the machine);
  - "Advanced" fan mode is applied through the validated msi-ec sysfs
    interface instead of a raw EC write.
- Fan curves now use the WMI platform sysfs interface when it is available
  (patched msi-wmi-platform driver, e.g. on MSI Summit E16 Flip A11UCT):
  - duty curves are read/written through the `fan1_duty_curve` /
    `fan2_duty_curve` sysfs attributes; one write is a single validated WMAM
    `SET_FAN` window write — no raw EC access;
  - duty curve writes go through the privileged helper daemon
    (`mcontrolcenter-helper`, new `setWmiFanCurve` D-Bus method with strict
    attribute/value validation), because the sysfs attributes are root-owned;
  - temperature points are presented as read-only (the EC firmware owns
    them, and its WMI interface exposes them read-only as well), matching
    the behavior of MSI's own software;
  - fan modes (auto/silent/basic/advanced) keep going through msi-ec sysfs.
  Custom fan curves stay unavailable (with a graceful message) when neither
  the WMI curve interface nor a safe raw path is present.

## [0.4.1] 2023-08-09
- Fixed UI lags when resizing the window (issue https://github.com/dmitry-s93/MControlCenter/issues/97)
- Updated Norwegian Bokmål (Thanks [msigurdsen](https://github.com/msigurdsen))
- Added support for German language (Thanks [EchterAlsFake](https://github.com/EchterAlsFake))
- Added support for Hungarian language (Thanks [BiRo96](https://github.com/BiRo96))

## [0.4.0] 2023-03-06
- Added fan control support
- Updated Norwegian Bokmål (Thanks [msigurdsen](https://github.com/msigurdsen))
- Updated Spanish translation (Thanks [rottenpants466](https://www.github.com/rottenpants466))
- Updated Italian translation (Thanks [rottenpants466](https://www.github.com/rottenpants466))
- Updated Portuguese translation (Thanks [rottenpants466](https://www.github.com/rottenpants466))

## [0.3.2] 2022-11-28
- Added fan mode display
- Added description on mode tab
- Added "Cooler Boost" item to tray menu
- Added support for [acpi_ec](https://github.com/musikid/acpi_ec) kernel module
- Added support for Italian language (Thanks [marraviglioso](https://github.com/marraviglioso))
- Added support for Portuguese (Brazilian) language (Thanks [rottenpants466](https://www.github.com/rottenpants466))
- Updated Spanish translation (Thanks [rottenpants466](https://www.github.com/rottenpants466))
- Updated Portuguese translation (Thanks [rottenpants466](https://www.github.com/rottenpants466))
- Fixed display of the window when clicking the tray icon if the application was minimized

## [0.3.1] 2022-11-04
- Added support for Norwegian Bokmål (Thanks [msigurdsen](https://github.com/msigurdsen))
- Added support for Turkish language (Thanks [emrgncr](https://github.com/emrgncr))
- Corrected display of the current battery level
- Fixed fan 1 speed display for some devices
- Fixed "FN <-> Super" for some devices (Issue https://github.com/dmitry-s93/MControlCenter/issues/53)

## [0.3.0] 2022-10-28
- Added support for Spanish language (Thanks [ssergio-ll](https://www.github.com/ssergio-ll))
- Ability to set the battery charge limit from 30% to 100% in steps of 1
- Prevent launching more than one instance
- Added application icon to system tray
- Added saving and restoring settings
- Some optimizations
- Fixed turning on/off webcam

## [0.2.0] 2022-10-16
- Added mode change (High Performance, Balanced, Silent, Super Battery)
- Added support for Portuguese language (Thanks [rottenpants466](https://www.github.com/rottenpants466))
- Run helper as D-BUS service
- Some optimizations
- Fixed typos

## [0.1.2] 2022-10-02

- GUI improvements
- Some optimizations
- Added link to bug tracker
- Added "Fully charged" battery status
- Added "Fully charged (Discharging)" battery status

## [0.1.1] 2022-09-21

- Added charging status display
- Added localization support
- Added support for Russian language
- Some optimizations

## [0.1.0 alpha] 2022-09-12

- First release
