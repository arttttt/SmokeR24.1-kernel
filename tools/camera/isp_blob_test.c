/*
 * ISP blob wrapper test — calls libnvisp_v3.so to properly init ISP
 *
 * Approach: dlopen libnvisp_v3.so, call NvIspOpen → NvIspCtrlInitialize →
 * NvIspSetConfiguration → NvIspProcessFrame with our raw buffer.
 *
 * Key discovery: NvIspSetOutputT12x — T12x-specific output DMA setup
 * that our manual cmdbuf test is missing.
 *
 * Build: arm-linux-gnueabihf-gcc -std=gnu99 -static -o isp_blob_test isp_blob_test.c -ldl
 * Note: -static won't work with dlopen. Use dynamic linking:
 *   arm-linux-gnueabihf-gcc -std=gnu99 -o isp_blob_test isp_blob_test.c -ldl
 *
 * Usage: isp_blob_test
 *
 * TODO: fill in struct layouts once RE/headers are found
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>

/* NvError — from nverror.h */
typedef uint32_t NvError;
#define NvSuccess 0

/* NvBool */
typedef uint32_t NvBool;
#define NV_TRUE  1
#define NV_FALSE 0

/* Opaque handle types — actual size TBD from RE */
typedef void *NvIspHandle;
typedef void *NvIspContext;

/* NvIspIspSelect — which ISP to use */
typedef enum {
    NvIspIspSelect_A = 0,   /* ISP-A, class 0x32 */
    NvIspIspSelect_B = 1,   /* ISP-B, class 0x34 */
} NvIspIspSelect;

/* Function pointer types */
typedef NvError (*NvIspOpen_t)(NvIspHandle *handle);
typedef void    (*NvIspClose_t)(NvIspHandle handle);
typedef NvError (*NvIspCtrlInitialize_t)(NvIspContext *ctx, NvIspIspSelect isp);
typedef void    (*NvIspCtrlCleanup_t)(NvIspContext ctx);
typedef NvError (*NvIspSetConfiguration_t)(NvIspHandle handle, void *config);
typedef NvError (*NvIspProcessFrame_t)(NvIspHandle handle, /* args TBD */...);
typedef NvError (*NvIspFlush_t)(NvIspHandle handle);
typedef NvError (*NvIspSetOutputT12x_t)(NvIspHandle handle, /* args TBD */...);
typedef NvError (*NvIspHwSettingsCreate_t)(NvIspHandle handle, void **settings);
typedef NvError (*NvIspHwSettingsApply_t)(NvIspHandle handle, void *settings);

int main(int argc, char **argv)
{
    printf("=== ISP Blob Wrapper Test ===\n\n");
    NvError err;

    /* Pre-load NvRm dependencies */
    void *nvrm = dlopen("/system/vendor/lib/libnvrm.so", RTLD_NOW | RTLD_GLOBAL);
    printf("libnvrm.so: %p %s\n", nvrm, nvrm ? "OK" : dlerror());
    void *nvos = dlopen("/system/vendor/lib/libnvos.so", RTLD_NOW | RTLD_GLOBAL);
    printf("libnvos.so: %p %s\n", nvos, nvos ? "OK" : dlerror());
    fflush(stdout);

    /* Initialize NvRm before calling NvIsp */
    void *rm_handle = NULL;
    if (nvrm) {
        typedef NvError (*NvRmOpen_t)(void **handle, uint32_t unused);
        NvRmOpen_t pRmOpen = dlsym(nvrm, "NvRmOpenNew");
        if (!pRmOpen) pRmOpen = dlsym(nvrm, "NvRmOpen");
        printf("NvRmOpen func: %p\n", pRmOpen);
        fflush(stdout);
        if (pRmOpen) {
            printf("Calling NvRmOpen...\n"); fflush(stdout);
            err = pRmOpen(&rm_handle, 0);
            printf("NvRmOpen: err=%u handle=%p\n", err, rm_handle); fflush(stdout);
        }
    }

    /* Load libnvisp_v3.so */
    void *lib = dlopen("libnvisp_v3.so", RTLD_NOW);
    if (!lib) {
        /* Try full path */
        lib = dlopen("/system/vendor/lib/libnvisp_v3.so", RTLD_NOW);
    }
    if (!lib) {
        printf("dlopen failed: %s\n", dlerror());
        return 1;
    }
    printf("libnvisp_v3.so loaded at %p\n", lib);

    /* Resolve symbols */
    NvIspOpen_t pOpen = dlsym(lib, "NvIspOpen");
    NvIspClose_t pClose = dlsym(lib, "NvIspClose");
    NvIspCtrlInitialize_t pCtrlInit = dlsym(lib, "NvIspCtrlInitialize");
    NvIspCtrlCleanup_t pCtrlCleanup = dlsym(lib, "NvIspCtrlCleanup");
    NvIspSetConfiguration_t pSetConfig = dlsym(lib, "NvIspSetConfiguration");
    NvIspFlush_t pFlush = dlsym(lib, "NvIspFlush");
    NvIspSetOutputT12x_t pSetOutputT12x = dlsym(lib, "NvIspSetOutputT12x");
    NvIspHwSettingsCreate_t pHwCreate = dlsym(lib, "NvIspHwSettingsCreate");
    NvIspHwSettingsApply_t pHwApply = dlsym(lib, "NvIspHwSettingsApply");
    NvIspProcessFrame_t pProcessFrame = dlsym(lib, "NvIspProcessFrame");

    printf("NvIspOpen:            %p\n", pOpen);
    printf("NvIspClose:           %p\n", pClose);
    printf("NvIspCtrlInitialize:  %p\n", pCtrlInit);
    printf("NvIspSetConfiguration:%p\n", pSetConfig);
    printf("NvIspFlush:           %p\n", pFlush);
    printf("NvIspSetOutputT12x:   %p\n", pSetOutputT12x);
    printf("NvIspHwSettingsCreate:%p\n", pHwCreate);
    printf("NvIspHwSettingsApply: %p\n", pHwApply);
    printf("NvIspProcessFrame:    %p\n", pProcessFrame);
    fflush(stdout);

    if (!pOpen || !pClose || !pCtrlInit) {
        printf("Missing critical symbols\n");
        dlclose(lib);
        return 1;
    }

    /* Step 1: NvIspOpen — allocates 0x1330 byte context internally */
    NvIspHandle handle = NULL;
    printf("\nCalling NvIspOpen(&handle)...\n");
    fflush(stdout);
    err = pOpen(&handle);
    printf("NvIspOpen: err=%u handle=%p\n", err, handle);
    fflush(stdout);
    if (err != NvSuccess) {
        printf("NvIspOpen failed\n");
        dlclose(lib);
        return 1;
    }

    /* Step 2: NvIspCtrlInitialize for ISP-B */
    /* NOTE: NvIspCtrlInitialize takes (NvIspContext*, NvIspIspSelect)
     * but the relationship between handle and context is unclear.
     * The context might be embedded in handle, or handle IS the context.
     * Try passing handle as context pointer. */
    printf("\nAttempting NvIspCtrlInitialize for ISP-B...\n");
    err = pCtrlInit((NvIspContext *)handle, NvIspIspSelect_B);
    printf("NvIspCtrlInitialize: err=%u\n", err);

    /* TODO: Once we know struct layouts:
     * Step 3: NvIspSetConfiguration
     * Step 4: NvIspSetOutputT12x (T124-specific output setup!)
     * Step 5: NvIspProcessFrame with raw buffer
     * Step 6: Read output buffer
     */

    /* Cleanup */
    pClose(handle);
    printf("\nNvIspClose done\n");

    dlclose(lib);
    return 0;
}
