/*
 * Test NvIspProcessFrame with MIUI blob — based on full RE of all 4 vtable functions
 *
 * Corrected arg mapping (from disasm of 0x1784):
 *   arg1  (r0):  handle
 *   arg2  (r1):  array[0] = mode: 1=reprocess(0x09+0x0B), 2=init/streaming(0x05)
 *   arg3  (r2):  array[1] = output crop_left  (0=skip dim check)
 *   arg4  (r3):  array[2] = output crop_top
 *   arg5:        array[3] = output crop_right
 *   arg6:        array[4] = output crop_bottom
 *   arg7:        array[5] = INPUT surface ptr (NvRmSurface*) — SETUP writes 0xE3x regs
 *   arg8:        array[6] = 0
 *   arg9  (r5):  config = OUTPUT surface desc (NvRmSurface[3] + numPlanes + input_crop)
 *   arg10 (sl):  flush_fence_out ptr (or 0)
 *   arg11 (r9):  fence_out ptr (0=blocking wait)
 *   arg12 (fp):  STATUS ptr (uint32_t, init 0) — SUBMIT two-pass protocol!
 *   arg13:       &frame_count_out
 *
 * SUBMIT two-pass: 1st call *status==0 → set 1, return 0xa. 2nd call *status>=1 → submit.
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

    printf("=== NvIspProcessFrame Test (RE-based) ===\n");

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
    /* NvIspSetConfiguration(handle, type, config, &size) */
    typedef NvError (*SetConfig_t)(void *handle, uint32_t type, void *config, uint32_t *size);
    /* 13 args: r0-r3 + 9 on stack */
    typedef NvError (*ProcessFrame_t)(void *handle,
        uint32_t a2, uint32_t a3, uint32_t a4,
        uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8,
        void *config,     /* arg9: output surface config */
        uint32_t a10,     /* arg10: flush_fence_out */
        uint32_t a11,     /* arg11: fence_out (0=blocking) */
        uint32_t a12,     /* arg12: status ptr */
        uint32_t *frame_count /* arg13 */
        );

    RmOpen_t pRmOpen = dlsym(dlopen("libnvrm.so", RTLD_NOLOAD), "NvRmOpenNew");
    IspOpen_t pIspOpen = dlsym(lib, "NvIspOpen");
    IspClose_t pIspClose = dlsym(lib, "NvIspClose");
    HwCreate_t pHwCreate = dlsym(lib, "NvIspHwSettingsCreate");
    HwApply_t pHwApply = dlsym(lib, "NvIspHwSettingsApply");
    HwDestroy_t pHwDestroy = dlsym(lib, "NvIspHwSettingsDestroy");
    SetConfig_t pSetConfig = dlsym(lib, "NvIspSetConfiguration");
    ProcessFrame_t pProcess = dlsym(lib, "NvIspProcessFrame");

    printf("  ProcessFrame=%p SetConfig=%p\n", pProcess, pSetConfig);

    /* Init ISP */
    void *hRm = NULL;
    pRmOpen(&hRm);

    void *isp = NULL;
    NvError err = pIspOpen(hRm, 1, &isp);
    printf("  IspOpen: err=0x%x isp=%p\n", err, isp);
    if (err) return 1;

    /* Apply calibration (lens shading + tone curves) */
    void *settings = NULL;
    pHwCreate(isp, &settings);
    pHwApply(settings);
    printf("  Calibration applied\n");

    /*
     * NvIspSetConfiguration — REQUIRED before ProcessFrame
     * Type=2: enable output surface (simple toggle)
     * Type=1: set pixel format (16 enum fields → HW registers)
     *
     * param (arg4) is always &size — handler validates and corrects
     */

    /* Type=2: Enable output surface */
    uint32_t sc2_mode = 1;   /* 1=enable standard output */
    uint32_t sc2_size = 4;
    err = pSetConfig(isp, 2, &sc2_mode, &sc2_size);
    printf("  SetConfig(type=2, mode=1): err=0x%x\n", err);

    /* Type=1: Set pixel format for Bayer BGGR → R4G4B4A4 reprocess
     * 0x40-byte struct with 16 enum fields:
     *   [0x00] surface_type: 0=input, 1=streaming, 2=reprocess
     *   [0x04] in_pix_fmt: 0=Bayer8/Y8, 7=16bpp Bayer10+
     *   [0x08] out_pix_fmt: 2=16bpp 4:4:4:4, 10=32bpp 8:8:8:8
     *   [0x0C] color_space: 0=Bayer/default
     *   [0x10-0x3C] stage repeats (mirror primary for now) */
    /* Try multiple format combinations — find one that works */
    uint32_t fmt_cfg[16];
    uint32_t sc1_size;

    /* Combo table: {surface_type, in_pix, out_pix, colorspace, description} */
    struct { uint32_t st, ip, op, cs; const char *desc; } combos[] = {
        {2, 0, 10, 0, "reprocess bayer8→8888"},
        {2, 7, 10, 0, "reprocess bayer10→8888"},
        {2, 0,  2, 0, "reprocess bayer8→4444"},
        {2, 7,  2, 0, "reprocess bayer10→4444"},
        {2, 0,  0, 0, "reprocess all-default"},
        {2, 0,  5, 0, "reprocess bayer8→yuv422"},
        {1, 0, 10, 0, "streaming bayer8→8888"},
        {0, 0, 10, 0, "input bayer8→8888"},
    };
    int best = -1;
    for (int i = 0; i < (int)(sizeof(combos)/sizeof(combos[0])); i++) {
        memset(fmt_cfg, 0, sizeof(fmt_cfg));
        fmt_cfg[0] = combos[i].st;
        fmt_cfg[1] = combos[i].ip;
        fmt_cfg[2] = combos[i].op;
        fmt_cfg[3] = combos[i].cs;
        sc1_size = 0x40;
        err = pSetConfig(isp, 1, fmt_cfg, &sc1_size);
        printf("  SetConfig(type=1, %s): err=0x%x\n", combos[i].desc, err);
        if (err == 0 && best < 0) best = i;
    }
    if (best >= 0) {
        /* Re-apply the working combo */
        memset(fmt_cfg, 0, sizeof(fmt_cfg));
        fmt_cfg[0] = combos[best].st;
        fmt_cfg[1] = combos[best].ip;
        fmt_cfg[2] = combos[best].op;
        fmt_cfg[3] = combos[best].cs;
        sc1_size = 0x40;
        pSetConfig(isp, 1, fmt_cfg, &sc1_size);
        printf("  → Using: %s\n", combos[best].desc);
    } else {
        printf("  WARNING: no SetConfig combo worked, proceeding anyway\n");
    }

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

    /* Allocate nvmap buffers */
    int nvmap_fd = open("/dev/nvmap", O_RDWR | O_SYNC);
    struct { union { uint32_t size; int32_t fd; uint32_t id; }; uint32_t handle; } ch;

    /* Output buffer — R4G4B4A4 = 2 bytes/pixel */
    int out_size = W * H * 4;  /* over-allocate */
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
    printf("  Buffers: in=%u out=%u\n", in_h, out_h);

    /*
     * Config struct (arg9) = OUTPUT surface description
     * Layout: NvRmSurface surfaces[3] (0x00-0x8F, each 0x30 bytes)
     *         + uint32_t numPlanes (0x90)
     *         + crop rect (0x94-0xA0)
     *
     * NvRmSurface: Width(0), Height(4), ColorFormat(8), Layout(C),
     *              Pitch(10), hMem(14), Offset(18), pBase(1C),
     *              Kind(20), BlockHeightLog2(24), [pad to 0x30]
     *
     * VALIDATE checks:
     *   - Layout must be 1(pitch) or 3(blocklinear)
     *   - Pitch & 0x3F == 0 (64-byte aligned)
     *   - Offset bit 5 == 0
     *   - Format in supported output list
     *   - numPlanes matches format (1 for RGB, 2 for NV12, 3 for YUV420P)
     */
    printf("\n[3] Building descriptors...\n");

    uint8_t config[256];
    memset(config, 0, sizeof(config));
    uint32_t *cfg = (uint32_t *)config;

    /* Output surface plane 0 (NvRmSurface at offset 0x00) */
    cfg[0x00/4] = W;                     /* Width */
    cfg[0x04/4] = H;                     /* Height */
    cfg[0x08/4] = 0x2016881a;            /* R8G8B8A8 (in VALIDATE list → ISP code 0x43) */
    cfg[0x0C/4] = 1;                     /* Layout: pitch linear */
    cfg[0x10/4] = W * 4;                 /* Pitch: 10368 (64-byte aligned: 10368/64=162) */
    cfg[0x14/4] = out_h;                 /* hMem: nvmap handle */
    cfg[0x18/4] = 0;                     /* Offset */

    /* numPlanes */
    cfg[0x90/4] = 1;

    /* Input crop — full frame
     * CORRECTED order from RE: 0x94=left, 0x98=top, 0x9C=right, 0xA0=bottom */
    cfg[0x94/4] = 0;                     /* crop_left */
    cfg[0x98/4] = 0;                     /* crop_top */
    cfg[0x9C/4] = W;                     /* crop_right */
    cfg[0xA0/4] = H;                     /* crop_bottom */

    printf("  config: %ux%u fmt=0x%x layout=%u pitch=%u hmem=%u planes=%u\n",
           cfg[0], cfg[1], cfg[2], cfg[3], cfg[4], cfg[5], cfg[0x90/4]);
    printf("  crop: L=%u T=%u R=%u B=%u\n",
           cfg[0x94/4], cfg[0x98/4], cfg[0x9C/4], cfg[0xA0/4]);

    /*
     * Input surface (arg7/array[5]) — NvRmSurface for raw Bayer input
     * SETUP reads this and writes to ISP input registers 0xE30-0xE3C
     */
    uint32_t in_surf[48];  /* 0x30*3 + extra = 192 bytes (struct may need up to 0x90+) */
    memset(in_surf, 0, sizeof(in_surf));
    in_surf[0x00/4] = W;                 /* Width */
    in_surf[0x04/4] = H;                 /* Height */
    /* Try multiple input formats to find one SETUP accepts */
    uint32_t in_fmts[] = {
        0x10a92087,  /* BayerS16BGGR */
        0x10A9200E,  /* X6Bayer10BGGR */
        0x08A92004,  /* Bayer8BGGR */
        0x10992087,  /* BayerS16RGGB */
        0x10200024,  /* raw ISP value (legacy) */
    };
    const char *in_fmt_names[] = {
        "BayerS16BGGR", "X6Bayer10BGGR", "Bayer8BGGR", "BayerS16RGGB", "legacy-0x10200024",
    };
    int in_fmt_idx = 0;  /* default: BayerS16BGGR */
    if (argc >= 3) in_fmt_idx = atoi(argv[2]) % (int)(sizeof(in_fmts)/sizeof(in_fmts[0]));
    in_surf[0x08/4] = in_fmts[in_fmt_idx];
    printf("  Using input format[%d]: %s (0x%08x)\n", in_fmt_idx, in_fmt_names[in_fmt_idx], in_fmts[in_fmt_idx]);
    in_surf[0x0C/4] = 1;                 /* Layout: pitch linear */
    in_surf[0x10/4] = W * 2;             /* Pitch: 5184 bytes */
    in_surf[0x14/4] = in_h;              /* hMem: nvmap handle */
    in_surf[0x18/4] = 0;                 /* Offset */

    printf("  in_surf: %ux%u fmt=0x%x layout=%u pitch=%u hmem=%u\n",
           in_surf[0], in_surf[1], in_surf[2], in_surf[3], in_surf[4], in_surf[5]);

    /* Probe: try each input format to find which passes VALIDATE+SETUP */
    printf("\n[4] Probing input formats...\n");
    fflush(stdout);
    int working_fmt = -1;
    for (int fi = 0; fi < (int)(sizeof(in_fmts)/sizeof(in_fmts[0])); fi++) {
        in_surf[0x08/4] = in_fmts[fi];
        uint32_t probe_status = 0;
        uint32_t probe_fc = 0;
        NvError pe = pProcess(isp,
            1, 0, 0, 0, 0,
            (uint32_t)(uintptr_t)in_surf, 0,
            config, 1, 0,
            (uint32_t)(uintptr_t)&probe_status, &probe_fc);
        printf("  fmt[%d] %s (0x%08x): err=0x%x status=%u\n",
               fi, in_fmt_names[fi], in_fmts[fi], pe, probe_status);
        if ((pe == 0 || pe == 0xa) && working_fmt < 0) working_fmt = fi;
    }
    fflush(stdout);

    if (working_fmt < 0) {
        printf("  FATAL: no input format passed VALIDATE+SETUP\n");
        goto cleanup;
    }

    /* Use the working format */
    in_surf[0x08/4] = in_fmts[working_fmt];
    printf("  → Using: %s (0x%08x)\n", in_fmt_names[working_fmt], in_fmts[working_fmt]);

    /* Now do the real two-pass ProcessFrame */
    uint32_t status = 0;
    uint32_t frame_count = 0;

    /* Need fresh ISP context — close and reopen to clear probe state */
    pHwDestroy(settings);
    pIspClose(isp);
    isp = NULL;
    err = pIspOpen(hRm, 1, &isp);
    if (err) { printf("  Reopen failed: 0x%x\n", err); return 1; }
    pHwCreate(isp, &settings);
    pHwApply(settings);
    sc2_mode = 1; sc2_size = 4;
    pSetConfig(isp, 2, &sc2_mode, &sc2_size);
    memset(fmt_cfg, 0, sizeof(fmt_cfg));
    fmt_cfg[0] = combos[best >= 0 ? best : 0].st;
    fmt_cfg[1] = combos[best >= 0 ? best : 0].ip;
    fmt_cfg[2] = combos[best >= 0 ? best : 0].op;
    fmt_cfg[3] = combos[best >= 0 ? best : 0].cs;
    sc1_size = 0x40;
    pSetConfig(isp, 1, fmt_cfg, &sc1_size);
    printf("  ISP reopened + reconfigured\n");

    printf("\n[5] NvIspProcessFrame (two-pass)...\n");
    fflush(stdout);

    err = pProcess(isp,
        1,                                    /* a2/array[0]: 1=reprocess */
        0, 0, 0, 0,                          /* a3-a6: output crop=0 */
        (uint32_t)(uintptr_t)in_surf,        /* a7/array[5]: INPUT surface */
        0,                                    /* a8: 0 */
        config,                               /* a9: OUTPUT config */
        1,                                    /* a10: mode=1 (ISP-A) — MUST be non-zero! */
        0,                                    /* a11: fence_out=NULL (blocking) */
        (uint32_t)(uintptr_t)&status,        /* a12: status ptr */
        &frame_count);
    printf("  Pass 1: err=0x%x status=%u frame=%u\n", err, status, frame_count);
    fflush(stdout);

    if (err != 0 && err != 0xa) {
        printf("  FATAL: unexpected error on pass 1\n");
        goto cleanup;
    }

    if (err == 0xa) {
        printf("  → SUBMIT first-pass (expected), calling pass 2...\n");
        fflush(stdout);

        /*
         * Pass 2: SUBMIT sees *status>=1 → builds trigger gathers → flushes → waits
         * Triggers: 0x09 (start) + 0x0B (process) to register 0x00C
         */
        err = pProcess(isp,
            1,
            0, 0, 0, 0,
            (uint32_t)(uintptr_t)in_surf,
            0,
            config,
            0,
            0,
            (uint32_t)(uintptr_t)&status,
            &frame_count);
        printf("  Pass 2: err=0x%x status=%u frame=%u\n", err, status, frame_count);
        fflush(stdout);
    }

    if (err == 0) {
        printf("  SUCCESS! ISP processed frame.\n");
    } else {
        printf("  ERROR: ProcessFrame failed with 0x%x\n", err);
        /* Try a third pass in case dual-output needs status=2 */
        if (err == 0xa && status == 2) {
            printf("  → Dual-output mode, trying pass 3...\n");
            err = pProcess(isp,
                1, 0, 0, 0, 0,
                (uint32_t)(uintptr_t)in_surf, 0,
                config, 0, 0,
                (uint32_t)(uintptr_t)&status,
                &frame_count);
            printf("  Pass 3: err=0x%x status=%u frame=%u\n", err, status, frame_count);
        }
    }

    /* Read output */
    printf("\n[6] Reading output...\n");
    uint8_t check[4096];
    memset(check, 0, sizeof(check));
    rw = (typeof(rw)){ (unsigned long)check, out_h, 0, 4096, 4096, 4096, 1 };
    ioctl(nvmap_fd, _IOW('N', 7, rw), &rw);
    int nz = 0;
    for (int i = 0; i < 4096; i++) if (check[i]) nz++;
    printf("  First 4KB: %d/4096 non-zero\n", nz);

    /* Print first 64 bytes hex */
    printf("  hex[0-63]: ");
    for (int i = 0; i < 64; i++) printf("%02x ", check[i]);
    printf("\n");

    /* Check for R8G8B8A8 pattern (4 bytes per pixel) */
    if (nz > 0) {
        printf("  Pixel samples (R8G8B8A8, 4 bytes each):\n");
        uint32_t *pix = (uint32_t *)check;
        for (int i = 0; i < 16; i++)
            printf("    px[%d] = 0x%08x (R=%u G=%u B=%u A=%u)\n",
                   i, pix[i],
                   (pix[i] >> 0) & 0xFF, (pix[i] >> 8) & 0xFF,
                   (pix[i] >> 16) & 0xFF, (pix[i] >> 24) & 0xFF);
    }

    /* Also dump a larger sample to file */
    if (err == 0 || nz > 100) {
        int dump_size = W * H * 4;  /* R8G8B8A8 = 4 bytes/pixel */
        uint8_t *dump = malloc(dump_size);
        if (dump) {
            memset(dump, 0, dump_size);
            for (int off = 0; off < dump_size && off < out_size; off += chunk) {
                int sz = (dump_size - off < chunk) ? dump_size - off : chunk;
                rw = (typeof(rw)){ (unsigned long)(dump+off), out_h, off, sz, sz, sz, 1 };
                ioctl(nvmap_fd, _IOW('N', 7, rw), &rw);
            }
            FILE *of = fopen("/data/local/tmp/isp_pf_out.raw", "wb");
            if (of) {
                fwrite(dump, 1, dump_size, of);
                fclose(of);
                printf("  Output saved: /data/local/tmp/isp_pf_out.raw (%d bytes)\n", dump_size);
            }
            free(dump);
        }
    }

cleanup:
    pHwDestroy(settings);
    pIspClose(isp);
    close(nvmap_fd);
    printf("\n=== Done ===\n");
    return 0;
}
