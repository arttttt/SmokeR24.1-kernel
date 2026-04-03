/*
 * isp_test_raw.c — Feed RAW file to ISP on stock kernel
 *
 * Loads RAW Bayer from file, submits to ISP with stock calibration,
 * saves ISP output (YUV) to file.
 *
 * Build: arm-linux-gnueabihf-gcc -std=gnu99 -static -o isp_test_raw isp_test_raw.c -lrt
 * Usage: isp_test_raw <input.raw> <output.raw> [a|b]
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

/* nvhost/nvmap ioctls — same as isp_test.c */
#define NVHOST_IOCTL_MAGIC 'H'
#define NVMAP_IOC_MAGIC 'N'

struct nvhost_set_nvmap_fd_args { uint32_t fd; } __attribute__((packed));
struct nvhost_get_param_arg { uint32_t param; uint32_t value; };
struct nvhost_syncpt_incr { uint32_t syncpt_id; uint32_t syncpt_incrs; };
struct nvhost_cmdbuf { uint32_t mem; uint32_t offset; uint32_t words; } __attribute__((packed));
struct nvhost32_submit_args {
	uint32_t submit_version, num_syncpt_incrs, num_cmdbufs, num_relocs;
	uint32_t num_waitchks, timeout, syncpt_incrs, cmdbufs;
	uint32_t relocs, reloc_shifts, waitchks, waitbases, class_ids;
	uint32_t pad[2], fences, fence;
} __attribute__((packed));
struct nvhost_fence { uint32_t syncpt_id; uint32_t value; };
struct nvhost_reloc { uint32_t cmdbuf_mem, cmdbuf_offset, target, target_offset; };
struct nvhost_reloc_shift { uint32_t shift; } __attribute__((packed));

struct nvmap_create_handle { union { uint32_t id; uint32_t size; int32_t fd; }; uint32_t handle; };
struct nvmap_alloc_handle { uint32_t handle, heap_mask, flags, align; };
struct nvmap_rw_handle {
	unsigned long addr; uint32_t handle, offset, elem_size, hmem_stride, user_stride, count;
} __attribute__((packed));
struct nvmap_pin_handle { uint32_t handles; unsigned long addr; uint32_t count; };

struct nvhost_ctrl_syncpt_waitex_args { uint32_t id; uint32_t thresh; int32_t timeout; uint32_t value; };

#define NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD _IOW(NVHOST_IOCTL_MAGIC, 5, struct nvhost_set_nvmap_fd_args)
#define NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT _IOWR(NVHOST_IOCTL_MAGIC, 16, struct nvhost_get_param_arg)
#define NVHOST32_IOCTL_CHANNEL_SUBMIT _IOWR(NVHOST_IOCTL_MAGIC, 15, struct nvhost32_submit_args)
#define NVMAP_IOC_CREATE _IOWR(NVMAP_IOC_MAGIC, 0, struct nvmap_create_handle)
#define NVMAP_IOC_ALLOC _IOW(NVMAP_IOC_MAGIC, 3, struct nvmap_alloc_handle)
#define NVMAP_IOC_FREE _IO(NVMAP_IOC_MAGIC, 4)
#define NVMAP_IOC_WRITE _IOW(NVMAP_IOC_MAGIC, 6, struct nvmap_rw_handle)
#define NVMAP_IOC_READ _IOW(NVMAP_IOC_MAGIC, 7, struct nvmap_rw_handle)
#define NVMAP_IOC_PIN_MULT _IOWR(NVMAP_IOC_MAGIC, 10, struct nvmap_pin_handle)
#define NVHOST_IOCTL_CTRL_SYNCPT_WAITEX _IOWR('H', 6, struct nvhost_ctrl_syncpt_waitex_args)

#define NVMAP_HEAP_IOVMM (1 << 30)
#define NVMAP_HEAP_CARVEOUT (1 << 0)

static int nvmap_fd, isp_fd, ctrl_fd;

