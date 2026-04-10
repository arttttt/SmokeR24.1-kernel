/*
 * isp_wrapper.c v2 — comprehensive camera pipeline tracer
 *
 * LD_PRELOAD wrapper to trace the full stock camera HAL flow:
 *   - nvhost-vi/isp/vic submits with cmdbuf hex dumps
 *   - nvmap buffer lifecycle (create/alloc/pin/mmap/free)
 *   - camera.pcl pipeline controller ioctls
 *   - imx179/ov5693 sensor ioctls (mode, exposure, gain)
 *   - ad5823 autofocus actuator
 *   - tegra_camera clock/power
 *
 * Build (on server, NDK sysroot for Android 4.4):
 *   arm-linux-androideabi-gcc --sysroot=$SYSROOT -shared -fPIC \
 *       -o isp_wrapper.so isp_wrapper.c
 *
 * Deploy:
 *   push to /system/lib/isp_wrapper.so (chmod 644)
 *   setenv LD_PRELOAD in init.rc mediaserver service
 *
 * Output: /data/local/tmp/isp_trace.log
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <time.h>
#include <linux/ioctl.h>

/* ================================================================
 * Ioctl magic numbers
 * ================================================================ */

#define NVHOST_IOCTL_MAGIC  'H'
#define NVMAP_IOC_MAGIC     'N'
#define NVC_IOCTL_MAGIC     'o'  /* sensors, pcl, focuser */
#define TEGRA_CAM_MAGIC     'i'  /* tegra_camera */

/* ================================================================
 * nvhost structures (from kernel include/linux/nvhost_ioctl.h)
 * Matched to Smoke-kernel-mocha layout
 * ================================================================ */

struct nvhost_cmdbuf {
	uint32_t mem;
	uint32_t offset;
	uint32_t words;
};

struct nvhost_reloc {
	uint32_t cmdbuf_mem;
	uint32_t cmdbuf_offset;
	uint32_t target;
	uint32_t target_offset;
};

struct nvhost_syncpt_incr {
	uint32_t syncpt_id;
	uint32_t syncpt_incrs;
};

/* 64-bit submit (NR=26) — matched to kernel */
struct nvhost_submit_args {
	uint32_t submit_version;
	uint32_t num_syncpt_incrs;
	uint32_t num_cmdbufs;
	uint32_t num_relocs;
	uint32_t num_waitchks;
	uint32_t timeout;
	uint32_t syncpt_incrs;
	uint32_t fence;
	uint64_t cmdbuf_exts;
	uint32_t flags;
	uint32_t reserved;
	uint64_t pad[2];
	uint64_t cmdbufs;
	uint64_t relocs;
	uint64_t reloc_shifts;
	uint64_t waitchks;
	uint64_t waitbases;
	uint64_t class_ids;
	uint64_t fences;
};

/* 32-bit submit (NR=15) — matched to kernel */
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

/* ================================================================
 * nvmap structures (from kernel include/linux/nvmap.h)
 * ================================================================ */

struct nvmap_create_handle {
	union {
		uint32_t id;
		uint32_t size;
		int32_t  fd;
	};
	uint32_t handle;
};

struct nvmap_alloc_handle {
	uint32_t handle;
	uint32_t heap_mask;
	uint32_t flags;
	uint32_t align;
};

struct nvmap_pin_handle {
	uint32_t *handles;
	unsigned long *addr;
	uint32_t count;
};

/* NVMAP_IOC_MMAP (NR=5) — binds nvmap handle to mmap'd VMA */
struct nvmap_map_caller {
	uint32_t handle;
	uint32_t offset;
	uint32_t length;
	uint32_t flags;
	uint32_t addr;
};

/* ================================================================
 * Sensor structures (from kernel include/media/)
 * ================================================================ */

/* imx179 mode (NR=1) */
struct imx179_mode {
	int xres;
	int yres;
	uint32_t frame_length;
	uint32_t coarse_time;
	uint16_t gain;
};

/* ov5693 mode (NR=1) */
struct ov5693_mode {
	int xres;
	int yres;
	uint32_t frame_length;
	uint32_t coarse_time;
	uint16_t gain;
};

/* group hold AE (used by both sensors) */
struct sensor_ae {
	uint32_t frame_length;
	uint32_t coarse_time;
	uint32_t gain;   /* extended to u32 for struct compat */
};

