# battery-guardian

Servicio para macOS (Apple Silicon) que mantiene la batería en la banda
**40%–100%** mientras el cargador está conectado, para reducir el tiempo que el
pack pasa a tensión alta (100%), principal factor de desgaste químico.

Incluye una **app de barra de menú** con porcentaje en vivo, estado del
guardián y pausa manual.

## Instalación desde GitHub

```bash
git clone https://github.com/JContreras11/battery-guardian.git
cd battery-guardian
sudo ./install.sh     # compila (clang + swiftc) e instala daemon + app de menú
```

Requisitos: Xcode Command Line Tools (`xcode-select --install`).
Desinstalación: `sudo ./uninstall.sh`.

> Sin firma de desarrollador Apple: la app de menú no está notarizada; si
> macOS la bloquea la primera vez, autorízala en Ajustes → Privacidad y
> Seguridad → "Abrir igualmente".

## Cómo funciona

- Lee el porcentaje real vía IOKit (`IOPSCopyPowerSourcesInfo`), igual que
  `pmset -g batt`.
- Habla con el SMC por el **user client moderno** (IOServiceOpen tipo 1 +
  apertura explícita). El firmware de macOS 26 bloqueó el interfaz legado
  (CH0C/CH0I devuelven kIOReturnUnsupported).
- Detecta automáticamente las llaves de este firmware:
  - Carga: `CHTE` (ui32, 4 bytes; 0x01 en el primer byte inhibe) o `CH0C`.
  - Adaptador: `CHIE` (0x08 desconecta la alimentación externa) o `CH0J`.
- Cada escritura se verifica releyendo el valor (como Battery-Toolkit).
- Al llegar a **100%** inhibe/desconecta; al bajar a **40%** restaura y vuelve
  a cargar. Sondeo cada 30 s; al detenerse (SIGTERM) restaura la carga normal.

## Comportamiento nativo de macOS (verificado)

macOS ya corta la carga activa al 100% (`IsCharging=No`, adaptador sin entrega),
pero **mantiene la celda estacionada a plena carga** mientras haya enchufe. Este
servicio elimina esa estancia en 100%.

## Uso

```bash
sudo ./install.sh      # compila e instala daemon + app de barra de menú (root)
sudo ./uninstall.sh    # restaura carga y desinstala todo
/usr/local/libexec/battery-guardian once   # una pasada manual (root)
/usr/local/libexec/battery-guardian status
```

## App de barra de menú (Battery Guardian.app)

- Icono con porcentaje en vivo y sufijo de estado: `⚡` cargando,
  `↓` inhibido (descargando hacia 40%), `⏸` en pausa, `✓` flotando/OK,
  `BG?` si el daemon no responde.
- Menú: estado detallado, **Pausar/Reanudar** (crea/borra
  `/usr/local/var/battery-guardian/paused`; el daemon restaura carga normal
  en pausa), ver logs y salir.
- Arranque automático con la sesión vía LaunchAgent
  `com.batteryguardian-menu`.
- Comunicación sin privilegios por archivos en `/usr/local/var/battery-guardian/`
  (`state.json` escrito por el daemon cada 30 s).

## Notas

- En modo clamshell con monitor externo, la desconexión del adaptador puede
  apagar la pantalla durante la fase de descarga (limitación conocida del
  hardware).
- La escritura SMC requiere root; por eso corre como LaunchDaemon.
- `smc_probe` es una herramienta de diagnóstico independiente: sondea qué
  llaves de control de carga expone tu firmware.
- Protocolo SMC derivado de AppleSMC.kext (Apple PowerManagement opensource) y
  de Battery-Toolkit de Marvin Häuser (BSD-3-Clause); licencia de este
  proyecto: GPL v2.
