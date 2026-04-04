/*
 * isp_test.c — Userspace ISP submit test for Tegra K1 (T124)
 *
 * Tests ISP via nvhost channel ioctl path (same as stock userspace).
 * Allocates buffers via nvmap, submits ISP command buffer, waits for syncpt.
 *
 * Build: arm-linux-gnueabihf-gcc -static -o isp_test isp_test.c
 * Run:   adb push isp_test /data/local/tmp/ && adb shell /data/local/tmp/isp_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>

/* ---- nvmap ioctls ---- */
#define NVMAP_IOC_MAGIC 'N'

struct nvmap_create_handle {
	union { uint32_t id; uint32_t size; int32_t fd; };
	uint32_t handle;
};

struct nvmap_alloc_handle {
	uint32_t handle;
	uint32_t heap_mask;
	uint32_t flags;
	uint32_t align;
};

struct nvmap_pin_handle_32 {
	uint32_t handles;
	uint32_t addr;
	uint32_t count;
};

#define NVMAP_IOC_CREATE    _IOWR(NVMAP_IOC_MAGIC, 0, struct nvmap_create_handle)
#define NVMAP_IOC_ALLOC     _IOW(NVMAP_IOC_MAGIC, 3, struct nvmap_alloc_handle)
#define NVMAP_IOC_FREE      _IO(NVMAP_IOC_MAGIC, 4)
#define NVMAP_IOC_GET_FD    _IOWR(NVMAP_IOC_MAGIC, 15, struct nvmap_create_handle)
#define NVMAP_IOC_PIN_MULT  _IOWR(NVMAP_IOC_MAGIC, 10, struct nvmap_pin_handle_32)
#define NVMAP_IOC_UNPIN_MULT _IOW(NVMAP_IOC_MAGIC, 11, struct nvmap_pin_handle_32)

#define NVMAP_HEAP_IOVMM    (1 << 30)
#define NVMAP_HANDLE_UNCACHEABLE   0
#define NVMAP_HANDLE_WRITE_COMBINE 1

/* ---- nvhost ioctls ---- */
#define NVHOST_IOC_MAGIC 'H'

struct nvhost_get_param_arg {
	uint32_t param;
	uint32_t value;
};

struct nvhost_set_nvmap_fd_args {
	uint32_t fd;
};

struct nvhost_ctrl_syncpt_waitex_args {
	uint32_t id;
	uint32_t thresh;
	int32_t  timeout;
	uint32_t value;
};

struct nvhost_cmdbuf {
	uint32_t mem;       /* dmabuf fd */
	uint32_t offset;    /* byte offset */
	uint32_t words;     /* num u32 words */
};

struct nvhost_syncpt_incr {
	uint32_t syncpt_id;
	uint32_t syncpt_incrs;
};

struct nvhost_reloc {
	uint32_t cmdbuf_mem;
	uint32_t cmdbuf_offset;
	uint32_t target;
	uint32_t target_offset;
};

struct nvhost_reloc_shift {
	uint32_t shift;
};

struct nvhost32_submit_args {
	uint32_t submit_version;
	uint32_t num_syncpt_incrs;
	uint32_t num_cmdbufs;
	uint32_t num_relocs;
	uint32_t num_waitchks;
	uint32_t timeout;
	uint32_t syncpt_incrs;  /* userptr */
	uint32_t cmdbufs;       /* userptr */
	uint32_t relocs;        /* userptr */
	uint32_t reloc_shifts;  /* userptr */
	uint32_t waitchks;      /* userptr */
	uint32_t waitbases;     /* userptr */
	uint32_t class_ids;     /* userptr */
	uint32_t pad[2];
	uint32_t fences;        /* userptr */
	uint32_t fence;         /* out */
};

#define NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD \
	_IOW(NVHOST_IOC_MAGIC, 5, struct nvhost_set_nvmap_fd_args)
#define NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT \
	_IOWR(NVHOST_IOC_MAGIC, 16, struct nvhost_get_param_arg)
