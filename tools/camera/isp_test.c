/*
 * isp_test.c — Userspace ISP init + submit test for Tegra K1 (T124)
 *
 * Replicates the EXACT same ISP init sequence (S1-S5) that the kernel
 * driver (isp_t124.c) does, but purely from userspace via nvhost/nvmap ioctls.
 * Then runs per-frame submit tests with various configurations.
 *
 * Build: arm-linux-gnueabihf-gcc -static -o isp_test isp_test.c
 * Run:   adb push isp_test /data/local/tmp/ && adb shell /data/local/tmp/isp_test
 *        Use "b" arg for ISP-B: /data/local/tmp/isp_test b
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

/* Calibration data for ISP-A and ISP-B */
#include "isp_test_cal.h"

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
struct nvhost_ctrl_syncpt_read_args {
	uint32_t id;
	uint32_t value;
};

#define NVHOST_IOCTL_CTRL_SYNCPT_WAITEX \
	_IOWR(NVHOST_IOC_MAGIC, 6, struct nvhost_ctrl_syncpt_waitex_args)
#define NVHOST_IOCTL_CTRL_SYNCPT_READ \
	_IOWR(NVHOST_IOC_MAGIC, 1, struct nvhost_ctrl_syncpt_read_args)

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
#define ISP_METHOD_CONTROL    0x00C
#define ISP_METHOD_ENABLE     0x015
#define ISP_METHOD_ISP_ENABLE 0x053
#define ISP_METHOD_STATS_BUF  0x100
#define ISP_METHOD_PROCESSING 0x500
#define ISP_METHOD_PROCESSING2 0x506
#define ISP_METHOD_RT_CONFIG  0x400
#define ISP_METHOD_RT_BUF_A   0x800
#define ISP_METHOD_RT_BUF_B   0x820
#define ISP_METHOD_RT_EXTRA   0xC00
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

/* Stock constants */
#define ISP_FORMAT_STOCK       0x04FE00E6
#define ISP_TRIGGER_RUNTIME    0x05
#define ISP_TRIGGER_POST_APPLY 0x0F

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

	/* Pin -> get IOVA */
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

static uint32_t nvhost_read_syncpt(int ctrl_fd, uint32_t id)
{
	struct nvhost_ctrl_syncpt_read_args ra;
	ra.id = id;
	ra.value = 0;
	ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_READ, &ra);
	return ra.value;
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

/* ================================================================
 * Zero-init block builder (matches isp_build_zero_init in isp_t124.c)
 * Produces ~1813 words of INCR/NONINCR opcodes zeroing all ISP regs.
 * ================================================================ */

static int build_zero_init(uint32_t *buf)
{
	int n = 0;

/* ZI(off, cnt): INCR opcode + cnt zero words */
#define ZI(off, cnt) do { \
	buf[n++] = NVHOST_OPCODE_INCR(off, cnt); \
	memset(&buf[n], 0, (cnt) * 4); n += (cnt); \
} while (0)
/* ZN(off, cnt): NONINCR opcode + cnt zero words */
#define ZN(off, cnt) do { \
	buf[n++] = NVHOST_OPCODE_NONINCR(off, cnt); \
	memset(&buf[n], 0, (cnt) * 4); n += (cnt); \
} while (0)

	ZI(0x202, 3); ZI(0x200, 2); ZI(0x205, 4);
	ZI(0x700, 16); ZI(0x750, 16);
	ZI(0xd00, 10); ZI(0xd0a, 1); ZN(0xd0b, 480);
	ZI(0xd0c, 2); ZI(0xd20, 6);
	ZI(0x900, 2); ZI(0x902, 1); ZN(0x903, 64);
	ZI(0x904, 2); ZI(0x906, 1); ZN(0x907, 36);
	ZI(0x908, 1); ZI(0x920, 10); ZI(0x909, 7);
	ZI(0x910, 9); ZI(0x919, 1); ZN(0x91a, 9);
	ZI(0x91b, 1); ZN(0x91c, 9);
	ZI(0x91d, 1); ZN(0x91e, 9);
	/* 0x91f = 0x00000002 (stock value, not zero) */
	buf[n++] = NVHOST_OPCODE_INCR(0x91f, 1);
	buf[n++] = 0x00000002;
	ZI(0x506, 9); ZI(0x600, 16); ZI(0x650, 1);
	ZI(0x651, 1); ZN(0x652, 257);
	ZI(0x653, 1); ZN(0x654, 257);
	ZI(0x655, 1); ZN(0x656, 257);
	ZI(0x657, 1); ZN(0x658, 257);
	ZI(0x300, 4); ZI(0x304, 4);
	/* Last: INCR(0x053, 2) = [0, work_buf_iova] — caller patches [n-1] */
	ZI(0x053, 2);

#undef ZI
#undef ZN
	return n; /* ~1813 */
}

/* Append zero_block + trigger(0x0F). Patches work_buf IOVA. Returns new n. */
static int append_zero_block(uint32_t *buf, int n, uint32_t work_iova)
{
	int zi = build_zero_init(&buf[n]);
	/* Patch last word of zero_init: 0x054 = work_buf IOVA */
	buf[n + zi - 1] = work_iova;
	n += zi;
	/* Append trigger POST_APPLY */
	buf[n++] = NVHOST_OPCODE_NONINCR(ISP_METHOD_CONTROL, 1);
	buf[n++] = ISP_TRIGGER_POST_APPLY;
	return n;
}

