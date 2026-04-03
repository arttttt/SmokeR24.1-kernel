/*
 * ISP Stable DMA Test — emulates real camera pipeline
 *
 * Phase 1: Init — calibration + static config + trigger 0x0F (once)
 * Phase 2: Frame — output surfaces + input + trigger 0x05 (per frame)
 *
 * Single process, single buffer set, ISP stays open.
 *
 * Build: armv7a-linux-androideabi19-clang -std=gnu99 -fPIE -pie -o isp_stable_test isp_stable_test.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <time.h>

/* nvhost */
#define NVHOST_IOCTL_MAGIC 'H'
struct nvhost_set_nvmap_fd_args { uint32_t fd; } __attribute__((packed));
struct nvhost_get_param_args { uint32_t value; } __attribute__((packed));
struct nvhost_get_param_arg { uint32_t param; uint32_t value; };
struct nvhost_syncpt_incr { uint32_t syncpt_id; uint32_t syncpt_incrs; };
struct nvhost_cmdbuf { uint32_t mem; uint32_t offset; uint32_t words; } __attribute__((packed));
struct nvhost_reloc { uint32_t cmdbuf_mem; uint32_t cmdbuf_offset; uint32_t target; uint32_t target_offset; };
struct nvhost_reloc_shift { uint32_t shift; } __attribute__((packed));
struct nvhost32_submit_args {
	uint32_t submit_version, num_syncpt_incrs, num_cmdbufs, num_relocs;
	uint32_t num_waitchks, timeout, syncpt_incrs, cmdbufs;
	uint32_t relocs, reloc_shifts, waitchks, waitbases, class_ids;
	uint32_t pad[2]; uint32_t fences, fence;
} __attribute__((packed));
struct nvhost_fence { uint32_t syncpt_id; uint32_t value; };

#define NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD _IOW(NVHOST_IOCTL_MAGIC, 5, struct nvhost_set_nvmap_fd_args)
#define NVHOST_IOCTL_CHANNEL_GET_SYNCPOINTS _IOR(NVHOST_IOCTL_MAGIC, 2, struct nvhost_get_param_args)
#define NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT _IOWR(NVHOST_IOCTL_MAGIC, 16, struct nvhost_get_param_arg)
#define NVHOST32_IOCTL_CHANNEL_SUBMIT _IOWR(NVHOST_IOCTL_MAGIC, 15, struct nvhost32_submit_args)

/* nvmap */
#define NVMAP_IOC_MAGIC 'N'
struct nvmap_create_handle { union { uint32_t id; uint32_t size; }; uint32_t handle; };
struct nvmap_alloc_handle { uint32_t handle, heap_mask, flags, align; };
struct nvmap_rw_handle { unsigned long addr; uint32_t handle, offset, elem_size, hmem_stride, user_stride, count; } __attribute__((packed));
struct nvmap_pin_handle { uint32_t handles; unsigned long addr; uint32_t count; } __attribute__((packed));

#define NVMAP_IOC_CREATE _IOWR(NVMAP_IOC_MAGIC, 0, struct nvmap_create_handle)
#define NVMAP_IOC_ALLOC _IOW(NVMAP_IOC_MAGIC, 3, struct nvmap_alloc_handle)
#define NVMAP_IOC_PIN_MULT _IOWR(NVMAP_IOC_MAGIC, 10, struct nvmap_pin_handle)
#define NVMAP_IOC_WRITE _IOW(NVMAP_IOC_MAGIC, 6, struct nvmap_rw_handle)
#define NVMAP_IOC_READ _IOW(NVMAP_IOC_MAGIC, 7, struct nvmap_rw_handle)
#define NVMAP_HEAP_IOVMM (1 << 30)

/* nvhost ctrl */
struct nvhost_ctrl_syncpt_waitex_args { uint32_t id, thresh; int32_t timeout; uint32_t value; };
#define NVHOST_IOCTL_CTRL_SYNCPT_WAITEX _IOWR('H', 6, struct nvhost_ctrl_syncpt_waitex_args)

/* host1x opcodes */
#define H1X_SETCLASS(c) ((0<<28)|((c)<<6))
#define H1X_INCR(o,n) ((1<<28)|((o)<<16)|(n))
#define H1X_NONINCR(o,n) ((2<<28)|((o)<<16)|(n))
#define H1X_IMM(o,v) ((4<<28)|((o)<<16)|(v))
#define H1X_SYNCPT(cond,id) H1X_IMM(0,((cond)<<8)|(id))
#define NOOP H1X_NONINCR(0,0)