/* ================================================================
 * ioctl NR definitions
 * ================================================================ */

/* nvhost channel */
#define NVHOST_SUBMIT_NR        26
#define NVHOST32_SUBMIT_NR      15
#define NVHOST_SET_NVMAP_NR     5
#define NVHOST_GET_SYNCPT_NR    16
#define NVHOST_GET_WAITBASE_NR  17
#define NVHOST_SET_CLK_NR       10
#define NVHOST_SET_TIMEOUT_NR   18
#define NVHOST_CHANNEL_OPEN_NR  112

/* nvmap */
#define NVMAP_CREATE_NR     0
#define NVMAP_ALLOC_NR      3
#define NVMAP_FREE_NR       4
#define NVMAP_MMAP_NR       5
#define NVMAP_PIN_NR        10
#define NVMAP_UNPIN_NR      11
#define NVMAP_GET_FD_NR     15
#define NVMAP_FROM_FD_NR    16

/* sensors (magic 'o') */
#define SENSOR_SET_MODE_NR          1
#define IMX179_SET_FRAME_LEN_NR     3
#define IMX179_SET_COARSE_TIME_NR   4
#define IMX179_SET_GAIN_NR          5
#define IMX179_SET_GROUP_HOLD_NR    7
#define OV5693_SET_FRAME_LEN_NR     2
#define OV5693_SET_COARSE_TIME_NR   3
#define OV5693_SET_GAIN_NR          4
#define OV5693_SET_GROUP_HOLD_NR    8
#define OV5693_SET_CAMERA_MODE_NR   10
#define NVC_PWR_WR_NR               102
#define NVC_PWR_RD_NR               103
#define NVC_PARAM_WR_NR             104
#define NVC_PARAM_RD_NR             105
#define SENSOR_SET_POWER_NR         20

/* camera.pcl */
#define PCL_CHIP_REG_NR     100
#define PCL_DEV_REG_NR      104
#define PCL_PWR_WR_NR       108
#define PCL_PWR_RD_NR       109
#define PCL_SEQ_WR_NR       112
#define PCL_SEQ_RD_NR       113
#define PCL_UPDATE_NR       116
#define PCL_LAYOUT_WR_NR    120
#define PCL_LAYOUT_RD_NR    121
#define PCL_PARAM_WR_NR     140
#define PCL_PARAM_RD_NR     141
#define PCL_DRV_ADD_NR      150

/* tegra_camera */
#define TCAM_ENABLE_NR      1
#define TCAM_DISABLE_NR     2
#define TCAM_CLK_SET_NR     3
#define TCAM_RESET_NR       4

/* ================================================================
 * Globals
 * ================================================================ */

static FILE *logfp;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static struct timespec t0;

/* tracked fds — any camera-related device */
#define MAX_FDS 256
static struct {
	int fd;
	char name[80];
} tracked[MAX_FDS];
static int num_tracked;

/* nvmap handle → userspace addr mapping */
#define MAX_HMAPS 1024
static struct {
	uint32_t handle;
	void    *addr;
	uint32_t len;
} hmaps[MAX_HMAPS];
static int num_hmaps;

/* ================================================================
 * Raw syscall wrappers
 * ================================================================ */

static inline int raw_open(const char *path, int flags, mode_t mode) {
	return syscall(__NR_open, path, flags, mode);
}

static inline int raw_ioctl(int fd, int request, void *arg) {
	return syscall(__NR_ioctl, fd, request, arg);
}

static inline void *raw_mmap(void *addr, size_t len, int prot,
			     int flags, int fd, off_t offset) {
	return (void *)syscall(__NR_mmap2, addr, len, prot, flags, fd,
			      (unsigned long)(offset >> 12));
}

/* ================================================================
 * Logging
 * ================================================================ */

static void trace_init(void) __attribute__((constructor));
static void trace_fini(void) __attribute__((destructor));

static unsigned int ms_since_init(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned int)((now.tv_sec - t0.tv_sec) * 1000 +
			      (now.tv_nsec - t0.tv_nsec) / 1000000);
}

static void tlog(const char *fmt, ...) {
	va_list ap;
	if (!logfp) return;
	pthread_mutex_lock(&log_lock);
	fprintf(logfp, "[%u] ", ms_since_init());
	va_start(ap, fmt);
	vfprintf(logfp, fmt, ap);
	va_end(ap);
	fflush(logfp);
	pthread_mutex_unlock(&log_lock);
}