/* Append calibration data + trigger. Patches last cal word = work_buf IOVA. Returns new n. */
static int append_cal_block(uint32_t *buf, int n,
			    const uint32_t *cal_data, int cal_words,
			    uint32_t work_iova)
{
	memcpy(&buf[n], cal_data, cal_words * 4);
	n += cal_words;
	/* Last word of cal data is the IOVA for 0x054 — patch it */
	buf[n - 1] = work_iova;
	/* Append trigger */
	buf[n++] = NVHOST_OPCODE_NONINCR(ISP_METHOD_CONTROL, 1);
	buf[n++] = ISP_TRIGGER_POST_APPLY;
	return n;
}

/* Append IMMEDIATE syncpt incr + NOOP */
static int append_syncpt_imm(uint32_t *buf, int n, uint32_t syncpt)
{
	/* IMM opcode: offset=0x000, value = (cond=0 << 8) | syncpt */
	buf[n++] = NVHOST_OPCODE_IMM(0x000, (0 << 8) | (syncpt & 0xFF));
	buf[n++] = NVHOST_OPCODE_NOOP;
	return n;
}

/* Submit cmdbuf and wait for syncpt. Returns 0 on success. */
static int submit_and_wait(int ch_fd, int ctrl_fd,
			   struct nvbuf *cmdbuf, int words,
			   uint32_t class_id, uint32_t syncpt,
			   const char *name)
{
	uint32_t fence;
	struct nvhost_syncpt_incr sp = { syncpt, 1 };

	printf("  %s: %d words ... ", name, words);
	fflush(stdout);

	if (nvhost_submit(ch_fd, cmdbuf, words, class_id, &sp, 1, NULL, 0, NULL, &fence))
		return -1;

	int ret = nvhost_wait_syncpt(ctrl_fd, syncpt, fence, 2000);
	if (ret)
		printf("  %s: TIMEOUT!\n", name);
	else
		printf("  %s: OK\n", name);
	return ret;
}

/* ================================================================
 * S5 Runtime Config builder
 * Matches isp_t124.c lines 504-709 exactly.
 * ================================================================ */