static int nvmap_fd, isp_fd, ctrl_fd;
static uint32_t syncpt_id;

static uint32_t nm_create(uint32_t sz) {
	struct nvmap_create_handle c = { .size = sz };
	return ioctl(nvmap_fd, NVMAP_IOC_CREATE, &c) < 0 ? 0 : c.handle;
}
static void nm_alloc(uint32_t h) {
	struct nvmap_alloc_handle a = { .handle=h, .heap_mask=NVMAP_HEAP_IOVMM, .flags=2, .align=4096 };
	ioctl(nvmap_fd, NVMAP_IOC_ALLOC, &a);
}
static uint32_t nm_pin(uint32_t h) {
	struct nvmap_pin_handle p = { .handles=h, .count=1 };
	return ioctl(nvmap_fd, NVMAP_IOC_PIN_MULT, &p) < 0 ? 0 : (uint32_t)p.addr;
}
static void nm_write(uint32_t h, uint32_t off, const void *d, uint32_t sz) {
	struct nvmap_rw_handle r = { .addr=(unsigned long)d, .handle=h, .offset=off, .elem_size=sz, .hmem_stride=sz, .user_stride=sz, .count=1 };
	ioctl(nvmap_fd, NVMAP_IOC_WRITE, &r);
}
static void nm_read(uint32_t h, uint32_t off, void *d, uint32_t sz) {
	struct nvmap_rw_handle r = { .addr=(unsigned long)d, .handle=h, .offset=off, .elem_size=sz, .hmem_stride=sz, .user_stride=sz, .count=1 };
	ioctl(nvmap_fd, NVMAP_IOC_READ, &r);
}
static void nm_zero(uint32_t h, uint32_t sz) {
	void *z = calloc(1, 65536);
	for (uint32_t o = 0; o < sz; o += 65536) {
		uint32_t c = (sz-o < 65536) ? sz-o : 65536;
		nm_write(h, o, z, c);
	}
	free(z);
}

static int do_submit(uint32_t cmd_h, uint32_t nwords,
	struct nvhost_reloc *rels, struct nvhost_reloc_shift *rsh, int nrels)
{
	struct nvhost_cmdbuf cb = { .mem=cmd_h, .words=nwords };
	struct nvhost_syncpt_incr si = { .syncpt_id=syncpt_id, .syncpt_incrs=1 };
	uint32_t cls = 0x32;
	struct nvhost_fence fence = {};
	struct nvhost32_submit_args sa = {};
	sa.num_syncpt_incrs=1; sa.num_cmdbufs=1; sa.num_relocs=nrels; sa.timeout=2000;
	sa.syncpt_incrs=(uint32_t)(uintptr_t)&si;
	sa.cmdbufs=(uint32_t)(uintptr_t)&cb;
	sa.relocs=(uint32_t)(uintptr_t)rels;
	sa.reloc_shifts=(uint32_t)(uintptr_t)rsh;
	sa.class_ids=(uint32_t)(uintptr_t)&cls;
	sa.fences=(uint32_t)(uintptr_t)&fence;
	if (ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa) < 0) return -1;
	struct nvhost_ctrl_syncpt_waitex_args w = { .id=syncpt_id, .thresh=fence.value, .timeout=2000 };
	ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_WAITEX, &w);
	return 0;
}

static void check(uint32_t out_h, const char *label) {
	uint8_t buf[4096] = {};
	nm_read(out_h, 0, buf, 4096);
	int nz = 0;
	for (int i = 0; i < 4096; i++) if (buf[i]) nz++;
	printf("  [%s] %d/4096 non-zero%s\n", label, nz, nz ? " *** ISP WROTE DATA ***" : "");
	printf("  hex: ");
	for (int i = 0; i < 32; i++) printf("%02x ", buf[i]);
	printf("\n");
}

