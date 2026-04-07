/*
 * v4l2_diag.c - V4L2 camera diagnostic tool for Tegra124 mocha
 *
 * Usage: v4l2_diag [options]
 *   -d <dev>    Video device (default: /dev/video0)
 *   -w <width>  Frame width (default: 1280)
 *   -h <height> Frame height (default: 720)
 *   -o <file>   Output file (default: /data/local/tmp/frame.raw)
 *   -n <count>  Number of frames to capture (default: 1)
 *   -t <ms>     Timeout in ms (default: 2000)
 *   -i          Info only (QUERYCAP + ENUM_FMT, no capture)
 *   -r          Read CSI/VI registers via /dev/mem
 *   -R          Read registers continuously while waiting for frame
 *
 * Build: arm-linux-gnueabihf-gcc -static -O2 -o v4l2_diag v4l2_diag.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <linux/videodev2.h>
#include <linux/media.h>
#include <getopt.h>

#define MAX_BUFFERS 4
#define VI_BASE     0x54080000
#define VI_SIZE     0x40000

/* Register offsets from VI base */
#define TEGRA_VI_CFG_VI_INCR_SYNCPT        0x000
#define TEGRA_VI_CFG_VI_INCR_SYNCPT_CNTRL  0x004
#define TEGRA_VI_CFG_CG_CTRL              0x0B8

/* VI_CSI_0 (port 0) */
#define VI_CSI_0_SW_RESET                  0x100
#define VI_CSI_0_SINGLE_SHOT               0x104
#define VI_CSI_0_IMAGE_DEF                 0x10C
#define VI_CSI_0_IMAGE_SIZE                0x118
#define VI_CSI_0_IMAGE_SIZE_WC             0x11C
#define VI_CSI_0_IMAGE_DT                  0x120
#define VI_CSI_0_SURFACE0_OFFSET_MSB       0x124
#define VI_CSI_0_SURFACE0_OFFSET_LSB       0x128
#define VI_CSI_0_SURFACE0_STRIDE           0x154
#define VI_CSI_0_ERROR_STATUS              0x184

/* VI_CSI_1 (port 1 = PP_B = OV5693) */
#define VI_CSI_1_SW_RESET                  0x200
#define VI_CSI_1_SINGLE_SHOT               0x204
#define VI_CSI_1_IMAGE_DEF                 0x20C
#define VI_CSI_1_IMAGE_SIZE                0x218
#define VI_CSI_1_IMAGE_SIZE_WC             0x21C
#define VI_CSI_1_IMAGE_DT                  0x220
#define VI_CSI_1_SURFACE0_OFFSET_MSB       0x224
#define VI_CSI_1_SURFACE0_OFFSET_LSB       0x228
#define VI_CSI_1_SURFACE0_STRIDE           0x254
#define VI_CSI_1_ERROR_STATUS              0x284

/* CSI Pixel Parser A */
#define PP_A_INPUT_STREAM_CONTROL          0x838
#define PP_A_PIXEL_STREAM_CONTROL0         0x83C
#define PP_A_PIXEL_STREAM_CONTROL1         0x840
#define PP_A_PIXEL_STREAM_GAP              0x844
#define PP_A_PIXEL_STREAM_PP_COMMAND       0x848
#define PP_A_PIXEL_STREAM_EXPECTED_FRAME   0x84C
#define PP_A_PIXEL_STREAM_PP_INT_MASK      0x850
#define PP_A_PIXEL_PARSER_STATUS           0x854
#define PP_A_CSI_SW_SENSOR_RESET           0x858

/* CSI Pixel Parser B (OV5693 front camera) */
#define PP_B_INPUT_STREAM_CONTROL          0x86C
#define PP_B_PIXEL_STREAM_CONTROL0         0x870
#define PP_B_PIXEL_STREAM_CONTROL1         0x874
#define PP_B_PIXEL_STREAM_GAP              0x878
#define PP_B_PIXEL_STREAM_PP_COMMAND       0x87C
#define PP_B_PIXEL_STREAM_EXPECTED_FRAME   0x880
#define PP_B_PIXEL_STREAM_PP_INT_MASK      0x884
#define PP_B_PIXEL_PARSER_STATUS           0x888
#define PP_B_CSI_SW_SENSOR_RESET           0x88C

/* CSI PHY */
#define CSI_PHY_CIL_COMMAND                0x908

/* CIL A */
#define CILA_PAD_CONFIG0                   0x92C
#define PHY_CILA_CONTROL0                  0x934
#define CSI_CIL_A_STATUS                   0x93C
#define CSI_CILA_STATUS                    0x940
#define CSI_CSICIL_SW_SENSOR_A_RESET       0x94C

