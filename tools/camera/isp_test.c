/*
 * isp_test.c — Userspace ISP test for Tegra K1 (T124) on stock kernel
 *
 * Runs ISP S1-S5 init + per-frame submit via nvhost/nvmap ioctls.
 * Per-frame gather matches stock MIUI camera exactly (from isp_trace).
 *
 * This is a REPROCESS test — feeds ISP from memory (not VI hardware path).
 * Creates a dummy RAW input buffer, submits to ISP, checks output.
 *
 * Build: arm-linux-gnueabihf-gcc -std=gnu99 -static -o isp_test isp_test.c
 * Run:   adb shell /data/local/tmp/isp_test [b]
 *        "b" = use ISP-B (front camera), default = ISP-A (rear)
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

struct nvmap_cache_op {
	uint32_t addr;   /* hmem */
	uint32_t handle;
	uint32_t length;
	uint32_t op;     /* 0=inv, 1=wb, 2=wbi */
};

#define NVMAP_IOC_CREATE    _IOWR(NVMAP_IOC_MAGIC, 0, struct nvmap_create_handle)
#define NVMAP_IOC_ALLOC     _IOW(NVMAP_IOC_MAGIC, 3, struct nvmap_alloc_handle)
#define NVMAP_IOC_FREE      _IO(NVMAP_IOC_MAGIC, 4)
#define NVMAP_IOC_GET_FD    _IOWR(NVMAP_IOC_MAGIC, 15, struct nvmap_create_handle)
#define NVMAP_IOC_PIN_MULT  _IOWR(NVMAP_IOC_MAGIC, 10, struct nvmap_pin_handle_32)
#define NVMAP_IOC_UNPIN_MULT _IOW(NVMAP_IOC_MAGIC, 11, struct nvmap_pin_handle_32)
#define NVMAP_IOC_CACHE     _IOW(NVMAP_IOC_MAGIC, 12, struct nvmap_cache_op)

#define NVMAP_HEAP_IOVMM    (1 << 30)
#define NVMAP_HANDLE_INNER_CACHEABLE 2

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

struct nvhost_ctrl_syncpt_read_args {
	uint32_t id;
	uint32_t value;
};

struct nvhost_cmdbuf {
	uint32_t mem;
	uint32_t offset;
	uint32_t words;
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
};

#define NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD \
	_IOW(NVHOST_IOC_MAGIC, 5, struct nvhost_set_nvmap_fd_args)
#define NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT \
	_IOWR(NVHOST_IOC_MAGIC, 16, struct nvhost_get_param_arg)
#define NVHOST32_IOCTL_CHANNEL_SUBMIT \
	_IOWR(NVHOST_IOC_MAGIC, 15, struct nvhost32_submit_args)
#define NVHOST_IOCTL_CTRL_SYNCPT_WAITEX \
	_IOWR(NVHOST_IOC_MAGIC, 6, struct nvhost_ctrl_syncpt_waitex_args)
#define NVHOST_IOCTL_CTRL_SYNCPT_READ \
	_IOWR(NVHOST_IOC_MAGIC, 1, struct nvhost_ctrl_syncpt_read_args)

/* ---- host1x opcodes ---- */
#define OP_SETCLASS(cl, off, mask)  ((0 << 28) | ((off) << 16) | ((cl) << 6) | (mask))
#define OP_INCR(off, count)         ((1 << 28) | ((off) << 16) | (count))
#define OP_NONINCR(off, count)      ((2 << 28) | ((off) << 16) | (count))
#define OP_IMM(off, val)            ((4 << 28) | ((off) << 16) | (val))
#define OP_NOOP                     0

/* IMM_INCR_SYNCPT: immediate syncpt increment via host1x IMM opcode
 * Format: OP_IMM(0x000, (cond << 8) | syncpt_id) */
#define OP_SYNCPT_INCR_IMM(id)  OP_IMM(0x000, (id))
#define OP_SYNCPT_INCR_COND(cond, id)  OP_IMM(0x000, ((cond) << 8) | (id))

/* ISP constants */
#define ISP_A_CLASS  0x32
#define ISP_B_CLASS  0x34
#define HOST1X_CLASS 0x01

#define ISP_FORMAT_STOCK       0x04FE00E6
#define ISP_TRIGGER_RUNTIME    0x05
#define ISP_TRIGGER_POST_APPLY 0x0F

#define ISP_SYNCPT_COND_OP_DONE    4
#define ISP_SYNCPT_COND_STATS_DONE 5
#define ISP_SYNCPT_COND_RD_DONE    6

