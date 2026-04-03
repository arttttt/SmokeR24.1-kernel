/*
 * isp_dma_test.c — Clean ISP DMA test for SmokeR24.1 kernel
 *
 * Tests ISP DMA processing with:
 * - Missing init registers from stock MMIO dump
 * - Pinned IOVA addresses (no relocs, avoids -12 bug)
 * - Sentinel pattern (0xDE) in output for honest verification
 * - Dual trigger: 0x0F (static config) then 0x05 (runtime)
 * - Work buffer for ISP (method 0x053/0x054)
 *
 * Build with Android NDK:
 *   armv7a-linux-androideabi19-clang -std=gnu99 -fPIE -pie \
 *     -o isp_dma_test isp_dma_test.c
 *
 * Or with Linaro (static):
 *   arm-linux-gnueabihf-gcc -std=gnu99 -static -o isp_dma_test isp_dma_test.c
 *
 * Usage: isp_dma_test [a|b]
 *   a = ISP-A (IMX179 rear, default)
 *   b = ISP-B (OV5693 front)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <time.h>
#include <sys/mman.h>

/* ---- nvhost ioctl definitions ---- */
#define NVHOST_IOCTL_MAGIC 'H'

struct nvhost_set_nvmap_fd_args { uint32_t fd; } __attribute__((packed));
struct nvhost_get_param_arg { uint32_t param; uint32_t value; };
struct nvhost_syncpt_incr { uint32_t syncpt_id; uint32_t syncpt_incrs; };
struct nvhost_cmdbuf { uint32_t mem; uint32_t offset; uint32_t words; } __attribute__((packed));
struct nvhost_cmdbuf_ext { int32_t pre_fence; uint32_t reserved; };

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
#define NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT \
	_IOWR(NVHOST_IOCTL_MAGIC, 16, struct nvhost_get_param_arg)
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
struct nvmap_rw_handle {
	unsigned long addr;
	uint32_t handle;
	uint32_t offset;
	uint32_t elem_size;
	uint32_t hmem_stride;
	uint32_t user_stride;
	uint32_t count;
} __attribute__((packed));
struct nvmap_pin_handle {
	uint32_t handles;
	unsigned long addr;
	uint32_t count;
};

#define NVMAP_IOC_CREATE   _IOWR(NVMAP_IOC_MAGIC, 0, struct nvmap_create_handle)
#define NVMAP_IOC_ALLOC    _IOW(NVMAP_IOC_MAGIC, 3, struct nvmap_alloc_handle)
#define NVMAP_IOC_FREE     _IO(NVMAP_IOC_MAGIC, 4)
#define NVMAP_IOC_WRITE    _IOW(NVMAP_IOC_MAGIC, 6, struct nvmap_rw_handle)
#define NVMAP_IOC_READ     _IOW(NVMAP_IOC_MAGIC, 7, struct nvmap_rw_handle)
#define NVMAP_IOC_PIN_MULT _IOWR(NVMAP_IOC_MAGIC, 10, struct nvmap_pin_handle)

#define NVMAP_HEAP_IOVMM           (1 << 30)
#define NVMAP_HEAP_CARVEOUT_GENERIC (1 << 0)
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
static inline uint32_t h1x_setclass(uint32_t cls, uint32_t off, uint32_t mask)
{ return (0u << 28) | (off << 16) | (cls << 6) | mask; }
static inline uint32_t h1x_incr(uint32_t off, uint32_t count)
{ return (1u << 28) | (off << 16) | count; }
static inline uint32_t h1x_nonincr(uint32_t off, uint32_t count)
{ return (2u << 28) | (off << 16) | count; }
static inline uint32_t h1x_imm(uint32_t off, uint32_t val)
{ return (4u << 28) | (off << 16) | val; }
static inline uint32_t h1x_imm_incr_syncpt(uint32_t cond, uint32_t id)
{ return h1x_imm(0, (cond << 8) | id); }

#define NOOP     h1x_nonincr(0, 0)
#define SENTINEL 0xDE

/* ---- globals ---- */
static int nvmap_fd = -1;
static int isp_fd = -1;
static int ctrl_fd = -1;

/* ---- nvmap helpers ---- */
static uint32_t nvmap_create(uint32_t size) {
	struct nvmap_create_handle ch = { .size = size };
	if (ioctl(nvmap_fd, NVMAP_IOC_CREATE, &ch) < 0) {
		perror("nvmap create"); return 0;
	}
	return ch.handle;
}

static int nvmap_alloc(uint32_t handle, uint32_t align) {
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

/* Allocate physically contiguous (for host1x CDMA gathers) */
static int nvmap_alloc_contig(uint32_t handle, uint32_t align) {
	struct nvmap_alloc_handle ah = {
		.handle = handle,
		.heap_mask = NVMAP_HEAP_CARVEOUT_GENERIC,
		.flags = NVMAP_HANDLE_WRITE_COMBINE,
		.align = align,
	};
	if (ioctl(nvmap_fd, NVMAP_IOC_ALLOC, &ah) < 0) {
		/* Fallback to IOVMM if carveout unavailable */
		perror("nvmap alloc contig (trying IOVMM)");
		ah.heap_mask = NVMAP_HEAP_IOVMM;
		if (ioctl(nvmap_fd, NVMAP_IOC_ALLOC, &ah) < 0) {
			perror("nvmap alloc iovmm fallback");
			return -1;
		}
		printf("  WARNING: cmdbuf on IOVMM, may not work for large gathers!\n");
	}
	return 0;
}

static int nvmap_write(uint32_t handle, uint32_t offset,
		       const void *data, uint32_t size) {
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
		perror("nvmap write"); return -1;
	}
	return 0;
}

static int nvmap_read(uint32_t handle, uint32_t offset,
		      void *data, uint32_t size) {
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
		perror("nvmap read"); return -1;
	}
	return 0;
}

static uint32_t nvmap_pin(uint32_t handle) {
	struct nvmap_pin_handle ph;
	ph.handles = handle;
	ph.addr = 0;
	ph.count = 1;
	if (ioctl(nvmap_fd, NVMAP_IOC_PIN_MULT, &ph) < 0) {
		perror("nvmap pin"); return 0;
	}
	return (uint32_t)ph.addr;
}

static void nvmap_free(uint32_t handle) {
	ioctl(nvmap_fd, NVMAP_IOC_FREE, handle);
}

/* ---- syncpt helpers ---- */
static int syncpt_wait(uint32_t id, uint32_t thresh, int timeout_ms) {
	struct nvhost_ctrl_syncpt_waitex_args a = {
		.id = id, .thresh = thresh, .timeout = timeout_ms,
	};
	return ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_WAITEX, &a);
}

/* ---- reloc definitions ---- */
struct nvhost_reloc {
	uint32_t cmdbuf_mem;
	uint32_t cmdbuf_offset;
	uint32_t target;
	uint32_t target_offset;
};
struct nvhost_reloc_shift { uint32_t shift; } __attribute__((packed));

#define MAX_RELOCS 16

/* ---- multi-gather submit (stock uses 6 gathers per frame) ---- */
#define MAX_GATHERS 8

struct gather_desc {
	uint32_t handle;
	uint32_t offset;   /* byte offset within handle */
	uint32_t words;
	uint32_t class_id; /* 0 = no SET_CLASS prefix */
};