/* ================================================================
 * FD tracking — which device each fd belongs to
 * ================================================================ */

static const char *fd_name(int fd) {
	int i;
	for (i = 0; i < num_tracked; i++)
		if (tracked[i].fd == fd)
			return tracked[i].name;
	return NULL;
}

static void track_fd(int fd, const char *path) {
	int i;
	/* update if fd reused */
	for (i = 0; i < num_tracked; i++) {
		if (tracked[i].fd == fd) {
			strncpy(tracked[i].name, path, 79);
			tracked[i].name[79] = 0;
			return;
		}
	}
	if (num_tracked < MAX_FDS) {
		tracked[num_tracked].fd = fd;
		strncpy(tracked[num_tracked].name, path, 79);
		tracked[num_tracked].name[79] = 0;
		num_tracked++;
	}
}

static int fd_has(int fd, const char *substr) {
	const char *n = fd_name(fd);
	return n && strstr(n, substr);
}

static int is_nvhost_channel(int fd) {
	const char *n = fd_name(fd);
	if (!n) return 0;
	/* channel fds: nvhost-vi, nvhost-isp, nvhost-vic (NOT ctrl-) */
	return strstr(n, "nvhost-") && !strstr(n, "ctrl-") &&
	       !strstr(n, "-as-") && !strstr(n, "-dbg-") &&
	       !strstr(n, "-prof-");
}

static int is_nvmap_fd(int fd) {
	return fd_has(fd, "nvmap");
}

static int is_sensor_fd(int fd) {
	return fd_has(fd, "imx179") || fd_has(fd, "ov5693");
}

static int is_pcl_fd(int fd) {
	return fd_has(fd, "camera.pcl");
}

static int is_focuser_fd(int fd) {
	return fd_has(fd, "ad5823") || fd_has(fd, "sh532u");
}

static int is_tegra_cam_fd(int fd) {
	return fd_has(fd, "tegra_camera");
}

/* ================================================================
 * Handle → addr map (populated by NVMAP_IOC_MMAP)
 * ================================================================ */

static void hmap_add(uint32_t handle, void *addr, uint32_t len) {
	int i;
	for (i = 0; i < num_hmaps; i++) {
		if (hmaps[i].handle == handle) {
			hmaps[i].addr = addr;
			hmaps[i].len = len;
			return;
		}
	}
	if (num_hmaps < MAX_HMAPS) {
		hmaps[num_hmaps].handle = handle;
		hmaps[num_hmaps].addr = addr;
		hmaps[num_hmaps].len = len;
		num_hmaps++;
	}
}

static void *hmap_find(uint32_t handle) {
	int i;
	for (i = 0; i < num_hmaps; i++)
		if (hmaps[i].handle == handle)
			return hmaps[i].addr;
	return NULL;
}

/* ================================================================
 * Hex dump
 * ================================================================ */

static void hex_dump(const char *tag, const uint32_t *data, int words) {
	int i;
	tlog("  [HEX %s] %d words:\n", tag, words);
	for (i = 0; i < words; i++) {
		if ((i % 8) == 0) tlog("    [%04x]", i * 4);
		tlog(" %08x", data[i]);
		if ((i % 8) == 7 || i == words - 1) tlog("\n");
	}
}

/* ================================================================
 * Submit dump — works for ALL nvhost channels (VI, ISP, VIC)
 * ================================================================ */

static const char *channel_tag(int fd) {
	const char *n = fd_name(fd);
	if (!n) return "?";
	if (strstr(n, "isp.1")) return "ISP.1";
	if (strstr(n, "isp"))   return "ISP";
	if (strstr(n, "vi.1"))  return "VI.1";
	if (strstr(n, "vi"))    return "VI";
	if (strstr(n, "vic"))   return "VIC";
	return n;
}

