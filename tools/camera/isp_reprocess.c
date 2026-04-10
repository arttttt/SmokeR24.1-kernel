/*
 * ISP reprocess test — feed raw Bayer frame through HW ISP
 *
 * Flow:
 *   1. Init ISP via MIUI blobs (NvIspOpen + HwSettingsApply = calibration)
 *   2. Allocate nvmap input/output buffers
 *   3. Load raw Bayer frame into input buffer
 *   4. Build reprocess gather:
 *      - Output surfaces (0xE00-0xE0A)
 *      - Input surfaces (0xE31-0xE3A) + trigger (0xE30)
 *      - ISP_ENABLE (0x015)
 *      - Processing block (0x500)
 *      - Syncpt incrs (cond 4,5,6)
 *      - ISP_CONTROL trigger (0x00C = 0x0B for reprocess)
 *   5. Submit via NvRmStream (from MIUI blobs)
 *   6. Wait for completion
 *   7. Dump output
 *
 * Input: 2592x1944 BG10 (10-bit Bayer BGGR, 16-bit LE containers)
 * Output: YUV (format 0x04FE00E6 from stock)
 *
 * Build:
 *   $CC --sysroot=$SYSROOT -std=gnu99 -pie -o isp_reprocess isp_reprocess.c -ldl
 *
 * Usage:
 *   LD_PRELOAD=nvrm_shim.so LD_LIBRARY_PATH=/data/local/tmp \
 *     ./isp_reprocess /data/local/tmp/front_raw.raw
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

typedef uint32_t NvError;
typedef uint32_t NvU32;
#define NvSuccess 0

/* ---- nvmap ioctls ---- */
#define NVMAP_IOC_MAGIC 'N'

struct nvmap_create_handle {
    union { uint32_t id; uint32_t size; int32_t fd; };
    uint32_t handle;
};
struct nvmap_alloc_handle {
    uint32_t handle; uint32_t heap_mask; uint32_t flags; uint32_t align;
};
struct nvmap_rw_handle {
    unsigned long addr; uint32_t handle; uint32_t offset;
    uint32_t elem_size; uint32_t hmem_stride; uint32_t user_stride; uint32_t count;
};
struct nvmap_pin_handle {
    uint32_t handles; unsigned long addr; uint32_t count;
};

#define NVMAP_IOC_CREATE   _IOWR(NVMAP_IOC_MAGIC, 0, struct nvmap_create_handle)
#define NVMAP_IOC_ALLOC    _IOW(NVMAP_IOC_MAGIC, 3, struct nvmap_alloc_handle)
#define NVMAP_IOC_FREE     _IO(NVMAP_IOC_MAGIC, 4)
#define NVMAP_IOC_WRITE    _IOW(NVMAP_IOC_MAGIC, 6, struct nvmap_rw_handle)
#define NVMAP_IOC_READ     _IOW(NVMAP_IOC_MAGIC, 7, struct nvmap_rw_handle)
#define NVMAP_IOC_PIN_MULT _IOWR(NVMAP_IOC_MAGIC, 10, struct nvmap_pin_handle)
#define NVMAP_IOC_GET_FD   _IOWR(NVMAP_IOC_MAGIC, 15, struct nvmap_create_handle)
#define NVMAP_HEAP_IOVMM   (1 << 30)
#define NVMAP_HANDLE_WRITE_COMBINE 2

/* ---- nvhost ioctls ---- */
#define NVHOST_IOCTL_MAGIC 'H'

struct nvhost_set_nvmap_fd_args { uint32_t fd; };
struct nvhost_get_param_arg { uint32_t param; uint32_t value; };
struct nvhost_syncpt_incr { uint32_t syncpt_id; uint32_t syncpt_incrs; };
struct nvhost_cmdbuf { uint32_t mem; uint32_t offset; uint32_t words; };
struct nvhost_reloc { uint32_t cmdbuf_mem; uint32_t cmdbuf_offset; uint32_t target; uint32_t target_offset; };
struct nvhost_reloc_shift { uint32_t shift; };
struct nvhost_fence { uint32_t syncpt_id; uint32_t value; };
struct nvhost_ctrl_syncpt_waitex_args { uint32_t id; uint32_t thresh; int32_t timeout; uint32_t value; };

struct nvhost32_submit_args {
    uint32_t submit_version; uint32_t num_syncpt_incrs; uint32_t num_cmdbufs;
    uint32_t num_relocs; uint32_t num_waitchks; uint32_t timeout;
    uint32_t syncpt_incrs; uint32_t cmdbufs; uint32_t relocs;
    uint32_t reloc_shifts; uint32_t waitchks; uint32_t waitbases;
    uint32_t class_ids; uint32_t pad[2]; uint32_t fences; uint32_t fence;
} __attribute__((packed));

