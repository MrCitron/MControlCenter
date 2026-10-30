#!/usr/bin/env bash
# wmi_probe_full.sh — WMI/EC probe campaign in 4 states, 100 % observation
#
# Machine: MSI Summit E16 Flip A11UCT (MS-1591, EC 1591EMS1.108)
# Purpose: collect the data needed to decode the fan-curve layout in the
#         WMI ACPI method WMAM (SCM0), by comparing 4 states: idle, under
#         load, shift eco, and back to the initial state.
#
# SAFETY GUARANTEES;
#   * only WMAM READ commands (get_*) are invoked;
#     no debugfs set_* file is touched, no EC write;
#   * the only permitted write is the shift_mode change through the
#     validated msi-ec sysfs interface, a manoeuvre already proven by the
#     user, the initial state being restored at the end of the campaign;
#   * everything else is sysfs/debugfs reading.
#
# Usage: sudo ./wmi_probe_full.sh
# Results: <script-directory>/wmi-evidence/<date>-<time>/

set -u

# ------------------------------------------------------------- prerequisites
[ "$(id -u)" -eq 0 ] || { echo "ERROR: run with sudo." >&2; exit 1; }

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
GUID="ABBC0F6E-8EA1-11D1-00A0-C90629100000"
HRM_W=/sys/devices/platform/msi-ec/shift_mode
PAYLOAD=/tmp/wmi-payload.bin

OUT_ROOT="${SCRIPT_DIR}/wmi-evidence"
OUT="${OUT_ROOT}/$(date +%Y%m%d-%H%M%S)"
mkdir -p "${OUT}"

MOUNTED=0
mountpoint -q /sys/kernel/debug || { mount -t debugfs none /sys/kernel/debug 2>/dev/null && MOUNTED=1 || true; }

DEBUGFS_DIR=$(find /sys/kernel/debug -maxdepth 1 -type d \( -name 'msi-wmi-platform-*' -o -name 'msi_wmi_platform-*' \) 2>/dev/null | head -1)
[ -n "${DEBUGFS_DIR}" ] || { echo "ERROR: debugfs msi-wmi-platform not found." >&2; ls /sys/kernel/debug/ | grep -i msi >&2; exit 2; }
echo "debugfs: ${DEBUGFS_DIR}"

ls /sys/bus/wmi/devices 2>/dev/null | grep -qi "${GUID}" || {
    echo "ERROR: WMI GUID ${GUID} absent." >&2
    exit 3
}

[ -e "${HRM_W}" ] || { echo "ERROR: msi-ec sysfs absent (${HRM_W})." >&2; exit 4; }

LOG="${OUT}/campaign.log"
exec > >(tee -a "${LOG}") 2>&1
echo "WMI probe campaign — $(date '+%F %T')"
echo "Output: ${OUT}"

# ----------------------------------------------------------------- sample
# Writes the full state <out>/<prefix>-<n>: WMI (all get_*, IPAR 0..3 for
# fan/thermal/temperature), raw ECRAM, hwmon, msi-ec sysfs, sensors.

sample() {
    local prefix="$1" idx="$2"
    local d="${OUT}/${prefix}-${idx}"
    mkdir -p "${d}"

    run_get() {
        local name="$1" ipar="$2" dest="${3:-}"
        [ -e "${DEBUGFS_DIR}/${name}" ] || { echo "${name} absent" >> "${d}/state.txt"; return 1; }
        python3 -c "open('${PAYLOAD}','wb').write(bytes([${ipar}])+bytes(31))" || return 1
        cat "${PAYLOAD}" > "${DEBUGFS_DIR}/${name}" 2>/dev/null || {
            echo "${name} ipar=${ipar}: request rejected" >> "${d}/state.txt"
            return 1
        }
        {
            echo "## ${name} ipar=${ipar}"
            head -c 32 "${DEBUGFS_DIR}/${name}" | od -A n -v -t x1
        } >> "${d}/wmi.txt"
    }

    : > "${d}/wmi.txt"
    run_get "get_ec" 0
    local i
    for i in 0 1 2; do
        run_get "get_fan" "$i"
        run_get "get_temperature" "$i"
        run_get "get_thermal" "$i"
    done
    run_get "get_thermal" 3    # 0x98-0x9E block of the table

    # Raw ECRAM page 0 through ec_sys (read-only if the module is loaded)
    {
        echo "## ECRAM page0 (ec_sys, 256 bytes)"
        if [ -e /sys/kernel/debug/ec/ec0/io ]; then
            cat /sys/kernel/debug/ec/ec0/io | od -A x -v -t x1
        else
            echo "unavailable (ec_sys not loaded)"
        fi
    } > "${d}/ecram.txt"

    # Environmental observation
    for w in /sys/bus/wmi/devices/${GUID}/hwmon/hwmon*; do
        echo "fan1: $(cat "$w/fan1_input" 2>/dev/null) rpm ($(cat "$w/fan1_label" 2>/dev/null))"
        echo "fan2: $(cat "$w/fan2_input" 2>/dev/null) rpm ($(cat "$w/fan2_label" 2>/dev/null))"
    done > "${d}/state.txt"
    echo "fan_mode:   $(cat /sys/devices/platform/msi-ec/fan_mode 2>/dev/null)"  >> "${d}/state.txt"
    echo "shift_mode: $(cat "${HRM_W}" 2>/dev/null)"                            >> "${d}/state.txt"
    echo "cpu temp: $(cat /sys/devices/platform/msi-ec/cpu/realtime_temperature 2>/dev/null) °C" >> "${d}/state.txt"
    echo "gpu temp: $(cat /sys/devices/platform/msi-ec/gpu/realtime_temperature 2>/dev/null) °C" >> "${d}/state.txt"
    if command -v sensors >/dev/null 2>&1; then
        sensors 2>/dev/null | grep -E 'Tctl|CPU|Package|temp1' | head -8 >> "${d}/state.txt"
    fi

    chown -R "${SUDO_USER:-$USER}" "${OUT}" 2>/dev/null || true
    echo "sample ${prefix}-${idx} written (${d})"
}

