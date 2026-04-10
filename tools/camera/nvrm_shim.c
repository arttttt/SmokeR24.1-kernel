/*
 * NvRm compatibility shim for MIUI blobs on SmokeR24.1 kernel.
 *
 * Problem: MIUI libnvrm.so calls NVMAP_IOC_MMAP (ioctl 'N'/5) which was
 * removed in newer kernels. The kernel returns ENOTTY, libnvrm ignores
 * the error, and subsequent code crashes on NULL push buffer pointer.
 *
 * Solution: Intercept ioctl() and translate NVMAP_IOC_MMAP to the new
 * path: NVMAP_IOC_GET_FD → mmap() on dmabuf fd.
 *
 * Old flow (MIUI libnvrm.so):
 *   addr = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, nvmap_fd, 0)
 *   ioctl(nvmap_fd, NVMAP_IOC_MMAP, {handle, 0, size, flags, addr})
 *   // kernel binds the VMA to the nvmap handle
 *
 * New flow (this shim):
 *   ioctl(nvmap_fd, NVMAP_IOC_GET_FD, {handle}) → dmabuf_fd
 *   addr = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, dmabuf_fd, offset)
 *   // return addr to caller via the map_caller struct
 *
 * Build:
 *   $CC --sysroot=$SYSROOT -std=gnu99 -shared -fPIC -o nvrm_shim.so nvrm_shim.c
 *
 * Usage: load via dlopen BEFORE libnvrm.so, or LD_PRELOAD
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/ioctl.h>

/* nvmap ioctl magic */
#define NVMAP_IOC_MAGIC 'N'

/* Old NVMAP_IOC_MMAP — removed in 24.1, returns ENOTTY */
struct nvmap_map_caller {
    uint32_t handle;
    uint32_t offset;
    uint32_t length;
    uint32_t flags;
    unsigned long addr;
};
#define NVMAP_IOC_MMAP _IOWR(NVMAP_IOC_MAGIC, 5, struct nvmap_map_caller)

/* New NVMAP_IOC_GET_FD — exports handle as dmabuf fd */
struct nvmap_create_handle {
    union {
        uint32_t id;
        uint32_t size;
        int32_t fd;
    };
    uint32_t handle;
};
#define NVMAP_IOC_GET_FD _IOWR(NVMAP_IOC_MAGIC, 15, struct nvmap_create_handle)

/* NVMAP_IOC_PARAM — get handle parameters */
struct nvmap_handle_param {
    uint32_t handle;
    uint32_t param;
    unsigned long result;
};
#define NVMAP_IOC_PARAM _IOWR(NVMAP_IOC_MAGIC, 8, struct nvmap_handle_param)
#define NVMAP_HANDLE_PARAM_SIZE 1

/* Track dmabuf fds so we can close them later */
#define MAX_MAPPED 64
static struct {
    unsigned long addr;
    uint32_t length;
    int dmabuf_fd;
} mapped[MAX_MAPPED];
static int num_mapped;

static int (*real_ioctl)(int fd, int request, ...) = NULL;

static void init_real_ioctl(void) {
    if (!real_ioctl) {
        real_ioctl = (int (*)(int, int, ...))dlsym(RTLD_NEXT, "ioctl");
        if (!real_ioctl) {
            /* fallback: use syscall */
            real_ioctl = (int (*)(int, int, ...))dlsym(RTLD_DEFAULT, "ioctl");
        }
    }
}

/*
 * Translate NVMAP_IOC_MMAP to GET_FD + mmap.
 *
 * The old flow was:
 *   1. userspace calls mmap(NULL, size, ..., nvmap_fd, 0) → gets a VMA
 *   2. userspace calls ioctl(nvmap_fd, NVMAP_IOC_MMAP, {handle, off, len, flags, addr})
 *   3. kernel remaps the VMA to point to the nvmap handle's pages
 *
 * We can't do step 3 without kernel support. Instead:
 *   1. Get a dmabuf fd for the handle via NVMAP_IOC_GET_FD
 *   2. munmap the old VMA (from step 1 of old flow)
 *   3. mmap with the dmabuf fd → new addr
 *   4. Write new addr back to caller's struct
 */
static int shim_nvmap_mmap(int fd, struct nvmap_map_caller *mc) {
    if (!mc || !mc->handle || !mc->length) {
        errno = EINVAL;
        return -1;
    }

    fprintf(stderr, "nvrm_shim: MMAP handle=%u offset=%u length=%u addr=0x%lx\n",
            mc->handle, mc->offset, mc->length, mc->addr);

    /* Step 1: Get dmabuf fd for this handle */
    struct nvmap_create_handle gf;
    memset(&gf, 0, sizeof(gf));
    gf.handle = mc->handle;
    if (real_ioctl(fd, NVMAP_IOC_GET_FD, &gf) < 0) {
        fprintf(stderr, "nvrm_shim: GET_FD failed for handle %u: %s\n",
                mc->handle, strerror(errno));
        return -1;
    }
    int dmabuf_fd = gf.fd;
    fprintf(stderr, "nvrm_shim: GET_FD handle=%u → dmabuf_fd=%d\n",
            mc->handle, dmabuf_fd);

    /* Step 2: Unmap old VMA if addr was pre-mapped */
    if (mc->addr) {
        munmap((void *)mc->addr, mc->length);
    }

    /* Step 3: mmap the dmabuf fd */
    void *new_addr = mmap(NULL, mc->length,
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED,
                          dmabuf_fd, mc->offset);
    if (new_addr == MAP_FAILED) {
        fprintf(stderr, "nvrm_shim: mmap dmabuf_fd=%d failed: %s\n",
                dmabuf_fd, strerror(errno));
        close(dmabuf_fd);
        return -1;
    }

    fprintf(stderr, "nvrm_shim: mapped handle=%u → addr=%p (len=%u)\n",
            mc->handle, new_addr, mc->length);

    /* Track for cleanup */
    if (num_mapped < MAX_MAPPED) {
        mapped[num_mapped].addr = (unsigned long)new_addr;
        mapped[num_mapped].length = mc->length;
        mapped[num_mapped].dmabuf_fd = dmabuf_fd;
        num_mapped++;
    } else {
        /* Can't track, but don't leak fd */
        /* Keep fd open — closing it would unmap the buffer */
    }

    /* Step 4: Return new address to caller */
    mc->addr = (unsigned long)new_addr;

    return 0;
}

/* Intercepted ioctl */
int ioctl(int fd, int request, ...) {
    void *arg;
    __builtin_va_list ap;
    __builtin_va_start(ap, request);
    arg = __builtin_va_arg(ap, void *);
    __builtin_va_end(ap);

    init_real_ioctl();

    /* Intercept NVMAP_IOC_MMAP */
    unsigned int nr = _IOC_NR(request);
    unsigned int type = _IOC_TYPE(request);

    if (type == NVMAP_IOC_MAGIC && nr == 5) {
        /* This is NVMAP_IOC_MMAP — translate it */
        return shim_nvmap_mmap(fd, (struct nvmap_map_caller *)arg);
    }

    return real_ioctl(fd, request, arg);
}