static int build_s5_runtime(uint32_t *cmd, int is_b, uint32_t work_iova)
{
	int n = 0;
	uint32_t class_id = is_b ? ISP_B_CLASS : ISP_A_CLASS;

	/* 0x400: runtime config (12 words) */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_RT_CONFIG, 12);
	cmd[n++] = 0x00000001;
	cmd[n++] = 0x004b0000;
	cmd[n++] = 0x00930000;
	cmd[n++] = 0x00220000;
	cmd[n++] = 0x2ff01000;
	cmd[n++] = 0x2ff01000;
	cmd[n++] = 0x2ff01000;
	cmd[n++] = 0x2ff01000;
	cmd[n++] = 0x00030000;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00020000;
	cmd[n++] = 0x00000000;

	/* 0x800: stats buffer A */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_RT_BUF_A, 3);
	cmd[n++] = work_iova;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;

	/* 0x820: stats buffer B */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_RT_BUF_B, 3);
	cmd[n++] = work_iova;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;

	/* 0x930: histogram config (18 words) */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	cmd[n++] = NVHOST_OPCODE_INCR(0x930, 18);
	cmd[n++] = 0x0000001c; cmd[n++] = 0x88888888;
	cmd[n++] = 0x78787800; cmd[n++] = 0x00000078;
	cmd[n++] = 0x88888888; cmd[n++] = 0x78787800;
	cmd[n++] = 0x00000078; cmd[n++] = 0x88888888;
	cmd[n++] = 0x78787800; cmd[n++] = 0x00000078;
	cmd[n++] = 0x88888888; cmd[n++] = 0x78787800;
	cmd[n++] = 0x00000078; cmd[n++] = 0x3fc00000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00070000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00070000;

	/* 0xC00: extra config */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_RT_EXTRA, 3);
	cmd[n++] = 0x00000101;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00100000;

	/* 0x202: input config -- sensor-specific dims */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	cmd[n++] = NVHOST_OPCODE_INCR(0x202, 3);
	cmd[n++] = 0x00000001;
	cmd[n++] = is_b ? 0x00780078 : 0x02000200; /* 0x203 */
	cmd[n++] = is_b ? 0x00780078 : 0x02000200; /* 0x204 */

	/* 0x200: input enable */
	cmd[n++] = NVHOST_OPCODE_INCR(0x200, 2);
	cmd[n++] = 0x00000001;
	cmd[n++] = 0x00000000;

	/* 0x205: input stride/format config */
	cmd[n++] = NVHOST_OPCODE_INCR(0x205, 4);
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x000600c8;
	cmd[n++] = 0x000f000f;
	cmd[n++] = is_b ? 0x00000000 : 0x00003333; /* 0x208 */

	/* 0x700: processing channel A (16 words) -- sensor-specific strides */
	cmd[n++] = NVHOST_OPCODE_INCR(0x700, 16);
	cmd[n++] = 0x00000001; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;
	cmd[n++] = is_b ? 0x00001a40 : 0x00001dc0; /* 0x705 */
	cmd[n++] = 0x00000000; cmd[n++] = 0x10000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00001000;
	cmd[n++] = is_b ? 0x00001a00 : 0x00001c50; /* 0x70b */
	cmd[n++] = 0x30001000; cmd[n++] = 0x30001000;
	cmd[n++] = 0x30001000; cmd[n++] = 0x30001000;

	/* 0x750: processing channel B (16 words) */
	cmd[n++] = NVHOST_OPCODE_INCR(0x750, 16);
	cmd[n++] = 0x00000003; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x30001000; cmd[n++] = 0x30001000;
	cmd[n++] = 0x30001000; cmd[n++] = 0x30001000;

	/* 0xd20: lens shading extra -- sensor-specific */
	cmd[n++] = NVHOST_OPCODE_INCR(0xd20, 6);
	cmd[n++] = is_b ? 0x00001101 : 0x00003101; /* 0xd20 */
	cmd[n++] = 0x00000000;
	cmd[n++] = is_b ? 0x00210000 : 0x01ec0000; /* 0xd22 */
	cmd[n++] = is_b ? 0x00210000 : 0x01ec0000; /* 0xd23 */
	cmd[n++] = is_b ? 0x00210000 : 0x01ec0000; /* 0xd24 */
	cmd[n++] = is_b ? 0x00210000 : 0x01ec0000; /* 0xd25 */

	/* 0x900: stats enable */
	cmd[n++] = NVHOST_OPCODE_INCR(0x900, 2);
	cmd[n++] = 0x00000001;
	cmd[n++] = 0x00000001;

	/* 0x904/0x908: stats config */
	cmd[n++] = NVHOST_OPCODE_INCR(0x904, 2);
	cmd[n++] = 0x00005555;
	cmd[n++] = 0x00000001;
	cmd[n++] = NVHOST_OPCODE_INCR(0x908, 1);
	cmd[n++] = 0x00005555;

	/* 0x920: stats window (10 words) */
	cmd[n++] = NVHOST_OPCODE_INCR(0x920, 10);
	cmd[n++] = 0x00000002; cmd[n++] = 0x10001660;
	cmd[n++] = 0x00000000; cmd[n++] = 0x1000f4a0;
	cmd[n++] = 0x0000fa80; cmd[n++] = 0x10000000;
	cmd[n++] = 0x00001c50; cmd[n++] = 0x30001000;
	cmd[n++] = 0x30001000; cmd[n++] = 0x30001000;

	/* 0x909: stats config (7 words) -- sensor-specific */
	cmd[n++] = NVHOST_OPCODE_INCR(0x909, 7);
	cmd[n++] = 0x00000001; cmd[n++] = 0xfc000f00;
	cmd[n++] = 0xf680f320; cmd[n++] = 0x0d80fde0;
	cmd[n++] = is_b ? 0x00000030 : 0x00000000; /* 0x90d */
	cmd[n++] = 0x1400002a;
	cmd[n++] = 0x3c00002b;

	/* 0x910: stats config (9 words) -- sensor-specific */
	cmd[n++] = NVHOST_OPCODE_INCR(0x910, 9);
	cmd[n++] = 0x00000003; cmd[n++] = 0x00000028;
	cmd[n++] = 0x01480029;
	cmd[n++] = is_b ? 0x0003030b : 0x00177e0b; /* 0x913 */
	cmd[n++] = 0x00990030; cmd[n++] = 0x00000800;
	cmd[n++] = 0x007b0666;
	cmd[n++] = is_b ? 0x00000036 : 0x00000039; /* 0x917 */
	cmd[n++] = is_b ? 0x00001f1f : 0x00000000; /* 0x918 */

	/* 0x91b */
	cmd[n++] = NVHOST_OPCODE_INCR(0x91b, 1);
	cmd[n++] = 0x00000000;

	/* 0x91c: NONINCR 9 words -- sensor-specific */
	cmd[n++] = NVHOST_OPCODE_NONINCR(0x91c, 9);
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000001;
	cmd[n++] = is_b ? 0x00000025 : 0x00000026; /* word 5 */
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000026;
	cmd[n++] = 0x00000361;

	/* 0x91d */
	cmd[n++] = NVHOST_OPCODE_INCR(0x91d, 1);
	cmd[n++] = 0x00000000;

	/* 0x91e: NONINCR 9 words */
	cmd[n++] = NVHOST_OPCODE_NONINCR(0x91e, 9);
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000780;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000780;
	cmd[n++] = 0x00000200;

	/* 0x91f */
	cmd[n++] = NVHOST_OPCODE_INCR(0x91f, 1);
	cmd[n++] = 0x00000032;

	/* 0x506: demosaic processing (9 words) -- same both ISPs */
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_PROCESSING2, 9);
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
	cmd[n++] = NVHOST_OPCODE_INCR(0x600, 16);
	cmd[n++] = 0x00000005; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
	cmd[n++] = 0x3fff0000; cmd[n++] = 0x3fff0000;
	cmd[n++] = 0x3fff0000; cmd[n++] = 0x10001000;

	/* 0x650: tone curve enable */
	cmd[n++] = NVHOST_OPCODE_INCR(0x650, 1);
	cmd[n++] = 0x00000003;

	/* 0x651 */
	cmd[n++] = NVHOST_OPCODE_INCR(0x651, 1);
	cmd[n++] = 0x00000000;

	return n;
}

/* ================================================================
 * ISP Init Sequence (S1-S5) — matches isp_t124_stream_init() exactly
 * ================================================================ */

