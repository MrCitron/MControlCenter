#!/usr/bin/env bash
# dump_wmi_readonly.sh — 100 % read-only WMI dump
# Kernel driver msi_wmi_platform (WMI ACPI method WMAM / SCM0)
# Machine: MSI Summit E16 Flip A11UCT (MS-1591, EC 1591EMS1.108)
#
# Safety: only WMAM READ commands are invoked
# (get_ec=0x03, get_temperature=0x0D, get_thermal=0x0F, get_fan=0x11,
# get_device=0x13, get_power=0x15, get_data=0x1B).
# No debugfs set_* file is touched. No EC write.
#
# Usage: sudo ./dump_wmi_readonly.sh [output-directory]

set -u

OUT="${1:-/tmp/wmi-dump}"
GUID="ABBC0F6E-8EA1-11D1-00A0-C90629100000"

# --- prerequisites -------------------------------------------------------

[ "$(id -u)" -eq 0 ] || { echo "ERROR: run with sudo." >&2; exit 1; }
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug 2>/dev/null

# Debugfs directory created by the driver: "msi-wmi-platform-<dev_name>"
# (dev_name = GUID + instance). Look for both possible spellings.
DEBUGFS_DIR=$(find /sys/kernel/debug -maxdepth 1 -type d \( -name 'msi-wmi-platform-*' -o -name 'msi_wmi_platform-*' \) 2>/dev/null | head -1)
[ -n "${DEBUGFS_DIR}" ] || { \
    echo "ERROR: debugfs msi-wmi-platform not found." >&2
    echo "Content of /sys/kernel/debug/ (msi entries):" >&2
    ls /sys/kernel/debug/ | grep -i msi >&2 || echo "(none) — debugfs may not be mounted: mount -t debugfs none /sys/kernel/debug" >&2
    exit 2
}
echo "debugfs: ${DEBUGFS_DIR}"

ls /sys/bus/wmi/devices | grep -qi "${GUID}" || {
    echo "ERROR: expected WMI GUID ${GUID} missing from /sys/bus/wmi/devices." >&2
    exit 3
}

mkdir -p "${OUT}"
PAYLOAD=/tmp/wmi-payload.bin

# --- mechanics -----------------------------------------------------------
# A debugfs get_* file triggers the WMAM read command when a 32-byte buffer
# is written into it; the first buffer byte is IPAR (group sub-index, see
# the DSDT/WMAM table). Reading the file back then returns the 32-byte
# buffer returned by the ACPI method.

run_get() {
    local name="$1" ipar="$2" dest="$3"
    if [ ! -e "${DEBUGFS_DIR}/${name}" ]; then
        echo "${name}: ABSENT from debugfs" >> "${OUT}/resume.txt"
        return 1
    fi
    python3 -c "open('${PAYLOAD}','wb').write(bytes([${ipar}])+bytes(31))" || return 1
    cat "${PAYLOAD}" > "${DEBUGFS_DIR}/${name}" 2>/dev/null || {
        echo "${name} ipar=${ipar}: write failed (request rejected)" >> "${OUT}/resume.txt"
        return 1
    }
    {
        echo "## ${name} ipar=${ipar} (WMAM GET command — 32-byte output)"
        head -c 32 "${DEBUGFS_DIR}/${name}" | od -A n -v -t x1
    } > "${dest}" 2>&1
    {
        echo "## ${name} ipar=${ipar}"
        head -c 32 "${DEBUGFS_DIR}/${name}" | od -A n -v -t x1
    } >> "${OUT}/resume.txt"
    echo "ok: ${dest}"
}

# --- dump ----------------------------------------------------------------

echo "== get_ec (0x03, IPAR 0) =="
run_get "get_ec" 0 "${OUT}/get_ec.txt"

echo "== get_fan (0x11, IPAR 0..3) =="
for i in 0 1 2 3; do
    run_get "get_fan" "$i" "${OUT}/get_fan-ipar${i}.txt"
done

echo "== get_thermal (0x0F, IPAR 0..3) =="
for i in 0 1 2 3; do
    run_get "get_thermal" "$i" "${OUT}/get_thermal-ipar${i}.txt"
done

echo "== get_temperature (0x0D, IPAR 0..3) =="
for i in 0 1 2 3; do
    run_get "get_temperature" "$i" "${OUT}/get_temperature-ipar${i}.txt"
done

echo "== get_device (0x13, IPAR 0) =="
run_get "get_device" 0 "${OUT}/get_device-ipar0.txt"

echo "== get_power (0x15, IPAR 0) =="
run_get "get_power" 0 "${OUT}/get_power-ipar0.txt"

echo "== get_data (0x1B, IPAR 0) =="
run_get "get_data" 0 "${OUT}/get_data-ipar0.txt"

# --- correlation: observable state without any write ---------------------

{
    echo
    echo "## Raw ECRAM page0 (ec_sys, read-only) — for correlation"
    if [ -r /sys/kernel/debug/ec/ec0/io ] && [ -e /sys/kernel/debug/ec/ec0/io ]; then
        echo "## 256 bytes, offset = EC address (page 0, direct access 0x80/0x81)"
        cat /sys/kernel/debug/ec/ec0/io | od -A x -v -t x1
    else
        echo "ec_sys not loaded — modprobe ec_sys (read-only ok) if needed"
    fi
    echo
    echo "## hwmon msi_wmi_platform (current RPMs)"
    for w in /sys/bus/wmi/devices/${GUID}/hwmon/hwmon*; do
        echo "fan1_input: $(cat "$w/fan1_input" 2>/dev/null) rpm"
        echo "fan2_input: $(cat "$w/fan2_input" 2>/dev/null) rpm"
        echo "fan3_input: $(cat "$w/fan3_input" 2>/dev/null) rpm"
        echo "fan4_input: $(cat "$w/fan4_input" 2>/dev/null) rpm"
    done
    echo
    echo "## msi-ec sysfs (current state)"
    echo "fan_mode:   $(cat /sys/devices/platform/msi-ec/fan_mode 2>/dev/null)"
    echo "shift_mode: $(cat /sys/devices/platform/msi-ec/shift_mode 2>/dev/null)"
    echo "cooler_boost: $(cat /sys/devices/platform/msi-ec/cooler_boost 2>/dev/null)"
    echo "cpu files: $(ls -1 /sys/devices/platform/msi-ec/cpu/ 2>/dev/null | tr '\n' ' ')"
    echo "gpu files: $(ls -1 /sys/devices/platform/msi-ec/gpu/ 2>/dev/null | tr '\n' ' ')"
    echo "EC firmware: $(cat /sys/devices/platform/msi-ec/fw_version 2>/dev/null)"
} >> "${OUT}/resume.txt"

rm -f "${PAYLOAD}"

echo
echo "------------------ SUMMARY ------------------"
cat "${OUT}/resume.txt"
echo "Dump directory: ${OUT}"