/* CIL B */
#define CILB_PAD_CONFIG0                   0x960
#define PHY_CILB_CONTROL0                  0x968
#define CSI_CIL_B_STATUS                   0x970
#define CSI_CILB_STATUS                    0x974

/* CIL C */
#define CILC_PAD_CONFIG0                   0x994
#define PHY_CILC_CONTROL0                  0x99C
#define CSI_CIL_C_STATUS                   0x9A4
#define CSI_CILC_STATUS                    0x9A8

/* CIL D */
#define CILD_PAD_CONFIG0                   0x9C8
#define PHY_CILD_CONTROL0                  0x9D0
#define CSI_CIL_D_STATUS                   0x9D8
#define CSI_CILD_STATUS                    0x9DC

/* CIL E (OV5693 front camera) */
#define CILE_PAD_CONFIG0                   0xA08
#define PHY_CILE_CONTROL0                  0xA10
#define CSI_CIL_E_INT_MASK                0xA14
#define CSI_CIL_E_STATUS                   0xA18
#define CSI_CILE_STATUS                    0xA1C

/* CSI clock override */
#define CSI_CLKEN_OVERRIDE                 0xAF4

/* CSI additional debug */
#define CSI_READONLY_STATUS                0xAEC
#define CSI_SW_STATUS_RESET                0xAF0
#define CSI_CAP_CIL                        0x808
#define CSI_CAP_CSI                        0x818
#define CSI_CAP_PP                         0x828
#define CSI_CIL_PAD_CONFIG0                0x90C  /* global CIL pad config */
#define CSI_DEBUG_CONTROL                  0xAE4
#define CSI_DEBUG_COUNTER_0                0xAE8

/* CILE escape mode registers */
#define CSI_CIL_E_ESCAPE_MODE_COMMAND      0xA1C
#define CSI_CIL_E_ESCAPE_MODE_DATA         0xA20
#define CSI_CSICIL_SW_SENSOR_E_RESET       0xA24

/* CSI Test Pattern Generator B */
#define CSI_PG_CTRL_B                      0x8C4
#define CSI_PG_PHASE_B                     0x8C8
#define CSI_PG_RED_FREQ_B                  0x8CC
#define CSI_PG_RED_FREQ_RATE_B             0x8D0
#define CSI_PG_GREEN_FREQ_B                0x8D4
#define CSI_PG_GREEN_FREQ_RATE_B           0x8D8
#define CSI_PG_BLUE_FREQ_B                 0x8DC
#define CSI_PG_BLUE_FREQ_RATE_B            0x8E0

/* PMC registers for DPD */
#define PMC_BASE                           0x7000E000  /* page-aligned */
#define PMC_SIZE                           0x1000
#define PMC_IO_DPD_REQ                     0x05B8  /* 0x7000E400+0x1B8 - 0x7000E000 */
#define PMC_IO_DPD_STATUS                  0x05BC
#define PMC_IO_DPD2_REQ                    0x05C0
#define PMC_IO_DPD2_STATUS                 0x05C4

struct buffer {
	void *start;
	size_t length;
};

static int xioctl(int fd, unsigned long request, void *arg)
{
	int r;
	do {
		r = ioctl(fd, request, arg);
	} while (r == -1 && errno == EINTR);
	return r;
}

static long long now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static const char *fcc_to_str(unsigned int fourcc, char *buf)
{
	buf[0] = fourcc & 0xff;
	buf[1] = (fourcc >> 8) & 0xff;
	buf[2] = (fourcc >> 16) & 0xff;
	buf[3] = (fourcc >> 24) & 0xff;
	buf[4] = 0;
	return buf;
}

/* ---- Register reading via /dev/mem ---- */

static volatile unsigned int *vi_map = NULL;
static volatile unsigned int *pmc_map = NULL;
static int mem_fd = -1;

static int map_vi_registers(void)
{
	mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (mem_fd < 0) {
		perror("open /dev/mem (need root)");
		return -1;
	}
	vi_map = mmap(NULL, VI_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, VI_BASE);
	if (vi_map == MAP_FAILED) {
		perror("mmap VI registers");
		vi_map = NULL;
		close(mem_fd);
		mem_fd = -1;
		return -1;
	}
	pmc_map = mmap(NULL, PMC_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, PMC_BASE);
	if (pmc_map == MAP_FAILED) {
		perror("mmap PMC registers");
		pmc_map = NULL;
		/* continue without PMC */
	}
	return 0;
}

static void unmap_vi_registers(void)
{
	if (vi_map) {
		munmap((void *)vi_map, VI_SIZE);
		vi_map = NULL;
	}
	if (pmc_map) {
		munmap((void *)pmc_map, PMC_SIZE);
		pmc_map = NULL;
	}
	if (mem_fd >= 0) {
		close(mem_fd);
		mem_fd = -1;
	}
}