static int isp_init(int ch_fd, int ctrl_fd, struct nvbuf *cmdbuf,
		    struct nvbuf *workbuf, uint32_t class_id, uint32_t syncpt,
		    int is_b)
{
	uint32_t *cmd = cmdbuf->cpu;
	int n, ret;
	const uint32_t *cal_data;
	int cal_words;

	if (is_b) {
		cal_data = isp_b_cal_data;
		cal_words = sizeof(isp_b_cal_data) / sizeof(isp_b_cal_data[0]);
	} else {
		cal_data = isp_a_cal_data;
		cal_words = sizeof(isp_a_cal_data) / sizeof(isp_a_cal_data[0]);
	}

	printf("\n========================================\n");
	printf("ISP INIT (class=0x%02x, %s)\n", class_id, is_b ? "ISP-B" : "ISP-A");
	printf("  work_iova=0x%08x cal_words=%d\n", workbuf->iova, cal_words);
	printf("========================================\n");

	/* --- S1: zero_block x2 + 0x018 tails + syncpt --- */
	printf("\n--- S1: zero_block x2 + 0x018 tails ---\n");
	n = 0;
	/* First zero_block */
	n = append_zero_block(cmd, n, workbuf->iova);
	/* First 0x018 tail (5 words) */
	cmd[n++] = NVHOST_OPCODE_INCR(0x018, 5);
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000400;
	cmd[n++] = 0x00000000; cmd[n++] = 0x00000200;
	cmd[n++] = 0x00000002;
	/* Second zero_block */
	n = append_zero_block(cmd, n, workbuf->iova);
	/* Second 0x018 tail (5 words) */
	cmd[n++] = NVHOST_OPCODE_INCR(0x018, 5);
	cmd[n++] = 0x0a00500a; cmd[n++] = 0x00008089;
	cmd[n++] = 0x013645cb; cmd[n++] = 0x000001e7;
	cmd[n++] = 0x00000001;
	/* IMMEDIATE syncpt */
	n = append_syncpt_imm(cmd, n, syncpt);
	printf("  S1: %d words\n", n);

	ret = submit_and_wait(ch_fd, ctrl_fd, cmdbuf, n, class_id, syncpt, "S1-init");
	if (ret) return ret;

	/* --- S2: zero_block + trigger + syncpt --- */
	printf("\n--- S2: zero_block + trigger ---\n");
	n = 0;
	n = append_zero_block(cmd, n, workbuf->iova);
	n = append_syncpt_imm(cmd, n, syncpt);
	printf("  S2: %d words\n", n);

	ret = submit_and_wait(ch_fd, ctrl_fd, cmdbuf, n, class_id, syncpt, "S2-cal");
	if (ret) return ret;

	/* --- S3: SET_CLASS + OP_DONE syncpt --- */
	printf("\n--- S3: SET_CLASS + OP_DONE ---\n");
	n = 0;
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	/* OP_DONE syncpt: IMM opcode, offset=0x000, value = (cond=1 << 8) | syncpt */
	cmd[n++] = NVHOST_OPCODE_IMM(0x000, (1 << 8) | (syncpt & 0xFF));
	cmd[n++] = NVHOST_OPCODE_NOOP;
	printf("  S3: %d words\n", n);

	ret = submit_and_wait(ch_fd, ctrl_fd, cmdbuf, n, class_id, syncpt, "S3-class");
	if (ret) return ret;

	/* --- S4: zero_block + trigger + syncpt --- */
	printf("\n--- S4: zero_block + trigger ---\n");
	n = 0;
	n = append_zero_block(cmd, n, workbuf->iova);
	n = append_syncpt_imm(cmd, n, syncpt);
	printf("  S4: %d words\n", n);

	ret = submit_and_wait(ch_fd, ctrl_fd, cmdbuf, n, class_id, syncpt, "S4-cal");
	if (ret) return ret;

	/* --- S5: full runtime config + calibration + trigger + syncpt --- */
	printf("\n--- S5: runtime config + calibration ---\n");
	n = 0;
	n += build_s5_runtime(&cmd[n], is_b, workbuf->iova);
	/* Append real calibration + trigger */
	n = append_cal_block(cmd, n, cal_data, cal_words, workbuf->iova);
	/* IMMEDIATE syncpt */
	n = append_syncpt_imm(cmd, n, syncpt);
	printf("  S5: %d words\n", n);

	ret = submit_and_wait(ch_fd, ctrl_fd, cmdbuf, n, class_id, syncpt, "S5-rtcfg");
	if (ret) return ret;

	printf("\n*** ISP INIT COMPLETE ***\n\n");
	return 0;
}

/* ================================================================
 * Per-frame submit helpers
 * ================================================================ */

static void check_output(struct nvbuf *outbuf)
{
	uint32_t *out32 = outbuf->cpu;
	int changed = 0;
	int i;
	printf("  Output first 8 words: ");
	for (i = 0; i < 8; i++)
		printf("0x%08x ", out32[i]);
	printf("\n");
	/* Check if any word changed from 0xDE fill */
	for (i = 0; i < 64; i++) {
		if (out32[i] != 0xDEDEDEDE) {
			changed = 1;
			break;
		}
	}
	if (changed)
		printf("  OUTPUT MODIFIED -- ISP PROCESSED!\n");
	else
		printf("  OUTPUT UNCHANGED (still 0xDE fill)\n");
}

/*
 * Test A: enable=0x03 + trigger=0x05 + cond=4 syncpt (no surfaces)
 * Known working combo from previous testing.
 */
static int test_A_nosurface(int ch_fd, int ctrl_fd, uint32_t syncpt,
			    uint32_t class_id, struct nvbuf *cmdbuf,
			    struct nvbuf *workbuf)
{
	uint32_t *cmd = cmdbuf->cpu;
	int n = 0;
	uint32_t fence;