/* ================================================================
 * nvmap buffer helper
 * ================================================================ */

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

	memset(b, 0, sizeof(*b));
	b->size = size;
	b->dmabuf_fd = -1;

	if (ioctl(nvmap_fd, NVMAP_IOC_CREATE, &ch)) {
		perror("nvmap CREATE"); return -1;
	}
	b->handle = ch.handle;

	ah.handle = b->handle;
	ah.heap_mask = NVMAP_HEAP_IOVMM;
	ah.flags = NVMAP_HANDLE_INNER_CACHEABLE;
	ah.align = align;
	if (ioctl(nvmap_fd, NVMAP_IOC_ALLOC, &ah)) {
		perror("nvmap ALLOC"); return -1;
	}

	gf.handle = b->handle;
	if (ioctl(nvmap_fd, NVMAP_IOC_GET_FD, &gf)) {
		perror("nvmap GET_FD"); return -1;
	}
	b->dmabuf_fd = gf.fd;

	ph.handles = b->handle;
	ph.addr = 0;
	ph.count = 1;
	if (ioctl(nvmap_fd, NVMAP_IOC_PIN_MULT, &ph)) {
		perror("nvmap PIN"); return -1;
	}
	b->iova = ph.addr;

	b->cpu = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
		      b->dmabuf_fd, 0);
	if (b->cpu == MAP_FAILED) {
		perror("mmap"); b->cpu = NULL; return -1;
	}

	printf("  buf: handle=%u fd=%d iova=0x%08x size=%u\n",
	       b->handle, b->dmabuf_fd, b->iova, b->size);
	return 0;
}

static void nvbuf_cache_wb(struct nvbuf *b)
{
	struct nvmap_cache_op cop;
	cop.addr = (uint32_t)(uintptr_t)b->cpu;
	cop.handle = b->handle;
	cop.length = b->size;
	cop.op = 2; /* writeback-invalidate */
	ioctl(nvmap_fd, NVMAP_IOC_CACHE, &cop);
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
	b->dmabuf_fd = -1;
}

/* ================================================================
 * nvhost submit helper
 * ================================================================ */

static int ctrl_fd = -1;

static int syncpt_wait(uint32_t id, uint32_t thresh, int timeout_ms)
{
	struct nvhost_ctrl_syncpt_waitex_args wa;
	wa.id = id;
	wa.thresh = thresh;
	wa.timeout = timeout_ms;
	wa.value = 0;
	if (ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_WAITEX, &wa)) {
		printf("  syncpt_wait TIMEOUT: id=%u thresh=%u (err=%s)\n",
		       id, thresh, strerror(errno));
		return -1;
	}
	return 0;
}

static uint32_t syncpt_read(uint32_t id)
{
	struct nvhost_ctrl_syncpt_read_args ra;
	ra.id = id;
	ra.value = 0;
	ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_READ, &ra);
	return ra.value;
}

struct submit_info {
	int ch_fd;
	struct nvbuf *cmdbuf;
	struct nvhost_cmdbuf gathers[4];
	int num_gathers;
	struct nvhost_syncpt_incr syncpts[4];
	int num_syncpts;
	struct nvhost_reloc relocs[4];
	struct nvhost_reloc_shift shifts[4];
	int num_relocs;
	uint32_t fences[4];
};

static int do_submit(struct submit_info *si)
{
	uint32_t class_ids[4];
	struct nvhost32_submit_args sa;
	int i;

	for (i = 0; i < si->num_gathers; i++)
		class_ids[i] = 0; /* class from gather SET_CLASS */

	memset(&sa, 0, sizeof(sa));
	sa.submit_version = 0;
	sa.num_syncpt_incrs = si->num_syncpts;
	sa.num_cmdbufs = si->num_gathers;
	sa.num_relocs = si->num_relocs;
	sa.timeout = 5000;
	sa.syncpt_incrs = (uint32_t)(uintptr_t)si->syncpts;
	sa.cmdbufs = (uint32_t)(uintptr_t)si->gathers;
	sa.relocs = (uint32_t)(uintptr_t)si->relocs;
	sa.reloc_shifts = (uint32_t)(uintptr_t)si->shifts;
	sa.class_ids = (uint32_t)(uintptr_t)class_ids;
	sa.fences = (uint32_t)(uintptr_t)si->fences;

	if (ioctl(si->ch_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa)) {
		perror("SUBMIT");
		return -1;
	}
	printf("  submit OK: fence=%u fences=[%u,%u,%u,%u]\n",
	       sa.fence, si->fences[0], si->fences[1],
	       si->fences[2], si->fences[3]);
	return 0;
}

