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
	} else if (!strcmp(mode, "ramp")) {
		/* night-light-like ramp: green/blue down to 78%/61%, back up */
		int step;
		long long t0 = now_us();
		for (step = 0; step <= 120; step++) {
			int down = step <= 60 ? step : 120 - step;
			cmu.csc[0] = 256;
			cmu.csc[1] = 0; cmu.csc[2] = 0; cmu.csc[3] = 0;
			cmu.csc[4] = (u16)(256 - down * 56 / 60);
			cmu.csc[5] = 0; cmu.csc[6] = 0; cmu.csc[7] = 0;
			cmu.csc[8] = (u16)(256 - down * 100 / 60);
			if (ioctl(fd, TEGRA_DC_EXT_SET_CMU_ALIGNED, &cmu) < 0) {
				perror("SET_CMU_ALIGNED");
				return 1;
			}
			usleep(16000);
		}
		printf("ramp done in %lldms (121 aligned writes)\n",
		       (now_us() - t0) / 1000);
	} else {
		fprintf(stderr, "usage: %s get|swap|ident|ramp\n", argv[0]);
		return 2;
	}

	close(fd);
	return 0;
}