static void dump_submit(int fd, struct nvhost_submit_args *s) {
	int i;
	struct nvhost_cmdbuf *cb;
	struct nvhost_syncpt_incr *sp;
	struct nvhost_reloc *rel;
	const char *tag = channel_tag(fd);

	tlog("[%s SUBMIT] ver=%u cmdbufs=%u relocs=%u syncpts=%u "
	     "timeout=%u flags=0x%x\n",
	     tag, s->submit_version, s->num_cmdbufs, s->num_relocs,
	     s->num_syncpt_incrs, s->timeout, s->flags);

	sp = (struct nvhost_syncpt_incr *)(uintptr_t)s->syncpt_incrs;
	if (sp) {
		for (i = 0; i < (int)s->num_syncpt_incrs; i++)
			tlog("  SYNCPT[%d]: id=%u incrs=%u\n",
			     i, sp[i].syncpt_id, sp[i].syncpt_incrs);
	}

	cb = (struct nvhost_cmdbuf *)(uintptr_t)s->cmdbufs;
	if (cb) {
		for (i = 0; i < (int)s->num_cmdbufs; i++) {
			tlog("  CMDBUF[%d]: handle=%u off=0x%x words=%u\n",
			     i, cb[i].mem, cb[i].offset, cb[i].words);
			void *base = hmap_find(cb[i].mem);
			if (base && cb[i].words > 0 && cb[i].words <= 8192) {
				uint32_t *data = (uint32_t *)
					((uint8_t *)base + cb[i].offset);
				char label[32];
				snprintf(label, sizeof(label),
					 "%s G%d h%u", tag, i, cb[i].mem);
				hex_dump(label, data, cb[i].words);
			}
		}
	}

	rel = (struct nvhost_reloc *)(uintptr_t)s->relocs;
	if (rel) {
		for (i = 0; i < (int)s->num_relocs; i++)
			tlog("  RELOC[%d]: cb_mem=%u cb_off=0x%x "
			     "tgt=%u tgt_off=0x%x\n",
			     i, rel[i].cmdbuf_mem, rel[i].cmdbuf_offset,
			     rel[i].target, rel[i].target_offset);
	}
}

static void dump_submit32(int fd, struct nvhost32_submit_args *s) {
	int i;
	struct nvhost_cmdbuf *cb;
	struct nvhost_syncpt_incr *sp;
	const char *tag = channel_tag(fd);

	tlog("[%s SUBMIT32] ver=%u cmdbufs=%u relocs=%u syncpts=%u "
	     "timeout=%u\n",
	     tag, s->submit_version, s->num_cmdbufs, s->num_relocs,
	     s->num_syncpt_incrs, s->timeout);

	sp = (struct nvhost_syncpt_incr *)(uintptr_t)s->syncpt_incrs;
	if (sp) {
		for (i = 0; i < (int)s->num_syncpt_incrs; i++)
			tlog("  SYNCPT[%d]: id=%u incrs=%u\n",
			     i, sp[i].syncpt_id, sp[i].syncpt_incrs);
	}

	cb = (struct nvhost_cmdbuf *)(uintptr_t)s->cmdbufs;
	if (cb) {
		for (i = 0; i < (int)s->num_cmdbufs; i++) {
			tlog("  CMDBUF[%d]: handle=%u off=0x%x words=%u\n",
			     i, cb[i].mem, cb[i].offset, cb[i].words);
			void *base = hmap_find(cb[i].mem);
			if (base && cb[i].words > 0 && cb[i].words <= 8192) {
				uint32_t *data = (uint32_t *)
					((uint8_t *)base + cb[i].offset);
				char label[32];
				snprintf(label, sizeof(label),
					 "%s G%d h%u", tag, i, cb[i].mem);
				hex_dump(label, data, cb[i].words);
			}
		}
	}
}

/* ================================================================
 * Sensor ioctl parser (imx179 / ov5693)
 * ================================================================ */

