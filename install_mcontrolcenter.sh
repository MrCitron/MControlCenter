#!/usr/bin/env bash
# install_mcontrolcenter.sh — installation locale complète (démon + app + autostart + module noyau)
# Fork dédié : MSI Summit E16 Flip A11UCT (MS-1591)
#
# À exécuter depuis la RACINE de ce dépôt. Il installe :
#   1. le démon privilégié et l'application depuis ./build ;
#   2. les fichiers d'activation D-Bus du démon (activation à la demande) ;
#   3. le .desktop + icône + entrée autostart XDG ;
#   4. le module noyau patché (wmi-fan-curve/msi-wmi-platform.ko) dans
#      /lib/modules/<kernel>/updates/ (prioritaire sur le module stock)
#      avec l'option allow_curve_writes=Y (consentement persistant) ;
#   5. la carte des modules (depmod).
#
# Usage : sudo ./install_mcontrolcenter.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD="${SCRIPT_DIR}/build"
MODSRC="${SCRIPT_DIR}/wmi-fan-curve/msi-wmi-platform.ko"
KVER="$(uname -r)"
KDIR="/lib/modules/${KVER}/updates/msi-wmi-platform"

fail() { echo "ERREUR : $*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || fail "exécuter avec sudo"
[ -x "${BUILD}/mcontrolcenter" ] || fail "binaire app absent (${BUILD}/mcontrolcenter) — lancer 'cmake -B build -S . && cmake --build build' d'abord"
[ -x "${BUILD}/helper/mcontrolcenter-helper" ] || fail "binaire démon absent (${BUILD}/helper/mcontrolcenter-helper)"
[ -f "${MODSRC}" ] || fail "module noyau patché absent (${MODSRC}) — lancer 'make' dans wmi-fan-curve/ d'abord"

echo "=== 1) Binaires ==="
install -m 755 "${BUILD}/mcontrolcenter" /usr/local/bin/mcontrolcenter
install -m 755 "${BUILD}/helper/mcontrolcenter-helper" /usr/libexec/mcontrolcenter-helper
echo "   /usr/local/bin/mcontrolcenter"
echo "   /usr/libexec/mcontrolcenter-helper"

echo "=== 2) Activation D-Bus du démon ==="
install -m 644 "${SCRIPT_DIR}/src/helper/mcontrolcenter.helper.service" \
    /usr/share/dbus-1/system-services/mcontrolcenter.helper.service
install -m 644 "${SCRIPT_DIR}/src/helper/mcontrolcenter-helper.conf" \
    /usr/share/dbus-1/system.d/mcontrolcenter-helper.conf
echo "   /usr/share/dbus-1/system-services/ + system.d/"

echo "=== 3) Application : desktop, icône, autostart ==="
install -m 644 "${SCRIPT_DIR}/resources/mcontrolcenter.desktop" /usr/share/applications/
install -m 644 "${SCRIPT_DIR}/src/images/mcontrolcenter.png" /usr/share/pixmaps/mcontrolcenter.png
install -m 644 "${SCRIPT_DIR}/resources/mcontrolcenter.desktop" /etc/xdg/autostart/
update-desktop-database 2>/dev/null || true
echo "   /usr/share/applications + /usr/share/pixmaps + /etc/xdg/autostart/"

echo "=== 4) Pilote noyau patché (persistant) ==="
mkdir -p "${KDIR}"
install -m 644 "${MODSRC}" "${KDIR}/msi-wmi-platform.ko"
printf '%s\n' "options msi_wmi_platform allow_curve_writes=Y" \
    > /etc/modprobe.d/msi-fan-curve-writes.conf
depmod -a "${KVER}"
echo "   module : ${KDIR} (updates/ prioritaire sur le module du stock)"
grep -H "allow_curve_writes" /etc/modprobe.d/msi-fan-curve-writes.conf | sed 's/^/   /'

echo
echo "=== VÉRIFICATION FINALE ==="
file /usr/local/bin/mcontrolcenter /usr/libexec/mcontrolcenter-helper | sed 's/^/   /'
MKO_PATH="$(modinfo -n msi_wmi_platform 2>/dev/null || echo '?')"
echo "   module actif au prochain boot : ${MKO_PATH}"
case "${MKO_PATH}" in
    *updates*) echo "   ✓ le module patché (updates/) est prioritaire" ;;
    *)         echo "   ⚠ le module du stock reste prioritaire — re-vérifier depmod" ;;
esac

echo
echo "Installation terminée. Pour activer sans reboot :"
echo "  sudo rmmod msi_wmi_platform && sudo modprobe msi_wmi_platform"
echo "La session suivante démarrera l'application automatiquement."
