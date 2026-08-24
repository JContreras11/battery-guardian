/*
 * battery-guardian — ciclos de carga responsables para MacBook (Apple Silicon).
 *
 * Mantiene la bateria dentro de la banda [LOW_RESUME .. HIGH_STOP] mientras el
 * cargador este conectado:
 *   - Al llegar a HIGH_STOP (100%) inhibe la carga y desconecta la alimentacion
 *     externa para que el pack se descargue.
 *   - Al bajar a LOW_RESUME (40%) reactiva la carga.
 *
 * Protocolo SMC: user client moderno de AppleSMC (IOServiceOpen tipo 1 +
 * kSMCUserClientOpen + selector kSMCHandleYPCEvent), constantes derivadas de
 * AppleSMC.kext (Apple PowerManagement opensource) y de Battery-Toolkit de
 * Marvin Haeuser (BSD-3-Clause). Llaves: CHTE (ui32) o CH0C para carga;
 * CHIE o CH0J para el adaptador, segun firmware.
 *
 * Este archivo se distribuye bajo GPL v2 (compatible con las referencias).
 *
 * Uso:
 *   battery-guardian run      bucle principal (lo invoca launchd como root)
 *   battery-guardian once     una sola pasada de decision (para pruebas)
 *   battery-guardian status   imprime bateria y estado de las llaves SMC
 *   battery-guardian reset    restaura carga normal
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <syslog.h>
#include <time.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>

#define HIGH_STOP     100
#define LOW_RESUME    40
#define POLL_SECONDS  30
#define HEARTBEAT_EVERY 120 /* ticks (~1 h) */

#define kSMCUserClientOpen  0
#define kSMCUserClientClose 1
#define kSMCHandleYPCEvent  2
#define SMC_CMD_READ_BYTES   5
#define SMC_CMD_WRITE_BYTES  6
#define SMC_CMD_READ_KEYINFO 9
#define SMC_RESULT_SUCCESS   0

/* Directorio compartido con la app de barra de menu (legible por el usuario) */
#define STATE_DIR    "/usr/local/var/battery-guardian"
#define STATE_FILE   STATE_DIR "/state.json"
#define PAUSE_FILE   STATE_DIR "/paused"

typedef char UInt32Char_t[5];

typedef struct {
    char   major;
    char   minor;
    char   build;
    char   reserved[1];
    uint16_t release;
} KeyVers_t;

typedef struct {
    uint16_t version;
    uint16_t length;
    uint32_t cpuPLimit;
    uint32_t gpuPLimit;
    uint32_t memPLimit;
} KeyPLimit_t;

typedef struct {
    uint32_t dataSize;
    uint32_t dataType;
    char     dataAttributes;
} KeyInfo_t;

typedef unsigned char SMCBytes_t[32];

typedef struct {
    uint32_t    key;
    KeyVers_t   vers;
    KeyPLimit_t pLimitData;
    KeyInfo_t   keyInfo;
    char        result;
    char        status;
    char        data8;
    uint32_t    data32;
    SMCBytes_t  bytes;
} SMCKeyData_t;

typedef struct {
    UInt32Char_t key;
    uint32_t     dataSize;
    UInt32Char_t dataType;
    SMCBytes_t   bytes;
} SMCVal_t;

static io_connect_t g_conn = 0;
static volatile sig_atomic_t g_stop = 0;

/* llaves detectadas al arrancar */
static char g_charge_key[5] = {0};
static uint32_t g_charge_size = 0;
static unsigned char g_charge_off_bytes[4] = {0x01, 0, 0, 0};

static char g_adapter_key[5] = {0};
static unsigned char g_adapter_off = 0x08;     /* CHIE: 08; CH0J: 0x20 */

static uint32_t key_to_uint(const char *s)
{
    return ((uint32_t)(unsigned char)s[0] << 24) |
           ((uint32_t)(unsigned char)s[1] << 16) |
           ((uint32_t)(unsigned char)s[2] << 8) |
           ((uint32_t)(unsigned char)s[3]);
}

static kern_return_t smc_call(int index, SMCKeyData_t *in, SMCKeyData_t *out)
{
    size_t insz = sizeof(*in), outsz = sizeof(*out);
    return IOConnectCallStructMethod(g_conn, index, in, insz, out, &outsz);
}

