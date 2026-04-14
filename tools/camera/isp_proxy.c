/*
 * Proxy libnvisp_v3.so — intercepts NvIsp* calls, logs parameters,
 * forwards to real library loaded from libnvisp_v3_real.so.
 *
 * Install:
 *   mv /system/vendor/lib/libnvisp_v3.so /system/vendor/lib/libnvisp_v3_real.so
 *   cp isp_proxy.so /system/vendor/lib/libnvisp_v3.so
 *
 * Build:
 *   $CC --sysroot=$SYSROOT -std=gnu99 -shared -fPIC -o isp_proxy.so isp_proxy.c -ldl -llog
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <dlfcn.h>
#include <android/log.h>

#define TAG "ISP_PROXY"
#define LOG(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static void *real_lib = NULL;

static void ensure_real(void) {
    if (!real_lib) {
        real_lib = dlopen("/system/vendor/lib/libnvisp_v3_real.so", RTLD_NOW);
        if (!real_lib) LOG("FATAL: cannot load real lib: %s", dlerror());
    }
}

static void *get_real(const char *name) {
    ensure_real();
    void *sym = dlsym(real_lib, name);
    if (!sym) LOG("WARN: symbol %s not found", name);
    return sym;
}

static void hexdump(const char *label, const void *ptr, int size) {
    if (!ptr) { LOG("%s: NULL", label); return; }
    const uint8_t *p = ptr;
    char buf[256];
    for (int i = 0; i < size && i < 256; i += 16) {
        int n = 0;
        n += sprintf(buf + n, "%s +%03x:", label, i);
        for (int j = 0; j < 16 && i + j < size; j++)
            n += sprintf(buf + n, " %02x", p[i + j]);
        LOG("%s", buf);
    }
}

/* ---- Intercepted functions ---- */

typedef uint32_t (*FnOpen)(uint32_t, uint32_t, void**);
uint32_t NvIspOpen(uint32_t rm, uint32_t inst, void **handle) {
    FnOpen fn = get_real("NvIspOpen");
    LOG("NvIspOpen(rm=0x%x, inst=%u, handle=%p)", rm, inst, handle);
    uint32_t r = fn(rm, inst, handle);
    LOG("  -> 0x%x, *handle=%p", r, handle ? *handle : NULL);
    return r;
}

typedef uint32_t (*FnClose)(void*);
uint32_t NvIspClose(void *h) {
    LOG("NvIspClose(%p)", h);
    return ((FnClose)get_real("NvIspClose"))(h);
}

typedef uint32_t (*FnHwCreate)(void*, void**);
uint32_t NvIspHwSettingsCreate(void *h, void **settings) {
    FnHwCreate fn = get_real("NvIspHwSettingsCreate");
    LOG("NvIspHwSettingsCreate(%p)", h);
    uint32_t r = fn(h, settings);
    LOG("  -> 0x%x, *settings=%p", r, settings ? *settings : NULL);
    return r;
}

typedef uint32_t (*FnHwApply)(void*);
uint32_t NvIspHwSettingsApply(void *s) {
    LOG("NvIspHwSettingsApply(%p)", s);
    return ((FnHwApply)get_real("NvIspHwSettingsApply"))(s);
}

typedef uint32_t (*FnSetConfig)(void*, uint32_t, uint32_t, uint32_t);
uint32_t NvIspSetConfiguration(void *h, uint32_t a, uint32_t b, uint32_t c) {
    LOG("NvIspSetConfiguration(%p, %u, 0x%x, 0x%x)", h, a, b, c);
    return ((FnSetConfig)get_real("NvIspSetConfiguration"))(h, a, b, c);
}

/* ProcessFrame — the key function. Dump ALL args. */
typedef uint32_t (*FnProcessFrame)(void*, uint32_t, uint32_t, uint32_t,
    uint32_t, uint32_t, void*, uint32_t, void*, void*, void*, void*, void*);