	printf("\n=== TEST A: enable=0x03 trigger=0x05 cond4 (no surfaces) ===\n");

	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);

	/* ISP_ENABLE */
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_ENABLE, 1);
	cmd[n++] = 0x00000003;

	/* Stats buffer */
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_STATS_BUF, 4);
	cmd[n++] = workbuf->iova;
	cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0;

	/* cond=4 syncpt incr */
	cmd[n++] = NVHOST_OPCODE_NONINCR(0x000, 1);
	cmd[n++] = (4 << 8) | (syncpt & 0xFF);

	/* Trigger */
	cmd[n++] = NVHOST_OPCODE_NONINCR(ISP_METHOD_CONTROL, 1);
	cmd[n++] = ISP_TRIGGER_RUNTIME;

	/* Safety IMMEDIATE */
	cmd[n++] = NVHOST_OPCODE_IMM(0x000, (0 << 8) | (syncpt & 0xFF));
	cmd[n++] = NVHOST_OPCODE_NOOP;

	struct nvhost_syncpt_incr sp = { syncpt, 2 };
	if (nvhost_submit(ch_fd, cmdbuf, n, class_id, &sp, 1, NULL, 0, NULL, &fence))
		return -1;

	int ret = nvhost_wait_syncpt(ctrl_fd, syncpt, fence - 1, 500);
	if (ret) {
		printf("  cond=4 DID NOT fire\n");
		nvhost_wait_syncpt(ctrl_fd, syncpt, fence, 2000);
	} else {
		printf("  cond=4 FIRED!\n");
	}
	return ret;
}

/*
 * Test B: enable=0x03, trigger=0x05 WITH output+input surfaces + processing + stats
 */
static int test_B_surfaces(int ch_fd, int ctrl_fd, uint32_t syncpt,
			   uint32_t class_id, struct nvbuf *cmdbuf,
			   struct nvbuf *inbuf, struct nvbuf *outbuf,
			   struct nvbuf *workbuf, uint32_t W, uint32_t H)
{
	uint32_t *cmd = cmdbuf->cpu;
	int n = 0;
	uint32_t fence;
	uint32_t in_stride = W * 2;
	uint32_t y_stride = (W + 63) & ~63;
	uint32_t uv_stride = ((W / 2) + 63) & ~63;
	uint32_t y_size = y_stride * H;
	uint32_t uv_size = uv_stride * (H / 2);

	printf("\n=== TEST B: enable=0x03 trigger=0x05 WITH surfaces %ux%u ===\n", W, H);
	printf("  in_iova=0x%08x out_iova=0x%08x work_iova=0x%08x\n",
	       inbuf->iova, outbuf->iova, workbuf->iova);

	memset(inbuf->cpu, 0x42, inbuf->size);
	memset(outbuf->cpu, 0xDE, outbuf->size);

	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);

	/* Output config */
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_WIDTH, 1);
	cmd[n++] = ((W - 1) & 0x3FFF) << 16;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_HEIGHT, 1);
	cmd[n++] = ((H - 1) & 0x3FFF) << 16;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_FORMAT, 1);
	cmd[n++] = ISP_FORMAT_STOCK;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_COLOR, 1);
	cmd[n++] = 0x00000000;

	/* Output surfaces Y/U/V */
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
	cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0;
	cmd[n++] = 0; cmd[n++] = 0;
	cmd[n++] = (H << 16) | W;

	/* Input (v3 reprocess methods) */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_IN_DIMS, 1);
	cmd[n++] = (W & 0x7FFF) | (H << 16);
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_IN_FORMAT, 1);
	cmd[n++] = 0x11000020; /* RAW Bayer single-plane linear */
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_IN_SURF0, 3);
	cmd[n++] = inbuf->iova;
	cmd[n++] = 0;
	cmd[n++] = in_stride;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_IN_STRIP, 1);
	cmd[n++] = W & 0x3FFF;

	/* ISP_ENABLE */
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_ENABLE, 1);
	cmd[n++] = 0x00000003;

	/* Input trigger */
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_IN_TRIGGER, 1);
	cmd[n++] = 1;

	/* Stats buffer */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_STATS_BUF, 4);
	cmd[n++] = workbuf->iova;
	cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0;

	/* cond=4 syncpt */
	cmd[n++] = NVHOST_OPCODE_NONINCR(0x000, 1);
	cmd[n++] = (4 << 8) | (syncpt & 0xFF);

	/* Trigger */
	cmd[n++] = NVHOST_OPCODE_NONINCR(ISP_METHOD_CONTROL, 1);
	cmd[n++] = ISP_TRIGGER_RUNTIME;

	/* Safety IMMEDIATE */
	cmd[n++] = NVHOST_OPCODE_IMM(0x000, (0 << 8) | (syncpt & 0xFF));
	cmd[n++] = NVHOST_OPCODE_NOOP;

	printf("  cmdbuf: %d words\n", n);

	struct nvhost_syncpt_incr sp = { syncpt, 2 };
	if (nvhost_submit(ch_fd, cmdbuf, n, class_id, &sp, 1, NULL, 0, NULL, &fence))
		return -1;

	int ret = nvhost_wait_syncpt(ctrl_fd, syncpt, fence - 1, 2000);
	if (ret) {
		printf("  cond=4 DID NOT fire\n");
		nvhost_wait_syncpt(ctrl_fd, syncpt, fence, 5000);
	} else {
		printf("  cond=4 FIRED!\n");
		check_output(outbuf);
	}
	return ret;
}

/*
 * Test C: enable=0x07, trigger=0x0F WITH surfaces (full pipeline)
 */