/* Simple submit: 2 gathers (data + syncpt), 1 syncpt, wait */
static int submit_and_wait(int ch_fd, struct nvbuf *cmdbuf, int words,
			   uint32_t syncpt_id, const char *name)
{
	struct submit_info si;
	uint32_t *cmd = (uint32_t *)cmdbuf->cpu;
	int syncpt_off = words; /* syncpt gather starts right after data */

	/* Append syncpt incr as separate gather */
	cmd[syncpt_off] = OP_SYNCPT_INCR_IMM(syncpt_id);
	cmd[syncpt_off + 1] = OP_NOOP;

	memset(&si, 0, sizeof(si));
	si.ch_fd = ch_fd;
	si.gathers[0].mem = cmdbuf->dmabuf_fd;
	si.gathers[0].offset = 0;
	si.gathers[0].words = words;
	si.gathers[1].mem = cmdbuf->dmabuf_fd;
	si.gathers[1].offset = words * 4;
	si.gathers[1].words = 2;
	si.num_gathers = 2;
	si.syncpts[0].syncpt_id = syncpt_id;
	si.syncpts[0].syncpt_incrs = 1;
	si.num_syncpts = 1;

	nvbuf_cache_wb(cmdbuf);

	printf("  %s: %d words ...", name, words);
	if (do_submit(&si))
		return -1;

	if (syncpt_wait(syncpt_id, si.fences[0], 2000)) {
		printf("  %s: TIMEOUT\n", name);
		return -1;
	}
	printf("  %s: OK\n", name);
	return 0;
}

/* ================================================================
 * ISP zero-init builder (matches kernel isp_build_zero_init)
 * ================================================================ */

static int build_zero_init(uint32_t *buf)
{
	int n = 0;
#define ZI(off, cnt) do { buf[n++] = OP_INCR(off, cnt); \
	memset(&buf[n], 0, (cnt)*4); n += (cnt); } while(0)
#define ZN(off, cnt) do { buf[n++] = OP_NONINCR(off, cnt); \
	memset(&buf[n], 0, (cnt)*4); n += (cnt); } while(0)
	ZI(0x202, 3); ZI(0x200, 2); ZI(0x205, 4);
	ZI(0x700, 16); ZI(0x750, 16);
	ZI(0x500, 6); ZI(0x015, 1);
	ZI(0xd00, 10); ZI(0xd0a, 1); ZN(0xd0b, 480);
	ZI(0xd0c, 2); ZI(0xd20, 6);
	ZI(0x900, 2); ZI(0x902, 1); ZN(0x903, 64);
	ZI(0x904, 2); ZI(0x906, 1); ZN(0x907, 36);
	ZI(0x908, 1); ZI(0x920, 10); ZI(0x909, 7);
	ZI(0x910, 9); ZI(0x919, 1); ZN(0x91a, 9);
	ZI(0x91b, 1); ZN(0x91c, 9);
	ZI(0x91d, 1); ZN(0x91e, 9);
	buf[n++] = OP_INCR(0x91f, 1);
	buf[n++] = 0x00000002;
	ZI(0x506, 9); ZI(0x600, 16); ZI(0x650, 1);
	ZI(0x651, 1); ZN(0x652, 257);
	ZI(0x653, 1); ZN(0x654, 257);
	ZI(0x655, 1); ZN(0x656, 257);
	ZI(0x657, 1); ZN(0x658, 257);
	ZI(0x300, 4); ZI(0x304, 4);
	ZI(0x053, 2);
#undef ZI
#undef ZN
	return n;
}

/* Build zero block: zero_init + patch work_buf + trigger (matches stock S1/S2/S4) */
static int build_zero_block(uint32_t *buf, uint32_t safe_iova)
{
	int n = build_zero_init(buf);
	/* Patch last word of zero_init (0x054) = work_buf iova */
	buf[n - 1] = safe_iova;

	buf[n++] = OP_NONINCR(0x00C, 1);
	buf[n++] = ISP_TRIGGER_POST_APPLY;
	return n;
}

/* Append cal data + trigger */
static int append_cal(uint32_t *buf, int n, const uint32_t *cal, int cal_words,
		      uint32_t work_iova)
{
	memcpy(&buf[n], cal, cal_words * 4);
	n += cal_words;
	buf[n - 1] = work_iova; /* patch last word = work_buf iova */
	buf[n++] = OP_NONINCR(0x00C, 1);
	buf[n++] = ISP_TRIGGER_POST_APPLY;
	return n;
}

/* ================================================================
 * S5 runtime config (from kernel isp_t124.c)
 * ================================================================ */

