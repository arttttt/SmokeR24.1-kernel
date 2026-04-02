/*
 * ISP hardware test for stock Tegra K1 kernel
 *
 * Submits command buffers to ISP-A via /dev/nvhost-isp using
 * nvmap for buffer allocation and nvhost ioctl for submission.
 *
 * Usage: isp_test [ping|dump|dma]
 *   ping  - submit minimal job, verify syncpt
 *   dump  - read ISP-A MMIO registers via devmem
 *   dma   - attempt DMA frame processing
 *
 * Build: arm-linux-gnueabihf-gcc -static -o isp_test isp_test.c
 * Push:  adb push isp_test /data/local/tmp/
 * Run:   adb shell /data/local/tmp/isp_test ping
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
#include <time.h>

/* ---- nvhost ioctl definitions ---- */
#define NVHOST_IOCTL_MAGIC 'H'

struct nvhost_set_nvmap_fd_args { uint32_t fd; } __attribute__((packed));
struct nvhost_get_param_args { uint32_t value; } __attribute__((packed));
struct nvhost_get_param_arg { uint32_t param; uint32_t value; };
struct nvhost_get_client_managed_syncpt_arg {
	uint64_t name; uint32_t param; uint32_t value;
};
struct nvhost_syncpt_incr { uint32_t syncpt_id; uint32_t syncpt_incrs; };
struct nvhost_cmdbuf { uint32_t mem; uint32_t offset; uint32_t words; } __attribute__((packed));
struct nvhost_cmdbuf_ext { int32_t pre_fence; uint32_t reserved; };
struct nvhost_reloc { uint32_t cmdbuf_mem; uint32_t cmdbuf_offset; uint32_t target; uint32_t target_offset; };
struct nvhost_reloc_shift { uint32_t shift; } __attribute__((packed));

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
	uint32_t pad[2];
	uint32_t fences;
	uint32_t fence;
} __attribute__((packed));

struct nvhost_fence { uint32_t syncpt_id; uint32_t value; };

#define NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD \
	_IOW(NVHOST_IOCTL_MAGIC, 5, struct nvhost_set_nvmap_fd_args)
#define NVHOST_IOCTL_CHANNEL_GET_SYNCPOINTS \
	_IOR(NVHOST_IOCTL_MAGIC, 2, struct nvhost_get_param_args)
#define NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT \
	_IOWR(NVHOST_IOCTL_MAGIC, 16, struct nvhost_get_param_arg)
#define NVHOST_IOCTL_CHANNEL_GET_CLIENT_MANAGED_SYNCPOINT \
	_IOWR(NVHOST_IOCTL_MAGIC, 19, struct nvhost_get_client_managed_syncpt_arg)
#define NVHOST32_IOCTL_CHANNEL_SUBMIT \
	_IOWR(NVHOST_IOCTL_MAGIC, 15, struct nvhost32_submit_args)

/* ---- nvmap ioctl definitions ---- */
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
struct nvmap_map_caller {
	uint32_t handle;
	uint32_t offset;
	uint32_t length;
	uint32_t flags;
	unsigned long addr;
};
struct nvmap_pin_handle {
	uint32_t handles;
	unsigned long addr;
	uint32_t count;
};

#define NVMAP_IOC_CREATE   _IOWR(NVMAP_IOC_MAGIC, 0, struct nvmap_create_handle)
#define NVMAP_IOC_ALLOC    _IOW(NVMAP_IOC_MAGIC, 3, struct nvmap_alloc_handle)
#define NVMAP_IOC_FREE     _IO(NVMAP_IOC_MAGIC, 4)
#define NVMAP_IOC_MMAP     _IOWR(NVMAP_IOC_MAGIC, 5, struct nvmap_map_caller)
#define NVMAP_IOC_PIN_MULT _IOWR(NVMAP_IOC_MAGIC, 10, struct nvmap_pin_handle)

#define NVMAP_HEAP_IOVMM   (1 << 30)
#define NVMAP_HANDLE_WRITE_COMBINE 2

/* ---- nvhost ctrl ---- */
struct nvhost_ctrl_syncpt_read_args { uint32_t id; uint32_t value; };
struct nvhost_ctrl_syncpt_waitex_args { uint32_t id; uint32_t thresh; int32_t timeout; uint32_t value; };

#define NVHOST_CTRL_MAGIC 'H'
#define NVHOST_IOCTL_CTRL_SYNCPT_READ \
	_IOWR(NVHOST_CTRL_MAGIC, 1, struct nvhost_ctrl_syncpt_read_args)
#define NVHOST_IOCTL_CTRL_SYNCPT_WAITEX \
	_IOWR(NVHOST_CTRL_MAGIC, 6, struct nvhost_ctrl_syncpt_waitex_args)

