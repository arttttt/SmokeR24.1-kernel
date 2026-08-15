/*
 * cmutest — probe the T124 DC CMU through the dc_ext ioctls.
 *
 * Struct layout and ioctl numbers mirror include/video/tegra_dc_ext.h
 * (V1 contract: CONFIG_TEGRA_DC_CMU); a size mismatch would make every
 * ioctl fail with -EFAULT, so the layout is asserted at build time.
 *
 * Modes:
 *   get            dump cmu_enable, csc and LUT edges
 *   swap           R<->B swap in csc via SET_CMU_ALIGNED (LUTs preserved)
 *   ident          csc = identity(256) via SET_CMU_ALIGNED (LUTs preserved)
 *   ramp           smooth warm ramp: 60 aligned writes, one per ~16ms
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>

typedef unsigned short u16;

struct tegra_dc_ext_cmu {
	u16 cmu_enable;
	u16 csc[9];
	u16 lut1[256];
	u16 lut2[960];
};

typedef char size_check[(sizeof(struct tegra_dc_ext_cmu) == 2452) ? 1 : -1];

#define TEGRA_DC_EXT_SET_CMU         _IOW('D', 0x0D, struct tegra_dc_ext_cmu)
#define TEGRA_DC_EXT_GET_CMU         _IOR('D', 0x0F, struct tegra_dc_ext_cmu)
#define TEGRA_DC_EXT_GET_CUSTOM_CMU  _IOR('D', 0x10, struct tegra_dc_ext_cmu)
#define TEGRA_DC_EXT_SET_CMU_ALIGNED _IOW('D', 0x16, struct tegra_dc_ext_cmu)

static long long now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static void dump(const struct tegra_dc_ext_cmu *c)
{
	printf("cmu_enable=%u\n", c->cmu_enable);
	printf("csc: %u %u %u / %u %u %u / %u %u %u\n",
	       c->csc[0], c->csc[1], c->csc[2],
	       c->csc[3], c->csc[4], c->csc[5],
	       c->csc[6], c->csc[7], c->csc[8]);
	printf("lut1[0,1,128,255] = %u %u %u %u\n",
	       c->lut1[0], c->lut1[1], c->lut1[128], c->lut1[255]);
	printf("lut2[0,1,480,959] = %u %u %u %u\n",
	       c->lut2[0], c->lut2[1], c->lut2[480], c->lut2[959]);
}

static int set_aligned(int fd, struct tegra_dc_ext_cmu *c)
{
	long long t0 = now_us();
	int ret = ioctl(fd, TEGRA_DC_EXT_SET_CMU_ALIGNED, c);
	long long t1 = now_us();
	printf("SET_CMU_ALIGNED ret=%d ioctl_time=%lldus\n", ret, t1 - t0);
	return ret;
}

int main(int argc, char **argv)
{
	static struct tegra_dc_ext_cmu cmu;
	const char *mode = argc > 1 ? argv[1] : "get";
	int fd = open("/dev/tegra_dc_0", O_RDWR);

	if (fd < 0) {
		perror("open /dev/tegra_dc_0");
		return 1;
	}

	if (ioctl(fd, TEGRA_DC_EXT_GET_CMU, &cmu) < 0) {
		perror("GET_CMU");
		return 1;
	}

	if (!strcmp(mode, "get")) {
		dump(&cmu);
	} else if (!strcmp(mode, "dump")) {
		/* The whole live pipeline, machine-readable: one value per line,
		 * cmu_enable, then csc[9], lut1[256], lut2[960] -- the input of
		 * the host-side emulator that turns a screencap into what the
		 * panel actually shows. */
		int i;
		printf("%u\n", cmu.cmu_enable);
		for (i = 0; i < 9; i++)
			printf("%u\n", cmu.csc[i]);
		for (i = 0; i < 256; i++)
			printf("%u\n", cmu.lut1[i]);
		for (i = 0; i < 960; i++)
			printf("%u\n", cmu.lut2[i]);
	} else if (!strcmp(mode, "swap")) {
		static const u16 sw[9] = { 0, 0, 256, 0, 256, 0, 256, 0, 0 };
		memcpy(cmu.csc, sw, sizeof(sw));
		if (set_aligned(fd, &cmu))
			return 1;
		printf("R<->B swap applied\n");
	} else if (!strcmp(mode, "ident")) {
		static const u16 id[9] = { 256, 0, 0, 0, 256, 0, 0, 0, 256 };
		memcpy(cmu.csc, id, sizeof(id));
		if (set_aligned(fd, &cmu))
			return 1;
		printf("identity csc restored\n");
	} else if (!strcmp(mode, "set")) {
		/* arbitrary csc, clamped to the 10-bit register field: the
		 * kernel writes the u16 unmasked and the hardware drops bits
		 * 10-15, so 512 would silently wrap to 0. */
		int i;
		if (argc < 11) {
			fprintf(stderr, "set needs 9 csc values\n");
			return 2;
		}
		for (i = 0; i < 9; i++) {
			int v = atoi(argv[2 + i]);
			if (v < 0)
				v = 0;
			if (v > 0x3ff)
				v = 0x3ff;
			cmu.csc[i] = (u16)v;
		}
		if (set_aligned(fd, &cmu))
			return 1;
		printf("csc set: %u %u %u / %u %u %u / %u %u %u\n",
		       cmu.csc[0], cmu.csc[1], cmu.csc[2],
		       cmu.csc[3], cmu.csc[4], cmu.csc[5],
		       cmu.csc[6], cmu.csc[7], cmu.csc[8]);
	} else if (!strcmp(mode, "ramp")) {
		/* warm ramp: green/blue down to g_end/b_end (S8.8), back up */
		int g_end = argc > 2 ? atoi(argv[2]) : 200;
		int b_end = argc > 3 ? atoi(argv[3]) : 156;
		int step;
		long long t0 = now_us();
		if (g_end < 0 || g_end > 256 || b_end < 0 || b_end > 256) {
			fprintf(stderr, "ramp ends must be 0..256\n");
			return 2;
		}
		for (step = 0; step <= 120; step++) {
			int down = step <= 60 ? step : 120 - step;
			cmu.csc[0] = 256;
			cmu.csc[1] = 0; cmu.csc[2] = 0; cmu.csc[3] = 0;
			cmu.csc[4] = (u16)(256 - down * (256 - g_end) / 60);
			cmu.csc[5] = 0; cmu.csc[6] = 0; cmu.csc[7] = 0;
			cmu.csc[8] = (u16)(256 - down * (256 - b_end) / 60);
			if (ioctl(fd, TEGRA_DC_EXT_SET_CMU_ALIGNED, &cmu) < 0) {
				perror("SET_CMU_ALIGNED");
				return 1;
			}
			usleep(16000);
		}
		printf("ramp done in %lldms (121 aligned writes)\n",
		       (now_us() - t0) / 1000);
	} else {
		fprintf(stderr, "usage: %s get|dump|swap|ident|set 9xcsc|ramp [g_end b_end]\n", argv[0]);
		return 2;
	}

	close(fd);
	return 0;
}