static int test_C_full(int ch_fd, int ctrl_fd, uint32_t syncpt,
		       uint32_t class_id, struct nvbuf *cmdbuf,
		       struct nvbuf *inbuf, struct nvbuf *outbuf,
		       struct nvbuf *workbuf, uint32_t W, uint32_t H)
{
	uint32_t *cmd = cmdbuf->cpu;
	int n = 0;
	uint32_t fence;
	uint32_t in_stride = W * 2;
	uint32_t y_stride = (W + 63) & ~63;
	uint32_t uv_stride = ((W / 2) + 63) & ~63;
	uint32_t y_size = y_stride * H;
	uint32_t uv_size = uv_stride * (H / 2);

	printf("\n=== TEST C: enable=0x07 trigger=0x0F WITH surfaces %ux%u ===\n", W, H);
	printf("  in_iova=0x%08x out_iova=0x%08x work_iova=0x%08x\n",
	       inbuf->iova, outbuf->iova, workbuf->iova);

	memset(inbuf->cpu, 0x42, inbuf->size);
	memset(outbuf->cpu, 0xDE, outbuf->size);

	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);

	/* Output config */
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_WIDTH, 1);
	cmd[n++] = ((W - 1) & 0x3FFF) << 16;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_HEIGHT, 1);
	cmd[n++] = ((H - 1) & 0x3FFF) << 16;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_FORMAT, 1);
	cmd[n++] = ISP_FORMAT_STOCK;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_COLOR, 1);
	cmd[n++] = 0x00000000;

	/* Output surfaces */
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

	/* Processing */
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_PROCESSING, 6);
	cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0;
	cmd[n++] = 0; cmd[n++] = 0;
	cmd[n++] = (H << 16) | W;

	/* Input */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_IN_DIMS, 1);
	cmd[n++] = (W & 0x7FFF) | (H << 16);
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_IN_FORMAT, 1);
	cmd[n++] = 0x11000020;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_IN_SURF0, 3);
	cmd[n++] = inbuf->iova;
	cmd[n++] = 0;
	cmd[n++] = in_stride;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_IN_STRIP, 1);
	cmd[n++] = W & 0x3FFF;

	/* ISP_ENABLE = 0x07 (full pipeline) */
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_ENABLE, 1);
	cmd[n++] = 0x00000007;

	/* Input trigger */
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_IN_TRIGGER, 1);
	cmd[n++] = 1;

	/* Stats buffer */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_STATS_BUF, 4);
	cmd[n++] = workbuf->iova;
	cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0;

	/* 3 conditional syncpt incrs: cond 4, 5, 6 */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	cmd[n++] = NVHOST_OPCODE_NONINCR(0x000, 1);
	cmd[n++] = (4 << 8) | (syncpt & 0xFF);
	cmd[n++] = NVHOST_OPCODE_NONINCR(0x000, 1);
	cmd[n++] = (5 << 8) | (syncpt & 0xFF);
	cmd[n++] = NVHOST_OPCODE_NONINCR(0x000, 1);
	cmd[n++] = (6 << 8) | (syncpt & 0xFF);

	/* Trigger POST_APPLY */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	cmd[n++] = NVHOST_OPCODE_NONINCR(ISP_METHOD_CONTROL, 1);
	cmd[n++] = ISP_TRIGGER_POST_APPLY;

	/* Safety IMMEDIATE */
	cmd[n++] = NVHOST_OPCODE_IMM(0x000, (0 << 8) | (syncpt & 0xFF));
	cmd[n++] = NVHOST_OPCODE_NOOP;

	printf("  cmdbuf: %d words\n", n);

	struct nvhost_syncpt_incr sp = { syncpt, 4 };
	if (nvhost_submit(ch_fd, cmdbuf, n, class_id, &sp, 1, NULL, 0, NULL, &fence))
		return -1;

	/* Wait for cond syncpts (fence-1 = after 3 conditional, before immediate) */
	int ret = nvhost_wait_syncpt(ctrl_fd, syncpt, fence - 1, 2000);
	if (ret) {
		printf("  cond syncpts DID NOT fire\n");
		nvhost_wait_syncpt(ctrl_fd, syncpt, fence, 5000);
	} else {
		printf("  cond syncpts FIRED!\n");
		check_output(outbuf);
	}
	return ret;
}

/*
 * Test D: Stock-like per-frame with 4 separate syncpts (memory/stats/loadv/stream)
 * Matches isp_t124_process_frame() exactly.
 * Uses all 4 syncpt params: cond4(param0), cond5(param1), cond6(param3), imm(param2)
 */