/* ---- host1x opcodes ---- */
static inline uint32_t host1x_opcode_setclass(uint32_t cls, uint32_t off, uint32_t mask)
{ return (0 << 28) | (off << 16) | (cls << 6) | mask; }
static inline uint32_t host1x_opcode_incr(uint32_t off, uint32_t count)
{ return (1 << 28) | (off << 16) | count; }
static inline uint32_t host1x_opcode_nonincr(uint32_t off, uint32_t count)
{ return (2 << 28) | (off << 16) | count; }
static inline uint32_t host1x_opcode_imm(uint32_t off, uint32_t val)
{ return (4 << 28) | (off << 16) | val; }
/* INCR_SYNCPT: method 0, immediate */
static inline uint32_t host1x_opcode_imm_incr_syncpt(uint32_t cond, uint32_t id)
{ return host1x_opcode_imm(0, (cond << 8) | id); }

#define NOOP host1x_opcode_nonincr(0, 0)
#define ISP_CLASS_ID 0x32

/* ---- helpers ---- */
static int nvmap_fd = -1;
static int isp_fd = -1;
static int ctrl_fd = -1;

static uint32_t nvmap_create(uint32_t size)
{
	struct nvmap_create_handle ch = { .size = size };
	if (ioctl(nvmap_fd, NVMAP_IOC_CREATE, &ch) < 0) {
		perror("nvmap create"); return 0;
	}
	return ch.handle;
}

static int nvmap_alloc(uint32_t handle, uint32_t align)
{
	struct nvmap_alloc_handle ah = {
		.handle = handle,
		.heap_mask = NVMAP_HEAP_IOVMM,
		.flags = NVMAP_HANDLE_WRITE_COMBINE,
		.align = align,
	};
	if (ioctl(nvmap_fd, NVMAP_IOC_ALLOC, &ah) < 0) {
		perror("nvmap alloc"); return -1;
	}
	return 0;
}

static void *nvmap_mmap(uint32_t handle, uint32_t size)
{
	void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE,
			  MAP_SHARED, nvmap_fd, 0);
	if (addr == MAP_FAILED) {
		perror("mmap"); return NULL;
	}
	struct nvmap_map_caller mc = {
		.handle = handle,
		.offset = 0,
		.length = size,
		.flags = 0,
		.addr = (unsigned long)addr,
	};
	if (ioctl(nvmap_fd, NVMAP_IOC_MMAP, &mc) < 0) {
		perror("nvmap mmap ioctl");
		munmap(addr, size);
		return NULL;
	}
	return addr;
}

static uint32_t nvmap_pin(uint32_t handle)
{
	/* For count=1, kernel writes result directly into op.addr field,
	 * not into *op.addr. See nvmap_ioctl.c:205-206 */
	struct {
		uint32_t handles;  /* handle value directly when count=1 */
		unsigned long addr; /* kernel writes IOVA here directly */
		uint32_t count;
	} __attribute__((packed)) ph;
	ph.handles = handle;
	ph.addr = 0;
	ph.count = 1;
	if (ioctl(nvmap_fd, NVMAP_IOC_PIN_MULT, &ph) < 0) {
		perror("nvmap pin"); return 0;
	}
	return (uint32_t)ph.addr;
}

static uint32_t syncpt_read(uint32_t id)
{
	struct nvhost_ctrl_syncpt_read_args a = { .id = id };
	if (ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_READ, &a) < 0) {
		perror("syncpt read"); return 0;
	}
	return a.value;
}

static int syncpt_wait(uint32_t id, uint32_t thresh, int timeout_ms)
{
	struct nvhost_ctrl_syncpt_waitex_args a = {
		.id = id, .thresh = thresh, .timeout = timeout_ms,
	};
	return ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_WAITEX, &a);
}

static int submit(uint32_t cmdbuf_handle, uint32_t num_words,
		  uint32_t syncpt_id, uint32_t syncpt_incrs,
		  uint32_t *fence_out)
{
	struct nvhost_cmdbuf cb = {
		.mem = cmdbuf_handle, .offset = 0, .words = num_words,
	};
	struct nvhost_syncpt_incr si = {
		.syncpt_id = syncpt_id, .syncpt_incrs = syncpt_incrs,
	};
	uint32_t class_id = ISP_CLASS_ID;
	struct nvhost_fence fence = { 0, 0 };

	struct nvhost32_submit_args sa;
	memset(&sa, 0, sizeof(sa));
	sa.submit_version = 0;
	sa.num_syncpt_incrs = 1;
	sa.num_cmdbufs = 1;
	sa.num_relocs = 0;
	sa.num_waitchks = 0;
	sa.timeout = 1000;
	sa.syncpt_incrs = (uint32_t)(uintptr_t)&si;
	sa.cmdbufs = (uint32_t)(uintptr_t)&cb;
	sa.class_ids = (uint32_t)(uintptr_t)&class_id;
	sa.fences = (uint32_t)(uintptr_t)&fence;

	if (ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa) < 0) {
		perror("submit");
		return -1;
	}
	if (fence_out) *fence_out = fence.value;
	return 0;
}

