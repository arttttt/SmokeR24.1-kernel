/*
 * ISP reprocess test — pure kernel path, NO blobs
 *
 * Opens ISP channel via NvRmChannelOpen (from libnvrm.so) for proper
 * HW init, but builds and submits gathers ourselves — no libnvisp_v3.so.
 *
 * This isolates the channel init (which we can't reproduce yet) from
 * the gather building (which we understand fully).
 *
 * Input: 2592x1944 BG10 (10-bit Bayer BGGR, 16-bit LE containers)
 * Output: RGBA 2592x1944 (32bpp)
 *
 * Build:
 *   $CC --sysroot=$SYSROOT -std=gnu99 -pie -o isp_reprocess_pure isp_reprocess_pure.c -ldl
 *
 * Usage:
 *   LD_LIBRARY_PATH=/system/vendor/lib ./isp_reprocess_pure /data/local/tmp/front_raw.raw
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/ioctl.h>

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

/* Clock/power control */
struct nvhost_clk_rate_args { uint32_t rate; uint32_t moduleid; };
#define NVHOST_IOCTL_CHANNEL_SET_CLK_RATE _IOW(NVHOST_IOCTL_MAGIC, 10, struct nvhost_clk_rate_args)

/* PIO register read/write (via ctrl node) */
struct nvhost32_ctrl_module_regrdwr_args {
    uint32_t id;
    uint32_t num_offsets;
    uint32_t block_size;
    uint32_t offsets;
    uint32_t values;
    uint32_t write;
};
/* NR=14 for channel fd, NR=5 for ctrl fd */
#define NVHOST32_IOCTL_CHANNEL_MODULE_REGRDWR _IOWR(NVHOST_IOCTL_MAGIC, 14, struct nvhost32_ctrl_module_regrdwr_args)

/* Host1X opcodes */
#define OP_SETCLASS(c,o,m) ((0<<28)|((o)<<16)|((c)<<6)|(m))
#define OP_INCR(o,n)       ((1<<28)|((o)<<16)|(n))
#define OP_NONINCR(o,n)    ((2<<28)|((o)<<16)|(n))
#define ISP_CLASS_A 0x32
#define ISP_CLASS_B 0x34
static int isp_class = ISP_CLASS_B;  /* default ISP-B */
#define ISP_CLASS isp_class