static int test_D_stock(int ch_fd, int ctrl_fd, struct nvbuf *cmdbuf,
			struct nvbuf *inbuf, struct nvbuf *outbuf,
			struct nvbuf *workbuf, uint32_t W, uint32_t H,
			uint32_t class_id,
			uint32_t sp_memory, uint32_t sp_stats,
			uint32_t sp_stream, uint32_t sp_loadv)
{
	uint32_t *cmd = cmdbuf->cpu;
	int n = 0;
	uint32_t fence;
	uint32_t in_stride = W * 2;
	uint32_t y_stride = (W + 63) & ~63;
	uint32_t uv_stride = ((W / 2) + 63) & ~63;
	uint32_t y_size = y_stride * H;
	uint32_t uv_size = uv_stride * (H / 2);

	printf("\n=== TEST D: Stock per-frame %ux%u (4 syncpts) ===\n", W, H);
	printf("  syncpts: mem=%u stats=%u stream=%u loadv=%u\n",
	       sp_memory, sp_stats, sp_stream, sp_loadv);
	printf("  in_iova=0x%08x out_iova=0x%08x work_iova=0x%08x\n",
	       inbuf->iova, outbuf->iova, workbuf->iova);

	memset(inbuf->cpu, 0x42, inbuf->size);
	memset(outbuf->cpu, 0xDE, outbuf->size);

	/* --- G[0]: full per-frame gather --- */

	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);

	/* Output width/height/format/color */
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_WIDTH, 1);
	cmd[n++] = ((W - 1) & 0x3FFF) << 16;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_HEIGHT, 1);
	cmd[n++] = ((H - 1) & 0x3FFF) << 16;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_FORMAT, 1);
	cmd[n++] = ISP_FORMAT_STOCK;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_COLOR, 1);
	cmd[n++] = 0x00000000;

	/* Output Y/U/V surfaces */
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_SURF_Y, 3);
	cmd[n++] = outbuf->iova;
	cmd[n++] = 0x00000000;
	cmd[n++] = y_stride;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_SURF_U, 3);
	cmd[n++] = outbuf->iova + y_size;
	cmd[n++] = 0x00000000;
	cmd[n++] = uv_stride;
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_OUT_SURF_V, 3);
	cmd[n++] = outbuf->iova + y_size + uv_size;
	cmd[n++] = 0x00000000;
	cmd[n++] = uv_stride;

	/* Processing */
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_PROCESSING, 6);
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;
	cmd[n++] = (H << 16) | W;

	/* Input (v3 reprocess methods) */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);

	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_IN_DIMS, 1);
	cmd[n++] = (W & 0x7FFF) | (H << 16);

	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_IN_FORMAT, 1);
	cmd[n++] = 0x11000020; /* RAW Bayer */

	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_IN_SURF0, 3);
	cmd[n++] = inbuf->iova;
	cmd[n++] = 0x00000000;
	cmd[n++] = in_stride;

	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_IN_STRIP, 1);
	cmd[n++] = W & 0x3FFF;

	/* ISP_ENABLE = 0x07 */
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_ENABLE, 1);
	cmd[n++] = 0x00000007;

	/* Input trigger = 1 */
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_IN_TRIGGER, 1);
	cmd[n++] = 0x00000001;

	/* Stats buffer */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	cmd[n++] = NVHOST_OPCODE_INCR(ISP_METHOD_STATS_BUF, 4);
	cmd[n++] = workbuf->iova;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;
	cmd[n++] = 0x00000000;

	/* 3 conditional syncpt incrs (memory cond4, stats cond5, loadv cond6) */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	cmd[n++] = NVHOST_OPCODE_NONINCR(0x000, 1);
	cmd[n++] = (4 << 8) | (sp_memory & 0xFF);
	cmd[n++] = NVHOST_OPCODE_NONINCR(0x000, 1);
	cmd[n++] = (5 << 8) | (sp_stats & 0xFF);
	cmd[n++] = NVHOST_OPCODE_NONINCR(0x000, 1);
	cmd[n++] = (6 << 8) | (sp_loadv & 0xFF);

	/* Trigger POST_APPLY (0x0F) */
	cmd[n++] = NVHOST_OPCODE_SETCLASS(class_id, 0, 0);
	cmd[n++] = NVHOST_OPCODE_NONINCR(ISP_METHOD_CONTROL, 1);
	cmd[n++] = ISP_TRIGGER_POST_APPLY;

	int g0_words = n;

	/* --- G[1]: immediate syncpt incr for stream --- */
	/* (In kernel we use separate gathers; here we just append since
	 * userspace submit uses a single cmdbuf anyway) */
	cmd[n++] = NVHOST_OPCODE_NONINCR(0x000, 1);
	cmd[n++] = sp_stream & 0xFF; /* cond=0 (immediate) */

	printf("  cmdbuf: G[0]=%d words + G[1]=2 words = %d total\n", g0_words, n);

	/* Read current syncpt values BEFORE submit */
	uint32_t mem_before = nvhost_read_syncpt(ctrl_fd, sp_memory);
	uint32_t stats_before = nvhost_read_syncpt(ctrl_fd, sp_stats);
	uint32_t loadv_before = nvhost_read_syncpt(ctrl_fd, sp_loadv);
	uint32_t stream_before = nvhost_read_syncpt(ctrl_fd, sp_stream);
	printf("  syncpt values before: mem=%u stats=%u stream=%u loadv=%u\n",
	       mem_before, stats_before, stream_before, loadv_before);

	/* Submit with 4 syncpt incrs (memory, stats, loadv, stream) */
	struct nvhost_syncpt_incr sps[4] = {
		{ sp_memory, 1 },
		{ sp_stats, 1 },
		{ sp_loadv, 1 },
		{ sp_stream, 1 },
	};

	if (nvhost_submit(ch_fd, cmdbuf, n, class_id, sps, 4, NULL, 0, NULL, &fence))
		return -1;

	/* Wait on stream syncpt (the immediate one — should always work) */
	printf("  Waiting for stream syncpt (immediate) ...\n");
	int ret = nvhost_wait_syncpt(ctrl_fd, sp_stream, stream_before + 1, 2000);
	if (ret) {
		printf("  Stream syncpt TIMEOUT!\n");
		return -1;
	}

	/* Now check if conditional syncpts fired */
	printf("  Checking conditional syncpts ...\n");
	int mem_ok = (nvhost_wait_syncpt(ctrl_fd, sp_memory, mem_before + 1, 500) == 0);
	int stats_ok = (nvhost_wait_syncpt(ctrl_fd, sp_stats, stats_before + 1, 500) == 0);
	int loadv_ok = (nvhost_wait_syncpt(ctrl_fd, sp_loadv, loadv_before + 1, 500) == 0);

	uint32_t mem_after = nvhost_read_syncpt(ctrl_fd, sp_memory);
	uint32_t stats_after = nvhost_read_syncpt(ctrl_fd, sp_stats);
	uint32_t loadv_after = nvhost_read_syncpt(ctrl_fd, sp_loadv);

	printf("  Results: memory(cond4)=%s(%u→%u) stats(cond5)=%s(%u→%u) loadv(cond6)=%s(%u→%u)\n",
	       mem_ok ? "YES" : "NO", mem_before, mem_after,
	       stats_ok ? "YES" : "NO", stats_before, stats_after,
	       loadv_ok ? "YES" : "NO", loadv_before, loadv_after);

	if (mem_ok || stats_ok || loadv_ok) {
		printf("  At least one conditional syncpt fired!\n");
		check_output(outbuf);
	}

	return ret;
}

