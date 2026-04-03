/*
 * ISP hardware test for SmokeR24.1 kernel
 *
 * Adapted from isp_test.c (stock kernel test).
 * Changes for 24.1:
 * - nvmap mmap replaced with NVMAP_IOC_WRITE/READ (mmap not supported)
 * - SET_CLASS kept in gathers (gather filter tested separately)
 *
 * Usage: isp_test_24 [ping|dma|tests]
 *
 * Build: arm-linux-gnueabihf-gcc -std=gnu99 -static -o isp_test_24 isp_test_24.c
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
#define NVMAP_IOC_WRITE    _IOW(NVMAP_IOC_MAGIC, 6, struct nvmap_rw_handle)
#define NVMAP_IOC_READ     _IOW(NVMAP_IOC_MAGIC, 7, struct nvmap_rw_handle)

struct nvmap_rw_handle {
	unsigned long addr;	/* user buffer */
	uint32_t handle;
	uint32_t offset;
	uint32_t elem_size;
	uint32_t hmem_stride;
	uint32_t user_stride;
	uint32_t count;
} __attribute__((packed));

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

/*
 * nvmap_mmap is not available on SmokeR24.1.
 * Use nvmap_write/nvmap_read instead for data transfer.
 * For cmdbuf: build locally, then nvmap_write to handle.
 */
static int nvmap_write(uint32_t handle, uint32_t offset,
		       const void *data, uint32_t size)
{
	struct nvmap_rw_handle rw = {
		.addr = (unsigned long)data,
		.handle = handle,
		.offset = offset,
		.elem_size = size,
		.hmem_stride = size,
		.user_stride = size,
		.count = 1,
	};
	if (ioctl(nvmap_fd, NVMAP_IOC_WRITE, &rw) < 0) {
		perror("nvmap write");
		return -1;
	}
	return 0;
}

static int nvmap_read(uint32_t handle, uint32_t offset,
		      void *data, uint32_t size)
{
	struct nvmap_rw_handle rw = {
		.addr = (unsigned long)data,
		.handle = handle,
		.offset = offset,
		.elem_size = size,
		.hmem_stride = size,
		.user_stride = size,
		.count = 1,
	};
	if (ioctl(nvmap_fd, NVMAP_IOC_READ, &rw) < 0) {
		perror("nvmap read");
		return -1;
	}
	return 0;
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

static int submit_with_relocs(uint32_t cmdbuf_handle, uint32_t num_words,
		  uint32_t syncpt_id, uint32_t syncpt_incrs,
		  struct nvhost_reloc *relocs, struct nvhost_reloc_shift *reloc_shifts,
		  int num_relocs,
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
	sa.num_relocs = num_relocs;
	sa.num_waitchks = 0;
	sa.timeout = 1000;
	sa.syncpt_incrs = (uint32_t)(uintptr_t)&si;
	sa.cmdbufs = (uint32_t)(uintptr_t)&cb;
	sa.relocs = (uint32_t)(uintptr_t)relocs;
	sa.reloc_shifts = (uint32_t)(uintptr_t)reloc_shifts;
	sa.class_ids = (uint32_t)(uintptr_t)&class_id;
	sa.fences = (uint32_t)(uintptr_t)&fence;

	if (ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa) < 0) {
		perror("submit");
		return -1;
	}
	if (fence_out) *fence_out = fence.value;
	return 0;
}

static int submit(uint32_t cmdbuf_handle, uint32_t num_words,
		  uint32_t syncpt_id, uint32_t syncpt_incrs,
		  uint32_t *fence_out)
{
	return submit_with_relocs(cmdbuf_handle, num_words, syncpt_id,
				  syncpt_incrs, NULL, NULL, 0, fence_out);
}

/* ---- tests ---- */

static int test_ping(uint32_t syncpt_id)
{
	uint32_t cmdbuf_h = nvmap_create(4096);
	if (!cmdbuf_h) return -1;
	nvmap_alloc(cmdbuf_h, 256);

	uint32_t cmd[2];
	int n = 0;
	cmd[n++] = host1x_opcode_imm_incr_syncpt(0 /* immediate */, syncpt_id);
	cmd[n++] = NOOP;

	if (nvmap_write(cmdbuf_h, 0, cmd, n * 4) < 0) return -1;

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

	return ret;
}

