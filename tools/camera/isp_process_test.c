/*
 * Test NvIspProcessFrame — two approaches:
 *   A) NvRmMemHandle buffers (so NvRm can resolve IOVA in PushReloc)
 *   B) Blob VALIDATE+SETUP+RUNTIME + manual trigger (hybrid)
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
/* nvhost structs (from kernel headers) */
struct nvhost_cmdbuf { uint32_t mem; uint32_t offset; uint32_t words; };
struct nvhost_syncpt_incr { uint32_t syncpt_id; uint32_t syncpt_incrs; };
struct nvhost32_submit_args {
    uint32_t submit_version;
    uint32_t num_syncpt_incrs;
    uint32_t num_cmdbufs;
    uint32_t num_relocs;
    uint32_t num_waitchks;
    uint32_t timeout;
    uint32_t syncpt_incrs;
    uint32_t cmdbufs;
    uint32_t relocs;
    uint32_t reloc_shifts;
    uint32_t waitchks;
    uint32_t waitbases;
    uint32_t class_ids;
    uint32_t fences;
    uint32_t fence;
};

typedef uint32_t NvError;
typedef uint32_t NvU32;
#define NvSuccess 0
#define W 2592
#define H 1944

/* host1x opcodes */
#define OP_INCR(off,cnt) (((1)<<28)|((off)<<16)|(cnt))
#define OP_NONINCR(off,cnt) (((2)<<28)|((off)<<16)|(cnt))

