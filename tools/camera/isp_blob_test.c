/*
 * ISP blob wrapper test v2 — calls libnvisp_v3.so to init/configure ISP
 *
 * Init chain (from jxd sources + Ghidra RE):
 *   1. NvRmOpenNew(&hRm)              — singleton in libnvrm.so
 *   2. NvIspOpen(&handle)             — allocs 0x1330 ctx, opens channel, syncpts
 *      inside: NvRmChannelOpen(hRm, &ch, 1, &modId)  ← needs hRm singleton
 *              NvRmStreamInit(hRm, ch, &stream)
 *              NvRmChannelGetModuleSyncPoint() ×4
 *              NvRmMemHandleAllocAttr() for stats (8×256KB)
 *              PopulateIspHwFunctions_T12x()
 *   3. NvIspCtrlInitialize(&ctx, ISP_A)  — opens /dev/nvhost-ctrl-isp
 *      inside: NvRmHostModuleRegWr(0xFC, 0x20)
 *
 * Signatures (from nvrm_init.h, nvrm_channel.h, binary strings):
 *   NvError NvRmOpenNew(NvRmDeviceHandle *pHandle)
 *   NvError NvRmOpen(NvRmDeviceHandle *pHandle, NvU32 DeviceId)
 *   NvError NvIspOpen(NvIspHandle *handle)
 *   NvError NvIspCtrlInitialize(NvIspContext*, NvIspIspSelect)
 *   NvError NvIspGetStatus(NvIspHandle, NvIspStatusType, void*, NvU32*)
 *   NvError NvIspHwSettingsCreate(...)
 *   NvError NvIspProcessFrame(...)
 *   NvError NvIspProcessFrame3(...)
 *
 * Build (NDK or cross-compiler, dynamic linking required for dlopen):
 *   armv7a-linux-androideabi19-clang -o isp_blob_test isp_blob_test.c -ldl -llog
 *   OR: arm-linux-gnueabihf-gcc -std=gnu99 -o isp_blob_test isp_blob_test.c -ldl
 *
 * Deploy:
 *   adb push isp_blob_test /data/local/tmp/
 *   adb push libnvisp_v3.so /data/local/tmp/   (extracted from git)
 *   adb shell LD_LIBRARY_PATH=/system/vendor/lib:/data/local/tmp /data/local/tmp/isp_blob_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>

/* ---- NV types (from nvrm_init.h, nverror.h) ---- */
typedef uint32_t NvError;
typedef uint32_t NvU32;
typedef uint32_t NvBool;
#define NvSuccess 0
#define NV_TRUE  1
#define NV_FALSE 0

typedef void *NvRmDeviceHandle;
typedef void *NvIspHandle;

/* NvIspIspSelect — which ISP (from binary strings + RE) */
typedef enum {
    NvIspIspSelect_A = 0,   /* ISP-A, class 0x32 */
    NvIspIspSelect_B = 1,   /* ISP-B, class 0x34 */
} NvIspIspSelect;

/* NvError codes (common subset from nverror.h) */
static const char *nverr_str(NvError e) {
    switch (e) {
    case 0x0:     return "NvSuccess";
    case 0x2:     return "NvError_NotImplemented";
    case 0x3:     return "NvError_NotSupported";
    case 0x4:     return "NvError_NotInitialized";
    case 0x5:     return "NvError_BadParameter";
    case 0x6:     return "NvError_Timeout";
    case 0x7:     return "NvError_InsufficientMemory";
    case 0x8:     return "NvError_ReadOnlyAttribute";
    case 0x9:     return "NvError_InvalidState";
    case 0xA:     return "NvError_InvalidAddress";
    case 0xB:     return "NvError_InvalidSize";
    case 0xD:     return "NvError_BadValue";
    case 0xF:     return "NvError_AlreadyAllocated";
    case 0x10:    return "NvError_Busy";
    case 0x10000: return "NvError_ModuleNotPresent";
    case 0x30004: return "NvError_IoctlFailed";
    case 0x30005: return "NvError_AccessDenied";
    case 0x30006: return "NvError_DeviceNotFound";
    case 0x30007: return "NvError_KernelDriverNotFound";
    case 0x30008: return "NvError_FileOperationFailed";
    case 0x40002: return "NvError_ResourceError";
    default:      return "unknown";
    }
}