static int isp_submit_multi(struct gather_desc *gathers, int num_gathers,
			    uint32_t syncpt_id, uint32_t syncpt_incrs,
			    struct nvhost_reloc *relocs,
			    struct nvhost_reloc_shift *reloc_shifts,
			    int num_relocs,
			    uint32_t *fence_out) {
	struct nvhost_cmdbuf cbs[MAX_GATHERS];
	uint32_t class_ids[MAX_GATHERS];
	struct nvhost_syncpt_incr si = {
		.syncpt_id = syncpt_id, .syncpt_incrs = syncpt_incrs,
	};
	struct nvhost_fence fence = { 0, 0 };

	for (int i = 0; i < num_gathers; i++) {
		cbs[i].mem = gathers[i].handle;
		cbs[i].offset = gathers[i].offset;
		cbs[i].words = gathers[i].words;
		class_ids[i] = gathers[i].class_id;
	}

	struct nvhost32_submit_args sa;
	memset(&sa, 0, sizeof(sa));
	sa.submit_version = 0;
	sa.num_syncpt_incrs = 1;
	sa.num_cmdbufs = num_gathers;
	sa.num_relocs = num_relocs;
	sa.num_waitchks = 0;
	sa.timeout = 2000;
	sa.syncpt_incrs = (uint32_t)(uintptr_t)&si;
	sa.cmdbufs = (uint32_t)(uintptr_t)cbs;
	sa.relocs = num_relocs ? (uint32_t)(uintptr_t)relocs : 0;
	sa.reloc_shifts = num_relocs ? (uint32_t)(uintptr_t)reloc_shifts : 0;
	sa.class_ids = (uint32_t)(uintptr_t)class_ids;
	sa.fences = (uint32_t)(uintptr_t)&fence;

	if (ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa) < 0) {
		perror("submit_multi");
		return -1;
	}
	if (fence_out) *fence_out = fence.value;
	return 0;
}
static int isp_submit(uint32_t cmdbuf_h, uint32_t num_words,
		      uint32_t syncpt_id, uint32_t class_id,
		      struct nvhost_reloc *relocs,
		      struct nvhost_reloc_shift *reloc_shifts,
		      int num_relocs,
		      uint32_t *fence_out) {
	struct nvhost_cmdbuf cb = {
		.mem = cmdbuf_h, .offset = 0, .words = num_words,
	};
	struct nvhost_syncpt_incr si = {
		.syncpt_id = syncpt_id, .syncpt_incrs = 1,
	};
	struct nvhost_fence fence = { 0, 0 };
	uint32_t cid = class_id;

	struct nvhost32_submit_args sa;
	memset(&sa, 0, sizeof(sa));
	sa.submit_version = 0;
	sa.num_syncpt_incrs = 1;
	sa.num_cmdbufs = 1;
	sa.num_relocs = num_relocs;
	sa.num_waitchks = 0;
	sa.timeout = 2000;
	sa.syncpt_incrs = (uint32_t)(uintptr_t)&si;
	sa.cmdbufs = (uint32_t)(uintptr_t)&cb;
	sa.relocs = num_relocs ? (uint32_t)(uintptr_t)relocs : 0;
	sa.reloc_shifts = num_relocs ? (uint32_t)(uintptr_t)reloc_shifts : 0;
	sa.class_ids = (uint32_t)(uintptr_t)&cid;
	sa.fences = (uint32_t)(uintptr_t)&fence;

	if (ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa) < 0) {
		perror("submit");
		return -1;
	}
	if (fence_out) *fence_out = fence.value;
	return 0;
}

/* Convenience: submit without relocs */
static int isp_submit_simple(uint32_t cmdbuf_h, uint32_t num_words,
			     uint32_t syncpt_id, uint32_t class_id,
			     uint32_t *fence_out) {
	return isp_submit(cmdbuf_h, num_words, syncpt_id, class_id,
			  NULL, NULL, 0, fence_out);
}

/* ---- time helper ---- */
static long elapsed_us(struct timespec *t0, struct timespec *t1) {
	return (t1->tv_sec - t0->tv_sec) * 1000000L +
	       (t1->tv_nsec - t0->tv_nsec) / 1000;
}

/* ---- check buffer against sentinel ---- */
static int check_sentinel(uint32_t handle, uint32_t offset, int size,
			  const char *label) {
	uint8_t *buf = malloc(size);
	if (!buf) { printf("  %s: malloc failed\n", label); return -1; }

	if (nvmap_read(handle, offset, buf, size) < 0) {
		free(buf); return -1;
	}

	int changed = 0;
	for (int i = 0; i < size; i++) {
		if (buf[i] != SENTINEL) changed++;
	}
	printf("  %s: %d/%d bytes changed from 0x%02X", label, changed, size, SENTINEL);
	if (changed > 0) {
		printf(" (ISP WROTE!)  hex[0..31]: ");
		for (int i = 0; i < 32 && i < size; i++)
			printf("%02x ", buf[i]);
	}
	printf("\n");

	free(buf);
	return changed;
}

/* ---- fill buffer with sentinel via nvmap_write ---- */
static int fill_sentinel(uint32_t handle, uint32_t offset, int size) {
	int chunk = 65536;
	uint8_t *buf = malloc(chunk);
	if (!buf) return -1;
	memset(buf, SENTINEL, chunk);

	for (int off = 0; off < size; off += chunk) {
		int sz = (size - off < chunk) ? size - off : chunk;
		if (nvmap_write(handle, offset + off, buf, sz) < 0) {
			free(buf); return -1;
		}
	}
	free(buf);
	return 0;
}

/* ---- build cmdbuf ---- */
/*
 * Stock ISP submit has 6 gathers per frame:
 *  1. syncpt incr
 *  2. output config + surfaces + input + trigger
 *  3. syncpt incr
 *  4. syncpt wait (for VI)
 *  5. syncpt incr
 *  6. calibration block
 *
 * For our standalone test (no VI), we combine into:
 *  Submit 1 (init): calibration + init regs + trigger 0x0F + syncpt
 *  Submit 2 (frame): output config + surfaces + input + trigger 0x05 + syncpt
 *
 * Additionally, we program missing init registers from stock MMIO dump
 * that NvIspSetConfiguration normally sets before any frame processing.
 */

