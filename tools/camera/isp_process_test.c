/*
 * Test NvIspProcessFrame with MIUI blob
 *
 * NvIspProcessFrame(handle, surf0, surf1, surf2,
 *                   hw_settings, trigger?, input_buf?, input_param?,
 *                   frame_count_out?)
 *
 * Build: $CC --sysroot=$SYSROOT -std=gnu99 -pie -o isp_process_test isp_process_test.c -ldl
 * Run: LD_PRELOAD=nvrm_shim.so LD_LIBRARY_PATH=/data/local/tmp ./isp_process_test raw_file
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

typedef uint32_t NvError;
typedef uint32_t NvU32;
#define NvSuccess 0

#define W 2592
#define H 1944

int main(int argc, char **argv)
{
    if (argc < 2) { printf("Usage: %s <raw_bayer>\n", argv[0]); return 1; }

    printf("=== NvIspProcessFrame Test ===\n");

    /* Load libs */
    dlopen("libnvos.so", RTLD_NOW | RTLD_GLOBAL);
    dlopen("libnvrm.so", RTLD_NOW | RTLD_GLOBAL);
    dlopen("libnvrm_graphics.so", RTLD_NOW | RTLD_GLOBAL);
    void *lib = dlopen("libnvisp_v3.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { printf("FATAL: %s\n", dlerror()); return 1; }

    typedef NvError (*RmOpen_t)(void **);
    typedef NvError (*IspOpen_t)(void *, NvU32, void **);
    typedef void (*IspClose_t)(void *);
    typedef NvError (*HwCreate_t)(void *, void **);
    typedef NvError (*HwApply_t)(void *);
    typedef NvError (*HwDestroy_t)(void *);
    /* 13 args! stack frame analysis:
     * r5=sp+0x58=original_sp+0x10=arg9, sl=arg10, r9=arg11, fp=arg12, arg13
     * r0=handle, r1-r3=args2-4, sp+0..sp+0x10=args5-9 */
    typedef NvError (*ProcessFrame_t)(void *handle,
        uint32_t a2, uint32_t a3, uint32_t a4,
        uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8,
        void *settings,   /* arg9 = r5 in func */
        uint32_t a10,     /* arg10 = sl */
        uint32_t a11,     /* arg11 = r9 (optional: input?) */
        uint32_t a12,     /* arg12 = fp (needed if a11!=0) */
        uint32_t *frame_count /* arg13 */
        );
    typedef NvError (*Flush_t)(void *);
    typedef NvError (*MemCreate_t)(void *, void **, NvU32);
    typedef NvError (*MemAlloc_t)(void *, void *, NvU32, NvU32, NvU32, NvU32);
    typedef void (*MemFree_t)(void *);
    typedef NvError (*MemPin_t)(void *, unsigned long *);

    RmOpen_t pRmOpen = dlsym(dlopen("libnvrm.so", RTLD_NOLOAD), "NvRmOpenNew");
    IspOpen_t pIspOpen = dlsym(lib, "NvIspOpen");
    IspClose_t pIspClose = dlsym(lib, "NvIspClose");
    HwCreate_t pHwCreate = dlsym(lib, "NvIspHwSettingsCreate");
    HwApply_t pHwApply = dlsym(lib, "NvIspHwSettingsApply");
    HwDestroy_t pHwDestroy = dlsym(lib, "NvIspHwSettingsDestroy");
    ProcessFrame_t pProcess = dlsym(lib, "NvIspProcessFrame");
    Flush_t pFlush = dlsym(lib, "NvIspFlush");

    /* NvRm memory functions for creating NvRmMemHandle-style buffers */
    void *lib_nvrm = dlopen("libnvrm.so", RTLD_NOLOAD);
    MemCreate_t pMemCreate = dlsym(lib_nvrm, "NvRmMemHandleCreate");
    MemAlloc_t pMemAlloc = dlsym(lib_nvrm, "NvRmMemHandleAlloc");
    MemFree_t pMemFree = dlsym(lib_nvrm, "NvRmMemHandleFree");

    printf("  ProcessFrame=%p\n", pProcess);

    /* Init ISP */
    void *hRm = NULL;
    pRmOpen(&hRm);

    void *isp = NULL;
    NvError err = pIspOpen(hRm, 1, &isp);
    printf("  IspOpen: err=0x%x isp=%p\n", err, isp);
    if (err) return 1;

    void *settings = NULL;
    pHwCreate(isp, &settings);
    pHwApply(settings);
    printf("  Calibration applied\n");

    /* Load raw file */
    printf("\n[2] Loading %s...\n", argv[1]);
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END);
    int fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *raw = malloc(fsize);
    fread(raw, 1, fsize, f);
    fclose(f);
    printf("  %d bytes loaded\n", fsize);

    /* Allocate nvmap buffers via ioctl (NvRm MemHandle is for blob's internal use) */
    int nvmap_fd = open("/dev/nvmap", O_RDWR | O_SYNC);

    /* Create NvRmMemHandles for output — blob expects these */
    /* For now, try passing nvmap handles directly as u32 */
    struct { union { uint32_t size; int32_t fd; uint32_t id; }; uint32_t handle; } ch;

    /* Output buffer */
    int out_size = W * H * 4;  /* RGBA just in case */
    ch.size = out_size;
    ioctl(nvmap_fd, _IOWR('N', 0, ch), &ch);
    uint32_t out_h = ch.handle;
    struct { uint32_t handle, heap, flags, align; } ah = { out_h, 1<<30, 2, 4096 };
    ioctl(nvmap_fd, _IOW('N', 3, ah), &ah);

    /* Input buffer */
    ch.size = fsize;
    ioctl(nvmap_fd, _IOWR('N', 0, ch), &ch);
    uint32_t in_h = ch.handle;
    ah.handle = in_h;
    ioctl(nvmap_fd, _IOW('N', 3, ah), &ah);

    /* Write raw data to input nvmap */
    struct { unsigned long addr; uint32_t handle, offset, elem_size, hmem_stride, user_stride, count; } rw;
    int chunk = 65536;
    for (int off = 0; off < fsize; off += chunk) {
        int sz = (fsize - off < chunk) ? fsize - off : chunk;
        rw = (typeof(rw)){ (unsigned long)(raw+off), in_h, off, sz, sz, sz, 1 };
        ioctl(nvmap_fd, _IOW('N', 6, rw), &rw);
    }
    free(raw);
    printf("  Buffers ready: in=%u out=%u\n", in_h, out_h);

    /* Build NvRmSurface for output (arg2)
     * NvRmSurface layout: Width, Height, ColorFormat, Layout, Pitch,
     *                     hMem, Offset, pBase, Kind, BlockHeightLog2 */
    printf("\n[3] Building surface descriptors...\n");
    uint32_t out_surf[10];
    memset(out_surf, 0, sizeof(out_surf));
    out_surf[0] = W;               /* Width */
    out_surf[1] = H;               /* Height */
    out_surf[2] = 0x2010531a;      /* ColorFormat: RGBA8 (X8Y8Z8W8) */
    out_surf[3] = 0;               /* Layout: pitch linear */
    out_surf[4] = W * 4;           /* Pitch: stride in bytes */
    out_surf[5] = out_h;           /* hMem: nvmap handle */
    out_surf[6] = 0;               /* Offset */
    printf("  out_surf: %ux%u fmt=0x%x pitch=%u hmem=%u\n",
           out_surf[0], out_surf[1], out_surf[2], out_surf[4], out_surf[5]);

    /* Build NvRmSurface for input (arg11) */
    uint32_t in_surf[10];
    memset(in_surf, 0, sizeof(in_surf));
    in_surf[0] = W;                /* Width */
    in_surf[1] = H;                /* Height */
    in_surf[2] = 0x10200024;       /* ColorFormat: 10-bit Bayer BGGR */
    in_surf[3] = 0;                /* Layout: pitch linear */
    in_surf[4] = W * 2;            /* Pitch: stride */
    in_surf[5] = in_h;             /* hMem: nvmap handle */
    in_surf[6] = 0;                /* Offset */
    printf("  in_surf: %ux%u fmt=0x%x pitch=%u hmem=%u\n",
           in_surf[0], in_surf[1], in_surf[2], in_surf[4], in_surf[5]);

    /* Config struct for arg9 — contains output dims + crop */
    uint8_t config[256];
    memset(config, 0, sizeof(config));
    ((uint32_t *)config)[0] = W;               /* output width */
    ((uint32_t *)config)[1] = H;               /* output height */
    ((uint32_t *)config)[2] = 0x2010531a;      /* output format */

    printf("\n[4] Calling NvIspProcessFrame...\n");
    fflush(stdout);

    uint32_t frame_count = 0;

    /* args 2-4: NvRmSurface* (output planes Y, U, V)
     * arg9: config struct
     * arg10: mode (1=ISP-A)
     * arg11: NvRmSurface* input (non-zero = reprocess)
     * arg12: input param (required if arg11!=0) */
    err = pProcess(isp,
                   (uint32_t)(uintptr_t)out_surf,  /* a2: output surface 0 */
                   0,                               /* a3: output surface 1 */
                   0,                               /* a4: output surface 2 */
                   0, 0, 0, 0,                      /* a5-a8: unused */
                   config,                           /* a9: config */
                   1,                                /* a10: mode */
                   (uint32_t)(uintptr_t)in_surf,    /* a11: input surface */
                   (uint32_t)fsize,                  /* a12: input param */
                   &frame_count                      /* a13: frame count */
                   );
    printf("  NvIspProcessFrame: err=0x%x frame_count=%u\n", err, frame_count);
    fflush(stdout);

    /* Read output */
    printf("\n[4] Reading output...\n");
    uint8_t check[4096];
    memset(check, 0, sizeof(check));
    rw = (typeof(rw)){ (unsigned long)check, out_h, 0, 4096, 4096, 4096, 1 };
    ioctl(nvmap_fd, _IOW('N', 7, rw), &rw);
    int nz = 0;
    for (int i = 0; i < 4096; i++) if (check[i]) nz++;
    printf("  First 4KB: %d/4096 non-zero\n", nz);
    printf("  hex: ");
    for (int i = 0; i < 32; i++) printf("%02x ", check[i]);
    printf("\n");

    /* Cleanup */
    pHwDestroy(settings);
    pIspClose(isp);
    close(nvmap_fd);
    printf("\n=== Done ===\n");
    return 0;
}