static int build_s5_runtime(uint32_t *buf, int is_b, uint32_t work_iova,
			    const uint32_t *cal, int cal_words)
{
	int n = 0;

	/* 0x400: runtime config */
	buf[n++] = OP_SETCLASS(is_b ? ISP_B_CLASS : ISP_A_CLASS, 0, 0);
	buf[n++] = OP_INCR(0x400, 12);
	buf[n++] = 0x00000001;
	buf[n++] = 0x004b0000;
	buf[n++] = 0x00930000;
	buf[n++] = 0x00220000;
	buf[n++] = work_iova + 0x10000;
	buf[n++] = work_iova + 0x10000;
	buf[n++] = work_iova + 0x10000;
	buf[n++] = work_iova + 0x10000;
	buf[n++] = 0x00030000;
	buf[n++] = 0x00000000;
	buf[n++] = 0x00020000;
	buf[n++] = 0x00000000;

	/* 0x800: stats buffer A */
	buf[n++] = OP_SETCLASS(is_b ? ISP_B_CLASS : ISP_A_CLASS, 0, 0);
	buf[n++] = OP_INCR(0x800, 3);
	buf[n++] = work_iova; buf[n++] = 0; buf[n++] = 0;

	/* 0x820: stats buffer B */
	buf[n++] = OP_SETCLASS(is_b ? ISP_B_CLASS : ISP_A_CLASS, 0, 0);
	buf[n++] = OP_INCR(0x820, 3);
	buf[n++] = work_iova; buf[n++] = 0; buf[n++] = 0;

	/* 0x930: histogram config */
	buf[n++] = OP_SETCLASS(is_b ? ISP_B_CLASS : ISP_A_CLASS, 0, 0);
	buf[n++] = OP_INCR(0x930, 18);
	buf[n++] = 0x0000001c; buf[n++] = 0x88888888;
	buf[n++] = 0x78787800; buf[n++] = 0x00000078;
	buf[n++] = 0x88888888; buf[n++] = 0x78787800;
	buf[n++] = 0x00000078; buf[n++] = 0x88888888;
	buf[n++] = 0x78787800; buf[n++] = 0x00000078;
	buf[n++] = 0x88888888; buf[n++] = 0x78787800;
	buf[n++] = 0x00000078; buf[n++] = 0x3fc00000;
	buf[n++] = 0x00000000; buf[n++] = 0x00070000;
	buf[n++] = 0x00000000; buf[n++] = 0x00070000;

	/* 0xC00 */
	buf[n++] = OP_SETCLASS(is_b ? ISP_B_CLASS : ISP_A_CLASS, 0, 0);
	buf[n++] = OP_INCR(0xC00, 3);
	buf[n++] = 0x00000101; buf[n++] = 0; buf[n++] = 0x00100000;

	/* 0x202-0x208 */
	buf[n++] = OP_SETCLASS(is_b ? ISP_B_CLASS : ISP_A_CLASS, 0, 0);
	buf[n++] = OP_INCR(0x202, 3);
	buf[n++] = 0x00000001;
	buf[n++] = is_b ? 0x00780078 : 0x02000200;
	buf[n++] = is_b ? 0x00780078 : 0x02000200;
	buf[n++] = OP_INCR(0x200, 2);
	buf[n++] = 0x00000001; buf[n++] = 0;
	buf[n++] = OP_INCR(0x205, 4);
	buf[n++] = 0; buf[n++] = 0x000600c8;
	buf[n++] = 0x000f000f;
	buf[n++] = is_b ? 0x00000000 : 0x00003333;

	/* 0x700: processing channel A */
	buf[n++] = OP_INCR(0x700, 16);
	buf[n++] = 0x00000001; buf[n++] = 0;
	buf[n++] = 0; buf[n++] = 0; buf[n++] = 0;
	buf[n++] = is_b ? 0x00001a40 : 0x00001dc0;
	buf[n++] = 0; buf[n++] = work_iova + 0x30000;
	buf[n++] = 0; buf[n++] = 0;
	buf[n++] = 0x00001000;
	buf[n++] = is_b ? 0x00001a00 : 0x00001c50;
	buf[n++] = work_iova + 0x20000; buf[n++] = work_iova + 0x20000;
	buf[n++] = work_iova + 0x20000; buf[n++] = work_iova + 0x20000;

	/* 0x750: processing channel B */
	buf[n++] = OP_INCR(0x750, 16);
	buf[n++] = 0x00000003; buf[n++] = 0;
	buf[n++] = 0; buf[n++] = 0;
	buf[n++] = 0; buf[n++] = 0;
	buf[n++] = 0; buf[n++] = 0;
	buf[n++] = 0; buf[n++] = 0;
	buf[n++] = 0; buf[n++] = 0;
	buf[n++] = work_iova + 0x20000; buf[n++] = work_iova + 0x20000;
	buf[n++] = work_iova + 0x20000; buf[n++] = work_iova + 0x20000;

	/* 0xd20: lens shading extra */
	buf[n++] = OP_INCR(0xd20, 6);
	buf[n++] = is_b ? 0x00001101 : 0x00003101;
	buf[n++] = 0;
	buf[n++] = is_b ? 0x00210000 : 0x01ec0000;
	buf[n++] = is_b ? 0x00210000 : 0x01ec0000;
	buf[n++] = is_b ? 0x00210000 : 0x01ec0000;
	buf[n++] = is_b ? 0x00210000 : 0x01ec0000;

	/* 0x900 stats */
	buf[n++] = OP_INCR(0x900, 2);
	buf[n++] = 1; buf[n++] = 1;
	buf[n++] = OP_INCR(0x904, 2);
	buf[n++] = 0x00005555; buf[n++] = 1;
	buf[n++] = OP_INCR(0x908, 1);
	buf[n++] = 0x00005555;

	/* 0x920 stats window */
	buf[n++] = OP_INCR(0x920, 10);
	buf[n++] = 0x00000002; buf[n++] = work_iova + 0x31660;
	buf[n++] = 0;          buf[n++] = work_iova + 0x3f4a0;
	buf[n++] = 0x0000fa80; buf[n++] = work_iova + 0x30000;
	buf[n++] = 0x00001c50; buf[n++] = work_iova + 0x20000;
	buf[n++] = work_iova + 0x20000; buf[n++] = work_iova + 0x20000;

	/* 0x909 stats config */
	buf[n++] = OP_INCR(0x909, 7);
	buf[n++] = 1; buf[n++] = 0xfc000f00;
	buf[n++] = 0xf680f320; buf[n++] = 0x0d80fde0;
	buf[n++] = is_b ? 0x00000030 : 0;
	buf[n++] = 0x1400002a; buf[n++] = 0x3c00002b;

	/* 0x910 */
	buf[n++] = OP_INCR(0x910, 9);
	buf[n++] = 3; buf[n++] = 0x00000028; buf[n++] = 0x01480029;
	buf[n++] = is_b ? 0x0003030b : 0x00177e0b;
	buf[n++] = 0x00990030; buf[n++] = 0x00000800;
	buf[n++] = 0x007b0666;
	buf[n++] = is_b ? 0x00000036 : 0x00000039;
	buf[n++] = is_b ? 0x00001f1f : 0;

	buf[n++] = OP_INCR(0x91b, 1); buf[n++] = 0;
	buf[n++] = OP_NONINCR(0x91c, 9);
	buf[n++] = 0; buf[n++] = 0; buf[n++] = 0; buf[n++] = 0;
	buf[n++] = 1;
	buf[n++] = is_b ? 0x00000025 : 0x00000026;
	buf[n++] = 0; buf[n++] = 0x00000026; buf[n++] = 0x00000361;

	buf[n++] = OP_INCR(0x91d, 1); buf[n++] = 0;
	buf[n++] = OP_NONINCR(0x91e, 9);
	buf[n++] = 0; buf[n++] = 0; buf[n++] = 0; buf[n++] = 0;
	buf[n++] = 0; buf[n++] = 0x00000780;
	buf[n++] = 0; buf[n++] = 0x00000780; buf[n++] = 0x00000200;

	buf[n++] = OP_INCR(0x91f, 1); buf[n++] = 0x00000032;

	/* 0x506 demosaic */
	buf[n++] = OP_INCR(0x506, 9);
	buf[n++] = 0x3f3fcff3; buf[n++] = 0;
	buf[n++] = 0x04c1304c; buf[n++] = 0x08220882;
	buf[n++] = 0; buf[n++] = 0x03d0f43d;
	buf[n++] = 0x08621886; buf[n++] = 0x01204812;
	buf[n++] = 0x06e1b86e;

	/* 0x600 GPP config */
	buf[n++] = OP_INCR(0x600, 16);
	buf[n++] = 5; buf[n++] = 0; buf[n++] = 0; buf[n++] = 0;
	buf[n++] = 0; buf[n++] = 0; buf[n++] = 0; buf[n++] = 0;
	buf[n++] = 0; buf[n++] = 0; buf[n++] = 0; buf[n++] = 0;
	buf[n++] = 0x3fff0000; buf[n++] = 0x3fff0000;
	buf[n++] = 0x3fff0000; buf[n++] = work_iova + 0x31000;

	/* 0x650 tone curve */
	buf[n++] = OP_INCR(0x650, 1); buf[n++] = 3;
	buf[n++] = OP_INCR(0x651, 1); buf[n++] = 0;

	/* Cal data (lens shading + tone curves) + trigger */
	n = append_cal(buf, n, cal, cal_words, work_iova);

	/* Syncpt (immediate incr on stream syncpt — added by caller) */
	return n;
}