static int test_dma(uint32_t syncpt_id, uint32_t trigger_val, uint32_t format_val, const char *tag)
{
	/* Detect ISP-B mode */
	int is_ispb = 0;
	FILE *marker = fopen("/data/local/tmp/.isp_b_mode", "r");
	if (marker) { is_ispb = 1; fclose(marker); }

	int W, H;
	if (is_ispb) {
		W = 2592; H = 1944;
	} else {
		W = 3280; H = 2460;
	}
	int Y_STRIDE = (W + 63) & ~63;
	int UV_STRIDE = Y_STRIDE / 2;
	int BPP = 2;
	int IN_SIZE = W * H * BPP;
	int Y_SIZE = Y_STRIDE * H;
	int UV_SIZE = UV_STRIDE * H / 2;
	int OUT_SIZE = Y_SIZE + UV_SIZE * 2;

	/* Allocate buffers via nvmap */
	uint32_t cmdbuf_h = nvmap_create(16384);  /* 4 pages for calibration + output */
	uint32_t in_h = nvmap_create(IN_SIZE);
	uint32_t out_h = nvmap_create(OUT_SIZE);
	if (!cmdbuf_h || !in_h || !out_h) return -1;

	nvmap_alloc(cmdbuf_h, 256);
	nvmap_alloc(in_h, 4096);
	nvmap_alloc(out_h, 4096);

	/* Build cmdbuf locally, then write to nvmap handle */
	uint32_t cmd[4096]; /* max 4096 words = 16KB */

	/* Fill input with pattern via nvmap_write */
	{
		uint32_t *in_tmp = malloc(IN_SIZE);
		if (in_tmp) {
			for (int i = 0; i < IN_SIZE / 4; i++)
				in_tmp[i] = 0xA5A50000 | (i & 0xFFFF);
			nvmap_write(in_h, 0, in_tmp, IN_SIZE);
			free(in_tmp);
			printf("input: filled with pattern via nvmap_write\n");
		} else {
			printf("input: malloc failed, using uninitialized\n");
		}
	}

	uint32_t in_phys = nvmap_pin(in_h);
	uint32_t out_phys = nvmap_pin(out_h);
	uint32_t out_y = out_phys;
	uint32_t out_u = out_phys + Y_SIZE;
	uint32_t out_v = out_phys + Y_SIZE + UV_SIZE;

	printf("in_phys=0x%08x out_phys=0x%08x (Y=+0 U=+0x%x V=+0x%x)\n",
	       in_phys, out_phys, Y_SIZE, Y_SIZE + UV_SIZE);
	printf("Y_STRIDE=%d UV_STRIDE=%d\n", Y_STRIDE, UV_STRIDE);

	/*
	 * Build command buffer matching stock sequence:
	 * 1. SET_CLASS(0x32) — required, works on stock kernel
	 * 2. Calibration block (1545 words from stock capture)
	 * 3. Output config + surfaces + input + trigger (45 words)
	 */
	int n = 0;

	/* Load calibration block from file */
	const char *cal_path = is_ispb ? "/data/local/tmp/isp_cal_b.bin"
	                               : "/data/local/tmp/isp_cal.bin";
	FILE *cal = fopen(cal_path, "rb");
	if (cal) {
		uint32_t cal_words;
		fread(&cal_words, 4, 1, cal);
		fseek(cal, 0, SEEK_SET);
		int nread = fread(&cmd[n], 4, 2048, cal);
		fclose(cal);
		printf("loaded calibration: %d words\n", nread);
		n += nread;
	} else {
		printf("no calibration file, using minimal config\n");
		/* Minimal: SET_CLASS + enable */
		cmd[n++] = host1x_opcode_setclass(ISP_CLASS_ID, 0, 0);
		cmd[n++] = host1x_opcode_incr(0x015, 1);
		cmd[n++] = 0x04040007;
	}

	/* Output config — exact stock values, adapted for 64x64 */
	cmd[n++] = host1x_opcode_setclass(ISP_CLASS_ID, 0, 0);

	/* 0x60D8-like block: E31/E33/E32/015/E30 */
	cmd[n++] = host1x_opcode_incr(0xE31, 1);
	cmd[n++] = W | (H << 16);         /* width | (height << 16) */
	cmd[n++] = host1x_opcode_incr(0xE33, 1);
	cmd[n++] = 0x04FE00E6;            /* same format as 0xE02 */
	cmd[n++] = host1x_opcode_incr(0xE32, 1);
	cmd[n++] = Y_STRIDE;              /* stride */
	cmd[n++] = host1x_opcode_incr(0x015, 1);
	cmd[n++] = 0x00000007;            /* enable mode */
	cmd[n++] = host1x_opcode_incr(0xE30, 1);
	cmd[n++] = 0x00000001;            /* output enable */

	/* 0x31C0-like block: Output dimensions */
	cmd[n++] = host1x_opcode_incr(0xE00, 1);
	cmd[n++] = ((W - 1) & 0x3FFF) << 16;
	cmd[n++] = host1x_opcode_incr(0xE01, 1);
	cmd[n++] = ((H - 1) & 0x3FFF) << 16;

	/* Output format */
	cmd[n++] = host1x_opcode_incr(0xE02, 1);
	cmd[n++] = format_val;
	cmd[n++] = host1x_opcode_incr(0xE03, 1);
	cmd[n++] = 0x00000000;

	/* Output surface Y */
	cmd[n++] = host1x_opcode_incr(0xE04, 3);
	cmd[n++] = out_y;
	cmd[n++] = 0x00000000;
	cmd[n++] = Y_STRIDE;

	/* Output surface U */
	cmd[n++] = host1x_opcode_incr(0xE07, 3);
	cmd[n++] = out_u;
	cmd[n++] = 0x00000000;
	cmd[n++] = UV_STRIDE;

	/* Output surface V */
	cmd[n++] = host1x_opcode_incr(0xE0A, 3);
	cmd[n++] = out_v;
	cmd[n++] = 0x00000000;
	cmd[n++] = UV_STRIDE;

	/* Processing — stock: 5 zeros + (h<<16)|w */
	cmd[n++] = host1x_opcode_incr(0x500, 6);
	cmd[n++] = 0;
	cmd[n++] = 0;
	cmd[n++] = 0;
	cmd[n++] = 0;
	cmd[n++] = 0;
	cmd[n++] = (H << 16) | W;

	/* Input buffer */
	cmd[n++] = host1x_opcode_setclass(ISP_CLASS_ID, 0, 0);
	cmd[n++] = host1x_opcode_incr(0x100, 4);
	cmd[n++] = in_phys;
	cmd[n++] = 0;
	cmd[n++] = 0;
	cmd[n++] = 0;

	/* Control trigger */
	cmd[n++] = host1x_opcode_setclass(ISP_CLASS_ID, 0, 0);
	cmd[n++] = host1x_opcode_nonincr(0x00C, 1);
	cmd[n++] = trigger_val;

	/* Syncpt OP_DONE */
	cmd[n++] = host1x_opcode_imm_incr_syncpt(1 /* OP_DONE */, syncpt_id);
	cmd[n++] = NOOP;

	printf("cmdbuf: %d words\n", n);

	/* Build relocs — tell kernel to pin buffers and patch addresses */
	#define MAX_RELOCS 8
	struct nvhost_reloc relocs[MAX_RELOCS];
	struct nvhost_reloc_shift reloc_shifts[MAX_RELOCS];
	int nr = 0;

	for (int i = 0; i < n; i++) {
		if (cmd[i] == out_y || cmd[i] == out_u || cmd[i] == out_v) {
			uint32_t target_off = 0;
			if (cmd[i] == out_u) target_off = Y_SIZE;
			else if (cmd[i] == out_v) target_off = Y_SIZE + UV_SIZE;

			relocs[nr].cmdbuf_mem = cmdbuf_h;
			relocs[nr].cmdbuf_offset = i * 4;
			relocs[nr].target = out_h;
			relocs[nr].target_offset = target_off;
			reloc_shifts[nr].shift = 0;
			nr++;
		}
		if (cmd[i] == in_phys) {
			relocs[nr].cmdbuf_mem = cmdbuf_h;
			relocs[nr].cmdbuf_offset = i * 4;
			relocs[nr].target = in_h;
			relocs[nr].target_offset = 0;
			reloc_shifts[nr].shift = 0;
			nr++;
		}
	}
	printf("  relocs: %d\n", nr);

	/* Write cmdbuf to nvmap handle */
	if (nvmap_write(cmdbuf_h, 0, cmd, n * 4) < 0) {
		printf("cmdbuf write failed\n");
		return -1;
	}

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	uint32_t fence;
	if (submit_with_relocs(cmdbuf_h, n, syncpt_id, 1,
			       relocs, reloc_shifts, nr, &fence) < 0)
		return -1;
	int ret = syncpt_wait(syncpt_id, fence, 1000);

	clock_gettime(CLOCK_MONOTONIC, &t1);
	long us = (t1.tv_sec - t0.tv_sec) * 1000000L +
		  (t1.tv_nsec - t0.tv_nsec) / 1000;

	printf("[%s] submit+wait: %s (%ld us)\n", tag, ret ? "TIMEOUT" : "OK", us);

	/* Invalidate output buffer cache to see ISP writes */
	/* Check if ISP wrote anything to output buffer */
	/* Use NVMAP_IOC_READ since mmap fails for large buffers */
	{
		/* Read first 4KB for quick check */
		uint8_t check_buf[4096];
		memset(check_buf, 0, sizeof(check_buf));
		struct {
			uint32_t addr;
			uint32_t handle;
			uint32_t offset;
			uint32_t elem_size;
			uint32_t hmem_stride;
			uint32_t user_stride;
			uint32_t count;
		} __attribute__((packed)) rw;
		rw.addr = (uint32_t)(uintptr_t)check_buf;
		rw.handle = out_h;
		rw.offset = 0;
		rw.elem_size = sizeof(check_buf);
		rw.hmem_stride = sizeof(check_buf);
		rw.user_stride = sizeof(check_buf);
		rw.count = 1;
		if (ioctl(nvmap_fd, _IOW('N', 7, rw), &rw) < 0) {
			perror("nvmap read");
		} else {
			int nonzero = 0;
			for (int i = 0; i < (int)sizeof(check_buf); i++) {
				if (check_buf[i] != 0) nonzero++;
			}
			printf("output: %d/4096 bytes non-zero%s\n",
			       nonzero,
			       nonzero ? " (ISP WROTE DATA!)" : " (untouched)");
			/* Show first 32 bytes as hex */
			printf("  hex: ");
			for (int i = 0; i < 32; i++)
				printf("%02x ", check_buf[i]);
			printf("\n");
		}

		/* Dump full Y plane to file for viewing */
		char fname[128];
		snprintf(fname, sizeof(fname), "/data/local/tmp/isp_%s.raw", tag);
		FILE *fp = fopen(fname, "wb");
		if (fp) {
			int chunk = 65536;
			uint8_t *buf = malloc(chunk);
			if (buf) {
				int off;
				for (off = 0; off < Y_SIZE; off += chunk) {
					int sz = (Y_SIZE - off < chunk) ? Y_SIZE - off : chunk;
					rw.addr = (uint32_t)(uintptr_t)buf;
					rw.handle = out_h;
					rw.offset = off;
					rw.elem_size = sz;
					rw.hmem_stride = sz;
					rw.user_stride = sz;
					rw.count = 1;
					if (ioctl(nvmap_fd, _IOW('N', 7, rw), &rw) < 0) break;
					fwrite(buf, 1, sz, fp);
				}
				free(buf);
				printf("Y plane dumped: %d bytes to /data/local/tmp/isp_output.raw\n", off);
			}
			fclose(fp);
		}
	}

	/* Stats readback: read from input buffer at offset 0x20000 */
	{
		uint8_t stats_buf[4096];
		memset(stats_buf, 0, sizeof(stats_buf));
		struct {
			uint32_t addr;
			uint32_t handle;
			uint32_t offset;
			uint32_t elem_size;
			uint32_t hmem_stride;
			uint32_t user_stride;
			uint32_t count;
		} __attribute__((packed)) rw;
		rw.addr = (uint32_t)(uintptr_t)stats_buf;
		rw.handle = in_h;
		rw.offset = 0x20000;  /* stats at +128KB */
		rw.elem_size = sizeof(stats_buf);
		rw.hmem_stride = sizeof(stats_buf);
		rw.user_stride = sizeof(stats_buf);
		rw.count = 1;
		if (ioctl(nvmap_fd, _IOW('N', 7, rw), &rw) < 0) {
			perror("nvmap read stats");
		} else {
			int nonzero = 0;
			for (int i = 0; i < (int)sizeof(stats_buf); i++)
				if (stats_buf[i] != 0) nonzero++;
			printf("\nstats region (in_buf+0x20000): %d/4096 bytes non-zero\n", nonzero);
			if (nonzero > 0) {
				/* Parse stats header */
				uint32_t *sw = (uint32_t *)stats_buf;
				printf("  header: 0x%08x 0x%08x 0x%08x 0x%08x\n",
				       sw[0], sw[1], sw[2], sw[3]);
				uint32_t type_word = sw[3]; /* offset 0x0C */
				uint32_t type = (type_word >> 24) & 0xFF;
				uint32_t count = type_word & 0x00FFFFFF;
				printf("  type_word=0x%08x -> type=%u, count=%u\n",
				       type_word, type, count);
				printf("  first data words: 0x%08x 0x%08x 0x%08x 0x%08x\n",
				       sw[4], sw[5], sw[6], sw[7]);
				printf("  hex[0..63]: ");
				for (int i = 0; i < 64; i++)
					printf("%02x ", stats_buf[i]);
				printf("\n");
			}

			/* Also dump stats to file */
			FILE *sf = fopen("/data/local/tmp/isp_stats.raw", "wb");
			if (sf) {
				/* Read full 128KB stats region */
				uint8_t *sbuf = malloc(0x20000);
				if (sbuf) {
					rw.addr = (uint32_t)(uintptr_t)sbuf;
					rw.offset = 0x20000;
					rw.elem_size = 0x20000;
					rw.hmem_stride = 0x20000;
					rw.user_stride = 0x20000;
					rw.count = 1;
					if (ioctl(nvmap_fd, _IOW('N', 7, rw), &rw) == 0) {
						fwrite(sbuf, 1, 0x20000, sf);
						printf("  stats dumped: 128KB to /data/local/tmp/isp_stats.raw\n");
					}
					free(sbuf);
				}
				fclose(sf);
			}
		}
	}

	return 0;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "ping";
	const char *extra = argc > 2 ? argv[2] : NULL;

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
		return test_dma(syncpt_id, 0x05, 0x04FE00E6, "stock_05");
	} else if (strcmp(mode, "tests") == 0) {
		printf("=== Test suite ===\n\n");

		printf("--- Test 1: Stock values (trigger=0x0F, format=0xE6) ---\n");
		test_dma(syncpt_id, 0x0F, 0x04FE00E6, "t1_stock");

		printf("\n--- Test 2: Trigger 0x05 (runtime) ---\n");
		test_dma(syncpt_id, 0x05, 0x04FE00E6, "t2_trig05");

		printf("\n--- Test 3: Trigger 0x09 ---\n");
		test_dma(syncpt_id, 0x09, 0x04FE00E6, "t3_trig09");

		printf("\n--- Test 4: Format 0x20 (minimal/default) ---\n");
		test_dma(syncpt_id, 0x0F, 0x00000020, "t4_fmt20");

		printf("\n--- Test 5: Format 0x22 ---\n");
		test_dma(syncpt_id, 0x0F, 0x00000022, "t5_fmt22");

		printf("\n--- Test 6: Format 0xCA ---\n");
		test_dma(syncpt_id, 0x0F, 0x000000CA, "t6_fmtCA");

		return 0;
	} else if (strcmp(mode, "dma_b") == 0) {
		/* ISP-B test: close ISP-A, open ISP-B */
		close(isp_fd);
		isp_fd = open("/dev/nvhost-isp.1", O_RDWR);
		if (isp_fd < 0) { perror("open nvhost-isp.1"); return 1; }
		struct nvhost_set_nvmap_fd_args nfa2 = { .fd = nvmap_fd };
		ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD, &nfa2);
		/* Get ISP-B syncpoint */
		struct nvhost_get_param_arg sp2 = { .param = 0, .value = 0 };
		if (ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &sp2) == 0 && sp2.value)
			syncpt_id = sp2.value;
		else {
			struct nvhost_get_param_args spa2;
			ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINTS, &spa2);
			syncpt_id = spa2.value;
		}
		printf("ISP-B syncpt = %u\n", syncpt_id);
		/* Create marker file so test_dma detects ISP-B */
		FILE *marker = fopen("/data/local/tmp/.isp_b_mode", "w");
		if (marker) fclose(marker);
		int ret = test_dma(syncpt_id, 0x05, 0x04FE00E6, "ispb_stock");
		remove("/data/local/tmp/.isp_b_mode");
		return ret;
	} else {
		printf("Usage: %s [ping|dma|tests]\n", argv[0]);
		return 1;
	}
}