int main()
{
	printf("=== ISP Stable Test — Camera Pipeline Emulation ===\n\n");

	nvmap_fd = open("/dev/nvmap", O_RDWR);
	isp_fd = open("/dev/nvhost-isp", O_RDWR);
	ctrl_fd = open("/dev/nvhost-ctrl", O_RDWR);
	if (nvmap_fd<0 || isp_fd<0 || ctrl_fd<0) { perror("open"); return 1; }

	struct nvhost_set_nvmap_fd_args nfa = { .fd=nvmap_fd };
	ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD, &nfa);

	struct nvhost_get_param_arg sp = { .param=0 };
	if (ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &sp)==0 && sp.value)
		syncpt_id = sp.value;
	else { struct nvhost_get_param_args s; ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINTS, &s); syncpt_id=s.value; }
	printf("syncpt=%u\n", syncpt_id);

	int W=3280, H=2460, YS=3328, UVS=1664;
	int IN_SZ=W*H*2, Y_SZ=YS*H, UV_SZ=UVS*H/2, OUT_SZ=Y_SZ+UV_SZ*2;

	/* Allocate all buffers once */
	uint32_t init_cmd_h = nm_create(16384);  /* init cmdbuf */
	uint32_t frame_cmd_h = nm_create(4096);  /* per-frame cmdbuf */
	uint32_t in_h = nm_create(IN_SZ);
	uint32_t out_h = nm_create(OUT_SZ);
	nm_alloc(init_cmd_h); nm_alloc(frame_cmd_h); nm_alloc(in_h); nm_alloc(out_h);

	uint32_t in_phys = nm_pin(in_h);
	uint32_t out_phys = nm_pin(out_h);
	uint32_t out_y=out_phys, out_u=out_phys+Y_SZ, out_v=out_phys+Y_SZ+UV_SZ;

	printf("in=0x%08x out=0x%08x\n", in_phys, out_phys);

	/* Fill input */
	uint32_t *tmp = malloc(IN_SZ);
	for (int i = 0; i < IN_SZ/4; i++) tmp[i] = 0xA5A50000|(i&0xFFFF);
	nm_write(in_h, 0, tmp, IN_SZ);
	free(tmp);
	printf("input filled\n\n");

	/* Load calibration */
	uint32_t cal[2048];
	int cal_n = 0;
	FILE *f = fopen("/data/local/tmp/isp_cal.bin", "rb");
	if (f) { cal_n = fread(cal, 4, 2048, f); fclose(f); }
	printf("calibration: %d words\n\n", cal_n);

	/* ============================================================
	 * BUILD INIT CMDBUF: calibration + static config + trigger 0x0F
	 * This is sent ONCE, like NvIspSetConfiguration
	 * ============================================================ */
	uint32_t icmd[4096];
	int in_ = 0;

	/* Calibration (includes SET_CLASS) */
	memcpy(&icmd[in_], cal, cal_n * 4);
	in_ += cal_n;

	/* Static output config */
	icmd[in_++] = H1X_SETCLASS(0x32);
	icmd[in_++] = H1X_INCR(0xE31, 1);  icmd[in_++] = W|(H<<16);
	icmd[in_++] = H1X_INCR(0xE33, 1);  icmd[in_++] = 0x04FE00E6;
	icmd[in_++] = H1X_INCR(0xE32, 1);  icmd[in_++] = YS;
	icmd[in_++] = H1X_INCR(0x015, 1);  icmd[in_++] = 0x00000007;
	icmd[in_++] = H1X_INCR(0xE30, 1);  icmd[in_++] = 1;

	/* Trigger 0x0F — static config apply */
	icmd[in_++] = H1X_SETCLASS(0x32);
	icmd[in_++] = H1X_NONINCR(0x00C, 1);
	icmd[in_++] = 0x0F;
	icmd[in_++] = H1X_SYNCPT(1, syncpt_id);
	icmd[in_++] = NOOP;

	/* Init relocs: none needed (no buffer addresses in init cmd) */
	nm_write(init_cmd_h, 0, icmd, in_*4);

	printf("init cmd: %d words\n", in_);

	/* ============================================================
	 * RUN TESTS — full gather submitted twice
	 * First submit initializes ISP, second processes frame
	 * ============================================================ */

	/* Build FULL cmdbuf: cal + config + surfaces + input + trigger + syncpt */
	uint32_t fcmd[4096];
	int fn = 0;

	/* Calibration */
	memcpy(&fcmd[fn], cal, cal_n * 4);
	fn += cal_n;

	/* Static config */
	fcmd[fn++] = H1X_SETCLASS(0x32);
	fcmd[fn++] = H1X_INCR(0xE31, 1);  fcmd[fn++] = W|(H<<16);
	fcmd[fn++] = H1X_INCR(0xE33, 1);  fcmd[fn++] = 0x04FE00E6;
	fcmd[fn++] = H1X_INCR(0xE32, 1);  fcmd[fn++] = YS;
	fcmd[fn++] = H1X_INCR(0x015, 1);  fcmd[fn++] = 0x00000007;
	fcmd[fn++] = H1X_INCR(0xE30, 1);  fcmd[fn++] = 1;

	/* Output dimensions + format */
	fcmd[fn++] = H1X_INCR(0xE00, 1);  fcmd[fn++] = ((W-1)&0x3FFF)<<16;
	fcmd[fn++] = H1X_INCR(0xE01, 1);  fcmd[fn++] = ((H-1)&0x3FFF)<<16;
	fcmd[fn++] = H1X_INCR(0xE02, 1);  fcmd[fn++] = 0x04FE00E6;
	fcmd[fn++] = H1X_INCR(0xE03, 1);  fcmd[fn++] = 0;

	/* Output surfaces */
	fcmd[fn++] = H1X_INCR(0xE04, 3);
	fcmd[fn++] = out_y; fcmd[fn++] = 0; fcmd[fn++] = YS;
	fcmd[fn++] = H1X_INCR(0xE07, 3);
	fcmd[fn++] = out_u; fcmd[fn++] = 0; fcmd[fn++] = UVS;
	fcmd[fn++] = H1X_INCR(0xE0A, 3);
	fcmd[fn++] = out_v; fcmd[fn++] = 0; fcmd[fn++] = UVS;

	/* Processing */
	fcmd[fn++] = H1X_INCR(0x500, 6);
	fcmd[fn++] = 0; fcmd[fn++] = 0; fcmd[fn++] = 0;
	fcmd[fn++] = 0; fcmd[fn++] = 0; fcmd[fn++] = (H<<16)|W;

	/* Input */
	fcmd[fn++] = H1X_SETCLASS(0x32);
	fcmd[fn++] = H1X_INCR(0x100, 4);
	fcmd[fn++] = in_phys; fcmd[fn++] = 0; fcmd[fn++] = 0; fcmd[fn++] = 0;

	/* Trigger 0x05 */
	fcmd[fn++] = H1X_SETCLASS(0x32);
	fcmd[fn++] = H1X_NONINCR(0x00C, 1);
	fcmd[fn++] = 0x05;
	fcmd[fn++] = H1X_SYNCPT(1, syncpt_id);
	fcmd[fn++] = NOOP;

	/* Relocs */
	struct nvhost_reloc frels[8];
	struct nvhost_reloc_shift frsh[8];
	int fnr = 0;
	for (int i = 0; i < fn; i++) {
		if (fcmd[i]==out_y || fcmd[i]==out_u || fcmd[i]==out_v) {
			uint32_t to = 0;
			if (fcmd[i]==out_u) to = Y_SZ;
			else if (fcmd[i]==out_v) to = Y_SZ+UV_SZ;
			frels[fnr].cmdbuf_mem=frame_cmd_h; frels[fnr].cmdbuf_offset=i*4;
			frels[fnr].target=out_h; frels[fnr].target_offset=to;
			frsh[fnr].shift=0; fnr++;
		}
		if (fcmd[i]==in_phys) {
			frels[fnr].cmdbuf_mem=frame_cmd_h; frels[fnr].cmdbuf_offset=i*4;
			frels[fnr].target=in_h; frels[fnr].target_offset=0;
			frsh[fnr].shift=0; fnr++;
		}
	}
	nm_write(frame_cmd_h, 0, fcmd, fn*4);
	printf("full cmdbuf: %d words, %d relocs\n\n", fn, fnr);

	/* Submit 1: Init (ISP configures but doesn't produce output) */
	printf("====== Submit 1: Init ======\n");
	nm_zero(out_h, OUT_SZ);
	do_submit(frame_cmd_h, fn, frels, frsh, fnr);
	check(out_h, "init");

	/* Submit 2-6: Frames (ISP processes and writes output) */
	int i;
	for (i = 2; i <= 6; i++) {
		char label[16];
		snprintf(label, sizeof(label), "frame%d", i-1);
		printf("\n====== Submit %d: %s ======\n", i, label);
		nm_zero(out_h, OUT_SZ);
		do_submit(frame_cmd_h, fn, frels, frsh, fnr);
		check(out_h, label);
	}

	printf("\n=== DONE ===\n");
	return 0;
}