static unsigned int vi_read(unsigned int offset)
{
	if (!vi_map) return 0xDEADBEEF;
	return vi_map[offset / 4];
}

static unsigned int pmc_read(unsigned int offset)
{
	if (!pmc_map) return 0xDEADBEEF;
	return pmc_map[offset / 4];
}

static void dump_cil_status_bits(const char *name, unsigned int val)
{
	if (val == 0) {
		printf("  %-25s = 0x%08X (clean)\n", name, val);
		return;
	}
	printf("  %-25s = 0x%08X", name, val);
	if (val & 0x001) printf(" SOT_SB_ERR");
	if (val & 0x002) printf(" SOT_MB_ERR");
	if (val & 0x004) printf(" SOT_MB_DIS");
	if (val & 0x010) printf(" CTRL_ERR");
	if (val & 0x080) printf(" LP_CLK");
	if (val & 0x100) printf(" CLK_CTRL_ERR");
	printf("\n");
}

static void dump_pp_status_bits(const char *name, unsigned int val)
{
	if (val == 0) {
		printf("  %-25s = 0x%08X (no packets)\n", name, val);
		return;
	}
	printf("  %-25s = 0x%08X", name, val);
	if (val & 0x0001) printf(" PKT_RCVD");
	if (val & 0x0004) printf(" SHORT_FRAME");
	if (val & 0x0010) printf(" SINGLE_SHOT_DONE");
	if (val & 0x0020) printf(" FRAME_COMPLETE");
	if (val & 0x0100) printf(" STALE_FRAME");
	if (val & 0x0200) printf(" EMBEDDED_LINE_CRC_ERR");
	if (val & 0x4000) printf(" HDR_ERR");
	printf("\n");
}

