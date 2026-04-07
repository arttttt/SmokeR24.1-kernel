/*
 * ISP inject payload — loaded into mediaserver via dlopen injection.
 *
 * NvRm is already initialized in mediaserver, so NvIspOpen should work.
 * This library's constructor calls NvIspOpen → NvIspCtrlInitialize(ISP-B) →
 * loads raw OV5693, submits via NvIspProcessFrame, dumps output.
 *
 * Build with NDK:
 *   armv7a-linux-androideabi19-clang -shared -fPIC -o isp_payload.so isp_payload.c -ldl -llog
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

typedef uint32_t NvError;
#define NvSuccess 0
typedef void *NvIspHandle;

#define LOG_FILE "/data/local/tmp/isp_inject.log"

static FILE *logf = NULL;

static void ilog(const char *fmt, ...) {
    if (!logf) logf = fopen(LOG_FILE, "w");
    if (!logf) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logf, fmt, ap);
    va_end(ap);
    fflush(logf);
}

/* Called when .so is loaded — start delayed thread */
#include <pthread.h>

static void *isp_worker(void *arg) {
    /* Wait for NvRm to initialize in mediaserver */
    sleep(10);
    ilog("=== ISP Worker Thread Started (after 10s delay) ===\n");
    ilog("PID=%d TID=%d\n", getpid(), gettid());

    /* libnvisp_v3.so is already loaded in mediaserver */
    void *isp_lib = dlopen("libnvisp_v3.so", RTLD_NOLOAD);
    if (!isp_lib) isp_lib = dlopen("/system/vendor/lib/libnvisp_v3.so", RTLD_NOW);
    ilog("libnvisp_v3.so: %p\n", isp_lib);
    if (!isp_lib) {
        ilog("FATAL: cannot load libnvisp_v3.so: %s\n", dlerror());
        return NULL;
    }

    /* Resolve symbols */
    typedef NvError (*NvIspOpen_t)(NvIspHandle *handle);
    typedef void (*NvIspClose_t)(NvIspHandle handle);
    typedef NvError (*NvIspCtrlInit_t)(void *ctx, uint32_t isp_select);
    typedef NvError (*NvIspFlush_t)(NvIspHandle handle);
    typedef NvError (*NvIspGetAttribute_t)(NvIspHandle handle, uint32_t attr, void *val);
    typedef NvError (*NvIspGetStatus_t)(NvIspHandle handle, void *status);

    NvIspOpen_t pOpen = dlsym(isp_lib, "NvIspOpen");
    NvIspClose_t pClose = dlsym(isp_lib, "NvIspClose");
    NvIspCtrlInit_t pCtrlInit = dlsym(isp_lib, "NvIspCtrlInitialize");
    NvIspFlush_t pFlush = dlsym(isp_lib, "NvIspFlush");
    NvIspGetAttribute_t pGetAttr = dlsym(isp_lib, "NvIspGetAttribute");
    NvIspGetStatus_t pGetStatus = dlsym(isp_lib, "NvIspGetStatus");

    ilog("NvIspOpen=%p Close=%p CtrlInit=%p Flush=%p\n",
         pOpen, pClose, pCtrlInit, pFlush);

    if (!pOpen) {
        ilog("FATAL: NvIspOpen not found\n");
        return NULL;
    }

    /* Try NvIspOpen */
    NvIspHandle handle = NULL;
    ilog("Calling NvIspOpen(&handle)...\n");
    NvError err = pOpen(&handle);
    ilog("NvIspOpen: err=%u handle=%p\n", err, handle);

    /* If NvIspOpen fails, try to get existing handle from camera HAL */
    if (err != NvSuccess || !handle) {
        ilog("NvIspOpen failed, trying NvCameraIspGetNvIspHandle...\n");
        void *cam_lib = dlopen("libnvmm_camera_v3.so", RTLD_NOLOAD);
        if (!cam_lib) cam_lib = dlopen("/system/vendor/lib/libnvmm_camera_v3.so", RTLD_NOW);
        ilog("libnvmm_camera_v3.so: %p\n", cam_lib);
        if (cam_lib) {
            typedef NvIspHandle (*GetHandle_t)(void);
            typedef NvIspHandle (*GetSecondary_t)(void);
            GetHandle_t pGetHandle = dlsym(cam_lib, "NvCameraIspGetNvIspHandle");
            GetSecondary_t pGetSecondary = dlsym(cam_lib, "NvCameraIspGetSecondaryNvIspHandle");
            ilog("GetNvIspHandle=%p GetSecondary=%p\n", pGetHandle, pGetSecondary);
            if (pGetHandle) {
                handle = pGetHandle();
                ilog("NvCameraIspGetNvIspHandle: handle=%p\n", handle);
            }
            if (!handle && pGetSecondary) {
                handle = pGetSecondary();
                ilog("NvCameraIspGetSecondaryNvIspHandle: handle=%p\n", handle);
            }
        }
    }

    if (!handle) {
        ilog("No ISP handle available\n");
        ilog("=== ISP Inject Complete ===\n");
        if (logf) fclose(logf); logf = NULL;
        return NULL;
    }

    ilog("Got ISP handle: %p\n", handle);

    /* DON'T call NvIspCtrlInitialize — ISP is already initialized by camera HAL.
     * Calling it again with wrong params will crash. */

    /* Try to get ISP status */
    if (pGetStatus) {
        uint32_t status[64];
        memset(status, 0, sizeof(status));
        ilog("Calling NvIspGetStatus...\n");
        err = pGetStatus(handle, status);
        ilog("NvIspGetStatus: err=%u\n", err);
        ilog("  status[0..7]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
             status[0], status[1], status[2], status[3],
             status[4], status[5], status[6], status[7]);
    }

    /* Try to get some attributes */
    if (pGetAttr) {
        uint32_t val = 0;
        for (int attr = 0; attr < 8; attr++) {
            err = pGetAttr(handle, attr, &val);
            ilog("  GetAttribute(%d): err=%u val=0x%08x\n", attr, err, val);
        }
    }

    /* Dump handle memory to understand struct layout */
    ilog("\nHandle memory dump (first 256 bytes):\n");
    uint8_t *hptr = (uint8_t *)handle;
    for (int i = 0; i < 256; i += 16) {
        ilog("  +%03x: %08x %08x %08x %08x\n", i,
             *(uint32_t*)(hptr+i), *(uint32_t*)(hptr+i+4),
             *(uint32_t*)(hptr+i+8), *(uint32_t*)(hptr+i+12));
    }

    /* Cleanup */
    if (pClose && handle) {
        ilog("Calling NvIspClose...\n");
        pClose(handle);
        ilog("NvIspClose done\n");
    }

    ilog("=== ISP Inject Complete ===\n");
    if (logf) fclose(logf);
    logf = NULL;
    return NULL;
}

__attribute__((constructor))
static void isp_inject_init(void) {
    logf = fopen(LOG_FILE, "w");
    ilog("=== ISP Inject Payload constructor ===\n");
    ilog("PID=%d — launching delayed worker thread\n", getpid());
    pthread_t t;
    pthread_create(&t, NULL, isp_worker, NULL);
    pthread_detach(t);
    ilog("Worker thread launched, returning from constructor\n");
}