static void log_sensor_ioctl(int fd, int nr, void *arg, int ret) {
	const char *name = fd_has(fd, "imx179") ? "IMX179" : "OV5693";
	int is_imx = fd_has(fd, "imx179");

	switch (nr) {
	case SENSOR_SET_MODE_NR: {
		struct ov5693_mode *m = arg; /* same layout for both */
		tlog("[%s SET_MODE] %dx%d frame_len=%u coarse=%u gain=%u ret=%d\n",
		     name, m->xres, m->yres, m->frame_length,
		     m->coarse_time, m->gain, ret);
		break;
	}
	case 2:
		if (!is_imx) /* ov5693 SET_FRAME_LENGTH */
			tlog("[%s SET_FRAME_LEN] %u ret=%d\n",
			     name, *(uint32_t *)arg, ret);
		else /* imx179 GET_STATUS */
			tlog("[%s GET_STATUS] %u ret=%d\n",
			     name, *(uint8_t *)arg, ret);
		break;
	case 3:
		if (is_imx)
			tlog("[IMX179 SET_FRAME_LEN] %u ret=%d\n",
			     *(uint32_t *)arg, ret);
		else
			tlog("[OV5693 SET_COARSE_TIME] %u ret=%d\n",
			     *(uint32_t *)arg, ret);
		break;
	case 4:
		if (is_imx)
			tlog("[IMX179 SET_COARSE_TIME] %u ret=%d\n",
			     *(uint32_t *)arg, ret);
		else
			tlog("[OV5693 SET_GAIN] %u ret=%d\n",
			     *(uint16_t *)arg, ret);
		break;
	case 5:
		if (is_imx)
			tlog("[IMX179 SET_GAIN] %u ret=%d\n",
			     *(uint16_t *)arg, ret);
		else
			tlog("[OV5693 GET_STATUS] %u ret=%d\n",
			     *(uint8_t *)arg, ret);
		break;
	case 7:
		if (is_imx) {
			struct sensor_ae *ae = arg;
			tlog("[IMX179 GROUP_HOLD] fl=%u ct=%u gain=%u ret=%d\n",
			     ae->frame_length, ae->coarse_time,
			     ae->gain, ret);
		}
		break;
	case 8:
		if (!is_imx) {
			struct sensor_ae *ae = arg;
			tlog("[OV5693 GROUP_HOLD] fl=%u ct=%u gain=%u ret=%d\n",
			     ae->frame_length, ae->coarse_time,
			     ae->gain, ret);
		}
		break;
	case SENSOR_SET_POWER_NR:
		tlog("[%s SET_POWER] %u ret=%d\n", name,
		     *(uint32_t *)arg, ret);
		break;
	case NVC_PWR_WR_NR:
		tlog("[%s PWR_WR] %d ret=%d\n", name,
		     *(int *)arg, ret);
		break;
	case NVC_PWR_RD_NR:
		tlog("[%s PWR_RD] %d ret=%d\n", name,
		     *(int *)arg, ret);
		break;
	default:
		tlog("[%s IOCTL] nr=%u ret=%d\n", name, nr, ret);
		break;
	}
}

/* ================================================================
 * PCL ioctl parser (camera.pcl)
 * ================================================================ */

static void log_pcl_ioctl(int fd, int nr, void *arg, int ret) {
	switch (nr) {
	case PCL_CHIP_REG_NR:
		tlog("[PCL CHIP_REG] ret=%d\n", ret);
		break;
	case PCL_DEV_REG_NR:
		tlog("[PCL DEV_REG] ret=%d\n", ret);
		break;
	case PCL_PWR_WR_NR:
		tlog("[PCL PWR_WR] %d ret=%d\n", *(int *)arg, ret);
		break;
	case PCL_PWR_RD_NR:
		tlog("[PCL PWR_RD] %d ret=%d\n", *(int *)arg, ret);
		break;
	case PCL_SEQ_WR_NR:
		tlog("[PCL SEQ_WR] ret=%d\n", ret);
		break;
	case PCL_SEQ_RD_NR:
		tlog("[PCL SEQ_RD] ret=%d\n", ret);
		break;
	case PCL_UPDATE_NR:
		tlog("[PCL UPDATE] ret=%d\n", ret);
		break;
	case PCL_LAYOUT_WR_NR:
		tlog("[PCL LAYOUT_WR] ret=%d\n", ret);
		break;
	case PCL_LAYOUT_RD_NR:
		tlog("[PCL LAYOUT_RD] ret=%d\n", ret);
		break;
	case PCL_PARAM_WR_NR:
		tlog("[PCL PARAM_WR] ret=%d\n", ret);
		break;
	case PCL_PARAM_RD_NR:
		tlog("[PCL PARAM_RD] ret=%d\n", ret);
		break;
	case PCL_DRV_ADD_NR:
		tlog("[PCL DRV_ADD] ret=%d\n", ret);
		break;
	default:
		tlog("[PCL IOCTL] nr=%u ret=%d\n", nr, ret);
		break;
	}
}

/* ================================================================
 * Focuser ioctl parser (ad5823)
 * ================================================================ */