#define NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD _IOW(NVHOST_IOCTL_MAGIC, 5, struct nvhost_set_nvmap_fd_args)
#define NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT _IOWR(NVHOST_IOCTL_MAGIC, 16, struct nvhost_get_param_arg)
#define NVHOST32_IOCTL_CHANNEL_SUBMIT _IOWR(NVHOST_IOCTL_MAGIC, 15, struct nvhost32_submit_args)
#define NVHOST_IOCTL_CTRL_SYNCPT_WAITEX _IOWR(NVHOST_IOCTL_MAGIC, 6, struct nvhost_ctrl_syncpt_waitex_args)

/* Host1X opcodes */
#define OP_SETCLASS(c,o,m) ((0<<28)|((o)<<16)|((c)<<6)|(m))
#define OP_INCR(o,n)       ((1<<28)|((o)<<16)|(n))
#define OP_NONINCR(o,n)    ((2<<28)|((o)<<16)|(n))
#define OP_IMM(o,v)        ((4<<28)|((o)<<16)|(v))
#define ISP_CLASS 0x32

/* Frame params */
#define W 2592
#define H 1944
#define BPP 2
#define IN_SIZE (W * H * BPP)
#define Y_STRIDE ((W + 63) & ~63)         /* 2624 */
#define UV_STRIDE (((W/2) + 63) & ~63)    /* 1344 */
#define Y_SIZE (Y_STRIDE * H)
#define UV_SIZE (UV_STRIDE * H / 2)
#define OUT_SIZE (Y_SIZE + UV_SIZE * 2)

static int nvmap_fd = -1;

