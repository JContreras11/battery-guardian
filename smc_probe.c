/*
 * smc_probe — prueba el user client moderno de AppleSMC (tipo 1 + open explícito)
 * y sondea llaves de control de carga. Solo lectura.
 * Protocolo: BSD-3-Clause estilo Battery-Toolkit (mhaeuser), constantes de
 * AppleSMC.kext (Apple opensource PowerManagement).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <IOKit/IOKitLib.h>

#define kSMCUserClientOpen  0
#define kSMCUserClientClose 1
#define kSMCHandleYPCEvent  2
#define SMC_CMD_READ_BYTES  5
#define SMC_CMD_WRITE_BYTES 6
#define SMC_CMD_READ_INDEX  8
#define SMC_CMD_READ_KEYINFO 9

typedef char UInt32Char_t[5];

typedef struct {
    char     major, minor, build, reserved[1];
    uint16_t release;
} KeyVers_t;

typedef struct {
    uint16_t version, length;
    uint32_t cpuPLimit, gpuPLimit, memPLimit;
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
    char        result, status, data8;
    uint32_t    data32;
    SMCBytes_t  bytes;
} SMCKeyData_t;

static io_connect_t g_conn;

static uint32_t key_to_uint(const char *s)
{
    return ((uint32_t)(unsigned char)s[0] << 24) |
           ((uint32_t)(unsigned char)s[1] << 16) |
           ((uint32_t)(unsigned char)s[2] << 8) |
           ((uint32_t)(unsigned char)s[3]);
}

static void uint_to_key(char *dst, uint32_t v)
{
    dst[0] = (char)(v >> 24); dst[1] = (char)(v >> 16);
    dst[2] = (char)(v >> 8);  dst[3] = (char)v; dst[4] = 0;
}

static kern_return_t smc_call(int idx, SMCKeyData_t *in, SMCKeyData_t *out)
{
    size_t i = sizeof(*in), o = sizeof(*out);
    return IOConnectCallStructMethod(g_conn, idx, in, i, out, &o);
}

/* retorna 0 OK; -1 fallo IO; -2 result SMC != 0 */
static int probe_key(const char *name)
{
    SMCKeyData_t in, out;
    memset(&in, 0, sizeof in); memset(&out, 0, sizeof out);
    in.key = key_to_uint(name);
    in.data8 = SMC_CMD_READ_KEYINFO;
    kern_return_t r = smc_call(kSMCHandleYPCEvent, &in, &out);
    if (r != KERN_SUCCESS) return -1;
    if (out.result != 0) return -2;

    uint32_t ds = out.keyInfo.dataSize;
    uint32_t dt = out.keyInfo.dataType;
    char tn[5];
    tn[0]=(char)(dt>>24); tn[1]=(char)(dt>>16); tn[2]=(char)(dt>>8); tn[3]=(char)dt; tn[4]=0;

    memset(&in, 0, sizeof in); memset(&out, 0, sizeof out);
    in.key = key_to_uint(name);
    in.keyInfo.dataSize = ds;
    in.data8 = SMC_CMD_READ_BYTES;
    r = smc_call(kSMCHandleYPCEvent, &in, &out);
    if (r != KERN_SUCCESS) return -1;
    if (out.result != 0) return -2;

    printf("%s [%s ds=%u attrs=%02x] valor: ", name, tn, ds,
           (unsigned char)out.keyInfo.dataAttributes & 0xFF);
    for (uint32_t b = 0; b < ds && b < 8; b++) printf("%02x", out.bytes[b]);
    printf("\n");
    return 0;
}

int main(void)
{
    io_iterator_t iter;
    CFMutableDictionaryRef match = IOServiceMatching("AppleSMC");
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) != KERN_SUCCESS) {
        fprintf(stderr, "no AppleSMC service\n");
        return 1;
    }
    io_object_t dev = IOIteratorNext(iter);
    IOObjectRelease(iter);
    if (!dev) { fprintf(stderr, "no SMC device\n"); return 1; }

    kern_return_t r = IOServiceOpen(dev, mach_task_self(), 1, &g_conn);
    IOObjectRelease(dev);
    if (r != KERN_SUCCESS) { fprintf(stderr, "IOServiceOpen tipo1: %08x\n", r); return 1; }

    /* apertura explícita del user client */
    r = IOConnectCallMethod(g_conn, kSMCUserClientOpen,
                            NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
    printf("open(userclient) = %08x\n", r);
    if (r != KERN_SUCCESS) { IOServiceClose(g_conn); return 1; }

    const char *keys[] = {"#KEY", "CH0C", "CHTE", "CHIE", "CH0J", "CH0I",
                          "B0CL", "ACEN", "CHBI", "BCLM", NULL};
    for (int i = 0; keys[i]; i++) {
        int pr = probe_key(keys[i]);
        if (pr == 0)      printf("  -> SOPORTADA\n");
        else if (pr == -2) printf("%s -> existe pero result!=0\n", keys[i]);
        else               printf("%s -> fallo IO\n", keys[i]);
    }

    IOConnectCallMethod(g_conn, kSMCUserClientClose,
                        NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
    IOServiceClose(g_conn);
    return 0;
}
