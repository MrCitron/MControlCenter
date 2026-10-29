/*
 * Constants shared between the MControlCenter application and its privileged
 * helper daemon for the WMI fan-curve sysfs interface exposed by the patched
 * msi-wmi-platform driver.
 *
 * Reads (EC duty/temperature/RPM state) go directly through the attributes
 * (world-readable). Writes are root-owned (0644) and are rejected by the
 * driver unless it was loaded with allow_curve_writes=1: they must go
 * through the helper daemon, exactly like the raw ec_sys EC writes.
 */

#ifndef WMIFANCURVE_H
#define WMIFANCURVE_H

inline constexpr const char *WMI_PLATFORM_DEVICE_PATH =
        "/sys/bus/wmi/devices/ABBC0F6E-8EA1-11D1-00A0-C90629100000";
inline constexpr const char *WMI_FAN_DUTY_CURVE_FAN1 = "fan1_duty_curve";
inline constexpr const char *WMI_FAN_DUTY_CURVE_FAN2 = "fan2_duty_curve";
inline constexpr int WMI_FAN_DUTY_CURVE_COUNT = 7;

#endif // WMIFANCURVE_H