static void dump_registers(const char *label)
{
	if (!vi_map) return;

	printf("\n=== CSI/VI Registers %s ===\n", label);

	printf("\n-- VI Config --\n");
	printf("  %-25s = 0x%08X\n", "CFG_CG_CTRL", vi_read(TEGRA_VI_CFG_CG_CTRL));

	printf("\n-- VI_CSI_1 (PORT_B / OV5693) --\n");
	printf("  %-25s = 0x%08X\n", "IMAGE_DEF", vi_read(VI_CSI_1_IMAGE_DEF));
	printf("  %-25s = 0x%08X\n", "IMAGE_SIZE", vi_read(VI_CSI_1_IMAGE_SIZE));
	printf("  %-25s = 0x%08X\n", "IMAGE_SIZE_WC", vi_read(VI_CSI_1_IMAGE_SIZE_WC));
	printf("  %-25s = 0x%08X\n", "IMAGE_DT", vi_read(VI_CSI_1_IMAGE_DT));
	printf("  %-25s = 0x%08X\n", "ERROR_STATUS", vi_read(VI_CSI_1_ERROR_STATUS));

	printf("\n-- Pixel Parser B (PP_B) --\n");
	printf("  %-25s = 0x%08X\n", "INPUT_STREAM_CONTROL", vi_read(PP_B_INPUT_STREAM_CONTROL));
	printf("  %-25s = 0x%08X\n", "STREAM_CONTROL0", vi_read(PP_B_PIXEL_STREAM_CONTROL0));
	printf("  %-25s = 0x%08X\n", "STREAM_CONTROL1", vi_read(PP_B_PIXEL_STREAM_CONTROL1));
	printf("  %-25s = 0x%08X\n", "STREAM_GAP", vi_read(PP_B_PIXEL_STREAM_GAP));
	printf("  %-25s = 0x%08X\n", "PP_COMMAND", vi_read(PP_B_PIXEL_STREAM_PP_COMMAND));
	printf("  %-25s = 0x%08X\n", "PP_INT_MASK", vi_read(PP_B_PIXEL_STREAM_PP_INT_MASK));
	dump_pp_status_bits("PP_B_STATUS", vi_read(PP_B_PIXEL_PARSER_STATUS));

	printf("\n-- Pixel Parser A (PP_A, for reference) --\n");
	dump_pp_status_bits("PP_A_STATUS", vi_read(PP_A_PIXEL_PARSER_STATUS));

	printf("\n-- CSI PHY --\n");
	printf("  %-25s = 0x%08X\n", "CIL_COMMAND", vi_read(CSI_PHY_CIL_COMMAND));
	printf("  %-25s = 0x%08X\n", "CLKEN_OVERRIDE", vi_read(CSI_CLKEN_OVERRIDE));

	printf("\n-- CIL E (OV5693 1-lane) --\n");
	printf("  %-25s = 0x%08X\n", "CILE_PAD_CONFIG0", vi_read(CILE_PAD_CONFIG0));
	printf("  %-25s = 0x%08X\n", "PHY_CILE_CONTROL0", vi_read(PHY_CILE_CONTROL0));
	printf("  %-25s = 0x%08X\n", "CIL_E_INT_MASK", vi_read(CSI_CIL_E_INT_MASK));
	dump_cil_status_bits("CIL_E_STATUS", vi_read(CSI_CIL_E_STATUS));
	dump_cil_status_bits("CILE_STATUS", vi_read(CSI_CILE_STATUS));

	printf("\n-- CIL C/D (brick 1 neighbors) --\n");
	printf("  %-25s = 0x%08X\n", "CILC_PAD_CONFIG0", vi_read(CILC_PAD_CONFIG0));
	printf("  %-25s = 0x%08X\n", "PHY_CILC_CONTROL0", vi_read(PHY_CILC_CONTROL0));
	dump_cil_status_bits("CIL_C_STATUS", vi_read(CSI_CIL_C_STATUS));
	printf("  %-25s = 0x%08X\n", "CILD_PAD_CONFIG0", vi_read(CILD_PAD_CONFIG0));
	printf("  %-25s = 0x%08X\n", "PHY_CILD_CONTROL0", vi_read(PHY_CILD_CONTROL0));
	dump_cil_status_bits("CIL_D_STATUS", vi_read(CSI_CIL_D_STATUS));

	printf("\n-- CIL A/B (brick 0, for reference) --\n");
	dump_cil_status_bits("CIL_A_STATUS", vi_read(CSI_CIL_A_STATUS));
	dump_cil_status_bits("CIL_B_STATUS", vi_read(CSI_CIL_B_STATUS));

	printf("\n-- CSI Debug/Status --\n");
	printf("  %-25s = 0x%08X\n", "CSI_READONLY_STATUS", vi_read(CSI_READONLY_STATUS));
	printf("  %-25s = 0x%08X\n", "CSI_CAP_CIL", vi_read(CSI_CAP_CIL));
	printf("  %-25s = 0x%08X\n", "CSI_CAP_CSI", vi_read(CSI_CAP_CSI));
	printf("  %-25s = 0x%08X\n", "CSI_CAP_PP", vi_read(CSI_CAP_PP));
	printf("  %-25s = 0x%08X\n", "CIL_PAD_CONFIG0 (global)", vi_read(CSI_CIL_PAD_CONFIG0));
	printf("  %-25s = 0x%08X\n", "CILE_ESC_CMD (0xA1C)", vi_read(CSI_CIL_E_ESCAPE_MODE_COMMAND));
	printf("  %-25s = 0x%08X\n", "CILE_ESC_DATA (0xA20)", vi_read(CSI_CIL_E_ESCAPE_MODE_DATA));
	printf("  %-25s = 0x%08X\n", "DEBUG_COUNTER_0", vi_read(CSI_DEBUG_COUNTER_0));

	if (pmc_map) {
		printf("\n-- PMC DPD Status --\n");
		printf("  %-25s = 0x%08X\n", "IO_DPD_STATUS", pmc_read(PMC_IO_DPD_STATUS));
		printf("  %-25s = 0x%08X\n", "IO_DPD2_STATUS", pmc_read(PMC_IO_DPD2_STATUS));
		/* CSIE DPD is in DPD2, bit 12 */
		{
			unsigned int dpd2 = pmc_read(PMC_IO_DPD2_STATUS);
			printf("  CSIE DPD (bit 12)       = %s\n",
				(dpd2 & (1 << 12)) ? "ON (pads powered down!)" : "OFF (pads active)");
			/* CSIA DPD is in DPD, bit 0; CSIB DPD is DPD, bit 1 */
			unsigned int dpd1 = pmc_read(PMC_IO_DPD_STATUS);
			printf("  CSIA DPD (bit 0)        = %s\n",
				(dpd1 & (1 << 0)) ? "ON" : "OFF");
			printf("  CSIB DPD (bit 1)        = %s\n",
				(dpd1 & (1 << 1)) ? "ON" : "OFF");
		}
	}

	printf("=== End Registers ===\n\n");
}

/* Tegra camera V4L2 control IDs (from camera_common.h) */
#define V4L2_CID_TEGRA_CAMERA_BASE	(V4L2_CTRL_CLASS_CAMERA | 0x2000)
#define V4L2_CID_FRAME_LENGTH		(V4L2_CID_TEGRA_CAMERA_BASE+0)
#define V4L2_CID_COARSE_TIME		(V4L2_CID_TEGRA_CAMERA_BASE+1)
#define V4L2_CID_EEPROM_DATA		(V4L2_CID_TEGRA_CAMERA_BASE+5)
#define V4L2_CID_OTP_DATA		(V4L2_CID_TEGRA_CAMERA_BASE+6)
#define V4L2_CID_FUSE_ID		(V4L2_CID_TEGRA_CAMERA_BASE+7)

