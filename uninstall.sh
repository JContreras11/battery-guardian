#!/bin/bash
# Desinstala battery-guardian (daemon + app de menú) y restaura la carga normal
# Uso: sudo ./uninstall.sh
set -euo pipefail

LABEL="com.batteryguardian"
MENU_LABEL="com.batteryguardian-menu"
REAL_USER="$(stat -f '%Su' /dev/console)"
UID_N="$(id -u "${REAL_USER}")"
BIN_DST="/usr/local/libexec/battery-guardian"
PLIST_DST="/Library/LaunchDaemons/${LABEL}.plist"
STATE_DIR="/usr/local/var/battery-guardian"

if [[ $EUID -ne 0 ]]; then
  echo "Ejecuta: sudo $0" >&2
  exit 1
fi

# 1. parar y borrar el agente de menú
launchctl bootout "gui/${UID_N}/${MENU_LABEL}" 2>/dev/null || true
rm -f "/Users/${REAL_USER}/Library/LaunchAgents/${MENU_LABEL}.plist"
rm -rf "/Applications/Battery Guardian.app"

# 2. parar el daemon y restaurar carga normal ANTES de borrar el binario
if launchctl print "system/${LABEL}" &>/dev/null; then
  launchctl bootout system/"${LABEL}" || true
fi
# etiqueta antigua del proyecto, si existiera
launchctl bootout system/com.jesusc.battery-guardian 2>/dev/null || true
[[ -x ${BIN_DST} ]] && "${BIN_DST}" reset

# 3. borrar restos
rm -f "${PLIST_DST}" /Library/LaunchDaemons/com.jesusc.battery-guardian.plist \
      "${BIN_DST}" /var/log/battery-guardian.err.log
rm -rf "${STATE_DIR}"
echo "battery-guardian desinstalado; carga restaurada al comportamiento por defecto."
