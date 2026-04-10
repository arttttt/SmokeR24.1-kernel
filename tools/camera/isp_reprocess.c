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
#define OUT_SIZE (W * 4 * H)  /* 32bpp */
/* Also keep YUV defines for diagnostics */
#define Y_STRIDE ((W + 63) & ~63)
#define UV_STRIDE (((W/2) + 63) & ~63)
#define Y_SIZE (Y_STRIDE * H)
#define UV_SIZE (UV_STRIDE * H / 2)

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
        printf("Usage: %s <raw_bayer_file> [num_frames]\n", argv[0]);
        return 1;
    }
    const char *raw_path = argv[1];
    int num_frames = (argc > 2) ? atoi(argv[2]) : 1;

    printf("=== ISP Reprocess Test ===\n");
    printf("Input: %s (%dx%d BG10), frames=%d\n", raw_path, W, H, num_frames);
    printf("Output: YUV %dx%d (Y=%d UV=%d total=%d)\n", W, H, Y_SIZE, UV_SIZE, OUT_SIZE);

    /* Open nvmap once before loop */
    nvmap_fd = open("/dev/nvmap", O_RDWR | O_SYNC);
    if (nvmap_fd < 0) { perror("open nvmap"); return 1; }

  for (int frame = 0; frame < num_frames; frame++) {
    printf("\n======= Frame %d/%d =======\n", frame+1, num_frames);

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

    /* Enable streaming stripping in shim — removes trigger + conditional syncpts */
    setenv("NVRM_SHIM_STRIP", "1", 1);

    err = pHwApply(hw_settings);
    printf("  HwSettingsApply: err=0x%x (streaming stripped by shim)\n", err);

    /* Disable stripping for our own reprocess gather */
    unsetenv("NVRM_SHIM_STRIP");

    /* Scan push buffer for output format (method 0xE02) set by calibration.
     * Push buffers are mmap'd by our shim — scan /proc/self/maps for them */
    printf("  Scanning calibration gather for output format (0xE02)...\n");
    {
        /* The blob's push buffer is at the shim mmap'd address.
         * Shim maps 16KB push buffers via dmabuf. Scan for INCR(0xE02,1) opcode. */
        FILE *maps = fopen("/proc/self/maps", "r");
        if (maps) {
            char line[256];
            while (fgets(line, sizeof(line), maps)) {
                unsigned long start, end;
                if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
                    unsigned long size = end - start;
                    if (size == 16384 || size == 32768) { /* push buffer size */
                        uint32_t *pb = (uint32_t *)start;
                        uint32_t words = size / 4;
                        for (uint32_t j = 0; j < words - 1; j++) {
                            uint32_t op = pb[j];
                            /* INCR(0xE02, 1) = 0x1E020001 */
                            if (op == 0x1E020001) {
                                printf("    Found 0xE02 output format = 0x%08x at pb+%u\n",
                                       pb[j+1], j);
                            }
                            /* INCR(0xE00, 1) = output width */
                            if (op == 0x1E000001) {
                                printf("    Found 0xE00 output width = 0x%08x at pb+%u\n",
                                       pb[j+1], j);
                            }
                            /* INCR(0xE01, 1) = output height */
                            if (op == 0x1E010001) {
                                printf("    Found 0xE01 output height = 0x%08x at pb+%u\n",
                                       pb[j+1], j);
                            }
                            /* INCR(0xE31, 1) = input dims */
                            if (op == 0x1E310001) {
                                printf("    Found 0xE31 input dims = 0x%08x at pb+%u\n",
                                       pb[j+1], j);
                            }
                            /* INCR(0xE33, 1) = input format */
                            if (op == 0x1E330001) {
                                printf("    Found 0xE33 input format = 0x%08x at pb+%u\n",
                                       pb[j+1], j);
                            }
                            /* INCR(0x015, 1) = ISP_ENABLE */
                            if (op == 0x10150001) {
                                printf("    Found 0x015 ISP_ENABLE = 0x%08x at pb+%u\n",
                                       pb[j+1], j);
                            }
                            /* INCR(0xE32, 1) = strip config */
                            if (op == 0x1E320001) {
                                printf("    Found 0xE32 strip config = 0x%08x at pb+%u\n",
                                       pb[j+1], j);
                            }
                            /* INCR(0x500, 6) = processing block */
                            if (op == 0x15000006) {
                                printf("    Found 0x500 processing = %08x %08x %08x %08x %08x %08x\n",
                                       pb[j+1], pb[j+2], pb[j+3], pb[j+4], pb[j+5], pb[j+6]);
                            }
                            /* INCR(0xE04, 3) = Y output surface */
                            if (op == 0x1E040003) {
                                printf("    Found 0xE04 Y surface = [%08x, %08x, stride=%u]\n",
                                       pb[j+1], pb[j+2], pb[j+3]);
                            }
                            /* INCR(0xE30, 1) = input trigger */
                            if (op == 0x1E300001) {
                                printf("    Found 0xE30 input trigger = %u\n", pb[j+1]);
                            }
                            /* NONINCR(0x00C, 1) = ISP control */
                            if (op == 0x200C0001) {
                                printf("    Found 0x00C ISP_CONTROL = 0x%02x\n", pb[j+1]);
                            }
                        }
                    }
                }
            }
            fclose(maps);
        }
    }

    /* Extract ISP channel fd and syncpt from handle struct */
    uint32_t *ctx = (uint32_t *)isp_handle;
    printf("  ctx[0]=0x%x ctx[1]=0x%x ctx[2]=0x%x ctx[3]=0x%x ctx[4]=0x%x\n",
           ctx[0], ctx[1], ctx[2], ctx[3], ctx[4]);

    /* DON'T close ISP blob — keep channel open to maintain power/clocks.
     * Extract ISP channel fd from blob's internal context. */
    printf("  Extracting ISP fd from blob context...\n");

    /* ctx layout (from handle dump):
     *   ctx[0]  = hRm (0x1)
     *   ctx[1]  = module_id (0x0b)
     *   ctx[2]  = class_id (0x32)
     *   ctx[3]  = NvRmChannel* ptr
     *   ctx[4]  = syncpt_id_base (0x22 = 34)
     * NvRmChannel layout (from jxd source):
     *   channel[0] = fd
     */
    void *nvrm_channel = (void *)(uintptr_t)ctx[3];
    int isp_fd = *(int *)nvrm_channel;
    printf("  NvRmChannel=%p isp_fd=%d\n", nvrm_channel, isp_fd);

    /* syncpts from ctx: ctx[4] has base, but we need to query them */
    printf("\n[2] Setup for reprocess (using blob's channel)...\n");

    printf("  nvmap_fd=%d\n", nvmap_fd);

    int ctrl_fd = open("/dev/nvhost-ctrl", O_RDWR);

    /* Get syncpoints from the ISP channel */
    struct nvhost_get_param_arg gsp;
    gsp.param = 0; ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gsp);
    uint32_t sp_memory = gsp.value;
    gsp.param = 1; ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gsp);
    uint32_t sp_stats = gsp.value;
    gsp.param = 2; ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gsp);
    uint32_t sp_loadv = gsp.value;
    printf("  syncpts: memory=%u stats=%u loadv=%u\n", sp_memory, sp_stats, sp_loadv);

    /* No force-increment needed — shim strips streaming from HwSettingsApply */

    /* ---- Step 3: Allocate buffers ---- */
    printf("\n[3] Allocate buffers...\n");

    uint32_t in_h = nvmap_create(IN_SIZE);
    uint32_t out_h = nvmap_create(OUT_SIZE);
    uint32_t stats_h = nvmap_create(262144);  /* 256KB stats buffer */
    uint32_t cmd_h = nvmap_create(16384);
    if (!in_h || !out_h || !stats_h || !cmd_h) return 1;
    nvmap_alloc(in_h); nvmap_alloc(out_h); nvmap_alloc(stats_h); nvmap_alloc(cmd_h);

    uint32_t in_iova = nvmap_pin(in_h);
    uint32_t out_iova = nvmap_pin(out_h);
    uint32_t stats_iova = nvmap_pin(stats_h);
    printf("  in_iova=0x%08x out_iova=0x%08x stats_iova=0x%08x\n",
           in_iova, out_iova, stats_iova);

    /* ---- Step 4: Load raw frame ---- */
    printf("\n[4] Loading %s...\n", raw_path);
    FILE *f = fopen(raw_path, "rb");
    if (!f) { perror("open raw"); return 1; }
    uint8_t *raw_buf = malloc(IN_SIZE);
    int nread = fread(raw_buf, 1, IN_SIZE, f);
    fclose(f);
    printf("  Read %d bytes\n", nread);

    /* Option: use test pattern instead of real data */
    int use_pattern = (argc > 2 && !strcmp(argv[2], "--pattern"));

    if (use_pattern) {
        printf("  Using TEST PATTERN (gradient) instead of raw data\n");
        /* Generate 10-bit gradient in 16-bit LE containers */
        uint16_t *pat = (uint16_t *)raw_buf;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                pat[y * W + x] = ((x + y) & 0x3FF); /* 10-bit gradient */
            }
        }
    }

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
    int y_reloc = -1, u_reloc = -1, v_reloc = -1, in_reloc = -1, stats_reloc = -1;

    /*
     * Replicate exact stock ISP-B per-frame gather from
     * docs/camera/unverified/stock-isp-b-cmdbuf-dump.txt
     * Only changes: IOVAs (relocs), trigger 0x0B (reprocess) instead of 0x05,
     * and added input block (0xE30-0xE34) for reprocess mode.
     */

    /* Output config — exactly as stock */
    cmd[n++] = OP_INCR(0xE00, 1);
    cmd[n++] = 0x0A1F0000;                /* width = 2592 */
    cmd[n++] = OP_INCR(0xE01, 1);
    cmd[n++] = 0x07970000;                /* height = 1944 */
    cmd[n++] = OP_INCR(0xE02, 1);
    cmd[n++] = 0x010000C9;                /* 8bpp format — ISP writes to plane 1 */
    cmd[n++] = OP_INCR(0xE03, 1);
    cmd[n++] = 0x00000000;

    /* Output surfaces — stock order, stock strides.
     * In reprocess mode, ISP writes main data to plane 2 (0xE07).
     * All three planes use full-res stride since ISP writes there. */
    cmd[n++] = OP_INCR(0xE04, 3);
    y_reloc = n;
    cmd[n++] = out_y_iova;                /* plane 1 */
    cmd[n++] = 0x00000000;
    cmd[n++] = W * 4;                     /* 32bpp stride = 10368 */
    /* Planes 2,3: stock strides */
    cmd[n++] = OP_INCR(0xE07, 3);
    u_reloc = n;
    cmd[n++] = out_u_iova;
    cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000540;                /* UV stride = 1344 */
    cmd[n++] = OP_INCR(0xE0A, 3);
    v_reloc = n;
    cmd[n++] = out_v_iova;
    cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000540;

    /* Processing block — stock values */
    cmd[n++] = OP_INCR(0x500, 6);
    cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000;
    cmd[n++] = 0x07980A20;                /* (H << 16) | W = stock value */

    /* SET_CLASS + Stats buffer — stock has SET_CLASS before stats */
    cmd[n++] = OP_SETCLASS(ISP_CLASS, 0, 0);
    cmd[n++] = OP_INCR(0x100, 4);
    stats_reloc = n;
    cmd[n++] = stats_iova;
    cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000;

    /* Input block (reprocess) — not in stock streaming, added for reprocess */
    cmd[n++] = OP_INCR(0xE31, 1);
    cmd[n++] = 0x07980A20;                /* input dims = (H << 16) | W */
    cmd[n++] = OP_INCR(0xE33, 1);
    cmd[n++] = 0x10200024;                /* input format from calibration */
    cmd[n++] = OP_INCR(0xE34, 3);
    in_reloc = n;
    cmd[n++] = in_iova;
    cmd[n++] = 0x00000000;
    cmd[n++] = W * BPP;                   /* input stride = 5184 */
    cmd[n++] = OP_INCR(0xE32, 1);
    cmd[n++] = (W & 0x3FFF);              /* strip width = full */
    cmd[n++] = OP_INCR(0xE30, 1);
    cmd[n++] = 1;                          /* input trigger */

    /* ISP_ENABLE — stock uses 7 for full pipeline */
    cmd[n++] = OP_INCR(0x015, 1);
    cmd[n++] = 7;

    /* Syncpt conditional incrs — stock format */
    cmd[n++] = OP_SETCLASS(ISP_CLASS, 0, 0);
    cmd[n++] = OP_NONINCR(0x000, 1);
    cmd[n++] = (4 << 8) | sp_memory;      /* cond 4: OP_DONE */
    cmd[n++] = OP_NONINCR(0x000, 1);
    cmd[n++] = (5 << 8) | sp_stats;       /* cond 5: STATS_DONE */
    cmd[n++] = OP_NONINCR(0x000, 1);
    cmd[n++] = (6 << 8) | sp_loadv;       /* cond 6: RD_DONE */

    /* ISP_CONTROL trigger — 0x0B for reprocess (stock uses 0x05 for streaming) */
    cmd[n++] = OP_SETCLASS(ISP_CLASS, 0, 0);
    cmd[n++] = OP_NONINCR(0x00C, 1);
    cmd[n++] = 0x0B;

    printf("  cmdbuf: %d words\n", n);

    /* Upload cmdbuf */
    nvmap_write(cmd_h, 0, cmd, n * 4);

    /* Build relocs */
    struct nvhost_reloc relocs[8];
    struct nvhost_reloc_shift shifts[8];
    int nr = 0;

    relocs[nr] = (struct nvhost_reloc){ cmd_h, y_reloc*4, out_h, 0 };
    shifts[nr++].shift = 0;
    relocs[nr] = (struct nvhost_reloc){ cmd_h, u_reloc*4, out_h, Y_SIZE };
    shifts[nr++].shift = 0;
    relocs[nr] = (struct nvhost_reloc){ cmd_h, v_reloc*4, out_h, Y_SIZE + UV_SIZE };
    shifts[nr++].shift = 0;
    relocs[nr] = (struct nvhost_reloc){ cmd_h, in_reloc*4, in_h, 0 };
    shifts[nr++].shift = 0;
    relocs[nr] = (struct nvhost_reloc){ cmd_h, stats_reloc*4, stats_h, 0 };
    shifts[nr++].shift = 0;

    /* ---- Step 6: Submit ---- */
    /* Read current syncpt value first */
    struct nvhost_ctrl_syncpt_waitex_args rd = { .id = sp_memory, .thresh = 0, .timeout = 0 };
    ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_WAITEX, &rd);
    printf("\n[6] Submit (syncpt %u current=%u)...\n", sp_memory, rd.value);

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
        /* Check multiple regions of output */
        uint8_t check[4096];
        int offsets[] = { 0, Y_SIZE/2, Y_SIZE-4096, Y_SIZE, Y_SIZE+UV_SIZE };
        const char *names[] = { "Y start", "Y mid", "Y end", "U start", "V start" };
        for (int r = 0; r < 5; r++) {
            nvmap_read(out_h, offsets[r], check, 4096);
            int nonzero = 0;
            for (int i = 0; i < 4096; i++)
                if (check[i] != 0) nonzero++;
            printf("  %s (off=%d): %d/4096 non-zero", names[r], offsets[r], nonzero);
            if (nonzero) {
                printf(" → ");
                for (int i = 0; i < 16; i++) printf("%02x ", check[i]);
            }
            printf("\n");
        }

        {
            /* Dump FULL output buffer (Y+U+V) */
            const char *outpath = "/data/local/tmp/isp_reprocess_out.yuv";
            FILE *fp = fopen(outpath, "wb");
            if (fp) {
                int chunk = 65536;
                uint8_t *buf = malloc(chunk);
                for (int off = 0; off < OUT_SIZE; off += chunk) {
                    int sz = (OUT_SIZE - off < chunk) ? OUT_SIZE - off : chunk;
                    nvmap_read(out_h, off, buf, sz);
                    fwrite(buf, 1, sz, fp);
                }
                free(buf);
                fclose(fp);
                printf("  Full output saved to %s (%d bytes)\n", outpath, OUT_SIZE);
            }

            /* Also scan for first non-zero byte in Y plane */
            uint8_t scan[256];
            int first_nz = -1;
            for (int off = 0; off < Y_SIZE && first_nz < 0; off += 256) {
                nvmap_read(out_h, off, scan, 256);
                for (int i = 0; i < 256; i++) {
                    if (scan[i] != 0) { first_nz = off + i; break; }
                }
            }
            printf("  First non-zero byte in Y plane: %s\n",
                   first_nz >= 0 ? "" : "NONE (all zeros)");
            if (first_nz >= 0) printf("    offset=%d (0x%x)\n", first_nz, first_nz);
        }
    }

    /* Cleanup — close ISP to reset state for next frame */
    printf("\n[cleanup]\n");
    pHwDestroy(hw_settings);
    pIspClose(isp_handle);
    isp_handle = NULL;
    hw_settings = NULL;
    close(ctrl_fd);
    /* Keep nvmap_fd open for next iteration */
    printf("  Frame %d done\n", frame+1);

  } /* end frame loop */

    close(nvmap_fd);
    printf("\n=== All %d frames done ===\n", num_frames);
    return 0;
}
