/*
 * NvRm module ID shim — translates old encoding to new.
 *
 * libnvisp_v3.so uses NVRM_MODULE_ID(id, inst) = (id << 8) | inst
 * but libnvrm_graphics.so from the same vendor tree expects raw enum values.
 *
 * Old encoding → New encoding:
 *   0x0B00 (ISP-A)  → 11
 *   0x0B01 (ISP-B)  → 12
 *   0x0400 (VI-A)   → 4   (id=4 kept, but instance encoding changes)
 *   0x0401 (VI-B)   → ?
 *   0x1200 (VIC)    → 18  (if needed)
 *
 * Usage: LD_PRELOAD=./nvrm_shim.so LD_LIBRARY_PATH=/data/local/tmp ./isp_blob_test
 *
 * Build:
 *   arm-linux-androideabi-gcc --sysroot=$SYSROOT -shared -fPIC -o nvrm_shim.so nvrm_shim.c -ldl
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <dlfcn.h>

typedef uint32_t NvError;
typedef uint32_t NvU32;

/* Old NVRM_MODULE_ID encoding: (module_enum << 8) | instance */
static NvU32 translate_module_id(NvU32 old_id)
{
    NvU32 module = old_id >> 8;
    NvU32 instance = old_id & 0xFF;

    /* If already a small number (new encoding), pass through */
    if (old_id < 256)
        return old_id;

    /*
     * Old enum (from nvrm_module.h):
     *   4  = Vi
     *  11  = Isp
     *  12  = (next after Isp in old enum)
     *
     * New libnvrm_graphics.so device table (from strings + brute-force):
     *   4  = display
     *  11  = ISP-A (/dev/nvhost-isp)
     *  12  = ISP-B (/dev/nvhost-isp.1)
     *
     * For ISP: old (11<<8)|inst → new 11+inst
     */
    switch (module) {
    case 11: /* ISP: instance 0=ISP-A, 1=ISP-B */
        return 11 + instance;
    case 4:  /* VI */
        return 4 + instance; /* guess: 4=VI-A, 5=VI-B */
    default:
        /* Generic: just use the module enum directly */
        return module + instance;
    }
}

typedef NvError (*NvRmChannelOpen_t)(void *hRm, void **phCh,
                                     NvU32 numMod, const NvU32 *modIds);

NvError NvRmChannelOpen(void *hRm, void **phCh,
                        NvU32 numModules, const NvU32 *pModuleIDs)
{
    static NvRmChannelOpen_t real_fn = NULL;
    if (!real_fn) {
        real_fn = (NvRmChannelOpen_t)dlsym(RTLD_NEXT, "NvRmChannelOpen");
        if (!real_fn) {
            fprintf(stderr, "nvrm_shim: cannot find real NvRmChannelOpen\n");
            return 0x4; /* NotInitialized */
        }
    }

    /* Translate all module IDs */
    NvU32 new_ids[8];
    NvU32 count = numModules > 8 ? 8 : numModules;
    for (NvU32 i = 0; i < count; i++) {
        new_ids[i] = translate_module_id(pModuleIDs[i]);
        if (new_ids[i] != pModuleIDs[i])
            fprintf(stderr, "nvrm_shim: module 0x%04x → %u\n",
                    pModuleIDs[i], new_ids[i]);
    }

    return real_fn(hRm, phCh, numModules, new_ids);
}