/* ---- tests ---- */

static int test_ping(uint32_t syncpt_id)
{
	uint32_t cmdbuf_h = nvmap_create(4096);
	if (!cmdbuf_h) return -1;
	nvmap_alloc(cmdbuf_h, 256);
	uint32_t *cmd = nvmap_mmap(cmdbuf_h, 4096);
	if (!cmd) return -1;

	int n = 0;
	cmd[n++] = host1x_opcode_imm_incr_syncpt(0 /* immediate */, syncpt_id);
	cmd[n++] = NOOP;

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	uint32_t fence;
	if (submit(cmdbuf_h, n, syncpt_id, 1, &fence) < 0) return -1;
	int ret = syncpt_wait(syncpt_id, fence, 500);

	clock_gettime(CLOCK_MONOTONIC, &t1);
	long us = (t1.tv_sec - t0.tv_sec) * 1000000L +
		  (t1.tv_nsec - t0.tv_nsec) / 1000;

	if (ret == 0)
		printf("ISP-A ping OK (%ld us), syncpt %u fence %u\n",
		       us, syncpt_id, fence);
	else
		printf("ISP-A ping TIMEOUT (%ld us)\n", us);

	munmap(cmd, 4096);
	return ret;
}

static int test_dma(uint32_t syncpt_id)
{
	#define W 64
	#define H 64
	#define BPP 2
	#define IN_SIZE  (W * H * BPP)
	#define OUT_SIZE (W * H * BPP)

	/* Allocate buffers via nvmap */
	uint32_t cmdbuf_h = nvmap_create(4096);
	uint32_t in_h = nvmap_create(IN_SIZE);
	uint32_t out_h = nvmap_create(OUT_SIZE);
	if (!cmdbuf_h || !in_h || !out_h) return -1;

	nvmap_alloc(cmdbuf_h, 256);
	nvmap_alloc(in_h, 4096);
	nvmap_alloc(out_h, 4096);

	uint32_t *cmd = nvmap_mmap(cmdbuf_h, 4096);
	uint32_t *in = nvmap_mmap(in_h, IN_SIZE);
	uint32_t *out = nvmap_mmap(out_h, OUT_SIZE);
	if (!cmd || !in || !out) return -1;

	uint32_t in_phys = nvmap_pin(in_h);
	uint32_t out_phys = nvmap_pin(out_h);
	printf("in_phys=0x%08x out_phys=0x%08x\n", in_phys, out_phys);

	/* Fill input with pattern, clear output */
	for (int i = 0; i < IN_SIZE / 4; i++) in[i] = 0xA5A50000 | i;
	memset(out, 0, OUT_SIZE);

	uint32_t in_stride = W * BPP;
	uint32_t out_stride = W * BPP;

	/* Build command buffer using stock register values */
	int n = 0;

	/* Block 1 (0x60D8-like): format/size + enable */
	cmd[n++] = host1x_opcode_incr(0xE31, 1);
	cmd[n++] = W | (H << 16);
	cmd[n++] = host1x_opcode_incr(0xE33, 1);
	cmd[n++] = 0x20;
	cmd[n++] = host1x_opcode_incr(0xE32, 1);
	cmd[n++] = out_stride;
	cmd[n++] = host1x_opcode_incr(0x015, 1);
	cmd[n++] = 0x04040007;  /* from stock dump */
	cmd[n++] = host1x_opcode_incr(0xE30, 1);
	cmd[n++] = 0x00000001;

	/* Block 2 (0x31C0-like): processing + output dims + surfaces */
	cmd[n++] = host1x_opcode_incr(0x500, 6);
	cmd[n++] = 0x00000001;
	cmd[n++] = W;
	cmd[n++] = H;
	cmd[n++] = in_stride;
	cmd[n++] = 0x00000001;
	cmd[n++] = 0x00000000;

	cmd[n++] = host1x_opcode_incr(0xE00, 1);
	cmd[n++] = ((W - 1) & 0x3FFF) << 16;
	cmd[n++] = host1x_opcode_incr(0xE01, 1);
	cmd[n++] = ((H - 1) & 0x3FFF) << 16;
	cmd[n++] = host1x_opcode_incr(0xE02, 1);
	cmd[n++] = 0x20;
	cmd[n++] = host1x_opcode_incr(0xE03, 1);
	cmd[n++] = 0;

	/* Output surface */
	cmd[n++] = host1x_opcode_incr(0xE04, 3);
	cmd[n++] = out_phys;
	cmd[n++] = out_stride;
	cmd[n++] = ((H - 1) << 16) | (W - 1);

	/* Input surface */
	cmd[n++] = host1x_opcode_incr(0xE34, 3);
	cmd[n++] = in_phys;
	cmd[n++] = in_stride;
	cmd[n++] = ((H - 1) << 16) | (W - 1);

	/* Stock control register values */
	cmd[n++] = host1x_opcode_incr(0x008, 1);
	cmd[n++] = 0xF000F800;  /* from stock dump */
	cmd[n++] = host1x_opcode_incr(0x018, 1);
	cmd[n++] = 0x0A00500A;  /* from stock dump */
	cmd[n++] = host1x_opcode_incr(0x019, 1);
	cmd[n++] = 0x00008089;  /* from stock dump */

	/* Trigger */
	cmd[n++] = host1x_opcode_nonincr(0x00C, 1);
	cmd[n++] = 0x0000000F;

	/* Syncpt OP_DONE */
	cmd[n++] = host1x_opcode_imm_incr_syncpt(1 /* OP_DONE */, syncpt_id);
	cmd[n++] = NOOP;

	printf("cmdbuf: %d words\n", n);

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	uint32_t fence;
	if (submit(cmdbuf_h, n, syncpt_id, 1, &fence) < 0) return -1;
	int ret = syncpt_wait(syncpt_id, fence, 1000);

	clock_gettime(CLOCK_MONOTONIC, &t1);
	long us = (t1.tv_sec - t0.tv_sec) * 1000000L +
		  (t1.tv_nsec - t0.tv_nsec) / 1000;

	printf("submit+wait: %s (%ld us)\n", ret ? "TIMEOUT" : "OK", us);

	/* Check output */
	int nonzero = 0;
	for (int i = 0; i < OUT_SIZE / 4; i++) {
		if (out[i] != 0) {
			if (nonzero < 4)
				printf("  out[%d] = 0x%08x\n", i, out[i]);
			nonzero++;
		}
	}
	printf("output: %d/%d words non-zero%s\n",
	       nonzero, OUT_SIZE / 4,
	       nonzero ? " (ISP wrote!)" : " (untouched)");

	munmap(cmd, 4096);
	munmap(in, IN_SIZE);
	munmap(out, OUT_SIZE);
	return 0;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "ping";

	nvmap_fd = open("/dev/nvmap", O_RDWR);
	if (nvmap_fd < 0) { perror("open nvmap"); return 1; }

	isp_fd = open("/dev/nvhost-isp", O_RDWR);
	if (isp_fd < 0) { perror("open nvhost-isp"); return 1; }

	ctrl_fd = open("/dev/nvhost-ctrl", O_RDWR);
	if (ctrl_fd < 0) { perror("open nvhost-ctrl"); return 1; }

	/* Set nvmap fd on channel */
	struct nvhost_set_nvmap_fd_args nfa = { .fd = nvmap_fd };
	if (ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD, &nfa) < 0) {
		perror("set nvmap fd");
	}

	/* Get syncpoint — try GET_SYNCPOINT(param=0) first, fallback to GET_SYNCPOINTS */
	uint32_t syncpt_id = 0;
	struct nvhost_get_param_arg sp_arg = { .param = 0, .value = 0 };
	if (ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &sp_arg) == 0 && sp_arg.value) {
		syncpt_id = sp_arg.value;
	} else {
		/* Try client-managed syncpoint */
		char name[] = "isp_test";
		struct nvhost_get_client_managed_syncpt_arg cms = {
			.name = (uint64_t)(uintptr_t)name,
			.param = 0,
			.value = 0,
		};
		if (ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_CLIENT_MANAGED_SYNCPOINT, &cms) == 0) {
			syncpt_id = cms.value;
		} else {
			/* Last resort: GET_SYNCPOINTS */
			struct nvhost_get_param_args spa;
			if (ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINTS, &spa) == 0)
				syncpt_id = spa.value;
		}
	}
	printf("ISP syncpt = %u, current = %u\n",
	       syncpt_id, syncpt_read(syncpt_id));

	if (strcmp(mode, "ping") == 0) {
		return test_ping(syncpt_id);
	} else if (strcmp(mode, "dma") == 0) {
		return test_dma(syncpt_id);
	} else {
		printf("Usage: %s [ping|dma]\n", argv[0]);
		return 1;
	}
}