/* ================================================================
 * Per-frame gather — exact stock 45-word format
 * ================================================================ */

static int build_per_frame(uint32_t *buf, uint32_t class_id,
			   uint32_t W, uint32_t H,
			   uint32_t out_iova, uint32_t stats_iova,
			   uint32_t y_stride, uint32_t uv_stride,
			   uint32_t sp_mem, uint32_t sp_stats, uint32_t sp_loadv)
{
	int n = 0;
	uint32_t y_size = y_stride * H;
	uint32_t uv_size = uv_stride * (H / 2);
	uint32_t out_y = out_iova;
	uint32_t out_u = out_iova + y_size;
	uint32_t out_v = out_iova + y_size + uv_size;

	/* SET_CLASS */
	buf[n++] = OP_SETCLASS(class_id, 0, 0);

	/* Output width/height/format/color */
	buf[n++] = OP_INCR(0xE00, 1); buf[n++] = ((W - 1) & 0x3FFF) << 16;
	buf[n++] = OP_INCR(0xE01, 1); buf[n++] = ((H - 1) & 0x3FFF) << 16;
	buf[n++] = OP_INCR(0xE02, 1); buf[n++] = ISP_FORMAT_STOCK;
	buf[n++] = OP_INCR(0xE03, 1); buf[n++] = 0;

	/* Y/U/V surfaces */
	buf[n++] = OP_INCR(0xE04, 3);
	buf[n++] = out_y; buf[n++] = 0; buf[n++] = y_stride;
	buf[n++] = OP_INCR(0xE07, 3);
	buf[n++] = out_u; buf[n++] = 0; buf[n++] = uv_stride;
	buf[n++] = OP_INCR(0xE0A, 3);
	buf[n++] = out_v; buf[n++] = 0; buf[n++] = uv_stride;

	/* Processing — stock: [0,0,0,0,0,(H<<16)|W] */
	buf[n++] = OP_INCR(0x500, 6);
	buf[n++] = 0; buf[n++] = 0; buf[n++] = 0;
	buf[n++] = 0; buf[n++] = 0;
	buf[n++] = (H << 16) | W;

	/* NO ISP_ENABLE — stock sets it once in S5, not per-frame */

	/* Stats */
	buf[n++] = OP_SETCLASS(class_id, 0, 0);
	buf[n++] = OP_INCR(0x100, 4);
	buf[n++] = stats_iova; buf[n++] = 0; buf[n++] = 0; buf[n++] = 0;

	/* Syncpt incrs: cond=4 (OP_DONE), cond=5 (STATS), cond=6 (RD_DONE) */
	buf[n++] = OP_SETCLASS(class_id, 0, 0);
	buf[n++] = OP_SETCLASS(class_id, 0, 0);
	buf[n++] = OP_NONINCR(0x000, 1);
	buf[n++] = (ISP_SYNCPT_COND_OP_DONE << 8) | sp_mem;
	buf[n++] = OP_NONINCR(0x000, 1);
	buf[n++] = (ISP_SYNCPT_COND_STATS_DONE << 8) | sp_stats;
	buf[n++] = OP_NONINCR(0x000, 1);
	buf[n++] = (ISP_SYNCPT_COND_RD_DONE << 8) | sp_loadv;

	/* Trigger */
	buf[n++] = OP_SETCLASS(class_id, 0, 0);
	buf[n++] = OP_NONINCR(0x00C, 1);
	buf[n++] = ISP_TRIGGER_RUNTIME;

	printf("  per-frame: %d words\n", n);
	return n;
}