# ---------------------------------------------------------------- campaign

# Initial state to restore
INITIAL_SHIFT=$(cat "${HRM_W}")
echo "initial shift_mode: ${INITIAL_SHIFT} (restored at the end)"

# 1) Idle — 2 samples spaced apart to measure the live/static mix
echo "=== 1) IDLE ==="
sleep 60
sample idle 1
sleep 30
sample idle 2

# 2) CPU load — simple burners, cleaned by trap
echo "=== 2) UNDER CPU LOAD (2 min at 100 %) ==="
NPROC=$(nproc)
BURN_PIDS=""
cleanup() {
    [ -n "${BURN_PIDS}" ] && kill ${BURN_PIDS} 2>/dev/null
    rm -f "${PAYLOAD}"
    return 0
}
trap cleanup EXIT INT TERM
for _ in $(seq "${NPROC}"); do
    timeout 240 bash -c 'while :; do :; done' &
    BURN_PIDS="${BURN_PIDS} $!"
done
echo "load started on ${NPROC} cores"
sleep 120
sample load 1
sleep 30
sample load 2
kill ${BURN_PIDS} 2>/dev/null
sleep 45

# 3) shift_mode = eco (validated msi-ec sysfs write)
echo "=== 3) SHIFT_MODE = ECO ==="
echo eco > "${HRM_W}"
sleep 45
sample eco 1
sleep 30
sample eco 2

# 4) Restore the initial state
echo "=== 4) RESTORATION (${INITIAL_SHIFT}) ==="
echo "${INITIAL_SHIFT}" > "${HRM_W}"
sleep 45
sample restore 1

cleanup

# ------------------------------------------------------------------ tally
chown -R "${SUDO_USER:-$USER}" "${OUT_ROOT}" 2>/dev/null || true
echo
echo "Campaign finished. Results: ${OUT}"
du -sh "${OUT}"

# Generate a compact summary (changed values vs idle-1) — quick merge
python3 - "${OUT}" > "${OUT}/summary-deltas.txt" <<'PYEOF'
import os, re, sys

def parse(path):
    res = {}
    cur = None
    for line in open(path):
        m = re.match(r"## (\S+) ipar=(\d+)", line)
        if m:
            cur = f"{m.group(1)}-ipar{m.group(2)}"
            res[cur] = []
        elif cur and re.match(r"^ [0-9a-f ]+$", line):
            res[cur] += line.split()
    return res

base_path = os.path.join(sys.argv[1], "idle-1", "wmi.txt")
base = parse(base_path)
print(f"{'group':24s} idle-1 -> delta per sample (different bytes)")
for label in ["idle-2", "load-1", "load-2", "eco-1", "eco-2", "restore-1"]:
    p = os.path.join(sys.argv[1], label, "wmi.txt")
    if not os.path.exists(p):
        continue
    other = parse(p)
    diffs = []
    for k, bv in base.items():
        ov = other.get(k)
        if ov is None: continue
        marked = []
        for i, (b, o) in enumerate(zip(bv, ov)):
            if b != o:
                marked.append(f"[{i}]{b}->{o}({k})")
        if marked:
            diffs.extend(marked[:6])
    print(f"{label:12s} {' '.join(diffs) if diffs else '(no difference)'}")
PYEOF
echo
echo "Delta summary: ${OUT}/summary-deltas.txt"