#define NVHOST32_IOCTL_CHANNEL_SUBMIT \
	_IOWR(NVHOST_IOC_MAGIC, 15, struct nvhost32_submit_args)
#define NVHOST_IOCTL_CTRL_SYNCPT_WAITEX \
	_IOWR(NVHOST_IOC_MAGIC, 6, struct nvhost_ctrl_syncpt_waitex_args)

/* ---- host1x opcodes ---- */
#define NVHOST_OPCODE_SETCLASS(cl, off, mask) \
	((0 << 28) | ((off) << 16) | ((cl) << 6) | (mask))
#define NVHOST_OPCODE_INCR(off, count) \
	((1 << 28) | ((off) << 16) | (count))
#define NVHOST_OPCODE_NONINCR(off, count) \
	((2 << 28) | ((off) << 16) | (count))
#define NVHOST_OPCODE_IMM(off, val) \
	((4 << 28) | ((off) << 16) | (val))
#define NVHOST_OPCODE_NOOP 0

/* ISP class IDs */
#define ISP_A_CLASS  0x32
#define ISP_B_CLASS  0x34

/* ISP methods */
#define ISP_METHOD_CONTROL   0x00C
#define ISP_METHOD_ENABLE    0x015
#define ISP_METHOD_STATS_BUF 0x100
#define ISP_METHOD_PROCESSING 0x500
#define ISP_METHOD_OUT_WIDTH  0xE00
#define ISP_METHOD_OUT_HEIGHT 0xE01
#define ISP_METHOD_OUT_FORMAT 0xE02
#define ISP_METHOD_OUT_COLOR  0xE03
#define ISP_METHOD_OUT_SURF_Y 0xE04
#define ISP_METHOD_OUT_SURF_U 0xE07
#define ISP_METHOD_OUT_SURF_V 0xE0A
#define ISP_METHOD_IN_TRIGGER 0xE30
#define ISP_METHOD_IN_DIMS    0xE31
#define ISP_METHOD_IN_STRIP   0xE32
#define ISP_METHOD_IN_FORMAT  0xE33
#define ISP_METHOD_IN_SURF0   0xE34

/* ---- helpers ---- */

static int nvmap_fd = -1;

struct nvbuf {
	uint32_t handle;
	int dmabuf_fd;
	uint32_t iova;
	void *cpu;
	uint32_t size;
};

static int nvbuf_alloc(struct nvbuf *b, uint32_t size, uint32_t align)
{
	struct nvmap_create_handle ch = { .size = size };
	struct nvmap_alloc_handle ah;
	struct nvmap_create_handle gf;
	struct nvmap_pin_handle_32 ph;
	int ret;

	memset(b, 0, sizeof(*b));
	b->size = size;

	/* Create handle */
	ret = ioctl(nvmap_fd, NVMAP_IOC_CREATE, &ch);
	if (ret) { perror("nvmap CREATE"); return -1; }
	b->handle = ch.handle;

	/* Alloc backing memory */
	ah.handle = b->handle;
	ah.heap_mask = NVMAP_HEAP_IOVMM;
	ah.flags = NVMAP_HANDLE_WRITE_COMBINE;
	ah.align = align;
	ret = ioctl(nvmap_fd, NVMAP_IOC_ALLOC, &ah);
	if (ret) { perror("nvmap ALLOC"); return -1; }

	/* Get dmabuf fd */
	gf.handle = b->handle;
	ret = ioctl(nvmap_fd, NVMAP_IOC_GET_FD, &gf);
	if (ret) { perror("nvmap GET_FD"); return -1; }
	b->dmabuf_fd = gf.fd;

	/* Pin → get IOVA */
	ph.handles = b->handle;  /* count==1: handle directly, not pointer */
	ph.addr = 0;
	ph.count = 1;
	ret = ioctl(nvmap_fd, NVMAP_IOC_PIN_MULT, &ph);
	if (ret) { perror("nvmap PIN"); return -1; }
	b->iova = ph.addr;

	/* mmap for CPU access */
	b->cpu = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
		      b->dmabuf_fd, 0);
	if (b->cpu == MAP_FAILED) {
		perror("mmap dmabuf");
		b->cpu = NULL;
		return -1;
	}

	printf("  buf: handle=%u fd=%d iova=0x%08x size=%u cpu=%p\n",
	       b->handle, b->dmabuf_fd, b->iova, b->size, b->cpu);
	return 0;
}