static void show_sensor_info(int fd)
{
	struct v4l2_ext_controls ctrls;
	struct v4l2_ext_control ctrl;
	int i;

	printf("=== Sensor Info (V4L2 Controls) ===\n");
	fflush(stdout);

	struct {
		__u32 id;
		const char *name;
		int max_len;
	} string_ctrls[] = {
		{ V4L2_CID_FUSE_ID, "Fuse ID", 32 },
		{ V4L2_CID_OTP_DATA, "OTP Data", 1024 },
		{ V4L2_CID_EEPROM_DATA, "EEPROM Data", 2048 },
	};

	for (i = 0; i < 3; i++) {
		char *buf = calloc(1, string_ctrls[i].max_len + 1);
		if (!buf)
			continue;

		memset(&ctrl, 0, sizeof(ctrl));
		memset(&ctrls, 0, sizeof(ctrls));

		ctrl.id = string_ctrls[i].id;
		ctrl.size = string_ctrls[i].max_len + 1;
		ctrl.string = buf;

		ctrls.ctrl_class = V4L2_CTRL_CLASS_CAMERA;
		ctrls.count = 1;
		ctrls.controls = &ctrl;

		if (ioctl(fd, VIDIOC_G_EXT_CTRLS, &ctrls) == 0) {
			if (buf[0]) {
				printf("  %-12s: %s\n", string_ctrls[i].name, buf);
			} else {
				printf("  %-12s: (empty)\n", string_ctrls[i].name);
			}
		} else {
			printf("  %-12s: (not available)\n", string_ctrls[i].name);
		}

		free(buf);
	}
	printf("\n");
}

/* ---- V4L2 operations ---- */

static void show_capabilities(int fd)
{
	struct v4l2_capability cap;
	char fcc[5];

	if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
		perror("VIDIOC_QUERYCAP");
		return;
	}

	printf("=== Device Capabilities ===\n");
	printf("  Driver:   %s\n", cap.driver);
	printf("  Card:     %s\n", cap.card);
	printf("  Bus:      %s\n", cap.bus_info);
	printf("  Version:  %u.%u.%u\n",
		(cap.version >> 16) & 0xFF,
		(cap.version >> 8) & 0xFF,
		cap.version & 0xFF);
	printf("  Caps:     0x%08X", cap.capabilities);
	if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) printf(" CAPTURE");
	if (cap.capabilities & V4L2_CAP_STREAMING)     printf(" STREAMING");
	if (cap.capabilities & V4L2_CAP_READWRITE)     printf(" READWRITE");
	printf("\n\n");

	printf("=== Supported Formats ===\n");
	struct v4l2_fmtdesc fmtdesc;
	memset(&fmtdesc, 0, sizeof(fmtdesc));
	fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	while (xioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
		printf("  [%d] %s (%s)", fmtdesc.index,
			fcc_to_str(fmtdesc.pixelformat, fcc),
			fmtdesc.description);
		if (fmtdesc.flags & V4L2_FMT_FLAG_COMPRESSED)
			printf(" [compressed]");
		printf("\n");

		/* Enumerate frame sizes for this format */
		struct v4l2_frmsizeenum frmsize;
		memset(&frmsize, 0, sizeof(frmsize));
		frmsize.pixel_format = fmtdesc.pixelformat;
		while (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0) {
			if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
				printf("    %ux%u\n",
					frmsize.discrete.width,
					frmsize.discrete.height);
			} else if (frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE ||
				   frmsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
				printf("    %u-%ux%u-%u (step %ux%u)\n",
					frmsize.stepwise.min_width,
					frmsize.stepwise.max_width,
					frmsize.stepwise.min_height,
					frmsize.stepwise.max_height,
					frmsize.stepwise.step_width,
					frmsize.stepwise.step_height);
				break;
			}
			frmsize.index++;
		}
		fmtdesc.index++;
	}
	printf("\n");

	/* Show current format */
	struct v4l2_format fmt;
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(fd, VIDIOC_G_FMT, &fmt) == 0) {
		printf("=== Current Format ===\n");
		printf("  %ux%u %s bytesperline=%u sizeimage=%u\n",
			fmt.fmt.pix.width, fmt.fmt.pix.height,
			fcc_to_str(fmt.fmt.pix.pixelformat, fcc),
			fmt.fmt.pix.bytesperline,
			fmt.fmt.pix.sizeimage);
		printf("\n");
	}
}

static void show_media_info(void)
{
	int fd = open("/dev/media0", O_RDWR);
	if (fd < 0) {
		printf("(no /dev/media0)\n");
		return;
	}

	struct media_device_info info;
	if (ioctl(fd, MEDIA_IOC_DEVICE_INFO, &info) == 0) {
		printf("=== Media Device ===\n");
		printf("  Driver:  %s\n", info.driver);
		printf("  Model:   %s\n", info.model);
		printf("  Serial:  %s\n", info.serial);
		printf("  Bus:     %s\n", info.bus_info);
		printf("\n");
	}
	close(fd);
}