uint32_t NvIspProcessFrame(void *handle, uint32_t mode,
    uint32_t crop_x1, uint32_t crop_y1,
    uint32_t crop_x2, uint32_t crop_y2,
    void *input_surf, uint32_t reserved,
    void *output_cfg, void *fence, void *fence_val,
    void *status, void *frame_count)
{
    FnProcessFrame fn = get_real("NvIspProcessFrame");
    LOG("=== NvIspProcessFrame ===");
    LOG("  handle=%p mode=%u crop=(%u,%u,%u,%u)",
        handle, mode, crop_x1, crop_y1, crop_x2, crop_y2);
    LOG("  input_surf=%p reserved=%u output_cfg=%p", input_surf, reserved, output_cfg);
    LOG("  fence=%p fence_val=%p status=%p count=%p", fence, fence_val, status, frame_count);

    if (input_surf) hexdump("INPUT", input_surf, 64);
    if (output_cfg) hexdump("OUTPUT", output_cfg, 160);

    uint32_t r = fn(handle, mode, crop_x1, crop_y1,
                    crop_x2, crop_y2, input_surf, reserved,
                    output_cfg, fence, fence_val, status, frame_count);

    LOG("  -> err=0x%x", r);
    if (status) LOG("  status=%u", *(uint32_t*)status);
    if (frame_count) LOG("  frame_count=%u", *(uint32_t*)frame_count);
    return r;
}

typedef uint32_t (*FnCopyDemosaic)(void*, void*);
uint32_t NvIspHwSettingsCopyDemosaic(void *dst, void *src) {
    LOG("NvIspHwSettingsCopyDemosaic(%p, %p)", dst, src);
    if (src) hexdump("DEMOSAIC_SRC", src, 64);
    return ((FnCopyDemosaic)get_real("NvIspHwSettingsCopyDemosaic"))(dst, src);
}

/* ---- Pass-through for all other exports ---- */

#define PASSTHROUGH(name, ret, ...) \
    typedef ret (*Fn_##name)(__VA_ARGS__); \
    ret name(__VA_ARGS__)

#define FWD0(name) \
    uint32_t name(void) { return ((uint32_t(*)(void))get_real(#name))(); }

#define FWD1(name) \
    uint32_t name(uint32_t a) { return ((uint32_t(*)(uint32_t))get_real(#name))(a); }

#define FWD2(name) \
    uint32_t name(uint32_t a, uint32_t b) { return ((uint32_t(*)(uint32_t,uint32_t))get_real(#name))(a,b); }

#define FWD3(name) \
    uint32_t name(uint32_t a, uint32_t b, uint32_t c) { return ((uint32_t(*)(uint32_t,uint32_t,uint32_t))get_real(#name))(a,b,c); }

#define FWD4(name) \
    uint32_t name(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return ((uint32_t(*)(uint32_t,uint32_t,uint32_t,uint32_t))get_real(#name))(a,b,c,d); }

#define FWD5(name) \
    uint32_t name(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) { return ((uint32_t(*)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t))get_real(#name))(a,b,c,d,e); }

/* Pass-throughs (arg count from nm + ghidra) */
FWD1(NvIspCtrlCleanup)
FWD3(NvIspCtrlInitialize)
FWD2(NvIspFlush)
FWD3(NvIspGetAttribute)
FWD4(NvIspGetConfiguration)
FWD2(NvIspGetStats)
FWD2(NvIspGetStatus)
FWD1(NvIspHwSettingsDestroy)
FWD1(NvIspHwSettingsDestroyClientHwSettingsList)
FWD4(NvIspHwSettingsGetAppliedSettings)
FWD4(NvIspHwSettingsGetAttribute)
FWD4(NvIspHwSettingsSetAttribute)
FWD2(NvIspHwSettingsClone)
FWD2(NvIspHwSettingsCopyBitwiseOperation)
FWD2(NvIspHwSettingsCopyGpp)
FWD2(NvIspHwSettingsCopyLensShading)
FWD2(NvIspHwSettingsCopyLumaEnhancement)
FWD2(NvIspHwSettingsCopyOutputDownScaler)
FWD3(NvIspSetAttribute)
FWD2(NvIspSetIspClockRate)
FWD2(NvIspSetMemoryBandwidth)
FWD2(NvIspSetStats)
FWD2(NvIspUpdateEmcClock)
FWD1(PopulateIspHwFunctions_T12x)

/* Utility functions — pass through */
FWD1(IsBayerColorFormat)
FWD2(NvCameraConvertDoubleToSFx)
FWD2(NvCameraConvertDoubleToUFx)
FWD2(NvCameraConvertRGrGbBToTlTrBlBr)
FWD2(NvCameraGetBayerComponent)
FWD5(NvCameraHwSettingsApply)
FWD3(NvCameraHwSettingsUpdateDirty)
FWD3(NvCameraMatMult3x3)
FWD1(NvSFxFixed2Float)
FWD1(NvSFxFloat2Fixed)