/* Frame params */
#define W 2592
#define H 1944
#define BPP 2
#define IN_SIZE  (W * H * BPP)
#define OUT_SIZE (W * 4 * H)

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
        printf("Usage: %s <raw_bayer_file> [format_hex] [isp_enable_hex] [--miui]\n", argv[0]);
        return 1;
    }

    int use_miui = 0, use_miui_init = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--miui") == 0) { use_miui = 1; use_miui_init = 1; }
        if (strcmp(argv[i], "--miui-init") == 0) use_miui_init = 1;
    }

    printf("=== ISP Pure Reprocess (no blobs)%s%s ===\n",
           use_miui_init ? " + MIUI init" : "",
           use_miui ? " + MIUI 0x200" : "");

    /* Open devices */
    nvmap_fd = open("/dev/nvmap", O_RDWR | O_SYNC);
    if (nvmap_fd < 0) { perror("open nvmap"); return 1; }

    /* Open ISP ctrl node first (like NvIspOpen) */
    int isp_ctrl_fd = open("/dev/nvhost-ctrl-isp", O_RDWR);
    if (isp_ctrl_fd < 0) perror("open isp ctrl (non-fatal)");

    /* Open ISP-A first for testing, fallback to ISP-B */
    int isp_fd = open("/dev/nvhost-isp", O_RDWR);
    if (isp_fd >= 0) {
        printf("ISP-A fd=%d\n", isp_fd);
        isp_class = ISP_CLASS_A;
    } else {
        isp_fd = open("/dev/nvhost-isp.1", O_RDWR);
        if (isp_fd < 0) { perror("open isp"); return 1; }
        printf("ISP-B fd=%d (fallback)\n", isp_fd);
        isp_class = ISP_CLASS_B;
    }

    /* NOTE: CHANNEL_OPEN ioctl (NR=112) causes kernel panic on 24.1.
     * On 24.1, open() already creates hwctx via nvhost_channelopen().
     * Skip CHANNEL_OPEN and rely on open() + init gather. */

    int ctrl_fd = open("/dev/nvhost-ctrl", O_RDWR);
    if (ctrl_fd < 0) { perror("open ctrl"); return 1; }

    /* Set nvmap fd for ISP channel */
    struct nvhost_set_nvmap_fd_args snf = { .fd = nvmap_fd };
    if (ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD, &snf) < 0)
        perror("set nvmap fd (non-fatal)");

    /* Set ISP clock rate — required for ISP to process frames.
     * moduleid encoding: [31:24]=clock_attr, [15:0]=module_id
     * clock_attr: 0=NVHOST_CLOCK, 1=NVHOST_BW
     * module_id: from enum nvhost_module_id (ISP=3) */
    struct nvhost_clk_rate_args clk;

    /* ISP core clock = 384 MHz */
    clk.rate = 384000000;
    clk.moduleid = 0;  /* clock index 0 = ISP core */
    if (ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_SET_CLK_RATE, &clk) < 0)
        perror("set ISP clk (non-fatal)");
    else
        printf("ISP clk set to %u Hz\n", clk.rate);

    /* EMC clock = 768 MHz */
    clk.rate = 768000000;
    clk.moduleid = 1;  /* clock index 1 = EMC */
    if (ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_SET_CLK_RATE, &clk) < 0)
        perror("set EMC clk (non-fatal)");
    else
        printf("EMC clk set to %u Hz\n", clk.rate);

    /* PIO write: ISP register 0xFC = 0x20 (enable/reset)
     * From RE of NvIspOpen — NvRmHostModuleRegWr(hRm, module_id, 1, {0xFC, 0x20})
     * This is the ISP top-level enable that must happen before any submit. */
    {
        uint32_t offset = 0xFC;
        uint32_t value = 0x20;
        struct nvhost32_ctrl_module_regrdwr_args rw;
        memset(&rw, 0, sizeof(rw));
        rw.id = 0x0B;           /* ISP module id */
        rw.num_offsets = 1;
        rw.block_size = 4;
        rw.offsets = (uint32_t)(uintptr_t)&offset;
        rw.values = (uint32_t)(uintptr_t)&value;
        rw.write = 1;
        /* NR=14 on channel fd — same as working pio_test */
        if (ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_MODULE_REGRDWR, &rw) == 0)
            printf("PIO write: ISP reg 0xFC = 0x20 OK\n");
        else
            perror("PIO write 0xFC FAILED");
    }

    /* Get waitbase (NR=17) — stock camera does this during init */
    struct nvhost_get_param_arg gwb;
    gwb.param = 0;
    if (ioctl(isp_fd, _IOWR(NVHOST_IOCTL_MAGIC, 17, struct nvhost_get_param_arg), &gwb) < 0)
        perror("get waitbase (non-fatal)");
    else
        printf("Waitbase param=0 → %u\n", gwb.value);

    /* Get waitbases (NR=3) */
    struct { uint32_t value; } gwbs;
    if (ioctl(isp_fd, _IOR(NVHOST_IOCTL_MAGIC, 3, gwbs), &gwbs) < 0)
        perror("get waitbases (non-fatal)");
    else
        printf("Waitbases → %u\n", gwbs.value);

    /* Get syncpoints */
    struct nvhost_get_param_arg gsp;
    gsp.param = 0; ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gsp);
    uint32_t sp_memory = gsp.value;
    gsp.param = 1; ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gsp);
    uint32_t sp_stats = gsp.value;
    gsp.param = 2; ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gsp);
    uint32_t sp_loadv = gsp.value;
    printf("ISP fd=%d, syncpts: memory=%u stats=%u loadv=%u\n",
           isp_fd, sp_memory, sp_stats, sp_loadv);

    /* Allocate buffers */
    uint32_t in_h = nvmap_create(IN_SIZE);
    uint32_t out_h = nvmap_create(OUT_SIZE);
    uint32_t work_h = nvmap_create(512 * 1024);  /* ISP work buffer */
    uint32_t cmd_h = nvmap_create(32768);  /* larger for lens shading + tone curves */
    if (!in_h || !out_h || !work_h || !cmd_h) { printf("alloc failed\n"); return 1; }
    nvmap_alloc(in_h); nvmap_alloc(out_h); nvmap_alloc(work_h); nvmap_alloc(cmd_h);

    uint32_t in_iova = nvmap_pin(in_h);
    uint32_t out_iova = nvmap_pin(out_h);
    uint32_t work_iova = nvmap_pin(work_h);
    printf("in_iova=0x%08x out_iova=0x%08x work_iova=0x%08x\n",
           in_iova, out_iova, work_iova);

    /* Load raw frame */
    printf("Loading %s...\n", argv[1]);
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open raw"); return 1; }
    uint8_t *raw_buf = malloc(IN_SIZE);
    fread(raw_buf, 1, IN_SIZE, f);
    fclose(f);

    int chunk = 65536;
    for (int off = 0; off < IN_SIZE; off += chunk) {
        int sz = (IN_SIZE - off < chunk) ? IN_SIZE - off : chunk;
        nvmap_write(in_h, off, raw_buf + off, sz);
    }
    free(raw_buf);

    /* Zero output */
    uint8_t *zeros = calloc(1, chunk);
    for (int off = 0; off < OUT_SIZE; off += chunk) {
        int sz = (OUT_SIZE - off < chunk) ? OUT_SIZE - off : chunk;
        nvmap_write(out_h, off, zeros, sz);
    }
    free(zeros);

    /* Init gather: configure ISP DMA pipeline (required for pixel output) */
    {
        uint32_t init_cmd[16];
        int ini = 0;
        init_cmd[ini++] = OP_SETCLASS(ISP_CLASS, 0, 0);
        if (use_miui_init) {
            printf("Using MIUI init: 0x018-0x01C + 0x01F + 0x05F\n");
            init_cmd[ini++] = OP_INCR(0x018, 5);
            init_cmd[ini++] = 0x0A00500A;  /* 0x018 */
            init_cmd[ini++] = 0x00008089;  /* 0x019 */
            init_cmd[ini++] = 0x013645CB;  /* 0x01A */
            init_cmd[ini++] = 0x000001E7;  /* 0x01B */
            init_cmd[ini++] = 0x00000001;  /* 0x01C */
            init_cmd[ini++] = OP_INCR(0x01F, 1);
            init_cmd[ini++] = 0x00000001;
            init_cmd[ini++] = OP_INCR(0x05F, 1);
            init_cmd[ini++] = 0x00000010;
        } else {
            init_cmd[ini++] = OP_INCR(0x019, 1);
            init_cmd[ini++] = 0x00000400;
            init_cmd[ini++] = OP_INCR(0x01B, 2);
            init_cmd[ini++] = 0x00000200;
            init_cmd[ini++] = 0x00000002;
        }

        uint32_t init_h = nvmap_create(4096);
        nvmap_alloc(init_h);
        nvmap_write(init_h, 0, init_cmd, ini * 4);

        /* G[1]: immediate syncpt incr */
        struct nvhost_get_param_arg gsp_s;
        gsp_s.param = 2;
        ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gsp_s);
        uint32_t sp_stream = gsp_s.value;
        printf("Stream syncpt=%u\n", sp_stream);

        uint32_t g1_data[2];
        g1_data[0] = (4 << 28) | sp_stream;  /* IMM incr */
        g1_data[1] = 0;
        nvmap_write(init_h, 256, g1_data, 8);

        struct nvhost_cmdbuf icbs[2];
        icbs[0] = (struct nvhost_cmdbuf){ .mem = init_h, .offset = 0, .words = ini };
        icbs[1] = (struct nvhost_cmdbuf){ .mem = init_h, .offset = 256, .words = 2 };
        struct nvhost_syncpt_incr isi = { .syncpt_id = sp_stream, .syncpt_incrs = 1 };
        uint32_t iclasses[2] = { ISP_CLASS, ISP_CLASS };
        struct nvhost_fence ifence = {0,0};

        struct nvhost32_submit_args isa;
        memset(&isa, 0, sizeof(isa));
        isa.submit_version = 0;
        isa.num_syncpt_incrs = 1;
        isa.num_cmdbufs = 2;
        isa.timeout = 5000;
        isa.syncpt_incrs = (uint32_t)(uintptr_t)&isi;
        isa.cmdbufs = (uint32_t)(uintptr_t)icbs;
        isa.class_ids = (uint32_t)(uintptr_t)iclasses;
        isa.fences = (uint32_t)(uintptr_t)&ifence;

        if (ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &isa) < 0)
            perror("init submit FAILED");
        else
            printf("Init submit OK (0x019/0x01B/0x01C), fence=%u\n", isa.fence);
    }

    /* Build reprocess gather with ISP pipeline init */
    #include "isp_lens_shading.h"
    uint32_t cmd[2048];
    int n = 0;
    int y_reloc = -1, u_reloc = -1, v_reloc = -1, in_reloc = -1;
    int work_reloc = -1;

    /* SETCLASS must be first — tells host1x which engine gets the commands */
    cmd[n++] = OP_SETCLASS(ISP_CLASS, 0, 0);

    /* ISP work buffer (0x053) — needed for cold start */
    cmd[n++] = OP_INCR(0x053, 2);
    work_reloc = n;
    cmd[n++] = 1;                         /* enable */
    cmd[n++] = 0;                         /* IOVA patched by reloc */

    /* 0x200 pipeline mode */
    cmd[n++] = OP_INCR(0x200, 9);
    if (use_miui) {
        printf("Using MIUI 0x200 block (pipeline mode)\n");
        cmd[n++] = 0x00000001;  /* 0x200 */
        cmd[n++] = 0x00000000;  /* 0x201 */
        cmd[n++] = 0x00000001;  /* 0x202 */
        cmd[n++] = 0x02000200;  /* 0x203 */
        cmd[n++] = 0x02000200;  /* 0x204 */
        cmd[n++] = 0x00000000;  /* 0x205 */
        cmd[n++] = 0x000600C8;  /* 0x206 */
        cmd[n++] = 0x000F000F;  /* 0x207 */
        cmd[n++] = 0x00003333;  /* 0x208 */
    } else {
        cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0;
        cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0;
        cmd[n++] = 0;
    }

    /* ---- S5 register blocks ---- */

    /* 0x700: processing channel A (16 words) */
    cmd[n++] = OP_INCR(0x700, 16);
    cmd[n++] = 0x00000001; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000;
    cmd[n++] = 0x00001a40;  /* ISP-B stride */
    cmd[n++] = 0x00000000; cmd[n++] = work_iova + 0x30000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00001000;
    cmd[n++] = 0x00001a00;  /* ISP-B */
    cmd[n++] = work_iova + 0x20000; cmd[n++] = work_iova + 0x20000;
    cmd[n++] = work_iova + 0x20000; cmd[n++] = work_iova + 0x20000;

    /* 0x750: processing channel B (16 words) */
    cmd[n++] = OP_INCR(0x750, 16);
    cmd[n++] = 0x00000003; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = work_iova + 0x20000; cmd[n++] = work_iova + 0x20000;
    cmd[n++] = work_iova + 0x20000; cmd[n++] = work_iova + 0x20000;

    /* 0xC00: extra config */
    cmd[n++] = OP_INCR(0xC00, 3);
    cmd[n++] = 0x00000101;
    cmd[n++] = 0x00000000;
    cmd[n++] = 0x00100000;

    /* 0x506: demosaic (9 words) */
    cmd[n++] = OP_INCR(0x506, 9);
    cmd[n++] = 0x3f3fcff3;
    cmd[n++] = 0x00000000;
    cmd[n++] = 0x04c1304c;
    cmd[n++] = 0x08220882;
    cmd[n++] = 0x00000000;
    cmd[n++] = 0x03d0f43d;
    cmd[n++] = 0x08621886;
    cmd[n++] = 0x01204812;
    cmd[n++] = 0x06e1b86e;

    /* 0x600: GPP config (16 words) */
    cmd[n++] = OP_INCR(0x600, 16);
    cmd[n++] = 0x00000005; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = 0x3fff0000; cmd[n++] = 0x3fff0000;
    cmd[n++] = 0x3fff0000; cmd[n++] = work_iova + 0x31000;

    /* 0x650: tone curve enable */
    cmd[n++] = OP_INCR(0x650, 1);
    cmd[n++] = 0x00000003;

    /* ---- End S5 blocks ---- */

    /* Lens shading control (0xD00, 10 words) — from stock color filter trace */
    cmd[n++] = OP_INCR(0xD00, 10);
    cmd[n++] = 0x00000001;  /* enable */
    cmd[n++] = 0x00ca4580;
    cmd[n++] = 0x006522c0;
    cmd[n++] = 0x00ca4580;
    cmd[n++] = 0x010db200;
    cmd[n++] = 0x0086d900;
    cmd[n++] = 0x010db200;
    cmd[n++] = 0x05100288;  /* grid: 1296x648 */
    cmd[n++] = 0x03cc01e6;  /* grid: 972x486 */
    cmd[n++] = 0x00000021;  /* mode */

    /* Lens shading enable */
    cmd[n++] = OP_INCR(0xD0A, 1);
    cmd[n++] = 1;

    /* Lens shading table — 480 words from stock OV5693 */
    cmd[n++] = OP_NONINCR(0xD0B, LS_DATA_WORDS);
    for (int i = 0; i < LS_DATA_WORDS; i++)
        cmd[n++] = ls_data[i];

    /* Tone curves — S-curve (shadows=1.0, mids ramp to 3.0, highlights=3.0) */
    for (int ch = 0; ch < 4; ch++) {
        cmd[n++] = OP_INCR(0x651 + ch * 2, 1);
        cmd[n++] = 0;
        cmd[n++] = OP_NONINCR(0x652 + ch * 2, 257);
        for (int i = 0; i < 257; i++) {
            uint32_t val;
            if (i < 64)
                val = 0x1000;  /* 1.0 — shadows */
            else if (i < 192)
                val = 0x1000 + (i - 64) * 0x2000 / 128;  /* ramp 1.0→3.0 */
            else
                val = 0x3000;  /* 3.0 — highlights */
            cmd[n++] = val;
        }
    }

    /* === MIUI-only register blocks (from stock camera gather #8) === */
    if (use_miui) {
        printf("Adding MIUI blocks: 0x400, 0x800, 0xD20, 0x930\n");

        /* 0x400-0x40A: unknown processing block */
        cmd[n++] = OP_INCR(0x400, 11);
        cmd[n++] = 0x00000001;  /* 0x400 */
        cmd[n++] = 0x004B0000;  /* 0x401 */
        cmd[n++] = 0x00930000;  /* 0x402 */
        cmd[n++] = 0x00220000;  /* 0x403 */
        cmd[n++] = 0x2FF01000;  /* 0x404 */
        cmd[n++] = 0x2FF01000;  /* 0x405 */
        cmd[n++] = 0x2FF01000;  /* 0x406 */
        cmd[n++] = 0x2FF01000;  /* 0x407 */
        cmd[n++] = 0x00030000;  /* 0x408 */
        cmd[n++] = 0x00000000;  /* 0x409 */
        cmd[n++] = 0x00020000;  /* 0x40A */

        /* 0x800/0x820: black level */
        cmd[n++] = OP_INCR(0x800, 1);
        cmd[n++] = 0x85001000;
        cmd[n++] = OP_INCR(0x820, 1);
        cmd[n++] = 0x85001000;

        /* 0xD20-0xD25: lens shading extra */
        cmd[n++] = OP_INCR(0xD20, 6);
        cmd[n++] = 0x00003101;
        cmd[n++] = 0x00000000;
        cmd[n++] = 0x01EC0000;
        cmd[n++] = 0x01EC0000;
        cmd[n++] = 0x01EC0000;
        cmd[n++] = 0x01EC0000;

        /* 0x930-0x941: extended stats config */
        cmd[n++] = OP_INCR(0x930, 16);
        cmd[n++] = 0x0000001C; cmd[n++] = 0x88888888;
        cmd[n++] = 0x78787800; cmd[n++] = 0x00000078;
        cmd[n++] = 0x88888888; cmd[n++] = 0x78787800;
        cmd[n++] = 0x00000078; cmd[n++] = 0x88888888;
        cmd[n++] = 0x78787800; cmd[n++] = 0x00000078;
        cmd[n++] = 0x88888888; cmd[n++] = 0x78787800;
        cmd[n++] = 0x00000078; cmd[n++] = 0x3FC00000;
        cmd[n++] = 0x00000000; cmd[n++] = 0x00070000;
        cmd[n++] = OP_INCR(0x941, 1);
        cmd[n++] = 0x00070000;
    }

    /* Output: dims + format */
    cmd[n++] = OP_INCR(0xE00, 1);
    cmd[n++] = ((W - 1) & 0x3FFF) << 16;
    cmd[n++] = OP_INCR(0xE01, 1);
    cmd[n++] = ((H - 1) & 0x3FFF) << 16;
    cmd[n++] = OP_INCR(0xE02, 1);
    uint32_t out_fmt = 0x43;  /* default R8G8B8A8 */
    if (argc > 2) out_fmt = strtoul(argv[2], NULL, 16);
    cmd[n++] = out_fmt;
    printf("Output format: 0x%08x\n", out_fmt);

    /* Output Y surface */
    cmd[n++] = OP_INCR(0xE04, 3);
    y_reloc = n;
    cmd[n++] = 0;
    cmd[n++] = 0x00000000;
    cmd[n++] = W * 4;
    /* U/V — same buffer */
    cmd[n++] = OP_INCR(0xE07, 3);
    u_reloc = n;
    cmd[n++] = 0;
    cmd[n++] = 0x00000000;
    cmd[n++] = W * 4;
    cmd[n++] = OP_INCR(0xE0A, 3);
    v_reloc = n;
    cmd[n++] = 0;
    cmd[n++] = 0x00000000;
    cmd[n++] = W * 4;

    /* Input: dims + format + surface + strip + trigger */
    cmd[n++] = OP_INCR(0xE31, 1);
    cmd[n++] = (H << 16) | W;
    cmd[n++] = OP_INCR(0xE33, 1);
    cmd[n++] = 0x10200024;                /* BG10 */
    cmd[n++] = OP_INCR(0xE34, 3);
    in_reloc = n;
    cmd[n++] = 0;                         /* IOVA patched by reloc */
    cmd[n++] = 0x00000000;
    cmd[n++] = W * BPP;                   /* input stride */
    cmd[n++] = OP_INCR(0xE32, 1);
    cmd[n++] = W & 0x3FFF;                /* strip width */
    cmd[n++] = OP_INCR(0xE30, 1);
    cmd[n++] = 1;                         /* input trigger */

    /* ISP_ENABLE — try different values */
    cmd[n++] = OP_INCR(0x015, 1);
    uint32_t isp_enable = 0x04040007;
    if (argc > 3) isp_enable = strtoul(argv[3], NULL, 16);
    cmd[n++] = isp_enable;
    printf("ISP_ENABLE: 0x%08x\n", isp_enable);

    /* Syncpt conditional incrs */
    cmd[n++] = OP_SETCLASS(ISP_CLASS, 0, 0);
    cmd[n++] = OP_NONINCR(0x000, 1);
    cmd[n++] = (4 << 8) | sp_memory;
    cmd[n++] = OP_NONINCR(0x000, 1);
    cmd[n++] = (5 << 8) | sp_stats;
    cmd[n++] = OP_NONINCR(0x000, 1);
    cmd[n++] = (6 << 8) | sp_loadv;

    /* Reprocess trigger: two-step 0x09 then 0x0B (from Ghidra RE) */
    cmd[n++] = OP_SETCLASS(ISP_CLASS, 0, 0);
    cmd[n++] = OP_NONINCR(0x00C, 1);
    cmd[n++] = 0x09;
    cmd[n++] = OP_NONINCR(0x00C, 1);
    cmd[n++] = 0x0B;

    printf("Gather: %d words\n", n);
    nvmap_write(cmd_h, 0, cmd, n * 4);

    /* Relocs */
    struct nvhost_reloc relocs[5];
    struct nvhost_reloc_shift shifts[5];
    int nr = 0;
    relocs[nr] = (struct nvhost_reloc){ cmd_h, (work_reloc+1)*4, work_h, 0 };
    shifts[nr++].shift = 0;
    relocs[nr] = (struct nvhost_reloc){ cmd_h, y_reloc*4, out_h, 0 };
    shifts[nr++].shift = 0;
    relocs[nr] = (struct nvhost_reloc){ cmd_h, u_reloc*4, out_h, 0 };
    shifts[nr++].shift = 0;
    relocs[nr] = (struct nvhost_reloc){ cmd_h, v_reloc*4, out_h, 0 };
    shifts[nr++].shift = 0;
    relocs[nr] = (struct nvhost_reloc){ cmd_h, in_reloc*4, in_h, 0 };
    shifts[nr++].shift = 0;

    /* Submit */
    struct nvhost_ctrl_syncpt_waitex_args rd = { .id = sp_memory, .thresh = 0, .timeout = 0 };
    ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_WAITEX, &rd);
    printf("Submit (syncpt %u cur=%u)...\n", sp_memory, rd.value);

    struct nvhost_cmdbuf cb = { .mem = cmd_h, .offset = 0, .words = n };
    struct nvhost_syncpt_incr si = { .syncpt_id = sp_memory, .syncpt_incrs = 1 };
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
        printf("SUBMIT FAILED\n");
        return 1;
    }

    uint32_t thresh = sa.fence;
    printf("Fence=%u, waiting...\n", thresh);

    struct nvhost_ctrl_syncpt_waitex_args wa = {
        .id = sp_memory, .thresh = thresh, .timeout = 5000
    };
    if (ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_WAITEX, &wa) < 0) {
        printf("TIMEOUT (syncpt %u thresh %u)\n", sp_memory, thresh);
    } else {
        printf("Done (syncpt=%u val=%u)\n", sp_memory, wa.value);
    }

    /* Read and check output */
    uint8_t check[64];
    nvmap_read(out_h, 0, check, 64);
    int nz = 0;
    for (int i = 0; i < 64; i++) if (check[i]) nz++;
    printf("Output first 64 bytes: %d non-zero → ", nz);
    for (int i = 0; i < 16; i++) printf("%02x ", check[i]);
    printf("\n");

    /* Dump full output */
    char outpath[128];
    snprintf(outpath, sizeof(outpath), "/data/local/tmp/isp_fmt_%08x.bin", out_fmt);
    FILE *fp = fopen(outpath, "wb");
    if (fp) {
        uint8_t *buf = malloc(chunk);
        for (int off = 0; off < OUT_SIZE; off += chunk) {
            int sz = (OUT_SIZE - off < chunk) ? OUT_SIZE - off : chunk;
            nvmap_read(out_h, off, buf, sz);
            fwrite(buf, 1, sz, fp);
        }
        free(buf);
        fclose(fp);
        printf("Saved to %s (%d bytes)\n", outpath, OUT_SIZE);
    }

    close(ctrl_fd);
    close(isp_fd);
    close(nvmap_fd);
    printf("=== Done ===\n");
    return 0;
}