static int build_init_cmdbuf(uint32_t *cmd, int max_words,
			     uint32_t class_id, uint32_t syncpt_id,
			     uint32_t work_iova,
			     const char *cal_path) {
	int n = 0;

	/* SET_CLASS */
	cmd[n++] = h1x_setclass(class_id, 0, 0);

	/* Load calibration from file (stock binary blob) */
	FILE *cal = fopen(cal_path, "rb");
	if (cal) {
		/* Skip first word if it's SET_CLASS (calibration files include it) */
		uint32_t first;
		int nread = fread(&first, 4, 1, cal);
		if (nread == 1 && (first >> 28) == 0) {
			/* First word is SET_CLASS — skip it, we already set class */
			printf("  calibration: skipping SET_CLASS header\n");
		} else {
			/* Not SET_CLASS — include it */
			cmd[n++] = first;
		}
		/* Read rest of calibration */
		nread = fread(&cmd[n], 4, max_words - n - 50, cal);
		fclose(cal);
		printf("  calibration: loaded %d words from %s\n", nread, cal_path);
		n += nread;

		/* Patch work buffer address: last word of calibration
		 * is stock IOVA for method 0x054 — replace with ours */
		if (n >= 2 && work_iova) {
			/* Find INCR(0x053, 2) and patch word after enable */
			for (int i = 0; i < n - 1; i++) {
				if (cmd[i] == h1x_incr(0x053, 2)) {
					printf("  patching work buf @ [%d+2]: "
					       "0x%08x -> 0x%08x\n",
					       i, cmd[i + 2], work_iova);
					cmd[i + 2] = work_iova;
					break;
				}
			}
		}
	} else {
		printf("  WARNING: no calibration file %s\n", cal_path);
	}

	/*
	 * Missing init registers from stock MMIO dump.
	 * These are set by NvIspSetConfiguration before first frame
	 * and never appear in per-frame cmdbuf.
	 * Write ALL non-zero stock MMIO registers.
	 */
	cmd[n++] = h1x_setclass(class_id, 0, 0);

	/* 0x008 = 0xF000F800 — input config */
	cmd[n++] = h1x_incr(0x008, 1);
	cmd[n++] = 0xF000F800;

	/* 0x00D = 0x00000100 — status (may be read-only, write anyway) */
	cmd[n++] = h1x_incr(0x00D, 1);
	cmd[n++] = 0x00000100;

	/* 0x014 = sensor-specific param */
	cmd[n++] = h1x_incr(0x014, 1);
	cmd[n++] = (class_id == 0x32) ? 0x00000339 : 0x0000019B;

	/* 0x015 = 0x04040007 — ISP enable + mode */
	cmd[n++] = h1x_incr(0x015, 1);
	cmd[n++] = 0x04040007;

	/* 0x018-0x01C — processing params */
	cmd[n++] = h1x_incr(0x018, 5);
	cmd[n++] = 0x0A00500A;  /* 0x018 */
	cmd[n++] = 0x00008089;  /* 0x019 */
	cmd[n++] = 0x013645CB;  /* 0x01A */
	cmd[n++] = 0x000001E7;  /* 0x01B */
	cmd[n++] = 0x00000001;  /* 0x01C */

	/* 0x01D = ISP_CG_CTRL */
	cmd[n++] = h1x_incr(0x01D, 1);
	cmd[n++] = 0x00000001;

	/* 0x01F = mode (1 for ISP-A, 3 for ISP-B per stock MMIO) */
	cmd[n++] = h1x_incr(0x01F, 1);
	cmd[n++] = (class_id == 0x32) ? 0x00000001 : 0x00000003;

	/* 0x024-0x026 — unknown, constant on stock (may be hash/signature) */
	cmd[n++] = h1x_incr(0x024, 3);
	cmd[n++] = 0xC6BFF67C;  /* 0x024 — same for A and B */
	cmd[n++] = 0x70C9A9EA;  /* 0x025 — per-session on stock */
	cmd[n++] = 0x33894D2B;  /* 0x026 — per-session on stock */

	/* 0x028-0x02A — unknown, all 0x07 on stock ISP-A */
	cmd[n++] = h1x_incr(0x028, 3);
	cmd[n++] = 0x00000007;  /* 0x028 */
	cmd[n++] = 0x00000007;  /* 0x029 */
	cmd[n++] = 0x00000007;  /* 0x02A — ISP-B has 0x07 too */

	/* 0x038 — unknown constant */
	cmd[n++] = h1x_incr(0x038, 1);
	cmd[n++] = 0x242CB07B;

	/* 0x03B — per-sensor */
	cmd[n++] = h1x_incr(0x03B, 1);
	cmd[n++] = (class_id == 0x32) ? 0x017BAD37 : 0x00100900;

	/* 0x03F — unknown */
	cmd[n++] = h1x_incr(0x03F, 1);
	cmd[n++] = 0x00000020;

	/* 0x051 — unknown */
	cmd[n++] = h1x_incr(0x051, 1);
	cmd[n++] = 0x017BA537;

	/* 0x05E-0x05F */
	cmd[n++] = h1x_incr(0x05E, 2);
	cmd[n++] = 0x00003232;  /* 0x05E */
	cmd[n++] = 0x00000010;  /* 0x05F — not written before */

	/* Trigger 0x0F — static config apply */
	cmd[n++] = h1x_nonincr(0x00C, 1);
	cmd[n++] = 0x0000000F;

	/* Syncpt OP_DONE */
	cmd[n++] = h1x_imm_incr_syncpt(1 /* OP_DONE */, syncpt_id);
	cmd[n++] = NOOP;

	return n;
}

static int build_frame_cmdbuf(uint32_t *cmd, int max_words,
			      uint32_t class_id, uint32_t syncpt_id,
			      int W, int H,
			      uint32_t in_iova,
			      uint32_t out_y_iova, uint32_t out_u_iova,
			      uint32_t out_v_iova,
			      uint32_t trigger) {
	int Y_STRIDE = (W + 63) & ~63;
	int UV_STRIDE = Y_STRIDE / 2;
	int n = 0;

	cmd[n++] = h1x_setclass(class_id, 0, 0);

	/* Output dimensions (0x31C0-like block) */
	cmd[n++] = h1x_incr(0xE00, 1);
	cmd[n++] = ((W - 1) & 0x3FFF) << 16;
	cmd[n++] = h1x_incr(0xE01, 1);
	cmd[n++] = ((H - 1) & 0x3FFF) << 16;

	/* Output format */
	cmd[n++] = h1x_incr(0xE02, 1);
	cmd[n++] = 0x04FE00E6;
	cmd[n++] = h1x_incr(0xE03, 1);
	cmd[n++] = 0x00000000;

	/* Output surface Y: [IOVA, 0, stride] */
	cmd[n++] = h1x_incr(0xE04, 3);
	cmd[n++] = out_y_iova;
	cmd[n++] = 0x00000000;
	cmd[n++] = Y_STRIDE;

	/* Output surface U */
	cmd[n++] = h1x_incr(0xE07, 3);
	cmd[n++] = out_u_iova;
	cmd[n++] = 0x00000000;
	cmd[n++] = UV_STRIDE;

	/* Output surface V */
	cmd[n++] = h1x_incr(0xE0A, 3);
	cmd[n++] = out_v_iova;
	cmd[n++] = 0x00000000;
	cmd[n++] = UV_STRIDE;

	/* Processing: 5 zeros + (H << 16) | W */
	cmd[n++] = h1x_incr(0x500, 6);
	cmd[n++] = 0;
	cmd[n++] = 0;
	cmd[n++] = 0;
	cmd[n++] = 0;
	cmd[n++] = 0;
	cmd[n++] = (H << 16) | W;

	/* Input buffer */
	cmd[n++] = h1x_setclass(class_id, 0, 0);
	cmd[n++] = h1x_incr(0x100, 4);
	cmd[n++] = in_iova;
	cmd[n++] = 0;
	cmd[n++] = 0;
	cmd[n++] = 0;

	/* 0x60D8-like block: secondary output config */
	cmd[n++] = h1x_incr(0xE31, 1);
	cmd[n++] = W | (H << 16);
	cmd[n++] = h1x_incr(0xE33, 1);
	cmd[n++] = 0x04FE00E6;
	cmd[n++] = h1x_incr(0xE32, 1);
	cmd[n++] = Y_STRIDE;
	cmd[n++] = h1x_incr(0xE30, 1);
	cmd[n++] = 0x00000001;

	/* Stock has 3 syncpt incrs before trigger:
	 * NONINCR(0x000, 1) → syncpt_id | (cond << 8)
	 * Conditions: 0x4=OP_DONE, 0x5=???, 0x6=???
	 * These may be required to arm ISP DMA engine */
	cmd[n++] = h1x_setclass(class_id, 0, 0);
	cmd[n++] = h1x_nonincr(0x000, 1);
	cmd[n++] = (4 << 8) | syncpt_id;  /* cond 4 = OP_DONE */
	cmd[n++] = h1x_nonincr(0x000, 1);
	cmd[n++] = (5 << 8) | syncpt_id;  /* cond 5 */
	cmd[n++] = h1x_nonincr(0x000, 1);
	cmd[n++] = (6 << 8) | syncpt_id;  /* cond 6 */

	/* Control trigger */
	cmd[n++] = h1x_setclass(class_id, 0, 0);
	cmd[n++] = h1x_nonincr(0x00C, 1);
	cmd[n++] = trigger;

	/* Syncpt OP_DONE */
	cmd[n++] = h1x_imm_incr_syncpt(1 /* OP_DONE */, syncpt_id);
	cmd[n++] = NOOP;

	return n;
}