static uint32_t nvmap_create(uint32_t size) {
    struct nvmap_create_handle ch = { .size = size };
    if (ioctl(nvmap_fd, NVMAP_IOC_CREATE, &ch) < 0) { perror("nvmap create"); return 0; }
    return ch.handle;
}
static int nvmap_alloc(uint32_t handle) {
    struct nvmap_alloc_handle ah = { .handle = handle, .heap_mask = NVMAP_HEAP_IOVMM,
        .flags = NVMAP_HANDLE_WRITE_COMBINE, .align = 4096 };
    if (ioctl(nvmap_fd, NVMAP_IOC_ALLOC, &ah) < 0) { perror("nvmap alloc"); return -1; }
    return 0;
}
static int nvmap_write(uint32_t handle, uint32_t offset, const void *data, uint32_t size) {
    struct nvmap_rw_handle rw = { .addr = (unsigned long)data, .handle = handle,
        .offset = offset, .elem_size = size, .hmem_stride = size, .user_stride = size, .count = 1 };
    if (ioctl(nvmap_fd, NVMAP_IOC_WRITE, &rw) < 0) { perror("nvmap write"); return -1; }
    return 0;
}
static int nvmap_read(uint32_t handle, uint32_t offset, void *data, uint32_t size) {
    struct nvmap_rw_handle rw = { .addr = (unsigned long)data, .handle = handle,
        .offset = offset, .elem_size = size, .hmem_stride = size, .user_stride = size, .count = 1 };
    if (ioctl(nvmap_fd, NVMAP_IOC_READ, &rw) < 0) { perror("nvmap read"); return -1; }
    return 0;
}
static uint32_t nvmap_pin(uint32_t handle) {
    struct nvmap_pin_handle ph = { .handles = handle, .addr = 0, .count = 1 };
    if (ioctl(nvmap_fd, NVMAP_IOC_PIN_MULT, &ph) < 0) { perror("nvmap pin"); return 0; }
    return (uint32_t)ph.addr;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: %s <raw_bayer_file>\n", argv[0]);
        return 1;
    }
    const char *raw_path = argv[1];

    printf("=== ISP Reprocess Test ===\n");
    printf("Input: %s (%dx%d BG10)\n", raw_path, W, H);
    printf("Output: YUV %dx%d (Y=%d UV=%d total=%d)\n", W, H, Y_SIZE, UV_SIZE, OUT_SIZE);

    /* ---- Step 1: Init ISP via MIUI blobs ---- */
    printf("\n[1] Init ISP blob...\n");

    void *lib_nvos = dlopen("libnvos.so", RTLD_NOW | RTLD_GLOBAL);
    void *lib_nvrm = dlopen("libnvrm.so", RTLD_NOW | RTLD_GLOBAL);
    void *lib_nvrm_gfx = dlopen("libnvrm_graphics.so", RTLD_NOW | RTLD_GLOBAL);
    void *lib_isp = dlopen("libnvisp_v3.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib_isp) { printf("FATAL: %s\n", dlerror()); return 1; }

    typedef NvError (*NvRmOpenNew_t)(void **);
    typedef NvError (*NvIspOpen_t)(void *, NvU32, void **);
    typedef void (*NvIspClose_t)(void *);
    typedef NvError (*NvIspHwSettingsCreate_t)(void *, void **);
    typedef NvError (*NvIspHwSettingsApply_t)(void *);
    typedef NvError (*NvIspHwSettingsDestroy_t)(void *);
    typedef NvError (*NvIspFlush_t)(void *);

    NvRmOpenNew_t pRmOpen = dlsym(lib_nvrm, "NvRmOpenNew");
    NvIspOpen_t pIspOpen = dlsym(lib_isp, "NvIspOpen");
    NvIspClose_t pIspClose = dlsym(lib_isp, "NvIspClose");
    NvIspHwSettingsCreate_t pHwCreate = dlsym(lib_isp, "NvIspHwSettingsCreate");
    NvIspHwSettingsApply_t pHwApply = dlsym(lib_isp, "NvIspHwSettingsApply");
    NvIspHwSettingsDestroy_t pHwDestroy = dlsym(lib_isp, "NvIspHwSettingsDestroy");
    NvIspFlush_t pFlush = dlsym(lib_isp, "NvIspFlush");

    void *hRm = NULL;
    pRmOpen(&hRm);
    printf("  hRm=%p\n", hRm);

    void *isp_handle = NULL;
    NvError err = pIspOpen(hRm, 1, &isp_handle); /* ISP-A = 1 for MIUI */
    printf("  NvIspOpen: err=0x%x handle=%p\n", err, isp_handle);
    if (err || !isp_handle) return 1;

    void *hw_settings = NULL;
    pHwCreate(isp_handle, &hw_settings);
    printf("  HwSettingsCreate: settings=%p\n", hw_settings);

    err = pHwApply(hw_settings);
    printf("  HwSettingsApply: err=0x%x\n", err);
    if (err) { printf("  Apply failed!\n"); return 1; }

    /* Extract ISP channel fd and syncpt from handle struct */
    /* From handle dump: offset 0x0C = channel handle ptr, offset 0x10 = syncpt base */
    uint32_t *ctx = (uint32_t *)isp_handle;
    printf("  ctx[0]=0x%x ctx[1]=0x%x ctx[2]=0x%x ctx[3]=0x%x ctx[4]=0x%x\n",
           ctx[0], ctx[1], ctx[2], ctx[3], ctx[4]);

    /* ---- Step 2: Open nvmap and ISP channel directly for reprocess ---- */
    printf("\n[2] Setup nvmap + ISP channel...\n");

    nvmap_fd = open("/dev/nvmap", O_RDWR | O_SYNC);
    if (nvmap_fd < 0) { perror("open nvmap"); return 1; }

    int isp_fd = open("/dev/nvhost-isp", O_RDWR);
    if (isp_fd < 0) { perror("open isp"); return 1; }

    struct nvhost_set_nvmap_fd_args snf = { .fd = nvmap_fd };
    ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD, &snf);

    int ctrl_fd = open("/dev/nvhost-ctrl", O_RDWR);

    /* Get syncpoints */
    struct nvhost_get_param_arg gsp;
    gsp.param = 0; ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gsp);
    uint32_t sp_memory = gsp.value;
    gsp.param = 1; ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gsp);
    uint32_t sp_stats = gsp.value;
    gsp.param = 2; ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gsp);
    uint32_t sp_loadv = gsp.value;
    printf("  syncpts: memory=%u stats=%u loadv=%u\n", sp_memory, sp_stats, sp_loadv);

    /* ---- Step 3: Allocate buffers ---- */
    printf("\n[3] Allocate buffers...\n");

    uint32_t in_h = nvmap_create(IN_SIZE);
    uint32_t out_h = nvmap_create(OUT_SIZE);
    uint32_t cmd_h = nvmap_create(16384);
    if (!in_h || !out_h || !cmd_h) return 1;
    nvmap_alloc(in_h); nvmap_alloc(out_h); nvmap_alloc(cmd_h);

    uint32_t in_iova = nvmap_pin(in_h);
    uint32_t out_iova = nvmap_pin(out_h);
    printf("  in_iova=0x%08x out_iova=0x%08x\n", in_iova, out_iova);

    /* ---- Step 4: Load raw frame ---- */
    printf("\n[4] Loading %s...\n", raw_path);
    FILE *f = fopen(raw_path, "rb");
    if (!f) { perror("open raw"); return 1; }
    uint8_t *raw_buf = malloc(IN_SIZE);
    int nread = fread(raw_buf, 1, IN_SIZE, f);
    fclose(f);
    printf("  Read %d bytes\n", nread);

    /* Upload to nvmap in chunks */
    int chunk = 65536;
    for (int off = 0; off < IN_SIZE; off += chunk) {
        int sz = (IN_SIZE - off < chunk) ? IN_SIZE - off : chunk;
        nvmap_write(in_h, off, raw_buf + off, sz);
    }
    free(raw_buf);
    printf("  Uploaded to nvmap\n");

    /* Zero output */
    uint8_t *zeros = calloc(1, chunk);
    for (int off = 0; off < OUT_SIZE; off += chunk) {
        int sz = (OUT_SIZE - off < chunk) ? OUT_SIZE - off : chunk;
        nvmap_write(out_h, off, zeros, sz);
    }
    free(zeros);

    /* ---- Step 5: Build reprocess command buffer ---- */
    printf("\n[5] Build reprocess cmdbuf...\n");

    uint32_t out_y_iova = out_iova;
    uint32_t out_u_iova = out_iova + Y_SIZE;
    uint32_t out_v_iova = out_iova + Y_SIZE + UV_SIZE;

    uint32_t cmd[512];
    int n = 0;

    /* Output block */
    cmd[n++] = OP_SETCLASS(ISP_CLASS, 0, 0);
    cmd[n++] = OP_INCR(0xE00, 1);
    cmd[n++] = ((W - 1) & 0x3FFF) << 16;
    cmd[n++] = OP_INCR(0xE01, 1);
    cmd[n++] = ((H - 1) & 0x3FFF) << 16;
    cmd[n++] = OP_INCR(0xE02, 1);
    cmd[n++] = 0x04FE00E6;  /* YUV output format from stock */
    cmd[n++] = OP_INCR(0xE03, 1);
    cmd[n++] = 0x00000000;  /* no digital gain/crop */

    /* Output Y plane */
    cmd[n++] = OP_INCR(0xE04, 3);
    int y_reloc = n; cmd[n++] = out_y_iova; cmd[n++] = 0; cmd[n++] = Y_STRIDE;
    /* Output U plane */
    cmd[n++] = OP_INCR(0xE07, 3);
    int u_reloc = n; cmd[n++] = out_u_iova; cmd[n++] = 0; cmd[n++] = UV_STRIDE;
    /* Output V plane */
    cmd[n++] = OP_INCR(0xE0A, 3);
    int v_reloc = n; cmd[n++] = out_v_iova; cmd[n++] = 0; cmd[n++] = UV_STRIDE;

    /* Processing block */
    cmd[n++] = OP_INCR(0x500, 6);
    cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0;
    cmd[n++] = 0; cmd[n++] = 0;
    cmd[n++] = (H << 16) | W;

    /* Input block (reprocess) */
    cmd[n++] = OP_INCR(0xE31, 1);
    cmd[n++] = (W & 0x7FFF) | (H << 16);  /* input dimensions */
    cmd[n++] = OP_INCR(0xE33, 1);
    cmd[n++] = 0x04FE00E6;  /* input format — TODO: should be Bayer format */
    cmd[n++] = OP_INCR(0xE34, 3);
    int in_reloc = n; cmd[n++] = in_iova; cmd[n++] = 0; cmd[n++] = W * BPP;  /* stride */
    cmd[n++] = OP_INCR(0xE32, 1);
    cmd[n++] = (W & 0x3FFF) | (0 << 16);  /* strip config: full width, no overlap */

    /* ISP_ENABLE */
    cmd[n++] = OP_INCR(0x015, 1);
    cmd[n++] = 7;  /* full pipeline */

    /* Input trigger — fires ISP processing */
    cmd[n++] = OP_INCR(0xE30, 1);
    cmd[n++] = 1;

    /* Syncpt incrs */
    cmd[n++] = OP_NONINCR(0x000, 1);
    cmd[n++] = (4 << 8) | sp_memory;   /* cond 4: OP_DONE */
    cmd[n++] = OP_NONINCR(0x000, 1);
    cmd[n++] = (5 << 8) | sp_stats;    /* cond 5: STATS */
    cmd[n++] = OP_NONINCR(0x000, 1);
    cmd[n++] = (6 << 8) | sp_loadv;    /* cond 6: RD_DONE */

    /* ISP_CONTROL trigger */
    cmd[n++] = OP_NONINCR(0x00C, 1);
    cmd[n++] = 0x0B;  /* reprocess trigger (0x09 or 0x0B from RE) */

    printf("  cmdbuf: %d words\n", n);

    /* Upload cmdbuf */
    nvmap_write(cmd_h, 0, cmd, n * 4);

    /* Build relocs */
    struct nvhost_reloc relocs[4];
    struct nvhost_reloc_shift shifts[4];
    int nr = 0;

    relocs[nr] = (struct nvhost_reloc){ cmd_h, y_reloc*4, out_h, 0 };
    shifts[nr++].shift = 0;
    relocs[nr] = (struct nvhost_reloc){ cmd_h, u_reloc*4, out_h, Y_SIZE };
    shifts[nr++].shift = 0;
    relocs[nr] = (struct nvhost_reloc){ cmd_h, v_reloc*4, out_h, Y_SIZE + UV_SIZE };
    shifts[nr++].shift = 0;
    relocs[nr] = (struct nvhost_reloc){ cmd_h, in_reloc*4, in_h, 0 };
    shifts[nr++].shift = 0;

    /* ---- Step 6: Submit ---- */
    printf("\n[6] Submit...\n");

    struct nvhost_cmdbuf cb = { .mem = cmd_h, .offset = 0, .words = n };
    struct nvhost_syncpt_incr si = { .syncpt_id = sp_memory, .syncpt_incrs = 3 };
    uint32_t class_id = ISP_CLASS;
    struct nvhost_fence fence = { 0, 0 };

    struct nvhost32_submit_args sa;
    memset(&sa, 0, sizeof(sa));
    sa.submit_version = 0;
    sa.num_syncpt_incrs = 1;
    sa.num_cmdbufs = 1;
    sa.num_relocs = nr;
    sa.timeout = 5000;
    sa.syncpt_incrs = (uint32_t)(uintptr_t)&si;
    sa.cmdbufs = (uint32_t)(uintptr_t)&cb;
    sa.relocs = (uint32_t)(uintptr_t)relocs;
    sa.reloc_shifts = (uint32_t)(uintptr_t)shifts;
    sa.class_ids = (uint32_t)(uintptr_t)&class_id;
    sa.fences = (uint32_t)(uintptr_t)&fence;

    if (ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa) < 0) {
        perror("submit");
        printf("  Submit failed!\n");
    } else {
        printf("  Submitted, fence=%u\n", fence.value);

        /* Wait */
        struct nvhost_ctrl_syncpt_waitex_args wa = {
            .id = sp_memory, .thresh = fence.value, .timeout = 5000
        };
        int ret = ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_WAITEX, &wa);
        if (ret < 0)
            printf("  TIMEOUT waiting for ISP (syncpt %u thresh %u)\n",
                   sp_memory, fence.value);
        else
            printf("  ISP done! syncpt=%u value=%u\n", sp_memory, wa.value);
    }

    /* ---- Step 7: Read output ---- */
    printf("\n[7] Reading output...\n");
    {
        uint8_t check[4096];
        nvmap_read(out_h, 0, check, 4096);
        int nonzero = 0;
        for (int i = 0; i < 4096; i++)
            if (check[i] != 0) nonzero++;
        printf("  First 4KB: %d/4096 non-zero bytes\n", nonzero);
        printf("  hex: ");
        for (int i = 0; i < 32; i++) printf("%02x ", check[i]);
        printf("\n");

        if (nonzero > 0) {
            /* Dump Y plane to file */
            const char *outpath = "/data/local/tmp/isp_reprocess_out.raw";
            FILE *fp = fopen(outpath, "wb");
            if (fp) {
                uint8_t *buf = malloc(65536);
                for (int off = 0; off < Y_SIZE; off += 65536) {
                    int sz = (Y_SIZE - off < 65536) ? Y_SIZE - off : 65536;
                    nvmap_read(out_h, off, buf, sz);
                    fwrite(buf, 1, sz, fp);
                }
                free(buf);
                fclose(fp);
                printf("  Y plane saved to %s (%d bytes)\n", outpath, Y_SIZE);
            }
        }
    }

    /* Cleanup */
    printf("\n[cleanup]\n");
    pHwDestroy(hw_settings);
    pIspClose(isp_handle);
    close(isp_fd);
    close(ctrl_fd);
    close(nvmap_fd);
    printf("=== Done ===\n");
    return 0;
}