/* Signal handler for catching crashes in blob calls */
static volatile int got_signal = 0;
static void sig_handler(int sig) {
    const char *name = sig == SIGSEGV ? "SIGSEGV" :
                       sig == SIGBUS  ? "SIGBUS"  :
                       sig == SIGABRT ? "SIGABRT" : "SIGNAL";
    /* Can't use printf in signal handler, use write */
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "\n!!! CAUGHT %s (%d) !!!\n", name, sig);
    write(2, buf, len);
    _exit(128 + sig);
}

/* ---- Function pointer types ---- */

/* NvRm */
typedef NvError (*NvRmOpenNew_t)(NvRmDeviceHandle *pHandle);
typedef NvError (*NvRmOpen_t)(NvRmDeviceHandle *pHandle, NvU32 DeviceId);
typedef void    (*NvRmClose_t)(NvRmDeviceHandle hDevice);

/* NvIsp lifecycle */
typedef NvError (*NvIspOpen_t)(NvIspHandle *handle);
typedef void    (*NvIspClose_t)(NvIspHandle handle);
typedef NvError (*NvIspCtrlInitialize_t)(void *ctx, NvU32 isp_select);
typedef void    (*NvIspCtrlCleanup_t)(void *ctx);

/* NvIsp config */
typedef NvError (*NvIspSetConfiguration_t)(NvIspHandle handle, void *config);
typedef NvError (*NvIspGetConfiguration_t)(NvIspHandle handle, void *config);
typedef NvError (*NvIspFlush_t)(NvIspHandle handle);
typedef NvError (*NvIspGetStatus_t)(NvIspHandle handle, NvU32 type, void *out, NvU32 *size);
typedef NvError (*NvIspGetAttribute_t)(NvIspHandle handle, NvU32 attr, void *val);
typedef NvError (*NvIspSetAttribute_t)(NvIspHandle handle, NvU32 attr, void *val);
typedef NvError (*NvIspGetCapabilities_t)(NvIspHandle handle, void *caps);
typedef NvError (*NvIspHwGetCapabilities_t)(void *caps);

/* NvIsp HW settings */
typedef NvError (*NvIspHwSettingsCreate_t)(void *handle, void **settings);
typedef NvError (*NvIspHwSettingsDestroy_t)(void *settings);
typedef NvError (*NvIspHwSettingsApply_t)(void *handle, void *settings);
typedef NvError (*NvIspHwSettingsSetAttribute_t)(void *settings, NvU32 attr, void *val);
typedef NvError (*NvIspHwSettingsClone_t)(void *src, void **dst);

/* NvIsp processing */
typedef NvError (*NvIspProcessFrame_t)(NvIspHandle handle, void *arg1, void *arg2);
typedef NvError (*NvIspProcessFrame3_t)(NvIspHandle handle, void *arg1, void *arg2, void *arg3);

/* PopulateIspHwFunctions_T12x */
typedef void (*PopulateIspHwFunctions_T12x_t)(void *vtable);

/* ---- Helpers ---- */

static void *load_lib(const char *name) {
    /* Try /data/local/tmp first, then vendor, then bare name */
    char path[256];
    void *h;

    snprintf(path, sizeof(path), "/data/local/tmp/%s", name);
    h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (h) { printf("  %s: loaded from /data/local/tmp/\n", name); return h; }

    snprintf(path, sizeof(path), "/system/vendor/lib/%s", name);
    h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (h) { printf("  %s: loaded from /system/vendor/lib/\n", name); return h; }

    h = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    if (h) { printf("  %s: loaded (default path)\n", name); return h; }

    printf("  %s: FAILED — %s\n", name, dlerror());
    return NULL;
}