static void nvbuf_free(struct nvbuf *b)
{
	struct nvmap_pin_handle_32 ph;
	if (b->cpu) munmap(b->cpu, b->size);
	if (b->handle) {
		ph.handles = b->handle;
		ph.count = 1;
		ioctl(nvmap_fd, NVMAP_IOC_UNPIN_MULT, &ph);
		ioctl(nvmap_fd, NVMAP_IOC_FREE, (void*)(uintptr_t)b->handle);
	}
	if (b->dmabuf_fd >= 0) close(b->dmabuf_fd);
	memset(b, 0, sizeof(*b));
}

static int nvhost_submit(int ch_fd, struct nvbuf *cmdbuf, int words,
			 uint32_t class_id,
			 struct nvhost_syncpt_incr *sp, int num_sp,
			 struct nvhost_reloc *relocs, int num_relocs,
			 struct nvhost_reloc_shift *shifts,
			 uint32_t *out_fence)
{
	struct nvhost32_submit_args sa;
	struct nvhost_cmdbuf cb;
	uint32_t fence_vals[4] = {0};
	int ret;

	cb.mem = cmdbuf->dmabuf_fd;
	cb.offset = 0;
	cb.words = words;

	memset(&sa, 0, sizeof(sa));
	sa.submit_version = 0;
	sa.num_syncpt_incrs = num_sp;
	sa.num_cmdbufs = 1;
	sa.num_relocs = num_relocs;
	sa.num_waitchks = 0;
	sa.timeout = 5000;
	sa.syncpt_incrs = (uint32_t)(uintptr_t)sp;
	sa.cmdbufs = (uint32_t)(uintptr_t)&cb;
	sa.relocs = (uint32_t)(uintptr_t)relocs;
	sa.reloc_shifts = (uint32_t)(uintptr_t)shifts;
	sa.waitchks = 0;
	sa.waitbases = 0;
	sa.class_ids = (uint32_t)(uintptr_t)&class_id;
	sa.fences = (uint32_t)(uintptr_t)fence_vals;

	ret = ioctl(ch_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa);
	if (ret) {
		perror("SUBMIT");
		return -1;
	}

	*out_fence = sa.fence;
	printf("  submit OK: fence=%u\n", sa.fence);
	return 0;
}

static int nvhost_wait_syncpt(int ctrl_fd, uint32_t id, uint32_t thresh, int timeout_ms)
{
	struct nvhost_ctrl_syncpt_waitex_args wa;
	wa.id = id;
	wa.thresh = thresh;
	wa.timeout = timeout_ms;
	wa.value = 0;

	int ret = ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_WAITEX, &wa);
	if (ret) {
		printf("  WAIT timeout: syncpt=%u thresh=%u (err=%d %s)\n",
		       id, thresh, errno, strerror(errno));
		return -1;
	}
	printf("  WAIT OK: syncpt=%u value=%u\n", id, wa.value);
	return 0;
}

/* ---- test scenarios ---- */

/*
 * Test 1: Ping — IMMEDIATE syncpt increment only.
 * If this fails, the channel doesn't work at all.
 */
static int test_ping(int ch_fd, int ctrl_fd, uint32_t syncpt, uint32_t class_id,
		     struct nvbuf *cmdbuf)
{
	uint32_t *cmd = cmdbuf->cpu;
	int n = 0;
	uint32_t fence;
	struct nvhost_syncpt_incr sp = { syncpt, 1 };

	printf("\n=== TEST 1: Ping (IMMEDIATE syncpt) ===\n");

	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	/* IMMEDIATE syncpt incr */
	cmd[n++] = NVHOST_OPCODE_IMM(0, (0 << 8) | (syncpt & 0xFF));
	cmd[n++] = NVHOST_OPCODE_NOOP;

