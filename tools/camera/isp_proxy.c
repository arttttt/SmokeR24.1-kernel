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

typedef uint32_t (*FnSetConfig)(void*, uint32_t, void*, void*);
uint32_t NvIspSetConfiguration(void *h, uint32_t type, void *data, void *data2) {
    LOG("NvIspSetConfiguration(%p, type=%u, data=%p, data2=%p)", h, type, data, data2);
    if (data) hexdump("SetConfig.data", data, 64);
    if (data2) hexdump("SetConfig.data2", data2, 32);
    uint32_t r = ((FnSetConfig)get_real("NvIspSetConfiguration"))(h, type, data, data2);
    LOG("  SetConfig -> 0x%x", r);
    return r;
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
    if (mode == 1) {
        LOG("  REPROCESS: input_surf=%p output_cfg=%p", input_surf, output_cfg);
        if (input_surf) hexdump("INPUT_SURF", input_surf, 64);
        if (output_cfg) hexdump("OUTPUT_CFG", output_cfg, 160);
    } else {
        LOG("  STREAMING: width=%u height=%u output_cfg=%p",
            (uint32_t)(uintptr_t)input_surf, reserved, output_cfg);
        if (output_cfg) hexdump("OUTPUT_CFG", output_cfg, 160);
    }
    LOG("  fence=%p fence_val=%p status=%p count=%p", fence, fence_val, status, frame_count);

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

/* Pass-through via function pointers initialized at load time.
 * Each function is a simple stub that jumps to the real implementation.
 * Using naked + bx to preserve caller's exact stack/register state. */
#define FWD(name) \
    static void *_real_##name = 0; \
    __attribute__((naked)) void name(void) { \
        __asm__ volatile( \
            "push {r0}\n\t" \
            "ldr r0, .L_" #name "_ptr\n\t" \
            "ldr r0, [r0]\n\t" \
            "mov r12, r0\n\t" \
            "pop {r0}\n\t" \
            "bx r12\n\t" \
            ".align 2\n\t" \
            ".L_" #name "_ptr: .word _real_" #name "\n\t" \
        ); \
    }

/* Initialize all real pointers */
static void init_fwd_ptrs(void) __attribute__((constructor));

#define INIT_FWD(name) _real_##name = get_real(#name);

FWD(NvIspCtrlCleanup)
FWD(NvIspCtrlInitialize)
FWD(NvIspFlush)
FWD(NvIspGetAttribute)
FWD(NvIspGetConfiguration)
FWD(NvIspGetStats)
FWD(NvIspGetStatus)
FWD(NvIspHwSettingsDestroy)
FWD(NvIspHwSettingsDestroyClientHwSettingsList)
FWD(NvIspHwSettingsGetAppliedSettings)
FWD(NvIspHwSettingsGetAttribute)
FWD(NvIspHwSettingsSetAttribute)
FWD(NvIspHwSettingsClone)
FWD(NvIspHwSettingsCopyBitwiseOperation)
FWD(NvIspHwSettingsCopyGpp)
FWD(NvIspHwSettingsCopyLensShading)
FWD(NvIspHwSettingsCopyLumaEnhancement)
FWD(NvIspHwSettingsCopyOutputDownScaler)
FWD(NvIspSetAttribute)
FWD(NvIspSetIspClockRate)
FWD(NvIspSetMemoryBandwidth)
FWD(NvIspSetStats)
FWD(NvIspUpdateEmcClock)
FWD(PopulateIspHwFunctions_T12x)
FWD(IsBayerColorFormat)
FWD(NvCameraConvertRGrGbBToTlTrBlBr)
FWD(NvCameraGetBayerComponent)
FWD(NvCameraHwSettingsApply)
FWD(NvCameraHwSettingsUpdateDirty)
FWD(NvCameraMatMult3x3)

static void init_fwd_ptrs(void) {
    INIT_FWD(NvIspCtrlCleanup) INIT_FWD(NvIspCtrlInitialize)
    INIT_FWD(NvIspFlush) INIT_FWD(NvIspGetAttribute)
    INIT_FWD(NvIspGetConfiguration) INIT_FWD(NvIspGetStats)
    INIT_FWD(NvIspGetStatus) INIT_FWD(NvIspHwSettingsDestroy)
    INIT_FWD(NvIspHwSettingsDestroyClientHwSettingsList)
    INIT_FWD(NvIspHwSettingsGetAppliedSettings)
    INIT_FWD(NvIspHwSettingsGetAttribute) INIT_FWD(NvIspHwSettingsSetAttribute)
    INIT_FWD(NvIspHwSettingsClone) INIT_FWD(NvIspHwSettingsCopyBitwiseOperation)
    INIT_FWD(NvIspHwSettingsCopyGpp) INIT_FWD(NvIspHwSettingsCopyLensShading)
    INIT_FWD(NvIspHwSettingsCopyLumaEnhancement)
    INIT_FWD(NvIspHwSettingsCopyOutputDownScaler)
    INIT_FWD(NvIspSetAttribute) INIT_FWD(NvIspSetIspClockRate)
    INIT_FWD(NvIspSetMemoryBandwidth) INIT_FWD(NvIspSetStats)
    INIT_FWD(NvIspUpdateEmcClock) INIT_FWD(PopulateIspHwFunctions_T12x)
    INIT_FWD(IsBayerColorFormat) INIT_FWD(NvCameraConvertRGrGbBToTlTrBlBr)
    INIT_FWD(NvCameraGetBayerComponent) INIT_FWD(NvCameraHwSettingsApply)
    INIT_FWD(NvCameraHwSettingsUpdateDirty) INIT_FWD(NvCameraMatMult3x3)
    LOG("init_fwd_ptrs done");
}

/* Float/double functions — must preserve FPU calling convention */
float NvCameraConvertDoubleToSFx(double a, double b) {
    typedef float (*Fn)(double, double);
    return ((Fn)get_real("NvCameraConvertDoubleToSFx"))(a, b);
}
float NvCameraConvertDoubleToUFx(double a, double b) {
    typedef float (*Fn)(double, double);
    return ((Fn)get_real("NvCameraConvertDoubleToUFx"))(a, b);
}
float NvSFxFixed2Float(uint32_t a) {
    typedef float (*Fn)(uint32_t);
    return ((Fn)get_real("NvSFxFixed2Float"))(a);
}
uint32_t NvSFxFloat2Fixed(float a) {
    typedef uint32_t (*Fn)(float);
    return ((Fn)get_real("NvSFxFloat2Fixed"))(a);
}
