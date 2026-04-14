/*
 * LD_PRELOAD shim to intercept NvIsp* calls from libnvisp_v3.so
 *
 * Build:
 *   $CC --sysroot=$SYSROOT -shared -fPIC -o isp_shim.so isp_shim.c -ldl
 *
 * Usage:
 *   LD_PRELOAD=/data/local/tmp/isp_shim.so mediaserver
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <dlfcn.h>
#include <string.h>

#define LOG(fmt, ...) fprintf(stderr, "[ISP_SHIM] " fmt "\n", ##__VA_ARGS__)

/* Dump raw memory as hex */
static void hexdump(const char *label, const void *ptr, int size) {
    const uint8_t *p = ptr;
    LOG("%s (%d bytes @ %p):", label, size, ptr);
    for (int i = 0; i < size; i += 16) {
        char line[80];
        int n = 0;
        n += sprintf(line + n, "  %04x: ", i);
        for (int j = 0; j < 16 && i + j < size; j++)
            n += sprintf(line + n, "%02x ", p[i + j]);
        LOG("%s", line);
    }
}

/* NvIspProcessFrame(hIsp, pInput, pOutput, ...) */
typedef uint32_t (*NvIspProcessFrame_t)(void *, void *, void *, void *, void *);
static NvIspProcessFrame_t real_ProcessFrame = NULL;

uint32_t NvIspProcessFrame(void *hIsp, void *pInput, void *pOutput,
                           void *pParam3, void *pParam4) {
    if (!real_ProcessFrame) {
        real_ProcessFrame = dlsym(RTLD_NEXT, "NvIspProcessFrame");
        if (!real_ProcessFrame) {
            LOG("ERROR: cannot find real NvIspProcessFrame");
            return 0xDEAD;
        }
    }

    LOG("=== NvIspProcessFrame ===");
    LOG("  hIsp=%p pInput=%p pOutput=%p p3=%p p4=%p",
        hIsp, pInput, pOutput, pParam3, pParam4);

    if (pInput) hexdump("INPUT surface", pInput, 128);
    if (pOutput) hexdump("OUTPUT surface", pOutput, 128);
    if (pParam3) hexdump("PARAM3", pParam3, 64);
    if (pParam4) hexdump("PARAM4", pParam4, 64);

    uint32_t ret = real_ProcessFrame(hIsp, pInput, pOutput, pParam3, pParam4);
    LOG("  → result: 0x%x", ret);
    return ret;
}

/* NvIspOpen / NvIspHwSettingsCreate for init tracing */
typedef uint32_t (*NvIspOpen_t)(void *, void *, void *);
static NvIspOpen_t real_Open = NULL;

uint32_t NvIspOpen(void *a, void *b, void *c) {
    if (!real_Open) real_Open = dlsym(RTLD_NEXT, "NvIspOpen");
    LOG("=== NvIspOpen(%p, %p, %p) ===", a, b, c);
    uint32_t ret = real_Open(a, b, c);
    LOG("  → 0x%x", ret);
    return ret;
}

typedef uint32_t (*NvIspHwSettingsCreate_t)(void *, void *);
static NvIspHwSettingsCreate_t real_HwCreate = NULL;

uint32_t NvIspHwSettingsCreate(void *a, void *b) {
    if (!real_HwCreate) real_HwCreate = dlsym(RTLD_NEXT, "NvIspHwSettingsCreate");
    LOG("=== NvIspHwSettingsCreate(%p, %p) ===", a, b);
    uint32_t ret = real_HwCreate(a, b);
    LOG("  → 0x%x, *b=%p", ret, b ? *(void**)b : NULL);
    if (b && *(void**)b)
        hexdump("HwSettings object", *(void**)b, 256);
    return ret;
}

typedef uint32_t (*NvIspHwSettingsApply_t)(void *);
static NvIspHwSettingsApply_t real_HwApply = NULL;

uint32_t NvIspHwSettingsApply(void *a) {
    if (!real_HwApply) real_HwApply = dlsym(RTLD_NEXT, "NvIspHwSettingsApply");
    LOG("=== NvIspHwSettingsApply(%p) ===", a);
    uint32_t ret = real_HwApply(a);
    LOG("  → 0x%x", ret);
    return ret;
}