static kern_return_t smc_open(void)
{
    io_iterator_t iter = 0;
    kern_return_t r;

    CFMutableDictionaryRef match = IOServiceMatching("AppleSMC");
    r = IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter);
    if (r != KERN_SUCCESS) return r;

    io_object_t dev = IOIteratorNext(iter);
    IOObjectRelease(iter);
    if (!dev) return kIOReturnNotFound;

    /* user client moderno: tipo 1 */
    r = IOServiceOpen(dev, mach_task_self(), 1, &g_conn);
    IOObjectRelease(dev);
    if (r != KERN_SUCCESS) return r;

    /* apertura explicita del user client */
    r = IOConnectCallMethod(g_conn, kSMCUserClientOpen,
                            NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
    if (r != KERN_SUCCESS) {
        IOServiceClose(g_conn);
        g_conn = 0;
    }
    return r;
}

static void smc_close(void)
{
    if (g_conn) {
        IOConnectCallMethod(g_conn, kSMCUserClientClose,
                            NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
        IOServiceClose(g_conn);
    }
    g_conn = 0;
}

static kern_return_t smc_get_keyinfo(const char *key4, KeyInfo_t *info)
{
    SMCKeyData_t in, out;
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    in.key = key_to_uint(key4);
    in.data8 = SMC_CMD_READ_KEYINFO;
    kern_return_t r = smc_call(kSMCHandleYPCEvent, &in, &out);
    if (r != KERN_SUCCESS) return r;
    if (out.result != SMC_RESULT_SUCCESS) return kIOReturnUnsupported;
    *info = out.keyInfo;
    return KERN_SUCCESS;
}

static kern_return_t smc_read_key(const char *key4, SMCVal_t *val)
{
    KeyInfo_t info;
    kern_return_t r = smc_get_keyinfo(key4, &info);
    if (r != KERN_SUCCESS) return r;

    SMCKeyData_t in, out;
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    memset(val, 0, sizeof *val);
    in.key = key_to_uint(key4);
    in.keyInfo.dataSize = info.dataSize;
    in.data8 = SMC_CMD_READ_BYTES;
    r = smc_call(kSMCHandleYPCEvent, &in, &out);
    if (r != KERN_SUCCESS) return r;
    if (out.result != SMC_RESULT_SUCCESS) return kIOReturnUnsupported;

    val->dataSize = info.dataSize;
    memcpy(val->bytes, out.bytes, sizeof out.bytes);
    return KERN_SUCCESS;
}

/* escribe y verifica releyendo (como Battery-Toolkit) */
static bool smc_write_key(const char *key4, const unsigned char *bytes, uint32_t size)
{
    SMCKeyData_t in, out;
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    in.key = key_to_uint(key4);
    in.keyInfo.dataSize = size;
    in.data8 = SMC_CMD_WRITE_BYTES;
    memcpy(in.bytes, bytes, size);
    kern_return_t r = smc_call(kSMCHandleYPCEvent, &in, &out);
    if (r != KERN_SUCCESS || out.result != SMC_RESULT_SUCCESS) {
        syslog(LOG_ERR, "escritura SMC %s fallo (io=%08x result=%d)",
               key4, r, out.result);
        return false;
    }

    SMCVal_t val;
    if (smc_read_key(key4, &val) != KERN_SUCCESS) return true; /* sin verificacion */
    return memcmp(val.bytes, bytes, size) == 0;
}

/* detecta las llaves soportadas por este firmware */
static bool detect_keys(void)
{
    KeyInfo_t info;

    /* carga: CHTE (ui32, 4 bytes) o CH0C (1 byte) */
    if (smc_get_keyinfo("CHTE", &info) == KERN_SUCCESS &&
        info.dataSize == 4) {
        snprintf(g_charge_key, sizeof g_charge_key, "CHTE");
        g_charge_size = 4;
        g_charge_off_bytes[0] = 0x01; g_charge_off_bytes[1] = 0;
        g_charge_off_bytes[2] = 0;    g_charge_off_bytes[3] = 0;
    } else if (smc_get_keyinfo("CH0C", &info) == KERN_SUCCESS &&
               info.dataSize == 1) {
        snprintf(g_charge_key, sizeof g_charge_key, "CH0C");
        g_charge_size = 1;
        g_charge_off_bytes[0] = 0x01;
    } else {
        return false;
    }

    /* adaptador: CHIE (0x08) o CH0J (0x20), 1 byte */
    if (smc_get_keyinfo("CHIE", &info) == KERN_SUCCESS && info.dataSize == 1) {
        snprintf(g_adapter_key, sizeof g_adapter_key, "CHIE");
        g_adapter_off = 0x08;
    } else if (smc_get_keyinfo("CH0J", &info) == KERN_SUCCESS &&
               info.dataSize == 1) {
        snprintf(g_adapter_key, sizeof g_adapter_key, "CH0J");
        g_adapter_off = 0x20;
    } else {
        g_adapter_key[0] = '\0'; /* sin control de adaptador: solo inhibir carga */
    }
    return true;
}

static bool charge_inhibited(bool *ok)
{
    SMCVal_t val;
    *ok = false;
    unsigned char on[4] = {0, 0, 0, 0};
    if (smc_read_key(g_charge_key, &val) != KERN_SUCCESS) return false;
    *ok = true;
    return memcmp(val.bytes, on, g_charge_size) != 0;
}

static bool set_charge_enabled(bool enable)
{
    unsigned char off_c[4] = {0, 0, 0, 0};
    const unsigned char *cbytes = enable ? off_c : g_charge_off_bytes;
    bool okc = smc_write_key(g_charge_key, cbytes, g_charge_size);

    bool oka = true;
    if (g_adapter_key[0]) {
        unsigned char abytes = enable ? 0x00 : g_adapter_off;
        oka = smc_write_key(g_adapter_key, &abytes, 1);
    }
    return okc && oka;
}

static int read_battery(int *pct, bool *onAC, bool *charging)
{
    *pct = -1; *onAC = false; *charging = false;

    CFTypeRef blob = IOPSCopyPowerSourcesInfo();
    if (!blob) return -1;
    CFArrayRef list = IOPSCopyPowerSourcesList(blob);
    if (!list) { CFRelease(blob); return -1; }

    int found = -1;
    CFIndex n = CFArrayGetCount(list);
    for (CFIndex i = 0; i < n; i++) {
        CFTypeRef src = CFArrayGetValueAtIndex(list, i);
        CFDictionaryRef desc = IOPSGetPowerSourceDescription(blob, src);
        if (!desc) continue;

        CFStringRef type = CFDictionaryGetValue(desc, CFSTR(kIOPSTypeKey));
        if (!type || !CFEqual(type, CFSTR(kIOPSInternalBatteryType))) continue;

        CFNumberRef capNum = CFDictionaryGetValue(desc, CFSTR(kIOPSCurrentCapacityKey));
        int cap = -1;
        if (capNum) CFNumberGetValue(capNum, kCFNumberIntType, &cap);
        *pct = cap;

        CFBooleanRef chg = CFDictionaryGetValue(desc, CFSTR(kIOPSIsChargingKey));
        if (chg) *charging = CFBooleanGetValue(chg);

        CFStringRef st = CFDictionaryGetValue(desc, CFSTR(kIOPSPowerSourceStateKey));
        if (st && CFEqual(st, CFSTR(kIOPSACPowerValue))) *onAC = true;

        found = 0;
    }

    CFRelease(list);
    CFRelease(blob);
    return found;
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* sin SA_RESTART: interrumpe sleep() */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

/* Una pasada: lee bateria + llaves SMC y escribe solo si hay que cambiar. */
static int tick_once(bool allow_writes)
{
    int pct = -1;
    bool onAC = false, charging = false;
    if (read_battery(&pct, &onAC, &charging) != 0) {
        syslog(LOG_ERR, "no se pudo leer la bateria");
        return 1;
    }

    bool paused = (access(PAUSE_FILE, F_OK) == 0);

    bool readOk = false;
    bool inh = charge_inhibited(&readOk);

    int desired = -1; /* -1 = mantener */
    if (paused) {
        desired = 0; /* en pausa: asegurar carga normal */
    } else if (pct >= HIGH_STOP) {
        desired = 1;
    } else if (pct <= LOW_RESUME) {
        desired = 0;
    } else if (readOk && inh) {
        desired = 1; /* banda media ya inhibida */
    }

    if (geteuid() != 0) allow_writes = false;

    bool writeApplied = false;
    if (allow_writes && desired >= 0 && readOk && (inh != (desired == 1))) {
        bool ok = set_charge_enabled(desired == 0);
        writeApplied = ok;
        if (!ok) {
            syslog(LOG_ERR, "fallo al %s carga",
                   desired ? "inhibir" : "restaurar");
        } else {
            syslog(LOG_NOTICE, "bateria %d%% AC=%d cargando=%d -> %s%s",
                   pct, onAC, charging,
                   desired ? "INHIBIR (descargar hasta 40%)"
                           : "CARGAR hasta 100%",
                   paused ? " [EN PAUSA]" : "");
        }
        inh = desired == 1;
    }

    /* publicar estado para la app de barra de menu */
    FILE *f = fopen(STATE_FILE ".tmp", "w");
    if (f) {
        fprintf(f,
            "{\"ts\":%ld,\"percent\":%d,\"ac\":%s,\"charging\":%s,"
            "\"inhibited\":%s,\"paused\":%s}\n",
            (long)time(NULL), pct, onAC ? "true" : "false",
            charging ? "true" : "false",
            (readOk && inh) ? "true" : "false",
            paused ? "true" : "false");
        fclose(f);
        rename(STATE_FILE ".tmp", STATE_FILE);
    }

    printf("{\"ts\":%ld,\"percent\":%d,\"ac\":%s,\"charging\":%s,"
           "\"inhibited\":%s,\"paused\":%s,\"applied\":%s}\n",
           (long)time(NULL), pct, onAC ? "true" : "false",
           charging ? "true" : "false",
           (readOk && inh) ? "true" : "false",
           paused ? "true" : "false",
           writeApplied ? "true" : "false");
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "run";

    if (strcmp(mode, "status") == 0) {
        int pct = -1; bool onAC = false, charging = false;
        if (read_battery(&pct, &onAC, &charging) != 0) {
            fprintf(stderr, "error leyendo bateria\n");
            return 1;
        }
        if (smc_open() != KERN_SUCCESS || !detect_keys()) {
            printf("bateria=%d%% ac=%s cargando=%s SMC=no-disponible\n",
                   pct, onAC ? "si" : "no", charging ? "si" : "no");
            return 0;
        }
        bool ok = false;
        bool inh = charge_inhibited(&ok);
        bool paused = (access(PAUSE_FILE, F_OK) == 0);
        printf("bateria=%d%% ac=%s cargando=%s clave=%s inhibida=%d pausa=%s\n",
               pct, onAC ? "si" : "no", charging ? "si" : "no",
               g_charge_key, ok ? (int)inh : -1, paused ? "si" : "no");
        smc_close();
        return 0;
    }

    if (strcmp(mode, "reset") == 0) {
        if (smc_open() != KERN_SUCCESS || !detect_keys()) {
            fprintf(stderr, "SMC no disponible\n");
            return 1;
        }
        bool ok = set_charge_enabled(true);
        printf("restaurar carga: %s\n", ok ? "ok" : "FALLO");
        smc_close();
        return ok ? 0 : 1;
    }

    if (smc_open() != KERN_SUCCESS) {
        fprintf(stderr, "no se pudo abrir AppleSMC user client\n");
        return 2;
    }
    if (!detect_keys()) {
        fprintf(stderr, "firmware sin llaves de control de carga conocidas\n");
        smc_close();
        return 2;
    }

    install_signals();
    openlog("battery-guardian", LOG_PID | LOG_NDELAY, LOG_DAEMON);
    syslog(LOG_NOTICE, "llaves: carga=%s adaptador=%s",
           g_charge_key, g_adapter_key[0] ? g_adapter_key : "(ninguno)");
    syslog(LOG_NOTICE, "inicio: banda %d%%-%d%%, sondeo cada %ds, uid=%d",
           LOW_RESUME, HIGH_STOP, POLL_SECONDS, geteuid());

    if (strcmp(mode, "once") == 0) {
        int rc = tick_once(true);
        smc_close();
        closelog();
        return rc;
    }

    long since_beat = 0;
    while (!g_stop) {
        tick_once(true);
        if (++since_beat >= HEARTBEAT_EVERY) {
            since_beat = 0;
            syslog(LOG_INFO, "latido ok");
        }
        for (int i = 0; i < POLL_SECONDS && !g_stop; i++)
            sleep(1);
    }

    syslog(LOG_NOTICE, "parando: restaurando carga normal");
    set_charge_enabled(true);
    smc_close();
    closelog();
    return 0;
}