int main(int argc, char **argv)
{
    if (argc < 3) { printf("Usage: %s <raw_bayer> <A|B>\n  A = NvRm handles\n  B = hybrid trigger\n", argv[0]); return 1; }
    int mode_a = (argv[2][0] == 'A' || argv[2][0] == 'a');
    int mode_b = (argv[2][0] == 'B' || argv[2][0] == 'b');

    printf("=== ISP ProcessFrame Test (A: NvRm handles, B: hybrid trigger) ===\n");

    /* Load libs */
    dlopen("libnvos.so", RTLD_NOW | RTLD_GLOBAL);
    void *lib_nvrm = dlopen("libnvrm.so", RTLD_NOW | RTLD_GLOBAL);
    dlopen("libnvrm_graphics.so", RTLD_NOW | RTLD_GLOBAL);
    void *lib = dlopen("libnvisp_v3.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { printf("FATAL: %s\n", dlerror()); return 1; }

    /* ISP API */
    typedef NvError (*RmOpen_t)(void **);
    typedef NvError (*IspOpen_t)(void *, NvU32, void **);
    typedef void (*IspClose_t)(void *);
    typedef NvError (*HwCreate_t)(void *, void **);
    typedef NvError (*HwApply_t)(void *);
    typedef NvError (*HwDestroy_t)(void *);
    typedef NvError (*SetConfig_t)(void *, uint32_t, void *, uint32_t *);
    typedef NvError (*Flush_t)(void *, void *);
    typedef NvError (*ProcessFrame_t)(void *,
        uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
        void *, uint32_t, uint32_t, uint32_t, uint32_t *);

    /* NvRm memory API */
    typedef NvError (*MemCreate_t)(void *hRm, void **phMem, NvU32 size);
    typedef NvError (*MemAlloc_t)(void *hMem, void *heaps, NvU32 numHeaps,
                                  NvU32 align, NvU32 attr);
    typedef void (*MemWrite_t)(void *hMem, NvU32 offset, void *data, NvU32 size);
    typedef void (*MemRead_t)(void *hMem, NvU32 offset, void *data, NvU32 size);

    RmOpen_t pRmOpen = dlsym(lib_nvrm, "NvRmOpenNew");
    IspOpen_t pIspOpen = dlsym(lib, "NvIspOpen");
    IspClose_t pIspClose = dlsym(lib, "NvIspClose");
    HwCreate_t pHwCreate = dlsym(lib, "NvIspHwSettingsCreate");
    HwApply_t pHwApply = dlsym(lib, "NvIspHwSettingsApply");
    HwDestroy_t pHwDestroy = dlsym(lib, "NvIspHwSettingsDestroy");
    SetConfig_t pSetConfig = dlsym(lib, "NvIspSetConfiguration");
    Flush_t pFlush = dlsym(lib, "NvIspFlush");
    ProcessFrame_t pProcess = dlsym(lib, "NvIspProcessFrame");

    MemCreate_t pMemCreate = dlsym(lib_nvrm, "NvRmMemHandleCreate");
    MemAlloc_t pMemAlloc = dlsym(lib_nvrm, "NvRmMemAlloc");
    MemWrite_t pMemWrite = dlsym(lib_nvrm, "NvRmMemWrite");
    MemRead_t pMemRead = dlsym(lib_nvrm, "NvRmMemRead");

    printf("  MemCreate=%p MemAlloc=%p MemWrite=%p MemRead=%p\n",
           pMemCreate, pMemAlloc, pMemWrite, pMemRead);
    printf("  Flush=%p\n", pFlush);

    /* Init */
    void *hRm = NULL;
    pRmOpen(&hRm);
    printf("  hRm=%p\n", hRm);

    void *isp = NULL;
    NvError err = pIspOpen(hRm, 1, &isp);
    printf("  IspOpen: err=0x%x isp=%p\n", err, isp);
    if (err) return 1;

    void *settings = NULL;
    pHwCreate(isp, &settings);
    pHwApply(settings);
    printf("  Calibration applied\n");

    uint32_t sc2_mode = 1, sc2_size = 4;
    pSetConfig(isp, 2, &sc2_mode, &sc2_size);
    uint32_t fmt_cfg[16] = {0};
    fmt_cfg[0] = 2; fmt_cfg[1] = 0; fmt_cfg[2] = 10;
    uint32_t sc1_size = 0x40;
    pSetConfig(isp, 1, fmt_cfg, &sc1_size);
    printf("  SetConfig done\n");

    /* Load raw */
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

    /* ========== Approach A: NvRmMemHandle buffers ========== */
    printf("\n[A] Creating NvRmMemHandle buffers...\n");
    fflush(stdout);

    int out_size = W * H * 4;
    void *out_mem = NULL, *in_mem = NULL;

    if (pMemCreate && pMemAlloc) {
        /* NvRmMemHandleCreate(hRm, &handle, size) */
        err = pMemCreate(hRm, &out_mem, out_size);
        printf("  MemCreate(out, %d): err=0x%x handle=%p\n", out_size, err, out_mem);

        if (err == 0 && out_mem) {
            /* NvRmMemAlloc(hMem, heaps, numHeaps, align, attr)
             * heaps=NULL → default, numHeaps=0, align=4096, attr=0 */
            err = pMemAlloc(out_mem, NULL, 0, 4096, 0);
            printf("  MemAlloc(out): err=0x%x\n", err);
        }

        err = pMemCreate(hRm, &in_mem, fsize);
        printf("  MemCreate(in, %d): err=0x%x handle=%p\n", fsize, err, in_mem);

        if (err == 0 && in_mem) {
            err = pMemAlloc(in_mem, NULL, 0, 4096, 0);
            printf("  MemAlloc(in): err=0x%x\n", err);
            if (err == 0 && pMemWrite) {
                printf("  Writing raw data to NvRm input buffer...\n");
                fflush(stdout);
                /* Write in chunks */
                int chunk = 65536;
                for (int off = 0; off < fsize; off += chunk) {
                    int sz = (fsize - off < chunk) ? fsize - off : chunk;
                    pMemWrite(in_mem, off, raw + off, sz);
                }
                printf("  Done\n");
            }
        }
    } else {
        printf("  NvRm memory API not available, skipping approach A\n");
    }
    free(raw);
    fflush(stdout);

    /* Also keep raw nvmap handles for approach B */
    int nvmap_fd = open("/dev/nvmap", O_RDWR | O_SYNC);
    struct { union { uint32_t size; int32_t fd; uint32_t id; }; uint32_t handle; } ch;
    ch.size = out_size;
    ioctl(nvmap_fd, _IOWR('N', 0, ch), &ch);
    uint32_t out_h_raw = ch.handle;
    struct { uint32_t handle, heap, flags, align; } ah = { out_h_raw, 1<<30, 2, 4096 };
    ioctl(nvmap_fd, _IOW('N', 3, ah), &ah);

    ch.size = fsize;
    ioctl(nvmap_fd, _IOWR('N', 0, ch), &ch);
    uint32_t in_h_raw = ch.handle;
    ah.handle = in_h_raw;
    ioctl(nvmap_fd, _IOW('N', 3, ah), &ah);

    /* Write raw to nvmap input (for approach B) */
    {
        FILE *f2 = fopen(argv[1], "rb");
        uint8_t *raw2 = malloc(fsize);
        fread(raw2, 1, fsize, f2);
        fclose(f2);
        struct { unsigned long addr; uint32_t handle, offset, elem_size, hmem_stride, user_stride, count; } rw;
        int chunk = 65536;
        for (int off = 0; off < fsize; off += chunk) {
            int sz = (fsize - off < chunk) ? fsize - off : chunk;
            rw = (typeof(rw)){ (unsigned long)(raw2+off), in_h_raw, off, sz, sz, sz, 1 };
            ioctl(nvmap_fd, _IOW('N', 6, rw), &rw);
        }
        free(raw2);
    }
    printf("  nvmap handles: in=%u out=%u\n", in_h_raw, out_h_raw);

    /* ========== Try Approach A: ProcessFrame with NvRm handles ========== */
    if (mode_a && out_mem && in_mem) {
        printf("\n[A] ProcessFrame with NvRmMemHandle...\n");
        fflush(stdout);

        uint8_t *config_a = calloc(1, 256);
        uint32_t *cfg_a = (uint32_t *)config_a;
        cfg_a[0x00/4] = W;
        cfg_a[0x04/4] = H;
        cfg_a[0x08/4] = 0x2016881a;  /* R8G8B8A8 */
        cfg_a[0x0C/4] = 1;           /* pitch */
        cfg_a[0x10/4] = W * 4;       /* pitch bytes */
        cfg_a[0x14/4] = (uint32_t)(uintptr_t)out_mem;  /* NvRmMemHandle! */
        cfg_a[0x90/4] = 1;
        cfg_a[0x94/4] = 0; cfg_a[0x98/4] = 0;
        cfg_a[0x9C/4] = W; cfg_a[0xA0/4] = H;

        uint32_t *in_surf_a = calloc(48, sizeof(uint32_t));
        in_surf_a[0] = W; in_surf_a[1] = H;
        in_surf_a[2] = 0x10a92087;   /* BayerS16BGGR */
        in_surf_a[3] = 1;            /* pitch */
        in_surf_a[4] = W * 2;
        in_surf_a[5] = (uint32_t)(uintptr_t)in_mem;  /* NvRmMemHandle! */

        printf("  config hMem=%p, in_surf hMem=%p\n", out_mem, in_mem);
        fflush(stdout);

        /* Direct vtable calls */
        uint32_t *isp_u32 = (uint32_t *)isp;
        typedef NvError (*ValidateFn)(void *, void *, void *);
        typedef NvError (*SetupFn)(void *, void *, void *, void *);
        typedef NvError (*RuntimeFn)(void *, void *, uint32_t, uint32_t);
        typedef NvError (*SubmitFn)(void *, uint32_t, uint32_t, uint32_t, uint32_t);

        uint32_t array_a[7] = {1, 0,0,0,0, (uint32_t)(uintptr_t)in_surf_a, 0};

        err = ((ValidateFn)(uintptr_t)isp_u32[0x130C/4])(isp, array_a, config_a);
        printf("  VALIDATE: err=0x%x\n", err);
        fflush(stdout);

        if (err == 0) {
            uint32_t seq = 0;
            err = ((SetupFn)(uintptr_t)isp_u32[0x1308/4])(isp, array_a, config_a, &seq);
            printf("  SETUP: err=0x%x seq=%u\n", err, seq);
            fflush(stdout);
        }
        if (err == 0) {
            err = ((RuntimeFn)(uintptr_t)isp_u32[0x12D4/4])(isp, config_a, 1, 0);
            printf("  RUNTIME: err=0x%x\n", err);
            fflush(stdout);
        }
        if (err == 0) {
            uint32_t fence_a[8] = {0}, status_a = 0;
            err = ((SubmitFn)(uintptr_t)isp_u32[0x1304/4])(
                isp, 1, (uint32_t)(uintptr_t)fence_a, 0, (uint32_t)(uintptr_t)&status_a);
            printf("  SUBMIT: err=0x%x status=%u fence={0x%x,0x%x}\n",
                   err, status_a, fence_a[0], fence_a[1]);
            fflush(stdout);

            if (err == 0 && pMemRead) {
                uint8_t check_a[64];
                pMemRead(out_mem, 0, check_a, 64);
                int nz = 0;
                for (int i = 0; i < 64; i++) if (check_a[i]) nz++;
                printf("  Output first 64 bytes: %d/64 non-zero\n", nz);
                printf("  hex: ");
                for (int i = 0; i < 32; i++) printf("%02x ", check_a[i]);
                printf("\n");
            }
        }
        free(config_a); free(in_surf_a);
    }

    /* ========== Approach B: Blob config + manual trigger ========== */
    if (!mode_b) goto done;
    printf("\n[B] Hybrid: blob VALIDATE+SETUP+RUNTIME + manual trigger...\n");
    fflush(stdout);

    /* Reopen ISP for clean state */
    pHwDestroy(settings);
    pIspClose(isp);
    isp = NULL;
    err = pIspOpen(hRm, 1, &isp);
    if (err) { printf("  Reopen failed: 0x%x\n", err); goto done; }
    pHwCreate(isp, &settings);
    pHwApply(settings);
    sc2_mode = 1; sc2_size = 4;
    pSetConfig(isp, 2, &sc2_mode, &sc2_size);
    memset(fmt_cfg, 0, sizeof(fmt_cfg));
    fmt_cfg[0] = 2; fmt_cfg[1] = 0; fmt_cfg[2] = 10;
    sc1_size = 0x40;
    pSetConfig(isp, 1, fmt_cfg, &sc1_size);

    {
        uint32_t *isp_u32 = (uint32_t *)isp;

        /* Use raw nvmap handles for surfaces */
        uint8_t *config_b = calloc(1, 256);
        uint32_t *cfg_b = (uint32_t *)config_b;
        cfg_b[0x00/4] = W; cfg_b[0x04/4] = H;
        cfg_b[0x08/4] = 0x2016881a; cfg_b[0x0C/4] = 1;
        cfg_b[0x10/4] = W * 4;
        cfg_b[0x14/4] = out_h_raw;  /* raw nvmap handle */
        cfg_b[0x90/4] = 1;
        cfg_b[0x94/4] = 0; cfg_b[0x98/4] = 0;
        cfg_b[0x9C/4] = W; cfg_b[0xA0/4] = H;

        uint32_t *in_surf_b = calloc(48, sizeof(uint32_t));
        in_surf_b[0] = W; in_surf_b[1] = H;
        in_surf_b[2] = 0x10a92087; in_surf_b[3] = 1;
        in_surf_b[4] = W * 2;
        in_surf_b[5] = in_h_raw;   /* raw nvmap handle */

        uint32_t array_b[7] = {1, 0,0,0,0, (uint32_t)(uintptr_t)in_surf_b, 0};

        typedef NvError (*ValidateFn)(void *, void *, void *);
        typedef NvError (*SetupFn)(void *, void *, void *, void *);
        typedef NvError (*RuntimeFn)(void *, void *, uint32_t, uint32_t);

        err = ((ValidateFn)(uintptr_t)isp_u32[0x130C/4])(isp, array_b, config_b);
        printf("  VALIDATE: err=0x%x\n", err);
        if (err == 0) {
            uint32_t seq = 0;
            err = ((SetupFn)(uintptr_t)isp_u32[0x1308/4])(isp, array_b, config_b, &seq);
            printf("  SETUP: err=0x%x\n", err);
        }
        if (err == 0) {
            err = ((RuntimeFn)(uintptr_t)isp_u32[0x12D4/4])(isp, config_b, 1, 0);
            printf("  RUNTIME: err=0x%x\n", err);
        }

        /*
         * Key insight: blob SETUP writes INPUT regs (0xE3x) but NOBODY writes
         * OUTPUT regs (0xE00-0xE0A). We need to push output config into the
         * blob's NvRmStream before calling SUBMIT.
         *
         * Dump ISP context to find NvRmStream pointer, then use NvRmStream API
         * to push output registers.
         */
        if (err == 0) {
            printf("  Dumping ISP context to find stream handle...\n");
            /* Scan first 0x30 bytes for pointers (large values = heap ptrs) */
            for (int i = 0; i < 0x30/4; i++)
                printf("    ctx+0x%02x = 0x%08x\n", i*4, isp_u32[i]);
            /* Also check around 0x1270 (syncpt struct) */
            printf("    ctx+0x1270 = 0x%08x (syncpt struct)\n", isp_u32[0x1270/4]);
            printf("    ctx+0x1318 = 0x%08x (ring buf)\n", isp_u32[0x1318/4]);
            fflush(stdout);

            /* Try to use NvRmStream API to push output registers.
             * NvRmStreamBegin signature: (pStream, numWords, numRelocs, numSyncptIncrs)
             * We need to find pStream — it should be a pointer in the ISP context. */
            typedef void *(*StreamBeginFn)(void *stream, uint32_t words, uint32_t waits, uint32_t relocs);
            typedef void (*StreamEndFn)(void *stream);
            typedef void *(*StreamPushSetClassFn)(void *stream, void *cur, uint32_t class_id, uint32_t subchan);
            typedef void (*StreamPushRelocFn)(void *stream, void *cur, void *hMem, uint32_t offset);

            StreamBeginFn pStreamBegin = dlsym(lib_nvrm, "NvRmStreamBegin");
            StreamEndFn pStreamEnd = dlsym(lib_nvrm, "NvRmStreamEnd");
            StreamPushSetClassFn pPushClass = dlsym(lib_nvrm, "NvRmStreamPushSetClass");
            StreamPushRelocFn pPushReloc = dlsym(lib_nvrm, "NvRmStreamPushReloc");
            printf("  StreamBegin=%p StreamEnd=%p PushClass=%p PushReloc=%p\n",
                   pStreamBegin, pStreamEnd, pPushClass, pPushReloc);
            fflush(stdout);

            /* The stream pointer: scan ISP context for a heap pointer that looks
             * like an NvRmStream. Candidates: any value >= 0x80000000 in first 0x20 bytes */
            void *stream = NULL;
            for (int i = 0; i < 0x20/4; i++) {
                if (isp_u32[i] >= 0x80000000) {
                    printf("    Candidate stream at ctx+0x%02x = 0x%08x\n", i*4, isp_u32[i]);
                    if (!stream) stream = (void *)(uintptr_t)isp_u32[i];
                }
            }

            if (stream && pStreamBegin && pStreamEnd) {
                printf("  Pushing output regs via NvRmStream (stream=%p)...\n", stream);
                fflush(stdout);

                /* Push output surface config: 0xE00(width), 0xE01(height), 0xE02(format) */
                void *cur = pStreamBegin(stream, 16, 0, 1);  /* 16 words, 0 waits, 1 reloc */
                if (cur && pPushClass) {
                    cur = pPushClass(stream, cur, 0x32, 0);  /* ISP-A class */

                    /* Manually write INCR opcodes into the stream */
                    uint32_t **pp = (uint32_t **)&cur;

                    /* Output dims + format */
                    *(*pp)++ = OP_INCR(0xE00, 1);
                    *(*pp)++ = ((W - 1) & 0x3FFF) << 16;
                    *(*pp)++ = OP_INCR(0xE01, 1);
                    *(*pp)++ = ((H - 1) & 0x3FFF) << 16;
                    *(*pp)++ = OP_INCR(0xE02, 1);
                    *(*pp)++ = 0x010000C9;  /* output format (from working isp_reprocess) */

                    /* Output surface Y plane */
                    *(*pp)++ = OP_INCR(0xE04, 3);
                    if (pPushReloc) {
                        /* Use reloc for IOVA resolution */
                        pPushReloc(stream, *pp, (void *)(uintptr_t)out_h_raw, 0);
                        (*pp)++;
                    } else {
                        *(*pp)++ = 0;  /* IOVA placeholder */
                    }
                    *(*pp)++ = 0;
                    *(*pp)++ = W * 4;  /* output pitch */

                    /* Processing block (demosaic) */
                    *(*pp)++ = OP_INCR(0x500, 1);
                    *(*pp)++ = 0;  /* no special flags */

                    pStreamEnd(stream);
                    printf("  Output regs pushed OK\n");
                } else {
                    printf("  StreamBegin returned %p, skipping push\n", cur);
                }
                fflush(stdout);
            }

            /* Now call SUBMIT via vtable — it will add triggers + flush everything */
            printf("  Calling SUBMIT...\n");
            fflush(stdout);
            typedef NvError (*SubmitFn)(void *, uint32_t, uint32_t, uint32_t, uint32_t);
            uint32_t fence_b[8] = {0}, status_b = 0;
            err = ((SubmitFn)(uintptr_t)isp_u32[0x1304/4])(
                isp, 1, (uint32_t)(uintptr_t)fence_b, 0, (uint32_t)(uintptr_t)&status_b);
            printf("  SUBMIT: err=0x%x status=%u fence={0x%x,0x%x}\n",
                   err, status_b, fence_b[0], fence_b[1]);
            fflush(stdout);

            /* === DUMP 1: Ring buffer — find where ISP wrote output === */
            printf("\n  [B-dump1] Ring buffer (ctx+0x1318):\n");
            uint32_t *ring = (uint32_t *)(uintptr_t)isp_u32[0x1318/4];
            if (ring) {
                /* Ring header */
                for (int i = 0; i < 16; i++)
                    printf("    ring+0x%02x = 0x%08x\n", i*4, ring[i]);
                /* First slot: ring_base + 0*0x250 + 0x10 */
                /* DMA handle at slot+0x23C */
                uint32_t *slot0 = (uint32_t *)((uint8_t *)ring + 0x10);
                printf("    slot0+0x238 = 0x%08x\n", slot0[0x238/4]);
                printf("    slot0+0x23C = 0x%08x (DMA handle?)\n", slot0[0x23C/4]);
                printf("    slot0+0x240 = 0x%08x\n", slot0[0x240/4]);
                printf("    slot0+0x244 = 0x%08x\n", slot0[0x244/4]);
                /* Scan for any nvmap-like handles (values 0x400-0x500 range) */
                printf("    Scanning slot0 for handles...\n");
                for (int i = 0; i < 0x250/4; i++) {
                    uint32_t v = slot0[i];
                    if (v >= 0x400 && v < 0x500)
                        printf("      slot0+0x%03x = 0x%08x (handle?)\n", i*4, v);
                }
            }
            fflush(stdout);

            /* === DUMP 2: NvRmStream struct layout === */
            printf("\n  [B-dump2] NvRmStream (ctx+0x0c -> %p):\n", stream);
            if (stream) {
                uint32_t *s = (uint32_t *)stream;
                for (int i = 0; i < 32; i++)
                    printf("    stream+0x%02x = 0x%08x\n", i*4, s[i]);
                /* SUBMIT accesses stream+0x4c as pCurrent */
                printf("    stream+0x4c (pCurrent?) = 0x%08x\n", s[0x4c/4]);
                printf("    stream+0x50 = 0x%08x\n", s[0x50/4]);
            }
            fflush(stdout);

            /* Check our output buffer (probably empty) */
            struct { unsigned long addr; uint32_t handle, offset, elem_size, hmem_stride, user_stride, count; } rw3;
            uint8_t check_b[4096];
            memset(check_b, 0, sizeof(check_b));
            rw3 = (typeof(rw3)){ (unsigned long)check_b, out_h_raw, 0, 4096, 4096, 4096, 1 };
            ioctl(nvmap_fd, _IOW('N', 7, rw3), &rw3);
            int nz = 0;
            for (int i = 0; i < 4096; i++) if (check_b[i]) nz++;
            printf("\n  Our output buf: %d/4096 non-zero\n", nz);

            /* Try reading the ring buffer's DMA handle if it looks valid */
            if (ring) {
                uint32_t *slot0 = (uint32_t *)((uint8_t *)ring + 0x10);
                uint32_t dma_h = slot0[0x23C/4];
                if (dma_h > 0 && dma_h < 0x10000) {
                    printf("  Trying to read DMA handle 0x%x...\n", dma_h);
                    memset(check_b, 0, sizeof(check_b));
                    rw3 = (typeof(rw3)){ (unsigned long)check_b, dma_h, 0, 4096, 4096, 4096, 1 };
                    int ret = ioctl(nvmap_fd, _IOW('N', 7, rw3), &rw3);
                    if (ret == 0) {
                        nz = 0;
                        for (int i = 0; i < 4096; i++) if (check_b[i]) nz++;
                        printf("  Ring DMA buf: %d/4096 non-zero\n", nz);
                        printf("  hex: ");
                        for (int i = 0; i < 64; i++) printf("%02x ", check_b[i]);
                        printf("\n");
                    } else {
                        printf("  Read failed (ret=%d)\n", ret);
                    }
                }
            }
        }
skip_b:
        free(config_b); free(in_surf_b);
    }

done:
    pHwDestroy(settings);
    pIspClose(isp);
    close(nvmap_fd);
    printf("\n=== Done ===\n");
    return 0;
}