static int do_capture(const char *dev, int width, int height,
		      const char *outfile, int nframes, int timeout_ms,
		      int read_regs, int poll_regs, int single_shot,
		      int framerate, int exposure, int gain,
		      int focus, const char *focus_dev)
{
	struct buffer buffers[MAX_BUFFERS];
	int nbuf = 0;
	char fcc[5];
	int fd, i, ret;

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror("open video device");
		return -1;
	}

	/* Set framerate BEFORE format so mode selection considers fps */
	if (framerate > 0) {
		struct v4l2_streamparm parm;
		memset(&parm, 0, sizeof(parm));
		parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		parm.parm.capture.timeperframe.numerator = 1;
		parm.parm.capture.timeperframe.denominator = framerate;
		if (xioctl(fd, VIDIOC_S_PARM, &parm) < 0)
			perror("VIDIOC_S_PARM (framerate)");
		else
			printf("Framerate set to %d fps\n", framerate);
	}

	/* Set format */
	struct v4l2_format fmt;
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_SBGGR10;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;

	printf("Requesting %dx%d SBGGR10...\n", width, height);
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("VIDIOC_S_FMT");
		/* Try without specifying format */
		memset(&fmt, 0, sizeof(fmt));
		fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		fmt.fmt.pix.width = width;
		fmt.fmt.pix.height = height;
		fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_SRGGB10;
		printf("Retrying with SRGGB10...\n");
		if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
			perror("VIDIOC_S_FMT (retry)");
			close(fd);
			return -1;
		}
	}

	printf("Got format: %dx%d %s bytesperline=%u sizeimage=%u\n",
		fmt.fmt.pix.width, fmt.fmt.pix.height,
		fcc_to_str(fmt.fmt.pix.pixelformat, fcc),
		fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);

	/* Request buffers */
	struct v4l2_requestbuffers req;
	memset(&req, 0, sizeof(req));
	req.count = single_shot ? 1 : MAX_BUFFERS;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		perror("VIDIOC_REQBUFS");
		close(fd);
		return -1;
	}
	printf("Buffers allocated: %d\n", req.count);
	nbuf = req.count;

	/* Map buffers */
	for (i = 0; i < nbuf; i++) {
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
			perror("VIDIOC_QUERYBUF");
			close(fd);
			return -1;
		}

		buffers[i].length = buf.length;
		buffers[i].start = mmap(NULL, buf.length,
					PROT_READ | PROT_WRITE, MAP_SHARED,
					fd, buf.m.offset);
		if (buffers[i].start == MAP_FAILED) {
			perror("mmap");
			close(fd);
			return -1;
		}
		printf("Buffer %d: length=%zu\n", i, buffers[i].length);
	}

	/* Fill buffers with marker pattern before queue */
	for (i = 0; i < nbuf; i++) {
		unsigned int *p = (unsigned int *)buffers[i].start;
		size_t j;
		for (j = 0; j < buffers[i].length / 4; j++)
			p[j] = 0xDEADBEEF;
		printf("Buffer %d: filled with 0xDEADBEEF\n", i);
	}

	/* Queue buffers */
	for (i = 0; i < nbuf; i++) {
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
			perror("VIDIOC_QBUF");
			close(fd);
			return -1;
		}
	}

	/* Set exposure and gain before streaming */
	if (exposure >= 0) {
		struct v4l2_ext_controls ctrls;
		struct v4l2_ext_control ctrl;
		memset(&ctrl, 0, sizeof(ctrl));
		memset(&ctrls, 0, sizeof(ctrls));
		ctrl.id = V4L2_CID_COARSE_TIME;
		ctrl.value = exposure;
		ctrls.ctrl_class = V4L2_CTRL_CLASS_CAMERA;
		ctrls.count = 1;
		ctrls.controls = &ctrl;
		if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) < 0)
			perror("set exposure");
		else
			printf("Exposure set to %d\n", exposure);
	}
	if (gain >= 0) {
		struct v4l2_control ctrl;
		memset(&ctrl, 0, sizeof(ctrl));
		ctrl.id = V4L2_CID_GAIN;
		ctrl.value = gain;
		if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0)
			perror("set gain");
		else
			printf("Gain set to %d\n", gain);
	}

	if (read_regs)
		dump_registers("BEFORE STREAMON");

	/* Start streaming */
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	printf("Starting stream...\n");
	long long t_start = now_ms();

	if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
		perror("VIDIOC_STREAMON");
		close(fd);
		return -1;
	}

	long long t_streamon = now_ms();
	printf("STREAMON took %lld ms\n", t_streamon - t_start);

	/* Set focus AFTER streamon (focuser powered by sensor stream) */
	if (focus >= 0) {
		const char *fdev = focus_dev ? focus_dev : dev;
		int focus_fd = open(fdev, O_RDWR);
		if (focus_fd < 0) {
			fprintf(stderr, "Cannot open focus dev %s: %s\n",
				fdev, strerror(errno));
		} else {
			struct v4l2_control ctrl;
			memset(&ctrl, 0, sizeof(ctrl));
			ctrl.id = V4L2_CID_FOCUS_ABSOLUTE;
			ctrl.value = focus;
			if (ioctl(focus_fd, VIDIOC_S_CTRL, &ctrl) < 0)
				perror("set focus");
			else
				printf("Focus set to %d on %s\n", focus, fdev);
			close(focus_fd);
			usleep(100000); /* 100ms settle time for VCM */
		}
	}

	if (read_regs)
		dump_registers("AFTER STREAMON");

	/* Capture frames */
	int captured = 0;
	for (int frame = 0; frame < nframes; frame++) {
		printf("\nWaiting for frame %d/%d...\n", frame + 1, nframes);

		fd_set fds;
		struct timeval tv;

		/* If poll_regs, check registers while waiting */
		int got_frame = 0;
		long long deadline = now_ms() + timeout_ms;

		while (!got_frame && now_ms() < deadline) {
			FD_ZERO(&fds);
			FD_SET(fd, &fds);

			int poll_timeout = poll_regs ? 200 : (deadline - now_ms());
			if (poll_timeout <= 0) poll_timeout = 1;
			tv.tv_sec = poll_timeout / 1000;
			tv.tv_usec = (poll_timeout % 1000) * 1000;

			ret = select(fd + 1, &fds, NULL, NULL, &tv);
			if (ret < 0) {
				if (errno == EINTR) continue;
				perror("select");
				break;
			}
			if (ret == 0) {
				/* Timeout on this iteration */
				if (poll_regs && vi_map) {
					long long elapsed = now_ms() - t_streamon;
					printf("[%lld ms] PP_B=0x%08X CIL_E=0x%08X CILE=0x%08X\n",
						elapsed,
						vi_read(PP_B_PIXEL_PARSER_STATUS),
						vi_read(CSI_CIL_E_STATUS),
						vi_read(CSI_CILE_STATUS));
				}
				continue;
			}

			/* Frame ready */
			struct v4l2_buffer buf;
			memset(&buf, 0, sizeof(buf));
			buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buf.memory = V4L2_MEMORY_MMAP;

			if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
				perror("VIDIOC_DQBUF");
				break;
			}

			long long t_frame = now_ms();
			printf("Frame %d captured: index=%d bytesused=%u (took %lld ms)\n",
				frame + 1, buf.index, buf.bytesused,
				t_frame - t_streamon);

			/* Check how much DMA actually wrote */
			{
				unsigned int *p = (unsigned int *)buffers[buf.index].start;
				size_t total = buf.bytesused / 4;
				size_t deadbeef = 0, zeros = 0, other = 0;
				size_t j;
				for (j = 0; j < total; j++) {
					if (p[j] == 0xDEADBEEF) deadbeef++;
					else if (p[j] == 0) zeros++;
					else other++;
				}
				printf("DMA check: deadbeef=%zu zeros=%zu other=%zu total=%zu\n",
					deadbeef, zeros, other, total);
				if (other > 0) {
					printf("  First non-marker: @%zu = 0x%08x\n",
						0UL, p[0]);
					for (j = 0; j < total; j++) {
						if (p[j] != 0xDEADBEEF && p[j] != 0) {
							printf("  First other: @%zu = 0x%08x\n",
								j, p[j]);
							break;
						}
					}
				}
			}

			if (read_regs)
				dump_registers("AFTER FRAME");

			/* Save frame */
			if (outfile && buf.bytesused > 0) {
				char filename[256];
				if (nframes == 1) {
					snprintf(filename, sizeof(filename), "%s", outfile);
				} else {
					snprintf(filename, sizeof(filename), "%s.%03d", outfile, frame);
				}
				int ofd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
				if (ofd >= 0) {
					size_t written = write(ofd, buffers[buf.index].start,
							       buf.bytesused);
					close(ofd);
					printf("Saved %zu bytes to %s\n", written, filename);
				} else {
					perror("open output file");
				}
			}

			/* Re-queue buffer (skip in single-shot mode) */
			if (!single_shot) {
				if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
					perror("VIDIOC_QBUF (requeue)");
				}
			}

			captured++;
			got_frame = 1;
		}

		if (!got_frame) {
			long long elapsed = now_ms() - t_streamon;
			printf("TIMEOUT waiting for frame %d after %lld ms!\n",
				frame + 1, elapsed);
			if (read_regs)
				dump_registers("TIMEOUT");
			/* Try DQBUF anyway — kernel may have returned buffer with ISP data */
			{
				struct v4l2_buffer buf;
				memset(&buf, 0, sizeof(buf));
				buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
				buf.memory = V4L2_MEMORY_MMAP;
				if (xioctl(fd, VIDIOC_DQBUF, &buf) == 0 && buf.bytesused > 0) {
					printf("Got buffer after timeout: index=%d bytesused=%u\n",
						buf.index, buf.bytesused);
					if (outfile) {
						char filename[256];
						snprintf(filename, sizeof(filename), "%s", outfile);
						FILE *fp = fopen(filename, "wb");
						if (fp) {
							size_t written = fwrite(buffers[buf.index].start,
								1, buf.bytesused, fp);
							fclose(fp);
							printf("Saved %zu bytes to %s\n", written, filename);
						}
					}
				}
			}
			break;
		}
	}

	/* Stop streaming */
	if (xioctl(fd, VIDIOC_STREAMOFF, &type) < 0) {
		perror("VIDIOC_STREAMOFF");
	}

	long long t_end = now_ms();
	printf("\nTotal: %d/%d frames in %lld ms\n", captured, nframes, t_end - t_start);

	/* Cleanup */
	for (i = 0; i < nbuf; i++)
		munmap(buffers[i].start, buffers[i].length);
	close(fd);
	return captured > 0 ? 0 : -1;
}

