import AppKit

// Battery Guardian — app de barra de menú para supervisar el demonio
// /usr/local/libexec/battery-guardian (LaunchDaemon com.batteryguardian).
//
// Comunicación por archivos (sin privilegios):
//   estado:  /usr/local/var/battery-guardian/state.json  (escrito por el daemon cada 30 s)
//   pausa:   crear/borrar /usr/local/var/battery-guardian/paused  (la app lo gestiona)
//
// La pausa la ejecuta el daemon (restaura carga normal mientras exista el archivo).

final class AppState {
    static let dir = "/usr/local/var/battery-guardian"
    static let stateFile = dir + "/state.json"
    static let pauseFile = dir + "/paused"

    var percent = -1
    var onAC = false
    var charging = false
    var inhibited = false   // carga inhibida por el guardian
    var paused = false      // pausa manual desde la app
    var daemonAlive = false // state.json con ts < 90 s
    var high = 80
    var low = 20

    static func load() -> AppState {
        let st = AppState()
        guard let raw = try? String(contentsOfFile: stateFile, encoding: .utf8) else {
            return st
        }
        func boolVal(_ key: String) -> Bool {
            raw.range(of: "\"\(key)\":true") != nil
        }
        func intVal(_ key: String) -> Int {
            if let r = raw.range(of: "\"\(key)\":(-?[0-9]+)", options: .regularExpression) {
                return Int(raw[r].components(separatedBy: ":")[1]) ?? -1
            }
            return -1
        }
        st.percent = intVal("percent")
        st.onAC = boolVal("ac")
        st.charging = boolVal("charging")
        st.inhibited = boolVal("inhibited")
        st.paused = boolVal("paused")
        st.high = intVal("high")
        st.low = intVal("low")
        if st.high <= 0 { st.high = 80 }
        if st.low <= 0 { st.low = 20 }
        if let r = raw.range(of: "\"ts\":([0-9]+)", options: .regularExpression) {
            let ts = Double(raw[r].components(separatedBy: ":")[1]) ?? 0
            st.daemonAlive = (Date().timeIntervalSince1970 - ts) < 90
        }
        return st
    }

    var statusText: String {
        guard daemonAlive else { return "Daemon no responde" }
        let band = " (banda \(low)-\(high)%)"
        if paused { return "En pausa — carga libre" }
        if inhibited { return "Inhibido — descargando hasta \(low)%\(band)" }
        if charging { return "Cargando hacia \(high)%" }
        return onAC ? "En \(high)% con cargador (flotando)" : "Sobre batería — guardian en espera"
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    let menu = NSMenu()
    let titleItem = NSMenuItem(title: "Battery Guardian", action: nil, keyEquivalent: "")
    let statusItem = NSMenuItem(title: "…", action: nil, keyEquivalent: "")
    let detailItem = NSMenuItem(title: "", action: nil, keyEquivalent: "")
    let pauseToggle = NSMenuItem(title: "Pausar guardian", action: #selector(togglePause), keyEquivalent: "p")
    var paused = false
    var timer: Timer?

    func applicationDidFinishLaunching(_ n: Notification) {
        item.button?.font = NSFont.monospacedDigitSystemFont(ofSize: 0, weight: .medium)
        item.menu = menu

        titleItem.isEnabled = false
        menu.addItem(titleItem)
        menu.addItem(.separator())
        menu.addItem(statusItem)
        menu.addItem(detailItem)
        menu.addItem(.separator())
        pauseToggle.target = self
        menu.addItem(pauseToggle)

        let openLogs = NSMenuItem(title: "Ver logs del daemon", action: #selector(openLogs), keyEquivalent: "")
        openLogs.target = self
        menu.addItem(openLogs)

        let quit = NSMenuItem(title: "Salir", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        menu.addItem(quit)

        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 10, repeats: true) { [weak self] _ in
            self?.refresh()
        }
    }

    func refresh() {
        let st = AppState.load()
        paused = st.paused

        // Título del ítem: porcentaje, o icono de alerta si el daemon no responde
        if st.daemonAlive && st.percent >= 0 {
            let suffix = st.paused ? " ⏸" : (st.inhibited ? " ↓" : (st.charging ? " ⚡" : " ✓"))
            item.button?.title = " \(st.percent)%\(suffix)"
        } else {
            item.button?.title = " BG?"
        }

        statusItem.title = st.statusText
        let df = DateFormatter()
        df.dateFormat = "HH:mm:ss"
        if let attrs = try? FileManager.default.attributesOfItem(atPath: AppState.stateFile),
           let date = attrs[.modificationDate] as? Date {
            detailItem.title = "Actualizado: \(df.string(from: date))"
        } else {
            detailItem.title = "Sin datos del daemon"
        }

        pauseToggle.title = st.paused ? "Reanudar guardian" : "Pausar guardian (carga libre)"
        pauseToggle.isEnabled = st.daemonAlive
    }

    @objc func togglePause() {
        let fm = FileManager.default
        if paused {
            try? fm.removeItem(atPath: AppState.pauseFile)
        } else {
            if !fm.createFile(atPath: AppState.pauseFile, contents: Data()) {
                // sin permiso de escritura en el dir compartido: avisar
                let a = NSAlert()
                a.messageText = "No se pudo crear el archivo de pausa"
                a.informativeText = "Verifica permisos en \(AppState.dir)"
                a.runModal()
                return
            }
        }
        refresh()
    }

    @objc func openLogs() {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/log")
        p.arguments = ["show", "--predicate", "process == \"battery-guardian\"", "--last", "1h"]
        let pipe = Pipe()
        p.standardOutput = pipe
        try? p.run()
        p.waitUntilExit()
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        let text = String(data: data, encoding: .utf8) ?? "(vacío)"
        let tmp = NSTemporaryDirectory() + "battery-guardian.log"
        try? text.write(toFile: tmp, atomically: true, encoding: .utf8)
        NSWorkspace.shared.open(URL(fileURLWithPath: tmp))
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory) // sin Dock, solo barra de menú
app.run()