/* ================================================================
 * main
 * ================================================================ */

int main(int argc, char **argv)
{
	int ch_fd, ctrl_fd;
	uint32_t syncpt;
	uint32_t sp_memory, sp_stats, sp_stream, sp_loadv;
	struct nvbuf cmdbuf, inbuf, outbuf, workbuf;
	const char *isp_dev = "/dev/nvhost-isp";
	uint32_t class_id = ISP_A_CLASS;
	int is_b = 0;
	uint32_t W, H;

	if (argc > 1 && strcmp(argv[1], "b") == 0) {
		isp_dev = "/dev/nvhost-isp.1";
		class_id = ISP_B_CLASS;
		is_b = 1;
		printf("Using ISP-B (class 0x%02x)\n", class_id);
	} else {
		printf("Using ISP-A (class 0x%02x)\n", class_id);
	}

	/* Default resolutions */
	if (is_b) { W = 2592; H = 1944; }
	else      { W = 3280; H = 2460; }

	if (argc > 2) {
		W = atoi(argv[2]);
		H = (argc > 3) ? atoi(argv[3]) : W;
	}
	printf("Resolution: %ux%u\n", W, H);

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

	/* Get all 4 syncpoints (params 0-3) */
	{
		struct nvhost_get_param_arg gp;
		int i;
		uint32_t *sps[] = { &sp_memory, &sp_stats, &sp_stream, &sp_loadv };
		const char *names[] = { "memory", "stats", "stream", "loadv" };

		for (i = 0; i < 4; i++) {
			gp.param = i;
			gp.value = 0;
			if (ioctl(ch_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gp)) {
				perror("GET_SYNCPOINT");
				printf("  Failed to get syncpt param %d\n", i);
				return 1;
			}
			*sps[i] = gp.value;
			printf("ISP syncpt[%d] (%s) = %u\n", i, names[i], gp.value);
		}
	}

	/* Use syncpt[0] (memory) as the primary for init submits,
	 * matching kernel's use of syncpt_stream (param 2) */
	syncpt = sp_stream;

	/* Allocate buffers */
	printf("\nAllocating buffers...\n");
	uint32_t cmdbuf_size = 128 * 1024; /* 128KB for init (S1-S5 can be large) */
	uint32_t in_size = W * H * 2;      /* RAW10 = 2 bpp */
	uint32_t out_size = W * H * 2;     /* YUV420 ~ 1.5 bpp, 2x safety */

	printf("  cmdbuf: %u bytes\n", cmdbuf_size);
	printf("  input:  %u bytes (%ux%u x 2)\n", in_size, W, H);
	printf("  output: %u bytes\n", out_size);
	printf("  workbuf: %u bytes\n", 256 * 1024);

	if (nvbuf_alloc(&cmdbuf, cmdbuf_size, 256)) return 1;
	if (nvbuf_alloc(&inbuf, in_size, 256)) return 1;
	if (nvbuf_alloc(&outbuf, out_size, 256)) return 1;
	if (nvbuf_alloc(&workbuf, 256 * 1024, 256)) return 1;

	/* ============================================================
	 * PHASE 1: ISP Init (S1-S5) — exactly matching kernel driver
	 * ============================================================ */

	if (isp_init(ch_fd, ctrl_fd, &cmdbuf, &workbuf, class_id, syncpt, is_b)) {
		printf("\nISP INIT FAILED! Aborting per-frame tests.\n");
		goto cleanup;
	}

	/* ============================================================
	 * PHASE 2: Per-frame submit tests
	 * ============================================================ */

	printf("\n========================================\n");
	printf("PER-FRAME TESTS (after full init)\n");
	printf("========================================\n");

	/* Test A: no surfaces, just enable+trigger+cond4 */
	test_A_nosurface(ch_fd, ctrl_fd, syncpt, class_id, &cmdbuf, &workbuf);

	/* Test B: enable=0x03, trigger=0x05, WITH surfaces */
	test_B_surfaces(ch_fd, ctrl_fd, syncpt, class_id, &cmdbuf,
			&inbuf, &outbuf, &workbuf, W, H);

	/* Test C: enable=0x07, trigger=0x0F, WITH surfaces (full pipeline) */
	test_C_full(ch_fd, ctrl_fd, syncpt, class_id, &cmdbuf,
		    &inbuf, &outbuf, &workbuf, W, H);

	/* Test D: stock-like per-frame with 4 separate syncpts */
	test_D_stock(ch_fd, ctrl_fd, &cmdbuf,
		     &inbuf, &outbuf, &workbuf, W, H,
		     class_id, sp_memory, sp_stats, sp_stream, sp_loadv);

cleanup:
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