	if (nvhost_submit(ch_fd, cmdbuf, n, class_id, &sp, 1, NULL, 0, NULL, &fence))
		return -1;

	return nvhost_wait_syncpt(ctrl_fd, syncpt, fence, 1000);
}

/*
 * Test 2: OP_DONE (cond=1) — write ISP_CONTROL trigger and check if OP_DONE fires.
 */
static int test_opdone(int ch_fd, int ctrl_fd, uint32_t syncpt, uint32_t class_id,
		       struct nvbuf *cmdbuf, uint32_t trigger_val)
{
	uint32_t *cmd = cmdbuf->cpu;
	int n = 0;
	uint32_t fence;
	struct nvhost_syncpt_incr sp = { syncpt, 2 };

	printf("\n=== TEST 2: OP_DONE trigger=0x%02x ===\n", trigger_val);

	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);

	/* Conditional syncpt incr: cond=1 (OP_DONE) */
	cmd[n++] = NVHOST_OPCODE_NONINCR(0x000, 1);
	cmd[n++] = (1 << 8) | (syncpt & 0xFF);

	/* Trigger ISP */
	cmd[n++] = NVHOST_OPCODE_NONINCR(ISP_METHOD_CONTROL, 1);
	cmd[n++] = trigger_val;

	/* Safety: IMMEDIATE incr so we don't hang forever */
	cmd[n++] = NVHOST_OPCODE_IMM(0, (0 << 8) | (syncpt & 0xFF));
	cmd[n++] = NVHOST_OPCODE_NOOP;

	if (nvhost_submit(ch_fd, cmdbuf, n, class_id, &sp, 1, NULL, 0, NULL, &fence))
		return -1;

	/* fence = syncpt value after 2 incrs. Try to wait for fence-1 (OP_DONE only) */
	int ret = nvhost_wait_syncpt(ctrl_fd, syncpt, fence - 1, 500);
	if (ret) {
		printf("  OP_DONE did NOT fire for trigger 0x%02x\n", trigger_val);
		/* Wait for IMMEDIATE to drain */
		nvhost_wait_syncpt(ctrl_fd, syncpt, fence, 1000);
	} else {
		printf("  OP_DONE FIRED for trigger 0x%02x!\n", trigger_val);
	}
	return ret;
}

/*
 * Test 3: Full ISP pipeline — init + process frame.
 * Uses reprocess mode (input via 0xE34) with trigger 0x09.
 */
static int test_reprocess(int ch_fd, int ctrl_fd, uint32_t syncpt, uint32_t class_id,
			  struct nvbuf *cmdbuf, struct nvbuf *inbuf, struct nvbuf *outbuf,
			  struct nvbuf *workbuf, uint32_t W, uint32_t H)
{
	uint32_t *cmd = cmdbuf->cpu;
	int n = 0;
	uint32_t fence;
	uint32_t in_stride = W * 2;
	uint32_t y_stride = (W + 63) & ~63;
	uint32_t uv_stride = ((W/2) + 63) & ~63;
	uint32_t y_size = y_stride * H;
	uint32_t uv_size = uv_stride * (H/2);

	printf("\n=== TEST 3: Reprocess %ux%u (trigger 0x09) ===\n", W, H);
	printf("  in_iova=0x%08x out_iova=0x%08x work_iova=0x%08x\n",
	       inbuf->iova, outbuf->iova, workbuf->iova);

	/* Fill input with test pattern */
	memset(inbuf->cpu, 0x42, inbuf->size);
	/* Fill output with deadbeef */
	memset(outbuf->cpu, 0xDE, outbuf->size);

	/* --- ISP PIO init: clock gating --- */
	/* Note: this is normally done by kernel finalize_poweron,
	 * but we write it explicitly in cmdbuf for safety */

