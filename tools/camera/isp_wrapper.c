/*
 * isp_wrapper.c — LD_PRELOAD wrapper to trace nvhost-isp and nvmap ioctls
 *
 * Intercepts open/ioctl/mmap to log all ISP host1x submits with
 * full cmdbuf hex dumps, syncpoints, relocs, and nvmap operations.
 *
 * Usage:
 *   arm-linux-gnueabihf-gcc -shared -fPIC -o isp_wrapper.so isp_wrapper.c -ldl
 *   LD_PRELOAD=/data/local/tmp/isp_wrapper.so cameraserver &
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
#include <linux/ioctl.h>

/* ---- nvhost ioctl structures (from kernel include/linux/nvhost_ioctl.h) ---- */

#define NVHOST_IOCTL_MAGIC 'H'

struct nvhost_cmdbuf {
	uint32_t mem;
	uint32_t offset;
	uint32_t words;
};

struct nvhost_cmdbuf_ext {
	int32_t pre_fence;
	uint32_t pad;
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

struct nvhost_syncpt_incr {
	uint32_t syncpt_id;
	uint32_t syncpt_incrs;
};

struct nvhost_submit_args {
	uint32_t submit_version;
	uint32_t num_syncpt_incrs;
	uint32_t num_cmdbufs;
	uint32_t num_relocs;
	uint32_t num_waitchks;
	uint32_t timeout;
	uint32_t flags;
	uint32_t fence;
	uint64_t syncpt_incrs;
	uint64_t cmdbuf_exts;
	uint64_t pad[3];
	uint64_t cmdbufs;
	uint64_t relocs;
	uint64_t reloc_shifts;
	uint64_t waitchks;
	uint64_t waitbases;
	uint64_t class_ids;
	uint64_t fences;
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
} __attribute__((packed));

/* nvmap ioctls */
#define NVMAP_IOC_MAGIC 'N'

struct nvmap_create_handle {
	union {
		uint32_t id;
		uint32_t size;
		int32_t fd;
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
	uint32_t count;
	uint64_t handles;
	uint64_t addr;
};

/* ---- ioctl numbers ---- */
#define NVHOST_SUBMIT_NR       26
#define NVHOST32_SUBMIT_NR     15
#define NVHOST_SET_NVMAP_NR    5
#define NVHOST_OPEN_NR         112
#define NVHOST_GET_SYNCPT_NR   16
#define NVHOST_SET_CLK_NR      10
#define NVHOST_SET_TIMEOUT_NR  18

#define NVMAP_CREATE_NR        0
#define NVMAP_ALLOC_NR         3
#define NVMAP_PIN_NR           10
#define NVMAP_GET_FD_NR        15
#define NVMAP_FROM_FD_NR       16
#define NVMAP_ALLOC_KIND_NR    100

/* ---- globals ---- */

static FILE *logfp;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

/* tracked fds */
#define MAX_FDS 256
static struct {
	int fd;
	char name[64];
} tracked[MAX_FDS];
static int num_tracked;

/* nvmap mmap tracking for cmdbuf readback */
#define MAX_MMAPS 512
static struct {
	uint32_t handle;
	void *addr;
	size_t len;
} mmaps[MAX_MMAPS];
static int num_mmaps;

/* original functions */
static int (*real_open)(const char *, int, ...);
static int (*real_ioctl)(int, int, ...);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);

static void trace_init(void) __attribute__((constructor));
static void trace_fini(void) __attribute__((destructor));

static void tlog(const char *fmt, ...) {
	va_list ap;
	if (!logfp) return;
	pthread_mutex_lock(&log_lock);
	va_start(ap, fmt);
	vfprintf(logfp, fmt, ap);
	va_end(ap);
	fflush(logfp);
	pthread_mutex_unlock(&log_lock);
}

static const char *fd_name(int fd) {
	int i;
	for (i = 0; i < num_tracked; i++)
		if (tracked[i].fd == fd)
			return tracked[i].name;
	return NULL;
}

static int is_isp_fd(int fd) {
	const char *n = fd_name(fd);
	return n && strstr(n, "isp");
}

static int is_nvmap_fd(int fd) {
	const char *n = fd_name(fd);
	return n && strstr(n, "nvmap");
}

static int is_tracked(int fd) {
	return fd_name(fd) != NULL;
}

static void track_fd(int fd, const char *path) {
	if (num_tracked < MAX_FDS) {
		tracked[num_tracked].fd = fd;
		strncpy(tracked[num_tracked].name, path, 63);
		tracked[num_tracked].name[63] = 0;
		num_tracked++;
	}
}

static void *find_mmap(uint32_t handle) {
	int i;
	for (i = 0; i < num_mmaps; i++)
		if (mmaps[i].handle == handle)
			return mmaps[i].addr;
	return NULL;
}

static void hex_dump(const char *tag, const uint32_t *data, int words) {
	int i;
	tlog("[HEX] %s: %d words\n", tag, words);
	for (i = 0; i < words; i++) {
		if ((i % 8) == 0) tlog("  [%3d]", i);
		tlog(" %08x", data[i]);
		if ((i % 8) == 7 || i == words - 1) tlog("\n");
	}
}

/* ---- submit dump ---- */

static void dump_submit(int fd, struct nvhost_submit_args *s) {
	int i;
	struct nvhost_cmdbuf *cb;
	struct nvhost_syncpt_incr *sp;
	struct nvhost_reloc *rel;

	tlog("[SUBMIT] fd=%d (%s) ver=%u cmdbufs=%u relocs=%u syncpts=%u timeout=%u flags=0x%x\n",
	     fd, fd_name(fd) ?: "?", s->submit_version,
	     s->num_cmdbufs, s->num_relocs, s->num_syncpt_incrs,
	     s->timeout, s->flags);

	/* syncpt incrs */
	sp = (struct nvhost_syncpt_incr *)(uintptr_t)s->syncpt_incrs;
	if (sp) {
		for (i = 0; i < (int)s->num_syncpt_incrs; i++)
			tlog("  SYNCPT[%d]: id=%u incrs=%u\n",
			     i, sp[i].syncpt_id, sp[i].syncpt_incrs);
	}

	/* cmdbufs */
	cb = (struct nvhost_cmdbuf *)(uintptr_t)s->cmdbufs;
	if (cb) {
		for (i = 0; i < (int)s->num_cmdbufs; i++) {
			tlog("  CMDBUF[%d]: mem=%u off=%u words=%u\n",
			     i, cb[i].mem, cb[i].offset, cb[i].words);
			/* try to read cmdbuf data via mmap */
			void *base = find_mmap(cb[i].mem);
			if (base && cb[i].words > 0 && cb[i].words < 4096) {
				uint32_t *data = (uint32_t *)((uint8_t *)base + cb[i].offset);
				char tag[64];
				snprintf(tag, sizeof(tag), "G[%d] mem=%u", i, cb[i].mem);
				hex_dump(tag, data, cb[i].words);
			}
		}
	}

	/* relocs */
	rel = (struct nvhost_reloc *)(uintptr_t)s->relocs;
	if (rel) {
		for (i = 0; i < (int)s->num_relocs; i++)
			tlog("  RELOC[%d]: cmdbuf_mem=%u cmdbuf_off=%u target=%u target_off=%u\n",
			     i, rel[i].cmdbuf_mem, rel[i].cmdbuf_offset,
			     rel[i].target, rel[i].target_offset);
	}
}

static void dump_submit32(int fd, struct nvhost32_submit_args *s) {
	int i;
	struct nvhost_cmdbuf *cb;
	struct nvhost_syncpt_incr *sp;

	tlog("[SUBMIT32] fd=%d (%s) ver=%u cmdbufs=%u relocs=%u syncpts=%u timeout=%u\n",
	     fd, fd_name(fd) ?: "?", s->submit_version,
	     s->num_cmdbufs, s->num_relocs, s->num_syncpt_incrs,
	     s->timeout);

	sp = (struct nvhost_syncpt_incr *)(uintptr_t)s->syncpt_incrs;
	if (sp) {
		for (i = 0; i < (int)s->num_syncpt_incrs; i++)
			tlog("  SYNCPT[%d]: id=%u incrs=%u\n",
			     i, sp[i].syncpt_id, sp[i].syncpt_incrs);
	}

	cb = (struct nvhost_cmdbuf *)(uintptr_t)s->cmdbufs;
	if (cb) {
		for (i = 0; i < (int)s->num_cmdbufs; i++) {
			tlog("  CMDBUF[%d]: mem=%u off=%u words=%u\n",
			     i, cb[i].mem, cb[i].offset, cb[i].words);
			void *base = find_mmap(cb[i].mem);
			if (base && cb[i].words > 0 && cb[i].words < 4096) {
				uint32_t *data = (uint32_t *)((uint8_t *)base + cb[i].offset);
				char tag[64];
				snprintf(tag, sizeof(tag), "G[%d] mem=%u", i, cb[i].mem);
				hex_dump(tag, data, cb[i].words);
			}
		}
	}
}

/* ---- hooked functions ---- */

int open(const char *path, int flags, ...) {
	va_list ap;
	mode_t mode = 0;
	int fd;

	if (!real_open)
		real_open = dlsym(RTLD_NEXT, "open");

	if (flags & O_CREAT) {
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}

	fd = real_open(path, flags, mode);

	if (fd >= 0 && (strstr(path, "nvhost") || strstr(path, "nvmap"))) {
		track_fd(fd, path);
		tlog("[OPEN] %s → fd=%d\n", path, fd);
	}

	return fd;
}

int ioctl(int fd, int request, ...) {
	va_list ap;
	void *arg;
	int ret;
	unsigned int nr, type;

	if (!real_ioctl)
		real_ioctl = dlsym(RTLD_NEXT, "ioctl");

	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);