#define RESOLVE(lib, type, name) \
    type p_##name = (type)dlsym(lib, #name); \
    printf("  %-35s %p\n", #name ":", (void*)p_##name)

/* ---- Main ---- */

int main(int argc, char **argv)
{
    NvError err;
    int use_isp_b = 0;

    if (argc > 1 && !strcmp(argv[1], "-b"))
        use_isp_b = 1;

    printf("=== ISP Blob Test v2 ===\n");
    printf("ISP select: %s\n\n", use_isp_b ? "ISP-B (0x34)" : "ISP-A (0x32)");

    /* Install crash handlers */
    signal(SIGSEGV, sig_handler);
    signal(SIGBUS, sig_handler);
    signal(SIGABRT, sig_handler);

    /* ---- Step 0: Load dependencies ---- */
    printf("[0] Loading libraries...\n");

    /* Load shim FIRST with RTLD_GLOBAL so it overrides NvRmChannelOpen
     * before libnvrm_graphics.so binds it */
    void *lib_shim = load_lib("nvrm_shim.so");
    if (lib_shim)
        printf("  (shim active — module ID translation enabled)\n");

    void *lib_nvos = load_lib("libnvos.so");
    void *lib_nvrm = load_lib("libnvrm.so");
    void *lib_nvrm_gfx = load_lib("libnvrm_graphics.so");
    void *lib_isp = load_lib("libnvisp_v3.so");
    printf("\n");
    fflush(stdout);

    if (!lib_nvos || !lib_nvrm) {
        printf("FATAL: cannot load libnvos/libnvrm\n");
        return 1;
    }
    if (!lib_isp) {
        printf("FATAL: cannot load libnvisp_v3.so\n");
        printf("  Extract: cd proprietary_vendor_nvidia && "
               "git show a112d11^:shield/camera/lib/libnvisp_v3.so > libnvisp_v3.so\n");
        printf("  Push:    adb push libnvisp_v3.so /data/local/tmp/\n");
        return 1;
    }

    /* ---- Step 1: NvRmOpenNew ---- */
    printf("[1] NvRm init...\n");

    RESOLVE(lib_nvrm, NvRmOpenNew_t, NvRmOpenNew);
    RESOLVE(lib_nvrm, NvRmOpen_t, NvRmOpen);
    RESOLVE(lib_nvrm, NvRmClose_t, NvRmClose);
    fflush(stdout);

    NvRmDeviceHandle hRm = NULL;

    if (p_NvRmOpenNew) {
        printf("  Calling NvRmOpenNew...\n"); fflush(stdout);
        err = p_NvRmOpenNew(&hRm);
        printf("  NvRmOpenNew: err=0x%x (%s) hRm=%p\n", err, nverr_str(err), hRm);
    } else if (p_NvRmOpen) {
        printf("  Calling NvRmOpen(0)...\n"); fflush(stdout);
        err = p_NvRmOpen(&hRm, 0);
        printf("  NvRmOpen: err=0x%x (%s) hRm=%p\n", err, nverr_str(err), hRm);
    } else {
        printf("  FATAL: no NvRmOpen/NvRmOpenNew symbol\n");
        return 1;
    }
    fflush(stdout);

    if (err != NvSuccess) {
        printf("  NvRm init FAILED — ISP blob needs working NvRm\n");
        printf("  This usually means /dev/nvhost-ctrl or /dev/nvmap is not accessible\n");
        return 1;
    }

    /* ---- Step 1b: Test NvRmChannelOpen directly ---- */
    /*
     * NvRmChannelOpen(NvRmDeviceHandle hDevice, NvRmChannelHandle *phChannel,
     *                 NvU32 NumModules, const NvRmModuleID *pModuleIDs)
     * NvRmModuleID = (module_id << 8) | instance
     * ISP-A: module 0x0B instance 0 → 0x0B00
     * ISP-B: module 0x0B instance 1 → 0x0B01
     */
    if (lib_nvrm_gfx) {
        printf("\n[1b] Testing NvRmChannelOpen directly...\n");

        typedef NvError (*NvRmChannelOpen_t)(void *hRm, void **phCh,
                                             NvU32 numMod, const NvU32 *modIds);
        typedef void (*NvRmChannelClose_t)(void *hCh);
        typedef NvError (*NvRmStreamInit_t)(void *hRm, void *hCh, void **phStream);
        typedef void (*NvRmStreamFree_t)(void *hStream);

        RESOLVE(lib_nvrm_gfx, NvRmChannelOpen_t, NvRmChannelOpen);
        RESOLVE(lib_nvrm_gfx, NvRmChannelClose_t, NvRmChannelClose);
        RESOLVE(lib_nvrm_gfx, NvRmStreamInit_t, NvRmStreamInit);
        RESOLVE(lib_nvrm_gfx, NvRmStreamFree_t, NvRmStreamFree);
        fflush(stdout);

        if (p_NvRmChannelOpen) {
            /* Try ISP-A: module 0x0B, instance 0 */
            NvU32 isp_mod = use_isp_b ? 0x0B01 : 0x0B00;
            void *hCh = NULL;
            printf("  NvRmChannelOpen(hRm=%p, mod=0x%04x)...\n", hRm, isp_mod);
            fflush(stdout);
            err = p_NvRmChannelOpen(hRm, &hCh, 1, &isp_mod);
            printf("  NvRmChannelOpen: err=0x%x (%s) hCh=%p\n",
                   err, nverr_str(err), hCh);
            fflush(stdout);

            if (err == NvSuccess && hCh) {
                printf("  Channel opened! Dumping handle:\n  ");
                for (int i = 0; i < 32; i += 4)
                    printf(" %08x", *(uint32_t *)((uint8_t *)hCh + i));
                printf("\n");

                /* Try NvRmStreamInit */
                if (p_NvRmStreamInit) {
                    void *hStream = NULL;
                    printf("  NvRmStreamInit...\n"); fflush(stdout);
                    err = p_NvRmStreamInit(hRm, hCh, &hStream);
                    printf("  NvRmStreamInit: err=0x%x (%s) hStream=%p\n",
                           err, nverr_str(err), hStream);
                    if (hStream && p_NvRmStreamFree)
                        p_NvRmStreamFree(hStream);
                }

                if (p_NvRmChannelClose)
                    p_NvRmChannelClose(hCh);
                printf("  Channel closed\n");
            }

            /* Brute-force: try all module IDs 0..31 in both encodings */
            printf("  Brute-force module ID scan:\n");
            fflush(stdout);
            for (NvU32 mid = 0; mid <= 31; mid++) {
                /* Try both encodings: raw and shifted */
                NvU32 variants[] = { mid, mid << 8, (mid << 8) | 0, mid | (0 << 8) };
                for (int v = 0; v < 2; v++) {
                    hCh = NULL;
                    err = p_NvRmChannelOpen(hRm, &hCh, 1, &variants[v]);
                    if (err == NvSuccess && hCh) {
                        printf("    *** mod=0x%04x (%d) → OK! hCh=%p\n",
                               variants[v], variants[v], hCh);
                        if (p_NvRmChannelClose) p_NvRmChannelClose(hCh);
                    }
                }
            }
            fflush(stdout);
        }
    }

    /* ---- Step 2: Resolve NvIsp symbols ---- */
    printf("\n[2] Resolving NvIsp symbols...\n");

    RESOLVE(lib_isp, NvIspOpen_t, NvIspOpen);
    RESOLVE(lib_isp, NvIspClose_t, NvIspClose);
    RESOLVE(lib_isp, NvIspCtrlInitialize_t, NvIspCtrlInitialize);
    RESOLVE(lib_isp, NvIspCtrlCleanup_t, NvIspCtrlCleanup);
    RESOLVE(lib_isp, NvIspFlush_t, NvIspFlush);
    RESOLVE(lib_isp, NvIspGetStatus_t, NvIspGetStatus);
    RESOLVE(lib_isp, NvIspGetAttribute_t, NvIspGetAttribute);
    RESOLVE(lib_isp, NvIspSetAttribute_t, NvIspSetAttribute);
    RESOLVE(lib_isp, NvIspGetCapabilities_t, NvIspGetCapabilities);
    RESOLVE(lib_isp, NvIspHwGetCapabilities_t, NvIspHwGetCapabilities);
    RESOLVE(lib_isp, NvIspSetConfiguration_t, NvIspSetConfiguration);
    RESOLVE(lib_isp, NvIspHwSettingsCreate_t, NvIspHwSettingsCreate);
    RESOLVE(lib_isp, NvIspHwSettingsDestroy_t, NvIspHwSettingsDestroy);
    RESOLVE(lib_isp, NvIspHwSettingsApply_t, NvIspHwSettingsApply);
    RESOLVE(lib_isp, NvIspHwSettingsClone_t, NvIspHwSettingsClone);
    RESOLVE(lib_isp, NvIspProcessFrame_t, NvIspProcessFrame);
    RESOLVE(lib_isp, NvIspProcessFrame3_t, NvIspProcessFrame3);
    RESOLVE(lib_isp, PopulateIspHwFunctions_T12x_t, PopulateIspHwFunctions_T12x);
    fflush(stdout);

    if (!p_NvIspOpen) {
        printf("  FATAL: NvIspOpen not found\n");
        return 1;
    }

    /* ---- Step 3: NvIspOpen ---- */
    /*
     * NvIspOpen internally does:
     *   NvOsAlloc(0x1330)                     — 4912-byte context
     *   PopulateIspHwFunctions_T12x(ctx)      — fill vtable for T124
     *   NvRmChannelOpen(hRm, &ch, 1, &modId)  — opens /dev/nvhost-isp
     *   NvRmStreamInit(hRm, ch, &stream)       — 32KB double-buffered pushbuf
     *   NvRmChannelGetModuleSyncPoint() ×4     — 4 syncpoints
     *   NvRmMemHandleAllocAttr()              — stats ring (8×256KB)
     *
     * hRm is taken from singleton set by NvRmOpenNew above.
     */
    printf("\n[3] NvIspOpen...\n"); fflush(stdout);

    NvIspHandle handle = NULL;
    err = p_NvIspOpen(&handle);
    printf("  NvIspOpen: err=0x%x (%s) handle=%p\n", err, nverr_str(err), handle);
    fflush(stdout);

    if (err != NvSuccess || !handle) {
        printf("  NvIspOpen FAILED\n");
        if (err == 0x30006)
            printf("  → DeviceNotFound: /dev/nvhost-isp not accessible?\n");
        else if (err == 0x7)
            printf("  → InsufficientMemory: stats buffer alloc failed (need vmalloc space)\n");
        else if (err == 0x30004)
            printf("  → IoctlFailed: nvhost ioctl error (ABI mismatch?)\n");
        goto cleanup_rm;
    }

    /* Dump first 64 bytes of handle to understand struct */
    printf("  Handle memory (first 64 bytes):\n  ");
    for (int i = 0; i < 64; i += 4)
        printf(" %08x", *(uint32_t *)((uint8_t *)handle + i));
    printf("\n");
    fflush(stdout);

    /* ---- Step 4: NvIspCtrlInitialize ---- */
    /*
     * Opens /dev/nvhost-ctrl-isp or /dev/nvhost-ctrl-isp.1
     * Writes NvRmHostModuleRegWr(0xFC, 0x20) — clock gating register
     *
     * Signature: NvError NvIspCtrlInitialize(NvIspContext*, NvIspIspSelect)
     * NvIspContext* is the handle itself (handle = context pointer)
     */
    if (p_NvIspCtrlInitialize) {
        NvU32 isp_sel = use_isp_b ? NvIspIspSelect_B : NvIspIspSelect_A;
        printf("\n[4] NvIspCtrlInitialize(handle, %s)...\n",
               use_isp_b ? "ISP_B" : "ISP_A");
        fflush(stdout);

        err = p_NvIspCtrlInitialize(handle, isp_sel);
        printf("  NvIspCtrlInitialize: err=0x%x (%s)\n", err, nverr_str(err));
        fflush(stdout);

        if (err != NvSuccess) {
            printf("  → /dev/nvhost-ctrl-isp open or clock config failed\n");
        }
    }

    /* ---- Step 5: NvIspHwGetCapabilities ---- */
    if (p_NvIspHwGetCapabilities) {
        printf("\n[5] NvIspHwGetCapabilities...\n"); fflush(stdout);
        uint32_t caps[32];
        memset(caps, 0, sizeof(caps));
        err = p_NvIspHwGetCapabilities(caps);
        printf("  err=0x%x (%s)\n", err, nverr_str(err));
        if (err == NvSuccess) {
            printf("  caps[0..7]:");
            for (int i = 0; i < 8; i++) printf(" %08x", caps[i]);
            printf("\n");
        }
        fflush(stdout);
    }

    /* ---- Step 6: NvIspGetStatus ---- */
    if (p_NvIspGetStatus) {
        printf("\n[6] NvIspGetStatus...\n"); fflush(stdout);
        uint32_t status[64];
        NvU32 status_size = sizeof(status);
        memset(status, 0, sizeof(status));
        /* Try type=0 */
        err = p_NvIspGetStatus(handle, 0, status, &status_size);
        printf("  type=0: err=0x%x (%s) size=%u\n", err, nverr_str(err), status_size);
        if (err == NvSuccess) {
            printf("  status[0..7]:");
            for (int i = 0; i < 8; i++) printf(" %08x", status[i]);
            printf("\n");
        }
        fflush(stdout);
    }

    /* ---- Step 7: NvIspHwSettingsCreate ---- */
    void *hw_settings = NULL;
    if (p_NvIspHwSettingsCreate) {
        printf("\n[7] NvIspHwSettingsCreate...\n"); fflush(stdout);
        err = p_NvIspHwSettingsCreate(handle, &hw_settings);
        printf("  err=0x%x (%s) settings=%p\n", err, nverr_str(err), hw_settings);
        fflush(stdout);

        if (err == NvSuccess && hw_settings) {
            printf("  Settings memory (first 64 bytes):\n  ");
            for (int i = 0; i < 64; i += 4)
                printf(" %08x", *(uint32_t *)((uint8_t *)hw_settings + i));
            printf("\n");
        }
    }

    /* ---- Step 8: Handle dump (full) ---- */
    printf("\n[8] Full handle dump (256 bytes):\n");
    for (int row = 0; row < 256; row += 32) {
        printf("  +%03x:", row);
        for (int i = 0; i < 32; i += 4)
            printf(" %08x", *(uint32_t *)((uint8_t *)handle + row + i));
        printf("\n");
    }
    fflush(stdout);

    /* ---- Cleanup ---- */
    printf("\n[cleanup]\n");

    if (hw_settings && p_NvIspHwSettingsDestroy) {
        p_NvIspHwSettingsDestroy(hw_settings);
        printf("  NvIspHwSettingsDestroy OK\n");
    }

    if (p_NvIspClose) {
        p_NvIspClose(handle);
        printf("  NvIspClose OK\n");
    }

cleanup_rm:
    if (hRm && p_NvRmClose) {
        p_NvRmClose(hRm);
        printf("  NvRmClose OK\n");
    }

    printf("\n=== Done ===\n");
    return 0;
}