	/* --- Per-frame command buffer --- */

	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);

	/* Output config */
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_WIDTH, 1);
	cmd[n++] = ((W - 1) & 0x3FFF) << 16;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_HEIGHT, 1);
	cmd[n++] = ((H - 1) & 0x3FFF) << 16;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_FORMAT, 1);
	cmd[n++] = 0x04FE00E6;  /* stock YUV420 format */
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_COLOR, 1);
	cmd[n++] = 0x00000000;

	/* Output surfaces — using direct IOVA (no reloc for simplicity) */
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_SURF_Y, 3);
	cmd[n++] = outbuf->iova;
	cmd[n++] = 0;
	cmd[n++] = y_stride;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_SURF_U, 3);
	cmd[n++] = outbuf->iova + y_size;
	cmd[n++] = 0;
	cmd[n++] = uv_stride;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_SURF_V, 3);
	cmd[n++] = outbuf->iova + y_size + uv_size;
	cmd[n++] = 0;
	cmd[n++] = uv_stride;

	/* Processing block */
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_PROCESSING, 6);
	cmd[n++] = 0;
	cmd[n++] = 0;
	cmd[n++] = 0;
	cmd[n++] = 0;
	cmd[n++] = 0;
	cmd[n++] = (H << 16) | W;

	/* Input (v3 reprocess methods) */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);

	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_IN_DIMS, 1);
	cmd[n++] = (W & 0x7FFF) | (H << 16);

	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_IN_FORMAT, 1);
	cmd[n++] = 0x11000020;  /* RAW Bayer single-plane linear */

	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_IN_SURF0, 3);
	cmd[n++] = inbuf->iova;
	cmd[n++] = 0;
	cmd[n++] = in_stride;

	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_IN_STRIP, 1);
	cmd[n++] = W & 0x3FFF;

	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_ENABLE, 1);
	cmd[n++] = 0x00000007;  /* reprocess mode enable */

	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_IN_TRIGGER, 1);
	cmd[n++] = 1;

	/* Stats buffer */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_STATS_BUF, 4);
	cmd[n++] = workbuf->iova;
	cmd[n++] = 0;
	cmd[n++] = 0;
	cmd[n++] = 0;

	/* Syncpt incrs: cond 4,5,6 */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	cmd[n++] = NVHOST_OPCODE_NONINCR(0x000, 1);
	cmd[n++] = (4 << 8) | (syncpt & 0xFF);   /* cond=4 */
	cmd[n++] = NVHOST_OPCODE_NONINCR(0x000, 1);
	cmd[n++] = (5 << 8) | (syncpt & 0xFF);   /* cond=5 */
	cmd[n++] = NVHOST_OPCODE_NONINCR(0x000, 1);
	cmd[n++] = (6 << 8) | (syncpt & 0xFF);   /* cond=6 */

	/* Trigger reprocess: 0x09, then 0x0B (stock reprocess sequence) */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	cmd[n++] = NVHOST_OPCODE_NONINCR(ISP_METHOD_CONTROL, 1);
	cmd[n++] = 0x09;
	cmd[n++] = NVHOST_OPCODE_NONINCR(ISP_METHOD_CONTROL, 1);
	cmd[n++] = 0x0B;

	/* Safety: IMMEDIATE so we don't hang */
	cmd[n++] = NVHOST_OPCODE_IMM(0, (0 << 8) | (syncpt & 0xFF));
	cmd[n++] = NVHOST_OPCODE_NOOP;

	printf("  cmdbuf: %d words\n", n);

	/* Submit with 4 syncpt incrs (3 conditional + 1 immediate) */
	struct nvhost_syncpt_incr sp = { syncpt, 4 };

	if (nvhost_submit(ch_fd, cmdbuf, n, class_id, &sp, 1, NULL, 0, NULL, &fence))
		return -1;

	/* Wait for cond syncpts (fence-1 = after 3 conditional incrs, before immediate) */
	int ret = nvhost_wait_syncpt(ctrl_fd, syncpt, fence - 1, 2000);
	if (ret) {
		printf("  ISP cond syncpts DID NOT fire\n");
		/* Drain immediate */
		nvhost_wait_syncpt(ctrl_fd, syncpt, fence, 5000);
	} else {
		printf("  ISP cond syncpts FIRED! Checking output...\n");
		uint32_t *out32 = outbuf->cpu;
		printf("  out[0]=0x%08x out[1]=0x%08x out[100]=0x%08x\n",
		       out32[0], out32[1], out32[100]);
		if (out32[0] == 0xDEDEDEDE)
			printf("  OUTPUT UNCHANGED (still deadbeef)\n");
		else
			printf("  OUTPUT MODIFIED — ISP PROCESSED!\n");
	}
	return ret;
}