static inline uint32_t h1x_setclass(uint32_t c, uint32_t o, uint32_t m)
{ return (o<<16)|(c<<6)|m; }
static inline uint32_t h1x_incr(uint32_t o, uint32_t c) { return (1u<<28)|(o<<16)|c; }
static inline uint32_t h1x_nonincr(uint32_t o, uint32_t c) { return (2u<<28)|(o<<16)|c; }
static inline uint32_t h1x_imm(uint32_t o, uint32_t v) { return (4u<<28)|(o<<16)|v; }

static uint32_t nv_create(uint32_t sz) {
	struct nvmap_create_handle h = { .size = sz };
	return ioctl(nvmap_fd, NVMAP_IOC_CREATE, &h) < 0 ? 0 : h.handle;
}
static int nv_alloc(uint32_t h, uint32_t heap, uint32_t align) {
	struct nvmap_alloc_handle a = { .handle=h, .heap_mask=heap, .flags=2, .align=align };
	return ioctl(nvmap_fd, NVMAP_IOC_ALLOC, &a);
}
static int nv_write(uint32_t h, uint32_t off, const void *d, uint32_t sz) {
	struct nvmap_rw_handle r = { .addr=(unsigned long)d, .handle=h, .offset=off,
		.elem_size=sz, .hmem_stride=sz, .user_stride=sz, .count=1 };
	return ioctl(nvmap_fd, NVMAP_IOC_WRITE, &r);
}
static int nv_read(uint32_t h, uint32_t off, void *d, uint32_t sz) {
	struct nvmap_rw_handle r = { .addr=(unsigned long)d, .handle=h, .offset=off,
		.elem_size=sz, .hmem_stride=sz, .user_stride=sz, .count=1 };
	return ioctl(nvmap_fd, NVMAP_IOC_READ, &r);
}
static uint32_t nv_pin(uint32_t h) {
	struct nvmap_pin_handle p = { .handles=h, .addr=0, .count=1 };
	return ioctl(nvmap_fd, NVMAP_IOC_PIN_MULT, &p) < 0 ? 0 : (uint32_t)p.addr;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		printf("Usage: %s <input.raw> <output.raw> [a|b]\n", argv[0]);
		return 1;
	}
	const char *in_path = argv[1];
	const char *out_path = argv[2];
	int use_b = (argc > 3 && argv[3][0] == 'b');

	int W = use_b ? 2592 : 3280;
	int H = use_b ? 1944 : 2460;
	int Y_STRIDE = (W + 63) & ~63;
	int UV_STRIDE = Y_STRIDE / 2;
	int Y_SIZE = Y_STRIDE * H;
	int UV_SIZE = UV_STRIDE * (H / 2);
	int OUT_SIZE = Y_SIZE + UV_SIZE * 2;
	int IN_SIZE = W * H * 2;
	uint32_t class_id = use_b ? 0x34 : 0x32;
	const char *cal_path = use_b ? "/data/local/tmp/isp_cal_b.bin"
				     : "/data/local/tmp/isp_cal.bin";

	printf("ISP RAW processor: %dx%d %s\n", W, H, use_b ? "ISP-B" : "ISP-A");
	printf("Input: %s (%d bytes expected)\n", in_path, IN_SIZE);
	printf("Output: %s (%d bytes)\n", out_path, OUT_SIZE);

	nvmap_fd = open("/dev/nvmap", O_RDWR);
	isp_fd = open(use_b ? "/dev/nvhost-isp.1" : "/dev/nvhost-isp", O_RDWR);
	ctrl_fd = open("/dev/nvhost-ctrl", O_RDWR);
	if (nvmap_fd < 0 || isp_fd < 0 || ctrl_fd < 0) {
		perror("open"); return 1;
	}

	struct nvhost_set_nvmap_fd_args nfa = { .fd = nvmap_fd };
	ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD, &nfa);

	struct nvhost_get_param_arg gpa = { .param = 0 };
	ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gpa);
	uint32_t sp_id = gpa.value;
	printf("Syncpt: %u\n", sp_id);

	/* Allocate buffers */
	uint32_t cmd_h = nv_create(32768);
	uint32_t in_h = nv_create(IN_SIZE);
	uint32_t out_h = nv_create(OUT_SIZE);
	if (!cmd_h || !in_h || !out_h) { printf("create fail\n"); return 1; }

	nv_alloc(cmd_h, NVMAP_HEAP_CARVEOUT, 4096);
	nv_alloc(in_h, NVMAP_HEAP_IOVMM, 4096);
	nv_alloc(out_h, NVMAP_HEAP_IOVMM, 4096);

	uint32_t in_iova = nv_pin(in_h);
	uint32_t out_iova = nv_pin(out_h);
	printf("IOVAs: in=0x%08x out=0x%08x\n", in_iova, out_iova);

	/* Load RAW input */
	FILE *fp = fopen(in_path, "rb");
	if (!fp) { perror("open input"); return 1; }
	{
		int chunk = 65536;
		uint8_t *buf = malloc(chunk);
		int off = 0;
		while (off < IN_SIZE) {
			int rd = fread(buf, 1, chunk < (IN_SIZE-off) ? chunk : (IN_SIZE-off), fp);
			if (rd <= 0) break;
			nv_write(in_h, off, buf, rd);
			off += rd;
		}
		free(buf);
		printf("Loaded %d bytes from %s\n", off, in_path);
	}
	fclose(fp);

	/* Load calibration */
	uint32_t cmd[4096];
	int n = 0;
	FILE *cal = fopen(cal_path, "rb");
	if (cal) {
		n = fread(cmd, 4, 4000, cal);
		fclose(cal);
		printf("Calibration: %d words from %s\n", n, cal_path);
	} else {
		printf("No calibration!\n"); return 1;
	}

	/* Output config (stock per-frame block) */
	cmd[n++] = h1x_setclass(class_id, 0, 0);
	cmd[n++] = h1x_incr(0xE00, 1); cmd[n++] = ((W-1)&0x3FFF)<<16;
	cmd[n++] = h1x_incr(0xE01, 1); cmd[n++] = ((H-1)&0x3FFF)<<16;
	cmd[n++] = h1x_incr(0xE02, 1); cmd[n++] = 0x04FE00E6;
	cmd[n++] = h1x_incr(0xE03, 1); cmd[n++] = 0;
	/* Y */
	cmd[n++] = h1x_incr(0xE04, 3);
	int reloc_y = n; cmd[n++] = out_iova; cmd[n++] = 0; cmd[n++] = Y_STRIDE;
	/* U */
	cmd[n++] = h1x_incr(0xE07, 3);
	int reloc_u = n; cmd[n++] = out_iova + Y_SIZE; cmd[n++] = 0; cmd[n++] = UV_STRIDE;
	/* V */
	cmd[n++] = h1x_incr(0xE0A, 3);
	int reloc_v = n; cmd[n++] = out_iova + Y_SIZE + UV_SIZE; cmd[n++] = 0; cmd[n++] = UV_STRIDE;
	/* Processing */
	cmd[n++] = h1x_incr(0x500, 6);
	cmd[n++]=0; cmd[n++]=0; cmd[n++]=0; cmd[n++]=0; cmd[n++]=0;
	cmd[n++] = (H<<16)|W;
	/* Input */
	cmd[n++] = h1x_setclass(class_id, 0, 0);
	cmd[n++] = h1x_incr(0x100, 4);
	int reloc_in = n; cmd[n++] = in_iova; cmd[n++]=0; cmd[n++]=0; cmd[n++]=0;
	/* Secondary output */
	cmd[n++] = h1x_incr(0xE31, 1); cmd[n++] = W|(H<<16);
	cmd[n++] = h1x_incr(0xE33, 1); cmd[n++] = 0x04FE00E6;
	cmd[n++] = h1x_incr(0xE32, 1); cmd[n++] = Y_STRIDE;
	cmd[n++] = h1x_incr(0x015, 1); cmd[n++] = 0x00000007;
	cmd[n++] = h1x_incr(0xE30, 1); cmd[n++] = 1;
	/* Trigger */
	cmd[n++] = h1x_nonincr(0x00C, 1); cmd[n++] = 0x05;
	cmd[n++] = h1x_imm(0, (1<<8)|sp_id); /* OP_DONE syncpt */
	cmd[n++] = h1x_nonincr(0, 0); /* NOOP */

	printf("Cmdbuf: %d words\n", n);
	nv_write(cmd_h, 0, cmd, n * 4);

	/* Relocs */
	struct nvhost_reloc relocs[4];
	struct nvhost_reloc_shift shifts[4];
	int nr = 0;
	relocs[nr] = (struct nvhost_reloc){cmd_h, reloc_y*4, out_h, 0}; shifts[nr++].shift=0;
	relocs[nr] = (struct nvhost_reloc){cmd_h, reloc_u*4, out_h, Y_SIZE}; shifts[nr++].shift=0;
	relocs[nr] = (struct nvhost_reloc){cmd_h, reloc_v*4, out_h, Y_SIZE+UV_SIZE}; shifts[nr++].shift=0;
	relocs[nr] = (struct nvhost_reloc){cmd_h, reloc_in*4, in_h, 0}; shifts[nr++].shift=0;

	/* Submit */
	struct nvhost_cmdbuf cb = { .mem=cmd_h, .words=n };
	struct nvhost_syncpt_incr si = { .syncpt_id=sp_id, .syncpt_incrs=1 };
	struct nvhost_fence fence = {};
	uint32_t cid = class_id;
	struct nvhost32_submit_args sa;
	memset(&sa, 0, sizeof(sa));
	sa.num_syncpt_incrs=1; sa.num_cmdbufs=1; sa.num_relocs=nr; sa.timeout=5000;
	sa.syncpt_incrs=(uint32_t)(uintptr_t)&si;
	sa.cmdbufs=(uint32_t)(uintptr_t)&cb;
	sa.relocs=(uint32_t)(uintptr_t)relocs;
	sa.reloc_shifts=(uint32_t)(uintptr_t)shifts;
	sa.class_ids=(uint32_t)(uintptr_t)&cid;
	sa.fences=(uint32_t)(uintptr_t)&fence;

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	if (ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa) < 0) {
		perror("submit"); return 1;
	}

	struct nvhost_ctrl_syncpt_waitex_args wa = {
		.id=sp_id, .thresh=fence.value, .timeout=5000 };
	int ret = ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_WAITEX, &wa);

	clock_gettime(CLOCK_MONOTONIC, &t1);
	long us = (t1.tv_sec-t0.tv_sec)*1000000L + (t1.tv_nsec-t0.tv_nsec)/1000;
	printf("Submit: %s (%ld us)\n", ret ? "TIMEOUT" : "OK", us);

	/* Read and save output */
	fp = fopen(out_path, "wb");
	if (fp) {
		int chunk = 65536;
		uint8_t *buf = malloc(chunk);
		int off = 0, nonzero = 0;
		while (off < OUT_SIZE) {
			int sz = (OUT_SIZE-off < chunk) ? OUT_SIZE-off : chunk;
			nv_read(out_h, off, buf, sz);
			fwrite(buf, 1, sz, fp);
			if (off == 0) {
				for (int i = 0; i < (sz < 4096 ? sz : 4096); i++)
					if (buf[i]) nonzero++;
			}
			off += sz;
		}
		free(buf);
		fclose(fp);
		printf("Output: %d bytes saved, first 4K: %d/4096 non-zero\n", off, nonzero);
	}

	printf("Done!\n");
	close(isp_fd); close(ctrl_fd); close(nvmap_fd);
	return 0;
}