/* ================================================================
 * main
 * ================================================================ */

int main(int argc, char **argv)
{
	int is_b = (argc > 1 && argv[1][0] == 'b');
	uint32_t class_id = is_b ? ISP_B_CLASS : ISP_A_CLASS;
	const char *dev_path = is_b ? "/dev/nvhost-isp.1" : "/dev/nvhost-isp";
	const uint32_t *cal_data = is_b ? isp_b_cal_data : isp_a_cal_data;
	int cal_words = is_b ? (int)(sizeof(isp_b_cal_data)/4) : (int)(sizeof(isp_a_cal_data)/4);

	uint32_t W = is_b ? 2592 : 3280;
	uint32_t H = is_b ? 1944 : 2460;
	uint32_t y_stride = (W + 63) & ~63;
	uint32_t uv_stride = ((W/2) + 63) & ~63;
	uint32_t out_size = y_stride * H + 2 * uv_stride * (H/2);

	int ch_fd = -1;
	struct nvbuf cmdbuf = {0}, work_buf = {0}, out_buf = {0}, stats_buf = {0};
	uint32_t sp_stream = 0, sp_mem = 0, sp_stats = 0, sp_loadv = 0;
	uint32_t *cmd;
	int n, err;

	printf("=== ISP Test: ISP-%s (%s) %ux%u ===\n",
	       is_b ? "B" : "A", dev_path, W, H);

	/* Open nvmap */
	nvmap_fd = open("/dev/nvmap", O_RDWR);
	if (nvmap_fd < 0) { perror("open nvmap"); return 1; }

	/* Open nvhost ctrl */
	ctrl_fd = open("/dev/nvhost-ctrl", O_RDWR);
	if (ctrl_fd < 0) { perror("open nvhost-ctrl"); return 1; }

	/* Open ISP channel */
	ch_fd = open(dev_path, O_RDWR);
	if (ch_fd < 0) { perror("open isp"); return 1; }

	/* Set nvmap fd */
	{
		struct nvhost_set_nvmap_fd_args sn = { .fd = nvmap_fd };
		if (ioctl(ch_fd, NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD, &sn)) {
			perror("SET_NVMAP_FD"); goto out;
		}
	}

	/* Get syncpoints */
	{
		struct nvhost_get_param_arg gp;
		gp.param = 0; /* first syncpt */
		if (ioctl(ch_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gp)) {
			perror("GET_SYNCPOINT 0"); goto out;
		}
		sp_stream = gp.value;

		gp.param = 1;
		ioctl(ch_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gp);
		sp_mem = gp.value;

		gp.param = 2;
		ioctl(ch_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gp);
		sp_stats = gp.value;

		gp.param = 3;
		ioctl(ch_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gp);
		sp_loadv = gp.value;
	}
	printf("syncpts: stream=%u mem=%u stats=%u loadv=%u\n",
	       sp_stream, sp_mem, sp_stats, sp_loadv);

	/* Allocate buffers */
	printf("Allocating buffers...\n");
	if (nvbuf_alloc(&cmdbuf, 32768, 4096)) goto out;
	if (nvbuf_alloc(&work_buf, 512*1024, 4096)) goto out;
	if (nvbuf_alloc(&out_buf, out_size, 4096)) goto out;
	if (nvbuf_alloc(&stats_buf, 65536, 4096)) goto out;

	memset(work_buf.cpu, 0, work_buf.size);
	memset(out_buf.cpu, 0xDE, out_buf.size);
	memset(stats_buf.cpu, 0, stats_buf.size);

	cmd = (uint32_t *)cmdbuf.cpu;

	/* ---- Test: minimal submit — just SET_CLASS + syncpt ---- */
	n = 0;
	cmd[n++] = OP_SETCLASS(class_id, 0, 0);

	err = submit_and_wait(ch_fd, &cmdbuf, n, sp_stream, "PING");
	if (err) {
		printf("PING FAILED — ISP not responding\n");
		goto out;
	}

	/* ---- S1: zero-init ×2 + 0x018 tails ---- */
	n = 0;
	cmd[n++] = OP_SETCLASS(class_id, 0, 0); /* host1x needs class before methods */
	{
		int z = build_zero_block(&cmd[n], work_buf.iova);
		n += z;
	}
	cmd[n++] = OP_INCR(0x018, 5);
	cmd[n++] = 0; cmd[n++] = 0x400; cmd[n++] = 0; cmd[n++] = 0x200; cmd[n++] = 2;
	{
		int z = build_zero_block(&cmd[n], work_buf.iova);
		n += z;
	}
	cmd[n++] = OP_INCR(0x018, 5);
	cmd[n++] = 0x0a00500a; cmd[n++] = 0x00008089;
	cmd[n++] = 0x013645cb; cmd[n++] = 0x000001e7; cmd[n++] = 1;

	err = submit_and_wait(ch_fd, &cmdbuf, n, sp_stream, "S1");
	if (err) goto out;

	/* ---- S2: zero-block ---- */
	n = 0;
	cmd[n++] = OP_SETCLASS(class_id, 0, 0);
	{
		int z = build_zero_block(&cmd[n], work_buf.iova);
		n += z;
	}

	err = submit_and_wait(ch_fd, &cmdbuf, n, sp_stream, "S2");
	if (err) printf("  (continuing despite S2 timeout)\n");

	/* ---- S3: SET_CLASS (cond=OP_DONE) ---- */
	n = 0;
	cmd[n++] = OP_SETCLASS(class_id, 0, 0);

	err = submit_and_wait(ch_fd, &cmdbuf, n, sp_stream, "S3");
	if (err) goto out;

	/* ---- S4: zero-block ---- */
	n = 0;
	cmd[n++] = OP_SETCLASS(class_id, 0, 0);
	{
		int z = build_zero_block(&cmd[n], work_buf.iova);
		n += z;
	}

	err = submit_and_wait(ch_fd, &cmdbuf, n, sp_stream, "S4");
	if (err) printf("  (continuing despite S4 timeout)\n");

	/* ---- S5: runtime config + cal ---- */
	n = build_s5_runtime(cmd, is_b, work_buf.iova, cal_data, cal_words);

	err = submit_and_wait(ch_fd, &cmdbuf, n, sp_stream, "S5");
	if (err) goto out;

	printf("\n=== ISP init S1-S5 complete ===\n\n");

	/* ---- Per-frame test ---- */
	printf("Submitting per-frame...\n");
	{
		int pf_off, pf_words, g2_off;
		struct submit_info si;

		/* Build per-frame gather */
		pf_off = 0;
		pf_words = build_per_frame(cmd, class_id, W, H,
					   out_buf.iova, stats_buf.iova,
					   y_stride, uv_stride,
					   sp_mem, sp_stats, sp_loadv);

		/* G[1]: immediate syncpt incr */
		g2_off = pf_words;
		cmd[g2_off] = OP_SYNCPT_INCR_IMM(sp_stream);
		cmd[g2_off + 1] = OP_NOOP;

		nvbuf_cache_wb(&cmdbuf);
		nvbuf_cache_wb(&out_buf);

		memset(&si, 0, sizeof(si));
		si.ch_fd = ch_fd;
		si.gathers[0].mem = cmdbuf.dmabuf_fd;
		si.gathers[0].offset = pf_off * 4;
		si.gathers[0].words = pf_words;
		si.gathers[1].mem = cmdbuf.dmabuf_fd;
		si.gathers[1].offset = g2_off * 4;
		si.gathers[1].words = 2;
		si.num_gathers = 2;

		si.syncpts[0].syncpt_id = sp_mem;
		si.syncpts[0].syncpt_incrs = 1;
		si.syncpts[1].syncpt_id = sp_stats;
		si.syncpts[1].syncpt_incrs = 1;
		si.syncpts[2].syncpt_id = sp_loadv;
		si.syncpts[2].syncpt_incrs = 1;
		si.syncpts[3].syncpt_id = sp_stream;
		si.syncpts[3].syncpt_incrs = 1;
		si.num_syncpts = 4;

		/* Reloc: patch output Y surface address */
		si.relocs[0].cmdbuf_mem = cmdbuf.dmabuf_fd;
		si.relocs[0].cmdbuf_offset = 10 * 4; /* word 10 = Y addr after SET_CLASS+8 output words+INCR */
		si.relocs[0].target = out_buf.dmabuf_fd;
		si.relocs[0].target_offset = 0;
		si.shifts[0].shift = 0;
		si.num_relocs = 1;

		printf("Per-frame submit (pf=%d words, g2=2 words)...\n", pf_words);
		if (do_submit(&si)) goto out;

		printf("Waiting for stream syncpt (id=%u, fence=%u)...\n",
		       sp_stream, si.fences[3]);
		if (syncpt_wait(sp_stream, si.fences[3], 3000)) {
			printf("PER-FRAME TIMEOUT\n");
		} else {
			printf("PER-FRAME OK!\n");
		}

		/* Check output */
		{
			uint32_t *out32 = (uint32_t *)out_buf.cpu;
			uint32_t total = out_size / 4;
			uint32_t nz = 0, dead = 0, zero = 0;
			uint32_t i;
			for (i = 0; i < total; i++) {
				if (out32[i] == 0xDEDEDEDE) dead++;
				else if (out32[i] == 0) zero++;
				else nz++;
			}
			printf("\nOutput: total=%u dead=0x%x zero=%u nonzero=%u\n",
			       total, dead, zero, nz);
			if (nz > 0)
				printf("  FIRST nonzero: out32[?]=0x%08x\n",
				       out32[0] != 0xDEDEDEDE && out32[0] != 0 ? out32[0] : out32[1]);

			/* Save to file */
			FILE *f = fopen("/data/local/tmp/isp_out.raw", "wb");
			if (f) {
				fwrite(out_buf.cpu, 1, out_size, f);
				fclose(f);
				printf("Saved %u bytes to /data/local/tmp/isp_out.raw\n", out_size);
			}
		}
	}

out:
	nvbuf_free(&stats_buf);
	nvbuf_free(&out_buf);
	nvbuf_free(&work_buf);
	nvbuf_free(&cmdbuf);
	if (ch_fd >= 0) close(ch_fd);
	if (ctrl_fd >= 0) close(ctrl_fd);
	if (nvmap_fd >= 0) close(nvmap_fd);
	printf("Done.\n");
	return 0;
}