/*
 * Test 4: Streaming mode — ISP_ENABLE=0x04040007, no input, trigger 0x05
 */
static int test_streaming(int ch_fd, int ctrl_fd, uint32_t syncpt, uint32_t class_id,
			  struct nvbuf *cmdbuf, struct nvbuf *outbuf,
			  struct nvbuf *workbuf, uint32_t W, uint32_t H)
{
	uint32_t *cmd = cmdbuf->cpu;
	int n = 0;
	uint32_t fence;
	uint32_t y_stride = (W + 63) & ~63;
	uint32_t uv_stride = ((W/2) + 63) & ~63;
	uint32_t y_size = y_stride * H;
	uint32_t uv_size = uv_stride * (H/2);

	printf("\n=== TEST 4: Streaming mode %ux%u (ISP_ENABLE=0x04040007) ===\n", W, H);

	memset(outbuf->cpu, 0xDE, outbuf->size);

	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);

	/* Output config */
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_WIDTH, 1);
	cmd[n++] = ((W - 1) & 0x3FFF) << 16;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_HEIGHT, 1);
	cmd[n++] = ((H - 1) & 0x3FFF) << 16;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_FORMAT, 1);
	cmd[n++] = 0x04FE00E6;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_COLOR, 1);
	cmd[n++] = 0;

	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_SURF_Y, 3);
	cmd[n++] = outbuf->iova;
	cmd[n++] = 0;
	cmd[n++] = y_stride;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_SURF_U, 3);
	cmd[n++] = outbuf->iova + y_size;
	cmd[n++] = 0;
	cmd[n++] = uv_stride;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_SURF_V, 3);
	cmd[n++] = outbuf->iova + y_size + uv_size;
	cmd[n++] = 0;
	cmd[n++] = uv_stride;

	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_PROCESSING, 6);
	cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0;
	cmd[n++] = 0; cmd[n++] = 0;
	cmd[n++] = (H << 16) | W;

	/* Streaming mode enable */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_ENABLE, 1);
	cmd[n++] = 0x04040007;

	/* Stats */
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_STATS_BUF, 4);
	cmd[n++] = workbuf->iova;
	cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0;

	/* Syncpt incrs */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	cmd[n++] = NVHOST_OPCODE_NONINCR(0x000, 1);
	cmd[n++] = (4 << 8) | (syncpt & 0xFF);
	cmd[n++] = NVHOST_OPCODE_NONINCR(0x000, 1);
	cmd[n++] = (5 << 8) | (syncpt & 0xFF);
	cmd[n++] = NVHOST_OPCODE_NONINCR(0x000, 1);
	cmd[n++] = (6 << 8) | (syncpt & 0xFF);

	/* Trigger streaming */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	cmd[n++] = NVHOST_OPCODE_NONINCR(ISP_METHOD_CONTROL, 1);
	cmd[n++] = 0x05;

	/* Safety */
	cmd[n++] = NVHOST_OPCODE_IMM(0, (0 << 8) | (syncpt & 0xFF));
	cmd[n++] = NVHOST_OPCODE_NOOP;

	struct nvhost_syncpt_incr sp = { syncpt, 4 };

	if (nvhost_submit(ch_fd, cmdbuf, n, class_id, &sp, 1, NULL, 0, NULL, &fence))
		return -1;

	int ret = nvhost_wait_syncpt(ctrl_fd, syncpt, fence - 1, 2000);
	if (ret) {
		printf("  Streaming mode: cond syncpts DID NOT fire\n");
		nvhost_wait_syncpt(ctrl_fd, syncpt, fence, 5000);
	} else {
		printf("  Streaming mode: cond syncpts FIRED!\n");
	}
	return ret;
}