/* ---- main ---- */
int main(int argc, char **argv)
{
	int use_ispb = 0;
	if (argc > 1 && argv[1][0] == 'b')
		use_ispb = 1;

	uint32_t class_id = use_ispb ? 0x34 : 0x32;
	const char *isp_dev = use_ispb ? "/dev/nvhost-isp.1" : "/dev/nvhost-isp";
	const char *cal_path = use_ispb ? "/data/local/tmp/isp_cal_b.bin"
					: "/data/local/tmp/isp_cal.bin";
	int W, H;
	if (use_ispb) { W = 2592; H = 1944; }
	else          { W = 3280; H = 2460; }

	int Y_STRIDE = (W + 63) & ~63;
	int UV_STRIDE = Y_STRIDE / 2;
	int Y_SIZE = Y_STRIDE * H;
	int UV_SIZE = UV_STRIDE * (H / 2);
	int OUT_SIZE = Y_SIZE + UV_SIZE * 2;
	int IN_SIZE = W * H * 2;   /* RAW Bayer 16bpp */
	int WORK_SIZE = 512 * 1024; /* 512KB work buffer */

	printf("=== ISP DMA Test ===\n");
	printf("ISP: %s (class 0x%02x)\n", use_ispb ? "ISP-B" : "ISP-A", class_id);
	printf("Dimensions: %dx%d, Y_STRIDE=%d UV_STRIDE=%d\n", W, H, Y_STRIDE, UV_STRIDE);
	printf("Buffers: in=%dKB out=%dKB work=%dKB\n",
	       IN_SIZE / 1024, OUT_SIZE / 1024, WORK_SIZE / 1024);

	/* Open devices */
	nvmap_fd = open("/dev/nvmap", O_RDWR);
	if (nvmap_fd < 0) { perror("open nvmap"); return 1; }

	isp_fd = open(isp_dev, O_RDWR);
	if (isp_fd < 0) { perror("open isp"); return 1; }

	ctrl_fd = open("/dev/nvhost-ctrl", O_RDWR);
	if (ctrl_fd < 0) { perror("open ctrl"); return 1; }

	/* Set nvmap fd for ISP channel */
	struct nvhost_set_nvmap_fd_args nfa = { .fd = nvmap_fd };
	if (ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD, &nfa) < 0)
		perror("set nvmap fd (non-fatal)");

	/* Get syncpoint */
	struct nvhost_get_param_arg gpa = { .param = 0 };
	if (ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gpa) < 0) {
		perror("get syncpt"); return 1;
	}
	uint32_t syncpt_id = gpa.value;
	printf("Syncpoint: %u\n", syncpt_id);

	/* Allocate nvmap buffers */
	uint32_t cmdbuf_h = nvmap_create(32768);  /* 8 pages, enough for cal+frame */
	uint32_t in_h     = nvmap_create(IN_SIZE);
	uint32_t out_h    = nvmap_create(OUT_SIZE);
	uint32_t work_h   = nvmap_create(WORK_SIZE);
	if (!cmdbuf_h || !in_h || !out_h || !work_h) {
		printf("ERROR: nvmap_create failed\n");
		return 1;
	}

	if (nvmap_alloc_contig(cmdbuf_h, 4096) < 0 ||
	    nvmap_alloc(in_h, 4096) < 0 ||
	    nvmap_alloc(out_h, 4096) < 0 ||
	    nvmap_alloc(work_h, 4096) < 0) {
		printf("ERROR: nvmap_alloc failed\n");
		return 1;
	}

	/* Pin to get IOVAs */
	uint32_t in_iova   = nvmap_pin(in_h);
	uint32_t out_iova  = nvmap_pin(out_h);
	uint32_t work_iova = nvmap_pin(work_h);
	if (!in_iova || !out_iova || !work_iova) {
		printf("ERROR: nvmap_pin failed\n");
		return 1;
	}

	uint32_t out_y_iova = out_iova;
	uint32_t out_u_iova = out_iova + Y_SIZE;
	uint32_t out_v_iova = out_iova + Y_SIZE + UV_SIZE;

	printf("IOVAs: in=0x%08x out=0x%08x work=0x%08x\n",
	       in_iova, out_iova, work_iova);
	printf("  Y=0x%08x U=0x%08x V=0x%08x\n",
	       out_y_iova, out_u_iova, out_v_iova);

	/* Fill input with test pattern */
	{
		int chunk = 65536;
		uint8_t *buf = malloc(chunk);
		if (buf) {
			/* RGGB Bayer pattern */
			for (int i = 0; i < chunk; i++)
				buf[i] = (uint8_t)(i * 7 + 0x42);
			for (int off = 0; off < IN_SIZE; off += chunk) {
				int sz = (IN_SIZE - off < chunk) ? IN_SIZE - off : chunk;
				nvmap_write(in_h, off, buf, sz);
			}
			free(buf);
		}
		printf("Input: filled with test pattern\n");
	}

	/* Fill output + work with sentinel */
	fill_sentinel(out_h, 0, OUT_SIZE);
	fill_sentinel(work_h, 0, WORK_SIZE);
	printf("Output+work: filled with 0x%02X sentinel\n", SENTINEL);

	/* ==== MMIO PRE-INIT ====
	 * Some ISP registers (0x014, 0x01F, 0x024-0x02A) don't accept
	 * writes through host1x command buffers — they're pure MMIO regs.
	 * Write them directly via /dev/mem before any host1x submit.
	 */
	printf("\n--- MMIO pre-init ---\n");
	{
		int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
		if (mem_fd >= 0) {
			uint32_t isp_base = (class_id == 0x32) ? 0x54600000 : 0x54680000;
			void *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
					 MAP_SHARED, mem_fd, isp_base);
			if (map != MAP_FAILED) {
				volatile uint32_t *regs = (volatile uint32_t *)map;

				/* Regs that don't write via host1x cmdbuf */
				regs[0x014] = (class_id == 0x32) ? 0x00000339 : 0x0000019B;
				regs[0x01F] = (class_id == 0x32) ? 0x00000001 : 0x00000003;
				regs[0x024] = 0xC6BFF67C;
				regs[0x025] = 0x70C9A9EA;
				regs[0x026] = 0x33894D2B;
				regs[0x028] = 0x00000007;
				regs[0x029] = 0x00000007;
				regs[0x02A] = 0x00000007;

				/* Readback verify */
				printf("  0x014=%08x (want 0x339)\n", regs[0x014]);
				printf("  0x01F=%08x (want 0x001)\n", regs[0x01F]);
				printf("  0x024=%08x (want 0xC6BFF67C)\n", regs[0x024]);
				printf("  0x028=%08x (want 0x007)\n", regs[0x028]);

				munmap(map, 4096);
			} else {
				perror("mmap ISP for pre-init");
			}
			close(mem_fd);
		} else {
			printf("  (cannot open /dev/mem)\n");
		}
	}

	/* ==== PING TEST ==== */
	printf("\n--- Ping ---\n");
	{
		uint32_t ping_cmd[2];
		ping_cmd[0] = h1x_imm_incr_syncpt(0 /* immediate */, syncpt_id);
		ping_cmd[1] = NOOP;
		nvmap_write(cmdbuf_h, 0, ping_cmd, 8);

		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		uint32_t fence;
		if (isp_submit_simple(cmdbuf_h, 2, syncpt_id, class_id, &fence) < 0) {
			printf("Ping submit failed!\n");
			return 1;
		}
		int ret = syncpt_wait(syncpt_id, fence, 500);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		printf("Ping: %s (%ld us)\n", ret ? "TIMEOUT" : "OK",
		       elapsed_us(&t0, &t1));
		if (ret) return 1;
	}

	/* MMIO writes AFTER ping (ISP fully powered now) */
	printf("\n--- MMIO post-ping init ---\n");
	{
		int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
		if (mem_fd >= 0) {
			uint32_t isp_base = (class_id == 0x32) ? 0x54600000 : 0x54680000;
			void *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
					 MAP_SHARED, mem_fd, isp_base);
			if (map != MAP_FAILED) {
				volatile uint32_t *regs = (volatile uint32_t *)map;

				printf("  before: 0x014=%08x 0x01F=%08x 0x024=%08x 0x028=%08x\n",
				       regs[0x014], regs[0x01F], regs[0x024], regs[0x028]);

				regs[0x014] = (class_id == 0x32) ? 0x00000339 : 0x0000019B;
				regs[0x01F] = (class_id == 0x32) ? 0x00000001 : 0x00000003;
				regs[0x024] = 0xC6BFF67C;
				regs[0x025] = 0x70C9A9EA;
				regs[0x026] = 0x33894D2B;
				regs[0x028] = 0x00000007;
				regs[0x029] = 0x00000007;
				regs[0x02A] = 0x00000007;

				printf("  after:  0x014=%08x 0x01F=%08x 0x024=%08x 0x028=%08x\n",
				       regs[0x014], regs[0x01F], regs[0x024], regs[0x028]);

				munmap(map, 4096);
			}
			close(mem_fd);
		}
	}

	/* ==== SUBMIT 1: Init (calibration + MMIO init + trigger 0x0F) ==== */
	printf("\n--- Submit 1: Init (0x0F) ---\n");
	int init_work_reloc_offset = -1;  /* byte offset of work_buf IOVA in cmdbuf */
	{
		uint32_t cmd[4096];
		int n = build_init_cmdbuf(cmd, 4096, class_id, syncpt_id,
					  work_iova, cal_path);
		printf("  init cmdbuf: %d words\n", n);

		/* Find work_buf IOVA position for reloc */
		for (int i = 0; i < n; i++) {
			if (cmd[i] == work_iova && i > 0) {
				init_work_reloc_offset = i * 4;
				printf("  work_buf reloc at word %d (offset 0x%x)\n",
				       i, init_work_reloc_offset);
				break;
			}
		}

		if (nvmap_write(cmdbuf_h, 0, cmd, n * 4) < 0) {
			printf("cmdbuf write failed\n"); return 1;
		}

		/* Build relocs for init: work buffer */
		struct nvhost_reloc relocs[MAX_RELOCS];
		struct nvhost_reloc_shift shifts[MAX_RELOCS];
		int nr = 0;
		if (init_work_reloc_offset >= 0) {
			relocs[nr].cmdbuf_mem = cmdbuf_h;
			relocs[nr].cmdbuf_offset = init_work_reloc_offset;
			relocs[nr].target = work_h;
			relocs[nr].target_offset = 0;
			shifts[nr].shift = 0;
			nr++;
		}
		printf("  init relocs: %d\n", nr);

		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		uint32_t fence;
		if (isp_submit(cmdbuf_h, n, syncpt_id, class_id,
			       relocs, shifts, nr, &fence) < 0) {
			printf("Init submit FAILED\n"); return 1;
		}
		int ret = syncpt_wait(syncpt_id, fence, 2000);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		printf("  Init: %s (%ld us)\n", ret ? "TIMEOUT" : "OK",
		       elapsed_us(&t0, &t1));
		if (ret) {
			printf("  TIMEOUT on init, aborting\n");
			goto cleanup;
		}
	}

	/* Check if init changed work buffer */
	printf("\n--- After init (ISP should be powered) ---\n");
	check_sentinel(work_h, 0, 4096, "work[0..4K]");
	check_sentinel(out_h, 0, 4096, "output Y[0..4K]");

	/* MMIO dump while ISP is definitely still powered (fd open, just submitted) */
	{
		int mem_fd = open("/dev/mem", O_RDONLY | O_SYNC);
		if (mem_fd >= 0) {
			uint32_t isp_base = (class_id == 0x32) ? 0x54600000 : 0x54680000;
			void *map = mmap(NULL, 4096, PROT_READ, MAP_SHARED, mem_fd, isp_base);
			if (map != MAP_FAILED) {
				volatile uint32_t *regs = (volatile uint32_t *)map;
				printf("  MMIO after init:\n");
				for (int i = 0; i < 96; i++) {
					uint32_t v = regs[i];
					if (v != 0 && v != 0xFFFFFFFF)
						printf("    %03x = 0x%08X\n", i, v);
					else if (v == 0xFFFFFFFF)
						printf("    %03x = FFFFFFFF (power-gated?)\n", i);
				}
				munmap(map, 4096);
			}
			close(mem_fd);
		}
	}

	/* ==== SUBMIT 2: Frame (output + input + trigger 0x05) ==== */
	printf("\n--- Submit 2: Frame (0x05) ---\n");
	{
		uint32_t cmd[256];
		int n = build_frame_cmdbuf(cmd, 256, class_id, syncpt_id,
					   W, H, in_iova,
					   out_y_iova, out_u_iova, out_v_iova,
					   0x05);
		printf("  frame cmdbuf: %d words\n", n);

		/* Build relocs: find IOVA positions in cmdbuf */
		struct nvhost_reloc relocs[MAX_RELOCS];
		struct nvhost_reloc_shift shifts[MAX_RELOCS];
		int nr = 0;

		for (int i = 0; i < n; i++) {
			if (cmd[i] == out_y_iova) {
				relocs[nr].cmdbuf_mem = cmdbuf_h;
				relocs[nr].cmdbuf_offset = i * 4;
				relocs[nr].target = out_h;
				relocs[nr].target_offset = 0;
				shifts[nr].shift = 0;
				nr++;
			} else if (cmd[i] == out_u_iova) {
				relocs[nr].cmdbuf_mem = cmdbuf_h;
				relocs[nr].cmdbuf_offset = i * 4;
				relocs[nr].target = out_h;
				relocs[nr].target_offset = Y_SIZE;
				shifts[nr].shift = 0;
				nr++;
			} else if (cmd[i] == out_v_iova) {
				relocs[nr].cmdbuf_mem = cmdbuf_h;
				relocs[nr].cmdbuf_offset = i * 4;
				relocs[nr].target = out_h;
				relocs[nr].target_offset = Y_SIZE + UV_SIZE;
				shifts[nr].shift = 0;
				nr++;
			} else if (cmd[i] == in_iova) {
				relocs[nr].cmdbuf_mem = cmdbuf_h;
				relocs[nr].cmdbuf_offset = i * 4;
				relocs[nr].target = in_h;
				relocs[nr].target_offset = 0;
				shifts[nr].shift = 0;
				nr++;
			}
		}
		printf("  frame relocs: %d\n", nr);

		if (nvmap_write(cmdbuf_h, 0, cmd, n * 4) < 0) {
			printf("cmdbuf write failed\n"); return 1;
		}

		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		uint32_t fence;
		if (isp_submit(cmdbuf_h, n, syncpt_id, class_id,
			       relocs, shifts, nr, &fence) < 0) {
			printf("Frame submit FAILED\n"); goto cleanup;
		}
		int ret = syncpt_wait(syncpt_id, fence, 2000);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		printf("  Frame: %s (%ld us)\n", ret ? "TIMEOUT" : "OK",
		       elapsed_us(&t0, &t1));
	}

	/* ==== SUBMIT 3: Second frame (pipeline warmup) ==== */
	printf("\n--- Submit 3: Frame 2 (0x05) ---\n");
	{
		/* Re-fill output with sentinel for clean check */
		fill_sentinel(out_h, 0, 4096);

		uint32_t cmd[256];
		int n = build_frame_cmdbuf(cmd, 256, class_id, syncpt_id,
					   W, H, in_iova,
					   out_y_iova, out_u_iova, out_v_iova,
					   0x05);

		/* Same relocs as submit 2 */
		struct nvhost_reloc relocs[MAX_RELOCS];
		struct nvhost_reloc_shift shifts[MAX_RELOCS];
		int nr = 0;

		for (int i = 0; i < n; i++) {
			if (cmd[i] == out_y_iova) {
				relocs[nr].cmdbuf_mem = cmdbuf_h;
				relocs[nr].cmdbuf_offset = i * 4;
				relocs[nr].target = out_h;
				relocs[nr].target_offset = 0;
				shifts[nr].shift = 0;
				nr++;
			} else if (cmd[i] == out_u_iova) {
				relocs[nr].cmdbuf_mem = cmdbuf_h;
				relocs[nr].cmdbuf_offset = i * 4;
				relocs[nr].target = out_h;
				relocs[nr].target_offset = Y_SIZE;
				shifts[nr].shift = 0;
				nr++;
			} else if (cmd[i] == out_v_iova) {
				relocs[nr].cmdbuf_mem = cmdbuf_h;
				relocs[nr].cmdbuf_offset = i * 4;
				relocs[nr].target = out_h;
				relocs[nr].target_offset = Y_SIZE + UV_SIZE;
				shifts[nr].shift = 0;
				nr++;
			} else if (cmd[i] == in_iova) {
				relocs[nr].cmdbuf_mem = cmdbuf_h;
				relocs[nr].cmdbuf_offset = i * 4;
				relocs[nr].target = in_h;
				relocs[nr].target_offset = 0;
				shifts[nr].shift = 0;
				nr++;
			}
		}

		if (nvmap_write(cmdbuf_h, 0, cmd, n * 4) < 0) {
			printf("cmdbuf write failed\n"); return 1;
		}

		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		uint32_t fence;
		if (isp_submit(cmdbuf_h, n, syncpt_id, class_id,
			       relocs, shifts, nr, &fence) < 0) {
			printf("Frame 2 submit FAILED\n"); goto cleanup;
		}
		int ret = syncpt_wait(syncpt_id, fence, 2000);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		printf("  Frame 2: %s (%ld us)\n", ret ? "TIMEOUT" : "OK",
		       elapsed_us(&t0, &t1));
	}

	/* ==== SUBMIT 4: Combined (cal + init_regs + output + trigger 0x0F, then 0x05) ==== */
	printf("\n--- Submit 4: Combined single submit ---\n");
	{
		/* Re-fill output with sentinel */
		fill_sentinel(out_h, 0, OUT_SIZE > 4096 ? 4096 : OUT_SIZE);
		fill_sentinel(work_h, 0, 4096);

		uint32_t cmd[4096];
		/* First: build init part (calibration + init regs + trigger 0x0F) */
		int n = build_init_cmdbuf(cmd, 4096, class_id, syncpt_id,
					  work_iova, cal_path);
		/* Remove syncpt + noop at end (last 2 words) to continue */
		n -= 2;

		/* Then: add frame part (output + input + trigger 0x05) */
		int frame_start = n;
		uint32_t frame_cmd[256];
		int fn = build_frame_cmdbuf(frame_cmd, 256, class_id, syncpt_id,
					    W, H, in_iova,
					    out_y_iova, out_u_iova, out_v_iova,
					    0x05);
		memcpy(&cmd[n], frame_cmd, fn * 4);
		n += fn;

		printf("  combined cmdbuf: %d words\n", n);

		/* Build relocs for combined */
		struct nvhost_reloc relocs[MAX_RELOCS];
		struct nvhost_reloc_shift shifts[MAX_RELOCS];
		int nr = 0;

		for (int i = 0; i < n; i++) {
			if (cmd[i] == work_iova) {
				relocs[nr].cmdbuf_mem = cmdbuf_h;
				relocs[nr].cmdbuf_offset = i * 4;
				relocs[nr].target = work_h;
				relocs[nr].target_offset = 0;
				shifts[nr].shift = 0;
				nr++;
			} else if (cmd[i] == out_y_iova) {
				relocs[nr].cmdbuf_mem = cmdbuf_h;
				relocs[nr].cmdbuf_offset = i * 4;
				relocs[nr].target = out_h;
				relocs[nr].target_offset = 0;
				shifts[nr].shift = 0;
				nr++;
			} else if (cmd[i] == out_u_iova) {
				relocs[nr].cmdbuf_mem = cmdbuf_h;
				relocs[nr].cmdbuf_offset = i * 4;
				relocs[nr].target = out_h;
				relocs[nr].target_offset = Y_SIZE;
				shifts[nr].shift = 0;
				nr++;
			} else if (cmd[i] == out_v_iova) {
				relocs[nr].cmdbuf_mem = cmdbuf_h;
				relocs[nr].cmdbuf_offset = i * 4;
				relocs[nr].target = out_h;
				relocs[nr].target_offset = Y_SIZE + UV_SIZE;
				shifts[nr].shift = 0;
				nr++;
			} else if (cmd[i] == in_iova) {
				relocs[nr].cmdbuf_mem = cmdbuf_h;
				relocs[nr].cmdbuf_offset = i * 4;
				relocs[nr].target = in_h;
				relocs[nr].target_offset = 0;
				shifts[nr].shift = 0;
				nr++;
			}
		}
		printf("  combined relocs: %d\n", nr);

		if (nvmap_write(cmdbuf_h, 0, cmd, n * 4) < 0) {
			printf("cmdbuf write failed\n"); return 1;
		}

		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		uint32_t fence;
		/* 2 syncpt incrs: 1 from init trigger implicit, 1 from OP_DONE */
		if (isp_submit(cmdbuf_h, n, syncpt_id, class_id,
			       relocs, shifts, nr, &fence) < 0) {
			printf("Combined submit FAILED\n"); goto cleanup;
		}
		int ret = syncpt_wait(syncpt_id, fence, 2000);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		printf("  Combined: %s (%ld us)\n", ret ? "TIMEOUT" : "OK",
		       elapsed_us(&t0, &t1));

		check_sentinel(out_h, 0, 4096, "output Y[0..4K]");
		check_sentinel(work_h, 0, 4096, "work[0..4K]");
	}

	/* ==== SUBMIT 5: Stock-style 6-gather single job ==== */
	printf("\n--- Submit 5: Stock 6-gather layout ---\n");
	{
		/* Re-fill output + work with sentinel */
		fill_sentinel(out_h, 0, OUT_SIZE > 65536 ? 65536 : OUT_SIZE);
		fill_sentinel(work_h, 0, 4096);

		/*
		 * Stock per-frame layout: 6 gathers in one job
		 * G1: 2 words  — syncpt incr
		 * G2: 45 words — output + surfaces + input + trigger 0x05
		 * G3: 2 words  — syncpt incr
		 * G4: 8 words  — syncpt wait (for VI) — skip, use NOOPs
		 * G5: 2 words  — syncpt incr
		 * G6: ~1545 words — calibration + init regs + trigger 0x0F
		 *
		 * NOTE: stock sends calibration LAST (G6), trigger 0x05 BEFORE (G2)!
		 * This means ISP sees: frame config → trigger 0x05 → cal → trigger 0x0F
		 * The 0x0F at end applies config for NEXT frame.
		 */

		/* Pack all gathers into one cmdbuf handle at different offsets */
		uint32_t cmd[4096];
		int offsets[6], sizes[6];
		int pos = 0;

		/* G1: syncpt incr (host1x class) */
		offsets[0] = pos;
		cmd[pos++] = h1x_setclass(0x01, 0, 0); /* host1x class */
		cmd[pos++] = h1x_imm_incr_syncpt(1 /* OP_DONE */, syncpt_id);
		sizes[0] = pos - offsets[0];

		/* G2: output + surfaces + input + trigger 0x05 (stock 45 words) */
		offsets[1] = pos;
		cmd[pos++] = h1x_setclass(class_id, 0, 0);
		/* Output dimensions */
		cmd[pos++] = h1x_incr(0xE00, 1);
		cmd[pos++] = ((W - 1) & 0x3FFF) << 16;
		cmd[pos++] = h1x_incr(0xE01, 1);
		cmd[pos++] = ((H - 1) & 0x3FFF) << 16;
		cmd[pos++] = h1x_incr(0xE02, 1);
		cmd[pos++] = 0x04FE00E6;
		cmd[pos++] = h1x_incr(0xE03, 1);
		cmd[pos++] = 0x00000000;
		/* Y surface */
		cmd[pos++] = h1x_incr(0xE04, 3);
		int reloc_y_pos = pos;
		cmd[pos++] = out_y_iova;
		cmd[pos++] = 0;
		cmd[pos++] = Y_STRIDE;
		/* U surface */
		cmd[pos++] = h1x_incr(0xE07, 3);
		int reloc_u_pos = pos;
		cmd[pos++] = out_u_iova;
		cmd[pos++] = 0;
		cmd[pos++] = UV_STRIDE;
		/* V surface */
		cmd[pos++] = h1x_incr(0xE0A, 3);
		int reloc_v_pos = pos;
		cmd[pos++] = out_v_iova;
		cmd[pos++] = 0;
		cmd[pos++] = UV_STRIDE;
		/* Processing */
		cmd[pos++] = h1x_incr(0x500, 6);
		cmd[pos++] = 0; cmd[pos++] = 0; cmd[pos++] = 0;
		cmd[pos++] = 0; cmd[pos++] = 0;
		cmd[pos++] = (H << 16) | W;
		/* Input */
		cmd[pos++] = h1x_setclass(class_id, 0, 0);
		cmd[pos++] = h1x_incr(0x100, 4);
		int reloc_in_pos = pos;
		cmd[pos++] = in_iova;
		cmd[pos++] = 0; cmd[pos++] = 0; cmd[pos++] = 0;
		/* Syncpt incrs (stock has 3 before trigger) */
		cmd[pos++] = h1x_setclass(class_id, 0, 0);
		cmd[pos++] = h1x_nonincr(0x000, 1);
		cmd[pos++] = (4 << 8) | syncpt_id;
		cmd[pos++] = h1x_nonincr(0x000, 1);
		cmd[pos++] = (5 << 8) | syncpt_id;
		cmd[pos++] = h1x_nonincr(0x000, 1);
		cmd[pos++] = (6 << 8) | syncpt_id;
		/* Trigger 0x05 */
		cmd[pos++] = h1x_setclass(class_id, 0, 0);
		cmd[pos++] = h1x_nonincr(0x00C, 1);
		cmd[pos++] = 0x00000005;
		sizes[1] = pos - offsets[1];

		/* G3: syncpt incr */
		offsets[2] = pos;
		cmd[pos++] = h1x_setclass(0x01, 0, 0);
		cmd[pos++] = h1x_imm_incr_syncpt(1, syncpt_id);
		sizes[2] = pos - offsets[2];

		/* G4: syncpt wait — stock waits for VI, we skip */
		offsets[3] = pos;
		cmd[pos++] = NOOP;
		cmd[pos++] = NOOP;
		sizes[3] = pos - offsets[3];

		/* G5: syncpt incr */
		offsets[4] = pos;
		cmd[pos++] = h1x_setclass(0x01, 0, 0);
		cmd[pos++] = h1x_imm_incr_syncpt(1, syncpt_id);
		sizes[4] = pos - offsets[4];

		/* G6: calibration + init regs + trigger 0x0F */
		offsets[5] = pos;
		cmd[pos++] = h1x_setclass(class_id, 0, 0);
		/* Load cal */
		FILE *cal = fopen(cal_path, "rb");
		if (cal) {
			uint32_t first;
			fread(&first, 4, 1, cal);
			if ((first >> 28) == 0) { /* skip SET_CLASS */ }
			else cmd[pos++] = first;
			int nr = fread(&cmd[pos], 4, 3000, cal);
			fclose(cal);
			pos += nr;
			/* Patch work buf addr */
			for (int i = offsets[5]; i < pos - 1; i++) {
				if (cmd[i] == h1x_incr(0x053, 2)) {
					cmd[i + 2] = work_iova;
					break;
				}
			}
		}
		/* Init regs — ALL non-zero stock MMIO values */
		cmd[pos++] = h1x_setclass(class_id, 0, 0);
		cmd[pos++] = h1x_incr(0x008, 1); cmd[pos++] = 0xF000F800;
		cmd[pos++] = h1x_incr(0x00D, 1); cmd[pos++] = 0x00000100;
		cmd[pos++] = h1x_incr(0x014, 1); cmd[pos++] = 0x00000339;
		cmd[pos++] = h1x_incr(0x015, 1); cmd[pos++] = 0x04040007;
		cmd[pos++] = h1x_incr(0x018, 5);
		cmd[pos++] = 0x0A00500A; cmd[pos++] = 0x00008089;
		cmd[pos++] = 0x013645CB; cmd[pos++] = 0x000001E7;
		cmd[pos++] = 0x00000001;
		cmd[pos++] = h1x_incr(0x01D, 1); cmd[pos++] = 0x00000001;
		cmd[pos++] = h1x_incr(0x01F, 1); cmd[pos++] = 0x00000001;
		cmd[pos++] = h1x_incr(0x024, 3);
		cmd[pos++] = 0xC6BFF67C; cmd[pos++] = 0x70C9A9EA;
		cmd[pos++] = 0x33894D2B;
		cmd[pos++] = h1x_incr(0x028, 3);
		cmd[pos++] = 0x00000007; cmd[pos++] = 0x00000007;
		cmd[pos++] = 0x00000007;
		cmd[pos++] = h1x_incr(0x038, 1); cmd[pos++] = 0x242CB07B;
		cmd[pos++] = h1x_incr(0x03B, 1); cmd[pos++] = 0x017BAD37;
		cmd[pos++] = h1x_incr(0x03F, 1); cmd[pos++] = 0x00000020;
		cmd[pos++] = h1x_incr(0x051, 1); cmd[pos++] = 0x017BA537;
		cmd[pos++] = h1x_incr(0x05E, 2);
		cmd[pos++] = 0x00003232; cmd[pos++] = 0x00000010;
		/* Secondary output config */
		cmd[pos++] = h1x_incr(0xE31, 1); cmd[pos++] = W | (H << 16);
		cmd[pos++] = h1x_incr(0xE33, 1); cmd[pos++] = 0x04FE00E6;
		cmd[pos++] = h1x_incr(0xE32, 1); cmd[pos++] = Y_STRIDE;
		cmd[pos++] = h1x_incr(0xE30, 1); cmd[pos++] = 0x00000001;
		/* Trigger 0x0F */
		cmd[pos++] = h1x_nonincr(0x00C, 1); cmd[pos++] = 0x0000000F;
		/* Final syncpt */
		cmd[pos++] = h1x_imm_incr_syncpt(1, syncpt_id);
		cmd[pos++] = NOOP;
		sizes[5] = pos - offsets[5];

		printf("  total: %d words, gathers: ", pos);
		for (int i = 0; i < 6; i++)
			printf("G%d=%d ", i + 1, sizes[i]);
		printf("\n");

		/* Write to cmdbuf handle */
		if (nvmap_write(cmdbuf_h, 0, cmd, pos * 4) < 0) {
			printf("cmdbuf write failed\n"); goto cleanup;
		}

		/* Build gathers */
		struct gather_desc gathers[6];
		for (int i = 0; i < 6; i++) {
			gathers[i].handle = cmdbuf_h;
			gathers[i].offset = offsets[i] * 4;
			gathers[i].words = sizes[i];
			gathers[i].class_id = class_id;
		}
		/* G1, G3, G5 are syncpt — host1x class, no ISP SET_CLASS */
		gathers[0].class_id = 0;
		gathers[2].class_id = 0;
		gathers[3].class_id = 0; /* NOOP */
		gathers[4].class_id = 0;

		/* Relocs for G2 (output + input) */
		struct nvhost_reloc relocs[MAX_RELOCS];
		struct nvhost_reloc_shift shifts[MAX_RELOCS];
		int rnr = 0;

		relocs[rnr].cmdbuf_mem = cmdbuf_h;
		relocs[rnr].cmdbuf_offset = reloc_y_pos * 4;
		relocs[rnr].target = out_h;
		relocs[rnr].target_offset = 0;
		shifts[rnr].shift = 0; rnr++;

		relocs[rnr].cmdbuf_mem = cmdbuf_h;
		relocs[rnr].cmdbuf_offset = reloc_u_pos * 4;
		relocs[rnr].target = out_h;
		relocs[rnr].target_offset = Y_SIZE;
		shifts[rnr].shift = 0; rnr++;

		relocs[rnr].cmdbuf_mem = cmdbuf_h;
		relocs[rnr].cmdbuf_offset = reloc_v_pos * 4;
		relocs[rnr].target = out_h;
		relocs[rnr].target_offset = Y_SIZE + UV_SIZE;
		shifts[rnr].shift = 0; rnr++;

		relocs[rnr].cmdbuf_mem = cmdbuf_h;
		relocs[rnr].cmdbuf_offset = reloc_in_pos * 4;
		relocs[rnr].target = in_h;
		relocs[rnr].target_offset = 0;
		shifts[rnr].shift = 0; rnr++;

		/* work buf reloc in G6 */
		for (int i = offsets[5]; i < pos - 1; i++) {
			if (cmd[i] == work_iova) {
				relocs[rnr].cmdbuf_mem = cmdbuf_h;
				relocs[rnr].cmdbuf_offset = i * 4;
				relocs[rnr].target = work_h;
				relocs[rnr].target_offset = 0;
				shifts[rnr].shift = 0; rnr++;
				break;
			}
		}
		printf("  relocs: %d\n", rnr);

		/* syncpt_incrs: G1(1) + G2(3 cond) + G3(1) + G5(1) + G6(1) = 7
		 * But ISP cond incrs (4/5/6) may not fire if DMA doesn't start.
		 * Try 7 first, fallback to 4 (OP_DONE only), then 1.
		 * Short timeout to avoid hanging device. */
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		uint32_t fence;
		if (isp_submit_multi(gathers, 6, syncpt_id, 4,
				     relocs, shifts, rnr, &fence) < 0) {
			printf("Stock-style submit FAILED\n");
			goto cleanup;
		}
		int ret = syncpt_wait(syncpt_id, fence, 500);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		printf("  Stock-style: %s (%ld us)\n",
		       ret ? "TIMEOUT" : "OK", elapsed_us(&t0, &t1));

		check_sentinel(out_h, 0, 4096, "output Y[0..4K]");
		check_sentinel(work_h, 0, 4096, "work[0..4K]");
	}

	/* ==== Results ==== */
	printf("\n--- Results ---\n");
	check_sentinel(out_h, 0, 4096, "output Y[0..4K]");
	check_sentinel(out_h, Y_SIZE, 4096, "output U[0..4K]");
	check_sentinel(out_h, Y_SIZE + UV_SIZE, 4096, "output V[0..4K]");
	check_sentinel(work_h, 0, 4096, "work[0..4K]");

	/* Stats region in input buffer at +0x20000 */
	/* First fill input stats area with sentinel, then resubmit */
	printf("\n--- Stats check (in_buf+0x20000) ---\n");
	{
		uint8_t stats_buf[256];
		if (nvmap_read(in_h, 0x20000, stats_buf, sizeof(stats_buf)) == 0) {
			int nonzero = 0;
			for (int i = 0; i < (int)sizeof(stats_buf); i++)
				if (stats_buf[i] != 0x42 && stats_buf[i] != 0)
					nonzero++;
			printf("  stats header: %02x %02x %02x %02x  %02x %02x %02x %02x\n",
			       stats_buf[0], stats_buf[1], stats_buf[2], stats_buf[3],
			       stats_buf[4], stats_buf[5], stats_buf[6], stats_buf[7]);
		}
	}

	/* Dump cmdbuf words for debugging */
	/* Dump cmdbuf after submit to see what kernel relocs wrote */
	printf("\n--- Cmdbuf after submit (reloc-patched) ---\n");
	{
		uint32_t dump[64];
		/* Read last frame cmdbuf (submit 4 combined) — it has relocs */
		if (nvmap_read(cmdbuf_h, 0, dump, sizeof(dump)) == 0) {
			printf("  First 32 words (calibration start):\n");
			for (int i = 0; i < 32; i++)
				printf("  [%03d] 0x%08x\n", i, dump[i]);
		}
		/* Read around output surface area — need to find word offset */
		/* Combined: cal=1566 words, then frame starts at ~1566 */
		int frame_off = 1566 * 4;  /* approximate */
		if (nvmap_read(cmdbuf_h, frame_off, dump, 64 * 4) == 0) {
			printf("  Frame region (offset %d):\n", frame_off);
			for (int i = 0; i < 64; i++)
				printf("  [%03d] 0x%08x%s\n", i, dump[i],
				       (dump[i] == out_y_iova) ? " ← out_Y baked" :
				       (dump[i] == out_u_iova) ? " ← out_U baked" :
				       (dump[i] == out_v_iova) ? " ← out_V baked" :
				       (dump[i] == in_iova) ? " ← in baked" :
				       (dump[i] == work_iova) ? " ← work baked" : "");
		}
	}

	printf("\n--- Init cmdbuf first 32 words ---\n");
	{
		uint32_t dump[32];
		if (nvmap_read(cmdbuf_h, 0, dump, sizeof(dump)) == 0) {
			for (int i = 0; i < 32; i++)
				printf("  [%03d] 0x%08x\n", i, dump[i]);
		}
	}

	/* Pause to allow MMIO reading while ISP is still powered */
	printf("\n--- ISP still powered, reading MMIO ---\n");
	{
		int mem_fd = open("/dev/mem", O_RDONLY | O_SYNC);
		if (mem_fd >= 0) {
			uint32_t isp_base = (class_id == 0x32) ? 0x54600000 : 0x54680000;
			void *map = mmap(NULL, 4096, PROT_READ, MAP_SHARED,
					 mem_fd, isp_base);
			if (map != MAP_FAILED) {
				volatile uint32_t *regs = (volatile uint32_t *)map;
				struct { int off; const char *name; } mmio[] = {
					{0x020/4, "0x008 input_cfg"},
					{0x030/4, "0x00C control"},
					{0x034/4, "0x00D status"},
					{0x050/4, "0x014 sensor_param"},
					{0x054/4, "0x015 enable"},
					{0x060/4, "0x018 proc0"},
					{0x064/4, "0x019 proc1"},
					{0x068/4, "0x01A cal0"},
					{0x06C/4, "0x01B cal1"},
					{0x070/4, "0x01C unk"},
					{0x074/4, "0x01D CG_CTRL"},
					{0x07C/4, "0x01F mode"},
					{0x14C/4, "0x053 ISP_EN"},
					{0x150/4, "0x054 work_buf"},
					{0x178/4, "0x05E unk2"},
				};
				for (int i = 0; i < (int)(sizeof(mmio)/sizeof(mmio[0])); i++) {
					printf("  MMIO %s = 0x%08X\n",
					       mmio[i].name, regs[mmio[i].off]);
				}
				munmap(map, 4096);
			} else {
				perror("mmap ISP MMIO");
			}
			close(mem_fd);
		} else {
			printf("  (cannot open /dev/mem for MMIO read)\n");
		}
	}

	/* Full MMIO dump — all 128 regs for comparison with stock */
	printf("\n--- Full MMIO dump (methods 0x000-0x07F) ---\n");
	{
		int mem_fd = open("/dev/mem", O_RDONLY | O_SYNC);
		if (mem_fd >= 0) {
			uint32_t isp_base = (class_id == 0x32) ? 0x54600000 : 0x54680000;
			void *map = mmap(NULL, 4096, PROT_READ, MAP_SHARED,
					 mem_fd, isp_base);
			if (map != MAP_FAILED) {
				volatile uint32_t *regs = (volatile uint32_t *)map;
				for (int i = 0; i < 128; i++) {
					uint32_t v = regs[i];
					if (v != 0)
						printf("  %03x %04x = 0x%08X\n",
						       i, i * 4, v);
				}
				munmap(map, 4096);
			}
			close(mem_fd);
		}
	}

cleanup:
	nvmap_free(cmdbuf_h);
	nvmap_free(in_h);
	nvmap_free(out_h);
	nvmap_free(work_h);
	close(isp_fd);
	close(ctrl_fd);
	close(nvmap_fd);
	return 0;
}