static void log_focuser_ioctl(int fd, int nr, void *arg, int ret) {
	switch (nr) {
	case NVC_PWR_WR_NR:
		tlog("[FOCUS PWR_WR] %d ret=%d\n", *(int *)arg, ret);
		break;
	case NVC_PARAM_WR_NR:
		tlog("[FOCUS PARAM_WR] ret=%d\n", ret);
		break;
	case NVC_PARAM_RD_NR:
		tlog("[FOCUS PARAM_RD] ret=%d\n", ret);
		break;
	default:
		tlog("[FOCUS IOCTL] nr=%u ret=%d\n", nr, ret);
		break;
	}
}

/* ================================================================
 * Hooked functions
 * ================================================================ */

/* Devices to track */
static int should_track(const char *path) {
	return strstr(path, "nvhost") || strstr(path, "nvmap") ||
	       strstr(path, "camera.pcl") ||
	       strstr(path, "imx179") || strstr(path, "ov5693") ||
	       strstr(path, "ad5823") || strstr(path, "sh532u") ||
	       strstr(path, "tegra_camera") ||
	       strstr(path, "tegra-fuse");
}

int open(const char *path, int flags, ...) {
	va_list ap;
	mode_t mode = 0;
	int fd;

	if (flags & O_CREAT) {
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}

	fd = raw_open(path, flags, mode);

	if (fd >= 0 && should_track(path)) {
		track_fd(fd, path);
		tlog("[OPEN] %s -> fd=%d\n", path, fd);
	}

	return fd;
}