	nr = _IOC_NR(request);
	type = _IOC_TYPE(request);

	/* Pre-call logging */
	if (type == NVHOST_IOCTL_MAGIC && is_isp_fd(fd)) {
		if (nr == NVHOST_SUBMIT_NR) {
			tlog("[PRE-SUBMIT] fd=%d\n", fd);
			dump_submit(fd, (struct nvhost_submit_args *)arg);
		} else if (nr == NVHOST32_SUBMIT_NR) {
			tlog("[PRE-SUBMIT32] fd=%d\n", fd);
			dump_submit32(fd, (struct nvhost32_submit_args *)arg);
		}
	}

	ret = real_ioctl(fd, request, arg);

	/* Post-call logging */
	if (!is_tracked(fd))
		goto out;

	if (type == NVHOST_IOCTL_MAGIC) {
		if (nr == NVHOST_SUBMIT_NR || nr == NVHOST32_SUBMIT_NR) {
			if (nr == NVHOST_SUBMIT_NR) {
				struct nvhost_submit_args *s = arg;
				tlog("[POST-SUBMIT] ret=%d fence=%u\n", ret, s->fence);
			} else {
				struct nvhost32_submit_args *s = arg;
				tlog("[POST-SUBMIT32] ret=%d fence=%u\n", ret, s->fence);
			}
		} else if (nr == NVHOST_SET_NVMAP_NR) {
			struct { uint32_t fd; } *a = arg;
			tlog("[SET_NVMAP_FD] fd=%d nvmap_fd=%u ret=%d\n", fd, a->fd, ret);
		} else if (nr == NVHOST_GET_SYNCPT_NR) {
			struct { uint32_t param; uint32_t value; } *a = arg;
			tlog("[GET_SYNCPT] fd=%d param=%u value=%u ret=%d\n",
			     fd, a->param, a->value, ret);
		} else if (nr == NVHOST_SET_CLK_NR) {
			tlog("[SET_CLK] fd=%d ret=%d\n", fd, ret);
		} else if (nr == NVHOST_SET_TIMEOUT_NR) {
			tlog("[SET_TIMEOUT] fd=%d ret=%d\n", fd, ret);
		} else if (nr == NVHOST_OPEN_NR) {
			struct { int32_t channel_fd; } *a = arg;
			tlog("[CHANNEL_OPEN] fd=%d channel_fd=%d ret=%d\n",
			     fd, a->channel_fd, ret);
		} else {
			tlog("[NVHOST_IOCTL] fd=%d nr=%u ret=%d\n", fd, nr, ret);
		}
	} else if (type == NVMAP_IOC_MAGIC && is_nvmap_fd(fd)) {
		if (nr == NVMAP_CREATE_NR) {
			struct nvmap_create_handle *a = arg;
			tlog("[NVMAP_CREATE] size=%u handle=%u ret=%d\n",
			     a->size, a->handle, ret);
		} else if (nr == NVMAP_ALLOC_NR) {
			struct nvmap_alloc_handle *a = arg;
			tlog("[NVMAP_ALLOC] handle=%u heap=0x%x flags=0x%x align=%u ret=%d\n",
			     a->handle, a->heap_mask, a->flags, a->align, ret);
		} else if (nr == NVMAP_PIN_NR) {
			struct nvmap_pin_handle *a = arg;
			uint32_t *handles = (uint32_t *)(uintptr_t)a->handles;
			uint32_t *addrs = (uint32_t *)(uintptr_t)a->addr;
			int i;
			for (i = 0; i < (int)a->count && i < 16; i++)
				tlog("[NVMAP_PIN] handle=%u → addr=0x%08x ret=%d\n",
				     handles[i], addrs ? addrs[i] : 0, ret);
		} else if (nr == NVMAP_GET_FD_NR) {
			struct nvmap_create_handle *a = arg;
			tlog("[NVMAP_GET_FD] handle=%u → fd=%d ret=%d\n",
			     a->handle, a->fd, ret);
		} else if (nr == NVMAP_FROM_FD_NR) {
			struct nvmap_create_handle *a = arg;
			tlog("[NVMAP_FROM_FD] fd=%d → handle=%u ret=%d\n",
			     a->fd, a->handle, ret);
		}
	}

out:
	return ret;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset) {
	if (!real_mmap)
		real_mmap = dlsym(RTLD_NEXT, "mmap");

	void *ret = real_mmap(addr, len, prot, flags, fd, offset);

	if (ret != MAP_FAILED && is_nvmap_fd(fd) && num_mmaps < MAX_MMAPS) {
		/* Store for cmdbuf readback — use offset as pseudo-handle */
		mmaps[num_mmaps].handle = (uint32_t)offset;
		mmaps[num_mmaps].addr = ret;
		mmaps[num_mmaps].len = len;
		num_mmaps++;
		tlog("[MMAP] fd=%d off=0x%lx len=%zu → %p\n",
		     fd, (long)offset, len, ret);
	}

	return ret;
}

/* On Android, use dlopen/dlsym from bionic (no libdl.so.2) */
#include <dlfcn.h>

static void trace_init(void) {
	real_open = dlsym(RTLD_NEXT, "open");
	real_ioctl = dlsym(RTLD_NEXT, "ioctl");
	real_mmap = dlsym(RTLD_NEXT, "mmap");

	logfp = fopen("/data/local/tmp/isp_trace.log", "w");
	if (logfp)
		tlog("[INIT] isp_wrapper loaded, pid=%d\n", getpid());
}

static void trace_fini(void) {
	if (logfp) {
		tlog("[EXIT] pid=%d\n", getpid());
		fclose(logfp);
		logfp = NULL;
	}
}