/* ---- main ---- */

int main(int argc, char **argv)
{
	int ch_fd, ctrl_fd;
	uint32_t syncpt;
	struct nvbuf cmdbuf, inbuf, outbuf, workbuf;
	const char *isp_dev = "/dev/nvhost-isp";
	uint32_t class_id = ISP_A_CLASS;
	uint32_t W = 64, H = 64; /* small test resolution */

	if (argc > 1 && strcmp(argv[1], "b") == 0) {
		isp_dev = "/dev/nvhost-isp.1";
		class_id = ISP_B_CLASS;
		printf("Using ISP-B (class 0x%02x)\n", class_id);
	} else {
		printf("Using ISP-A (class 0x%02x)\n", class_id);
	}
	if (argc > 2) {
		W = H = atoi(argv[2]);
		printf("Resolution: %ux%u\n", W, H);
	}

	/* Open nvmap */
	nvmap_fd = open("/dev/nvmap", O_RDWR);
	if (nvmap_fd < 0) { perror("open /dev/nvmap"); return 1; }

	/* Open ISP channel */
	ch_fd = open(isp_dev, O_RDWR);
	if (ch_fd < 0) { perror("open isp"); return 1; }

	/* Open host1x ctrl */
	ctrl_fd = open("/dev/nvhost-ctrl", O_RDWR);
	if (ctrl_fd < 0) { perror("open nvhost-ctrl"); return 1; }

	/* Set nvmap fd (legacy, no-op on 24.1 but call for compat) */
	{
		struct nvhost_set_nvmap_fd_args nfa = { .fd = nvmap_fd };
		ioctl(ch_fd, NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD, &nfa);
	}

	/* Get syncpoint */
	{
		struct nvhost_get_param_arg gp = { .param = 0 };
		if (ioctl(ch_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gp)) {
			perror("GET_SYNCPOINT");
			return 1;
		}
		syncpt = gp.value;
		printf("ISP syncpt[0] = %u\n", syncpt);
	}

	/* Allocate buffers */
	printf("Allocating buffers...\n");
	if (nvbuf_alloc(&cmdbuf, 16384, 256)) return 1;
	if (nvbuf_alloc(&inbuf, W * H * 2, 256)) return 1;  /* RAW10 = 2 bpp */
	if (nvbuf_alloc(&outbuf, W * H * 2, 256)) return 1; /* YUV420 ~ 1.5 bpp, 2x for safety */
	if (nvbuf_alloc(&workbuf, 256 * 1024, 256)) return 1; /* 256KB work/stats */

	/* Test 1: Ping */
	test_ping(ch_fd, ctrl_fd, syncpt, class_id, &cmdbuf);

	/* Test 2: OP_DONE with various triggers */
	test_opdone(ch_fd, ctrl_fd, syncpt, class_id, &cmdbuf, 0x0F); /* POST_APPLY */
	test_opdone(ch_fd, ctrl_fd, syncpt, class_id, &cmdbuf, 0x05); /* RUNTIME */
	test_opdone(ch_fd, ctrl_fd, syncpt, class_id, &cmdbuf, 0x09); /* REPROCESS A */
	test_opdone(ch_fd, ctrl_fd, syncpt, class_id, &cmdbuf, 0x0B); /* REPROCESS B */

	/* Test 3: Reprocess */
	test_reprocess(ch_fd, ctrl_fd, syncpt, class_id, &cmdbuf,
		       &inbuf, &outbuf, &workbuf, W, H);

	/* Test 4: Streaming */
	test_streaming(ch_fd, ctrl_fd, syncpt, class_id, &cmdbuf,
		       &outbuf, &workbuf, W, H);

	/* Cleanup */
	nvbuf_free(&workbuf);
	nvbuf_free(&outbuf);
	nvbuf_free(&inbuf);
	nvbuf_free(&cmdbuf);
	close(ctrl_fd);
	close(ch_fd);
	close(nvmap_fd);

	printf("\nDone.\n");
	return 0;
}