static void usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("  -d <dev>    Video device (default: /dev/video0)\n");
	printf("  -w <width>  Frame width (default: 1280)\n");
	printf("  -h <height> Frame height (default: 720)\n");
	printf("  -o <file>   Output file (default: /sdcard/Pictures/frame.raw)\n");
	printf("  -n <count>  Number of frames (default: 1)\n");
	printf("  -t <ms>     Timeout in ms (default: 2000)\n");
	printf("  -i          Info only (no capture)\n");
	printf("  -r          Read CSI/VI registers via /dev/mem\n");
	printf("  -R          Poll registers while waiting for frame\n");
	printf("  -S          Single-shot mode (1 buffer, no requeue)\n");
	printf("  -f <fps>    Set framerate (e.g. 30, 60, 90, 120)\n");
}

int main(int argc, char *argv[])
{
	const char *dev = "/dev/video0";
	const char *outfile = "/sdcard/Pictures/frame.raw";
	int width = 1280, height = 720;
	int nframes = 1;
	int timeout_ms = 2000;
	int info_only = 0;
	int read_regs = 0;
	int poll_regs = 0;
	int single_shot = 0;
	int framerate = 0;
	int exposure = -1;
	int gain = -1;
	int focus = -1;
	const char *focus_dev = NULL;
	int opt;

	while ((opt = getopt(argc, argv, "d:w:h:o:n:t:irRSf:e:g:F:D:")) != -1) {
		switch (opt) {
		case 'd': dev = optarg; break;
		case 'w': width = atoi(optarg); break;
		case 'h': height = atoi(optarg); break;
		case 'o': outfile = optarg; break;
		case 'n': nframes = atoi(optarg); break;
		case 't': timeout_ms = atoi(optarg); break;
		case 'i': info_only = 1; break;
		case 'r': read_regs = 1; break;
		case 'R': poll_regs = 1; read_regs = 1; break;
		case 'S': single_shot = 1; nframes = 1; break;
		case 'f': framerate = atoi(optarg); break;
		case 'e': exposure = atoi(optarg); break;
		case 'g': gain = atoi(optarg); break;
		case 'F': focus = atoi(optarg); break;
		case 'D': focus_dev = optarg; break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	printf("v4l2_diag - Tegra124 camera diagnostic tool\n\n");

	/* Try to map registers */
	if (read_regs || poll_regs) {
		if (map_vi_registers() < 0) {
			printf("Warning: cannot map registers, continuing without\n");
			read_regs = 0;
			poll_regs = 0;
		}
	}

	/* Show media device info */
	show_media_info();

	/* Open video device for info */
	int fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", dev, strerror(errno));
		unmap_vi_registers();
		return 1;
	}
	show_capabilities(fd);
	show_sensor_info(fd);
	close(fd);

	if (read_regs)
		dump_registers("IDLE STATE");

	if (!info_only) {
		int ret = do_capture(dev, width, height, outfile,
				     nframes, timeout_ms, read_regs, poll_regs,
				     single_shot, framerate, exposure, gain,
				     focus, focus_dev);
		unmap_vi_registers();
		return ret < 0 ? 1 : 0;
	}

	unmap_vi_registers();
	return 0;
}
