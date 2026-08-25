#!/bin/bash
# Compila e instala battery-guardian (LaunchDaemon) + Battery Guardian.app (barra de menú)
# Uso: sudo ./install.sh
set -euo pipefail
cd "$(dirname "$0")"

LABEL="com.batteryguardian"
MENU_LABEL="com.batteryguardian-menu"
REAL_USER="$(stat -f '%Su' /dev/console)"
BIN_SRC="./battery_guardian"
BIN_DST="/usr/local/libexec/battery-guardian"
PLIST_DST="/Library/LaunchDaemons/${LABEL}.plist"
STATE_DIR="/usr/local/var/battery-guardian"

echo "[1/7] Compilando daemon..."
clang -O2 -Wall -Wextra -framework IOKit -framework CoreFoundation \
      -o "${BIN_SRC}" battery_guardian.c

echo "[2/7] Compilando app de barra de menú..."
swiftc -O -o BatteryGuardianMenu BatteryGuardianMenu.swift

echo "[3/7] Instalando daemon en ${BIN_DST}..."
mkdir -p /usr/local/libexec
install -m 0755 "${BIN_SRC}" "${BIN_DST}"

echo "[4/7] Preparando directorio de estado y configuración..."
mkdir -p "${STATE_DIR}"
chown root:wheel "${STATE_DIR}"
chmod 0777 "${STATE_DIR}"
# configuración (no se sobrescribe si ya existe)
mkdir -p /usr/local/etc
if [[ ! -f /usr/local/etc/battery-guardian.conf ]]; then
  install -m 0644 ./config.example /usr/local/etc/battery-guardian.conf
fi

echo "[5/7] Instalando LaunchDaemon..."
install -m 0644 ./com.batteryguardian.plist "${PLIST_DST}"
# limpiar etiqueta de versiones anteriores del proyecto
launchctl bootout system/com.jesusc.battery-guardian 2>/dev/null || true
rm -f /Library/LaunchDaemons/com.jesusc.battery-guardian.plist
launchctl bootout system/"${LABEL}" 2>/dev/null || true
launchctl bootstrap system "${PLIST_DST}" 2>/dev/null || \
  launchctl kickstart -k system/"${LABEL}"

echo "[6/7] Instalando app de barra de menú para ${REAL_USER}..."
APP_DST="/Applications/Battery Guardian.app"
rm -rf "${APP_DST}"
mkdir -p "${APP_DST}/Contents/MacOS"
install -m 0755 BatteryGuardianMenu "${APP_DST}/Contents/MacOS/BatteryGuardianMenu"
install -m 0644 ./com.batteryguardian.app.plist "${APP_DST}/Contents/Info.plist"

# LaunchAgent de usuario (arranque automático con la sesión)
AGENT_PLIST="/Users/${REAL_USER}/Library/LaunchAgents/${MENU_LABEL}.plist"
mkdir -p "/Users/${REAL_USER}/Library/LaunchAgents"
# limpiar etiqueta antigua del proyecto (evita instancias duplicadas)
launchctl bootout "gui/$(id -u "${REAL_USER}")/com.jesusc.battery-guardian-menu" 2>/dev/null || true
rm -f "/Users/${REAL_USER}/Library/LaunchAgents/com.jesusc.battery-guardian-menu.plist"
install -m 0644 ./com.batteryguardian.agent.plist "${AGENT_PLIST}"
chown "${REAL_USER}:staff" "${AGENT_PLIST}"
launchctl bootout "gui/$(id -u "${REAL_USER}")/${MENU_LABEL}" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u "${REAL_USER}")" "${AGENT_PLIST}" 2>/dev/null || true

sleep 2
echo "--- Estado del daemon ---"
launchctl print system/"${LABEL}" 2>/dev/null | grep -E 'state|pid' | head -3 || true
"${BIN_DST}" status || true
echo "--- Estado del agente ---"
launchctl print "gui/$(id -u "${REAL_USER}")/${MENU_LABEL}" 2>/dev/null | grep -E 'state|pid' | head -3 || true
echo "Listo. Deberías ver 'BG?' o el porcentaje en la barra de menú."
echo "Logs del daemon: log show --predicate 'process == \"battery-guardian\"' --last 1h"