int ioctl(int fd, int request, ...) {
	va_list ap;
	void *arg;
	int ret;
	unsigned int nr, type;

	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);

	nr = _IOC_NR(request);
	type = _IOC_TYPE(request);

	/* Fast path: skip completely unrelated ioctls */
	if (type != NVHOST_IOCTL_MAGIC && type != NVMAP_IOC_MAGIC &&
	    type != NVC_IOCTL_MAGIC && type != TEGRA_CAM_MAGIC)
		return raw_ioctl(fd, request, arg);

	/* Pre-submit logging for all nvhost channels */
	if (type == NVHOST_IOCTL_MAGIC && is_nvhost_channel(fd)) {
		if (nr == NVHOST_SUBMIT_NR) {
			dump_submit(fd, (struct nvhost_submit_args *)arg);
		} else if (nr == NVHOST32_SUBMIT_NR) {
			dump_submit32(fd, (struct nvhost32_submit_args *)arg);
		}
	}

	ret = raw_ioctl(fd, request, arg);

	/* Skip untracked fds */
	if (!fd_name(fd))
		return ret;

	/* ---- nvhost post-call ---- */
	if (type == NVHOST_IOCTL_MAGIC) {
		if (nr == NVHOST_SUBMIT_NR) {
			struct nvhost_submit_args *s = arg;
			tlog("[%s POST-SUBMIT] ret=%d fence=%u\n",
			     channel_tag(fd), ret, s->fence);
		} else if (nr == NVHOST32_SUBMIT_NR) {
			struct nvhost32_submit_args *s = arg;
			tlog("[%s POST-SUBMIT32] ret=%d fence=%u\n",
			     channel_tag(fd), ret, s->fence);
		} else if (nr == NVHOST_SET_NVMAP_NR) {
			struct { uint32_t fd; } *a = arg;
			tlog("[%s SET_NVMAP_FD] nvmap_fd=%u ret=%d\n",
			     channel_tag(fd), a->fd, ret);
		} else if (nr == NVHOST_GET_SYNCPT_NR) {
			struct { uint32_t param; uint32_t value; } *a = arg;
			tlog("[%s GET_SYNCPT] param=%u -> id=%u ret=%d\n",
			     channel_tag(fd), a->param, a->value, ret);
		} else if (nr == NVHOST_SET_CLK_NR) {
			tlog("[%s SET_CLK] ret=%d\n", channel_tag(fd), ret);
		} else if (nr == NVHOST_SET_TIMEOUT_NR) {
			tlog("[%s SET_TIMEOUT] ret=%d\n",
			     channel_tag(fd), ret);
		} else if (nr != NVHOST_SUBMIT_NR &&
			   nr != NVHOST32_SUBMIT_NR) {
			tlog("[%s IOCTL] nr=%u ret=%d\n",
			     channel_tag(fd), nr, ret);
		}
	}

	/* ---- nvmap post-call ---- */
	else if (type == NVMAP_IOC_MAGIC && is_nvmap_fd(fd)) {
		if (nr == NVMAP_CREATE_NR) {
			struct nvmap_create_handle *a = arg;
			tlog("[NVMAP CREATE] size=%u -> handle=%u ret=%d\n",
			     a->size, a->handle, ret);
		} else if (nr == NVMAP_ALLOC_NR) {
			struct nvmap_alloc_handle *a = arg;
			tlog("[NVMAP ALLOC] handle=%u heap=0x%x "
			     "flags=0x%x align=%u ret=%d\n",
			     a->handle, a->heap_mask, a->flags,
			     a->align, ret);
		} else if (nr == NVMAP_FREE_NR) {
			tlog("[NVMAP FREE] handle=%u ret=%d\n",
			     (uint32_t)(uintptr_t)arg, ret);
		} else if (nr == NVMAP_MMAP_NR) {
			struct nvmap_map_caller *a = arg;
			tlog("[NVMAP MMAP] handle=%u -> addr=0x%x "
			     "off=%u len=%u flags=0x%x ret=%d\n",
			     a->handle, a->addr, a->offset,
			     a->length, a->flags, ret);
			if (ret == 0 && a->addr)
				hmap_add(a->handle, (void *)(uintptr_t)a->addr,
					 a->length);
		} else if (nr == NVMAP_PIN_NR) {
			struct nvmap_pin_handle *a = arg;
			tlog("[NVMAP PIN] count=%u ret=%d\n", a->count, ret);
		} else if (nr == NVMAP_UNPIN_NR) {
			struct nvmap_pin_handle *a = arg;
			tlog("[NVMAP UNPIN] count=%u ret=%d\n", a->count, ret);
		} else if (nr == NVMAP_GET_FD_NR) {
			struct nvmap_create_handle *a = arg;
			tlog("[NVMAP GET_FD] handle=%u -> fd=%d ret=%d\n",
			     a->handle, a->fd, ret);
		} else if (nr == NVMAP_FROM_FD_NR) {
			struct nvmap_create_handle *a = arg;
			tlog("[NVMAP FROM_FD] fd=%d -> handle=%u ret=%d\n",
			     a->fd, a->handle, ret);
		} else {
			tlog("[NVMAP IOCTL] nr=%u ret=%d\n", nr, ret);
		}
	}

	/* ---- sensor / pcl / focuser (magic 'o') ---- */
	else if (type == NVC_IOCTL_MAGIC) {
		if (is_sensor_fd(fd))
			log_sensor_ioctl(fd, nr, arg, ret);
		else if (is_pcl_fd(fd))
			log_pcl_ioctl(fd, nr, arg, ret);
		else if (is_focuser_fd(fd))
			log_focuser_ioctl(fd, nr, arg, ret);
	}

	/* ---- tegra_camera (magic 'i') ---- */
	else if (type == TEGRA_CAM_MAGIC && is_tegra_cam_fd(fd)) {
		switch (nr) {
		case TCAM_ENABLE_NR:
			tlog("[TCAM ENABLE] ret=%d\n", ret); break;
		case TCAM_DISABLE_NR:
			tlog("[TCAM DISABLE] ret=%d\n", ret); break;
		case TCAM_CLK_SET_NR:
			tlog("[TCAM CLK_SET] ret=%d\n", ret); break;
		case TCAM_RESET_NR:
			tlog("[TCAM RESET] ret=%d\n", ret); break;
		default:
			tlog("[TCAM IOCTL] nr=%u ret=%d\n", nr, ret); break;
		}
	}

	return ret;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset) {
	void *ret = raw_mmap(addr, len, prot, flags, fd, offset);

	if (ret != MAP_FAILED && is_nvmap_fd(fd)) {
		tlog("[MMAP] fd=%d off=0x%lx len=%zu -> %p\n",
		     fd, (long)offset, len, ret);
	}

	return ret;
}

/* ================================================================
 * Init / Fini
 * ================================================================ */

static void trace_init(void) {
	clock_gettime(CLOCK_MONOTONIC, &t0);
	logfp = fopen("/data/local/tmp/isp_trace.log", "w");
	if (logfp)
		tlog("isp_wrapper v2 loaded, pid=%d\n", getpid());
}

static void trace_fini(void) {
	if (logfp) {
		tlog("isp_wrapper exit, pid=%d\n", getpid());
		fclose(logfp);
		logfp = NULL;
	}
}
