#!/usr/bin/env bash
# install_mcontrolcenter.sh — complete local installation (daemon + app + autostart + kernel module)
# Dedicated fork: MSI Summit E16 Flip A11UCT (MS-1591)
#
# Run it from the ROOT of this repository. It installs:
#   1. the privileged daemon and the application from ./build ;
#   2. the D-Bus activation files of the daemon (on-demand activation) ;
#   3. the .desktop file + icon + XDG autostart entry ;
#   4. the patched kernel module (wmi-fan-curve/msi-wmi-platform.ko) into
#      /lib/modules/<kernel>/updates/ (takes priority over the stock module)
#      with the allow_curve_writes=Y option (persistent consent) ;
#   5. the module dependency map (depmod).
#
# Usage: sudo ./install_mcontrolcenter.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD="${SCRIPT_DIR}/build"
MODSRC="${SCRIPT_DIR}/wmi-fan-curve/msi-wmi-platform.ko"
KVER="$(uname -r)"
KDIR="/lib/modules/${KVER}/updates/msi-wmi-platform"

fail() { echo "ERROR: $*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || fail "run with sudo"
[ -x "${BUILD}/mcontrolcenter" ] || fail "app binary missing (${BUILD}/mcontrolcenter) — run 'cmake -B build -S . && cmake --build build' first"
[ -x "${BUILD}/helper/mcontrolcenter-helper" ] || fail "daemon binary missing (${BUILD}/helper/mcontrolcenter-helper)"
[ -f "${MODSRC}" ] || fail "patched kernel module missing (${MODSRC}) — run 'make' in wmi-fan-curve/ first"

echo "=== 1) Binaries ==="
install -m 755 "${BUILD}/mcontrolcenter" /usr/local/bin/mcontrolcenter
install -m 755 "${BUILD}/helper/mcontrolcenter-helper" /usr/libexec/mcontrolcenter-helper
echo "   /usr/local/bin/mcontrolcenter"
echo "   /usr/libexec/mcontrolcenter-helper"

echo "=== 2) Daemon D-Bus activation ==="
install -m 644 "${SCRIPT_DIR}/src/helper/mcontrolcenter.helper.service" \
    /usr/share/dbus-1/system-services/mcontrolcenter.helper.service
install -m 644 "${SCRIPT_DIR}/src/helper/mcontrolcenter-helper.conf" \
    /usr/share/dbus-1/system.d/mcontrolcenter-helper.conf
echo "   /usr/share/dbus-1/system-services/ + system.d/"

echo "=== 3) Application: desktop file, icon, autostart ==="
install -m 644 "${SCRIPT_DIR}/resources/mcontrolcenter.desktop" /usr/share/applications/
install -m 644 "${SCRIPT_DIR}/src/images/mcontrolcenter.png" /usr/share/pixmaps/mcontrolcenter.png
install -m 644 "${SCRIPT_DIR}/resources/mcontrolcenter.desktop" /etc/xdg/autostart/
update-desktop-database 2>/dev/null || true
echo "   /usr/share/applications + /usr/share/pixmaps + /etc/xdg/autostart/"

echo "=== 4) Patched kernel module (persistent) ==="
mkdir -p "${KDIR}"
install -m 644 "${MODSRC}" "${KDIR}/msi-wmi-platform.ko"
printf '%s\n' "options msi_wmi_platform allow_curve_writes=Y" \
    > /etc/modprobe.d/msi-fan-curve-writes.conf
depmod -a "${KVER}"
echo "   module: ${KDIR} (updates/ takes priority over the stock module)"
grep -H "allow_curve_writes" /etc/modprobe.d/msi-fan-curve-writes.conf | sed 's/^/   /'

echo
echo "=== FINAL VERIFICATION ==="
file /usr/local/bin/mcontrolcenter /usr/libexec/mcontrolcenter-helper | sed 's/^/   /'
MKO_PATH="$(modinfo -n msi_wmi_platform 2>/dev/null || echo '?')"
echo "   module active at next boot: ${MKO_PATH}"
case "${MKO_PATH}" in
    *updates*) echo "   ✓ the patched module (updates/) takes priority" ;;
    *)         echo "   ⚠ the stock module still takes priority — check depmod" ;;
esac

echo
echo "Installation complete. To activate without a reboot:"
echo "  sudo rmmod msi_wmi_platform && sudo modprobe msi_wmi_platform"
echo "The next session will start the application automatically."
