# battery-guardian

Servicio para macOS (Apple Silicon) que mantiene la batería en una banda de
carga **20%–80%** (configurable) mientras el cargador está conectado, para
maximizar la vida útil del pack según la evidencia científica sobre
degradación de Li-ion.

Incluye una **app de barra de menú** con porcentaje en vivo, estado del
guardián y pausa manual. Funciona también **durante el sueño del sistema**.

## ¿Por qué 20–80% y no 40–100%?

La degradación del Li-ion tiene dos frentes: el envejecimiento de calendario
(tiempo a alto voltaje) y el de ciclos (profundidad y techo de carga).

- **Techo 100% (4.20V/celda): 300–500 ciclos.** Techo ~80% (4.06–4.10V):
  **600–1,500 ciclos**. Cada −0.10V en el techo duplica aproximadamente la
  vida (Battery University BU-808, Tabla 4).
- **Estacionar al 100% a 25°C pierde ~20% de capacidad en un año** (80%
  retenido); al 40% de carga retiene 96% (BU-808, Tabla 3). El envejecimiento
  de calendario escala con el voltaje de almacenamiento.
- **Banda 75–25%: ~3,000 ciclos** frente a ~500 de la banda 100–25% en
  pruebas DST (BU-808, Fig. 6/8). La práctica industrial/EV usa techos de
  80–85% con pisos de 20–25%.
- Chalmers University midió **+44–130% de vida esperada** limitando la carga
  a ~50% SOC.

Referencias: [BU-808 — How to Prolong Lithium-based Batteries](https://batteryuniversity.com/article/bu-808-how-to-prolong-lithium-based-batteries)
· [Strategies to limit degradation (ScienceDirect)](https://www.sciencedirect.com/science/article/pii/S2352152X19314227)

**Compromiso:** con techo 80% usas ~80% de la energía por carga. Si un día
necesitas la batería llena, usa "Pausar guardian" desde la app y carga al
100% para el viaje.

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
- Al llegar al **techo** inhibe/desconecta; al bajar al **piso** restaura y
  vuelve a cargar. Sondeo cada 30 s; al detenerse (SIGTERM) restaura la
  carga normal.

### Durante el sueño del sistema

Al dormir, el proceso se congela pero **el SMC mantiene el último estado**.
El demonio además:

1. **Antes de dormir**: garantiza que la carga quede inhibida si la batería
   está por encima del piso (evita que el SMC siga cargando hacia el techo
   durante la noche).
2. **Programa despertares silenciosos** (`kIOPMAutoWake`, cada
   `WAKE_INTERVAL_MIN`, por defecto 60 min) para reevaluar la banda mientras
   duerme y evitar descargas profundas. Solo se programan con cargador
   conectado.
3. Al despertar, reevalúa inmediatamente y reprograma.

## Configuración

`/usr/local/etc/battery-guardian.conf` (se instala desde `config.example`,
no se sobrescribe al actualizar):

```bash
HIGH_STOP=80          # techo de carga (10-100)
LOW_RESUME=20         # piso de recarga (5-90; debe ser < HIGH_STOP-5)
WAKE_INTERVAL_MIN=60  # despertar silencioso durante sueño (15-360)
POLL_SECONDS=30       # sondeo despierto (10-300)
```

Tras editarlo: `sudo launchctl kickstart -k system/com.batteryguardian`

## Comportamiento nativo de macOS (verificado)

macOS ya corta la carga activa al 100% (`IsCharging=No`, adaptador sin
entrega), pero **mantiene la celda estacionada a plena carga** mientras haya
enchufe. Este servicio elimina esa estancia en el techo.

## Uso

```bash
sudo ./install.sh      # compila e instala daemon + app de barra de menú (root)
sudo ./uninstall.sh    # restaura carga y desinstala todo
/usr/local/libexec/battery-guardian once   # una pasada manual (root)
/usr/local/libexec/battery-guardian status
```

## App de barra de menú (Battery Guardian.app)

- Icono con porcentaje en vivo y sufijo de estado: `⚡` cargando,
  `↓` inhibido (descargando hacia el piso), `⏸` en pausa, `✓` flotando/OK,
  `BG?` si el daemon no responde.
- Menú: estado detallado con la banda activa, **Pausar/Reanudar** (crea/borra
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
