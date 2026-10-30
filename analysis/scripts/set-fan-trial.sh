#!/usr/bin/env bash
# set-fan-trial.sh — CONTROLLED trial write of the fan1 duty curve
# Machine: MSI Summit E16 Flip A11UCT (MS-1591, EC 1591EMS1.108)
#
# Guarantees:
#   * refuses to run if the module is not loaded with allow_curve_writes=1
#     (double safety guard with the driver);
#   * uses ONLY the sysfs interface of the driver (performs one full WMAM
#     SET_FAN window write — never a raw EC write, never debugfs set_*);
#   * modifies a SINGLE point (+3 on the point 1 of fan1, low-temperature
#     zone);
#   * restores the original values at the end AND on Ctrl-C;
#   * continuous monitoring for ~60 s: CPU temp, RPM, live duty.
#
# Usage: sudo ./set-fan-trial.sh

set -u

GUID="ABBC0F6E-8EA1-11D1-00A0-C90629100000"
BASE="/sys/bus/wmi/devices/${GUID}"
MSI_EC="/sys/devices/platform/msi-ec"
LOG="$(pwd)/set-fan-trial-$(date '+%Y%m%d-%H%M%S').log"

f1="${BASE}/fan1_duty_curve"
f2="${BASE}/fan2_duty_curve"

exec > >(tee -a "${LOG}") 2>&1

fail() { echo "ERROR: $*" >&2; exit 1; }

# --- prerequisites -------------------------------------------------------
[ "$(id -u)" -eq 0 ] || fail "run with sudo"
[ -r "$f1" ] || fail "fan1_duty_curve absent — is the patched driver loaded? (see README.md)"
[ -r "$MSI_EC/fan_mode" ] || true

ALLOWED=$(cat /sys/module/msi_wmi_platform/parameters/allow_curve_writes 2>/dev/null || echo N)
[ "$ALLOWED" = "Y" ] || fail "allow_curve_writes=$ALLOWED — reload the module with allow_curve_writes=1 (see README). No write will be attempted."

echo "=== STATE BEFORE ($(date '+%T')) ==="
snap() {
    echo "fan1_duty_curve : $(cat "$f1" 2>/dev/null)"
    echo "fan2_duty_curve : $(cat "$f2" 2>/dev/null)"
    echo "fan1_live_duty  : $(cat "${BASE}/fan1_live_duty" 2>/dev/null)"
    echo "fan2_live_duty  : $(cat "${BASE}/fan2_live_duty" 2>/dev/null)"
    echo "fan1_temp_curve : $(cat "${BASE}/fan1_temp_curve" 2>/dev/null)"
    echo "fan2_temp_curve : $(cat "${BASE}/fan2_temp_curve" 2>/dev/null)"
    for h in "${BASE}"/hwmon/hwmon*; do
        echo "RPM fan1/fan2   : $(cat "$h/fan1_input" 2>/dev/null) / $(cat "$h/fan2_input" 2>/dev/null) rpm"
    done
    echo "msi-ec fan_mode : $(cat "$MSI_EC/fan_mode" 2>/dev/null || echo 'n/a')  shift_mode: $(cat "$MSI_EC/shift_mode" 2>/dev/null || echo 'n/a')"
    echo "CPU temp (msi-ec): $(cat "$MSI_EC/cpu/realtime_temperature" 2>/dev/null || echo 'n/a') °C"
}
snap

echo
echo "The test will write +3 on the point 1 of the fan1 curve (low-temperature zone),"
echo "monitor for 60 s, THEN RESTORE the original values."
echo "Instructions: laptop plugged in, normal fans, no other EC modification in progress."
printf 'Do you confirm writing this curve point? [yes/no] '
read -r ANSWER
case "$ANSWER" in
    yes|YES|y|Y) ;;
    *) echo "Cancelled — no write performed."; exit 0 ;;
esac

# --- trial ---------------------------------------------------------------
BEFORE="$(cat "$f1")"
read -r -a ORIG <<< "$BEFORE"
[ "${#ORIG[@]}" -eq 7 ] || fail "fan1 curve unreadable (content: '$BEFORE')"

TARGET=("${ORIG[@]}")
TARGET[0]=$((ORIG[0] + 3))
NEW="${TARGET[*]}"

restore() {
    echo "[ROLLBACK] restoring: $ORIG_STR"
    echo "$ORIG_STR" > "$f1" 2>/dev/null || echo "!! restoration impossible"
}
ORIG_STR="${ORIG[*]}"
trap restore EXIT INT TERM

echo "Writing: $NEW   (orig: $ORIG_STR, $(date '+%T'))"
echo "$NEW" > "$f1" || fail "write rejected by the driver"

sleep 2
AFTER="$(cat "$f1")"
echo "Immediate readback: $AFTER"
[ "$AFTER" = "$NEW" ] || echo "!! IMMEDIATE MISMATCH — the EC refused/overwrote the change"

# --- 60 s monitoring -----------------------------------------------------
echo "=== MONITORING (60 s, log: ${LOG}) ==="
for i in $(seq 1 60); do
    T=$(cat "$MSI_EC/cpu/realtime_temperature" 2>/dev/null || echo '?')
    R1=$(cat "$BASE"/hwmon/hwmon*/fan1_input 2>/dev/null | head -1 || echo '?')
    R2=$(cat "$BASE"/hwmon/hwmon*/fan2_input 2>/dev/null | head -1 || echo '?')
    D1=$(cat "${BASE}/fan1_live_duty" 2>/dev/null || echo '?')
    printf "t=%02ds temp_cpu=%s°C rpm1=%s rpm2=%s live_duty1=%s\n" "$i" "$T" "$R1" "$R2" "$D1"
    sleep 1
done

echo "Readback after 60 s: $(cat "$f1")"

# --- restoration ----------------------------------------------------------
echo "$ORIG_STR" > "$f1" || echo "!! restoration write failed"
sleep 2
FINAL="$(cat "$f1")"
if [ "$FINAL" = "$BEFORE" ]; then
    echo "Restoration verified: $FINAL"
    echo "=== TRIAL COMPLETE — EC state identical to before the trial ==="
else
    echo "!! PARTIAL restoration: '$FINAL' (expected '$BEFORE')"
    echo "!! Re-run the manual restoration: echo \"$ORIG_STR\" > $f1"
fi

trap - EXIT INT TERM
exit 0
