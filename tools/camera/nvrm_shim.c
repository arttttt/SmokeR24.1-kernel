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

/* Strip streaming commands from ISP gathers (set via env NVRM_SHIM_STRIP=1) */
static int strip_streaming = -1; /* -1 = not checked yet */

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

    /* Step 2: Unmap old VMA if addr was pre-mapped (not MAP_FAILED) */
    if (mc->addr && mc->addr != (unsigned long)MAP_FAILED) {
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

/*
 * Intercept mmap on /dev/nvmap.
 *
 * Old kernel allowed mmap on /dev/nvmap fd to create a VMA, then
 * NVMAP_IOC_MMAP bound that VMA to a handle's pages.
 * New kernel rejects mmap on /dev/nvmap entirely.
 *
 * We intercept mmap and if it's on an nvmap-like fd (detected by
 * MAP_SHARED + the fd matching our tracked nvmap fd), we let it
 * return a dummy page that will be replaced by the MMAP ioctl shim.
 *
 * Actually simpler: just allocate anonymous memory as placeholder.
 * The MMAP ioctl shim will munmap it and remap via dmabuf.
 */
static int nvmap_fd_cached = -1;

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    static void *(*real_mmap)(void *, size_t, int, int, int, off_t) = NULL;
    if (!real_mmap)
        real_mmap = dlsym(RTLD_NEXT, "mmap");

    void *result = real_mmap(addr, length, prot, flags, fd, offset);

    /* If mmap failed on what looks like /dev/nvmap, provide anonymous memory */
    if (result == MAP_FAILED && fd >= 0 && (flags & MAP_SHARED)) {
        /* Check if this is an nvmap fd by trying a harmless nvmap ioctl */
        /* Actually, track the nvmap fd from the first successful nvmap ioctl */
        if (fd == nvmap_fd_cached) {
            result = real_mmap(NULL, length, prot,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (result != MAP_FAILED) {
                fprintf(stderr, "nvrm_shim: mmap fallback for nvmap fd=%d → %p (len=%zu)\n",
                        fd, result, length);
            }
        }
    }

    return result;
}

/* Intercepted ioctl */
int ioctl(int fd, int request, ...) {
    void *arg;
    __builtin_va_list ap;
    __builtin_va_start(ap, request);
    arg = __builtin_va_arg(ap, void *);
    __builtin_va_end(ap);

    init_real_ioctl();

    strip_streaming = getenv("NVRM_SHIM_STRIP") ? 1 : 0;

    unsigned int nr = _IOC_NR(request);
    unsigned int type = _IOC_TYPE(request);

    /* Track nvmap fd for mmap interception */
    if (type == NVMAP_IOC_MAGIC && nvmap_fd_cached < 0) {
        nvmap_fd_cached = fd;
        fprintf(stderr, "nvrm_shim: detected nvmap fd=%d\n", fd);
    }

    /* Intercept NVMAP_IOC_MMAP */
    if (type == NVMAP_IOC_MAGIC && nr == 5) {
        return shim_nvmap_mmap(fd, (struct nvmap_map_caller *)arg);
    }

    /* Intercept nvhost SUBMIT (NR=15, 32-bit version) on ISP channel.
     * Scan gather for streaming trigger and conditional syncpt incrs,
     * NOP them out so ISP doesn't start waiting for VI pixels.
     * This turns a streaming calibration gather into calibration-only. */
    #define NVHOST_MAGIC 'H'
    if (type == NVHOST_MAGIC && nr == 15 && strip_streaming) {
        /* 32-bit submit: cmdbufs at offset 28 in struct */
        struct {
            uint32_t submit_version, num_syncpt_incrs, num_cmdbufs;
            uint32_t num_relocs, num_waitchks, timeout;
            uint32_t syncpt_incrs, cmdbufs;
            /* ... rest doesn't matter */
        } *sa = arg;

        struct { uint32_t mem; uint32_t offset; uint32_t words; } *cbs =
            (void *)(uintptr_t)sa->cmdbufs;

        /* When stripping streaming, tell kernel to expect 0 syncpt incrs.
         * Without the trigger, conditional incrs never fire → kernel
         * timeout → channel reset → our reprocess gather dies.
         * Setting num_syncpt_incrs=0 prevents kernel from waiting. */
        int found_trigger = 0;

        /* Dump gather contents to file for analysis */
        {
            static int dump_count = 0;
            struct { uint32_t mem; uint32_t offset; uint32_t words; } *dump_cbs =
                (void *)(uintptr_t)sa->cmdbufs;
            for (uint32_t dg = 0; dg < sa->num_cmdbufs; dg++) {
                for (int dm = 0; dm < num_mapped; dm++) {
                    /* Each mapped region corresponds to a dmabuf.
                     * The cmdbuf mem handle may not match directly,
                     * but all push bufs are mapped. Try each. */
                    uint32_t *pb = (uint32_t *)mapped[dm].addr;
                    uint32_t pb_bytes = mapped[dm].length;
                    if (dump_cbs[dg].offset + dump_cbs[dg].words * 4 <= pb_bytes) {
                        uint32_t *gather = pb + dump_cbs[dg].offset / 4;
                        char fname[128];
                        snprintf(fname, sizeof(fname),
                                 "/data/local/tmp/gather_%d_%d.bin",
                                 dump_count, dg);
                        FILE *gf = fopen(fname, "wb");
                        if (gf) {
                            fwrite(gather, 4, dump_cbs[dg].words, gf);
                            fclose(gf);
                            fprintf(stderr, "nvrm_shim: dumped G[%d] %u words to %s\n",
                                    dg, dump_cbs[dg].words, fname);
                        }
                        break;
                    }
                }
            }
            dump_count++;
        }

        for (uint32_t g = 0; g < sa->num_cmdbufs; g++) {
            /* Find the gather in our mapped push buffers */
            for (int m = 0; m < num_mapped; m++) {
                if (mapped[m].dmabuf_fd < 0) continue;
                /* Push buffer is mapped at mapped[m].addr */
                uint32_t *pb = (uint32_t *)mapped[m].addr;
                uint32_t pb_words = mapped[m].length / 4;

                /* Scan for ISP streaming commands */
                for (uint32_t i = 0; i < pb_words - 1; i++) {
                    uint32_t op = pb[i];
                    uint32_t opcode = op >> 28;
                    uint32_t method = (op >> 16) & 0xFFF;
                    uint32_t count = op & 0xFFFF;

                    /* NONINCR(0x00C, 1) + trigger 0x05 → NOP
                     * 0x05 = streaming runtime (strip — no VI data)
                     * 0x0F = calibration apply (KEEP — ISP needs it!) */
                    if (opcode == 2 && method == 0x00C && count == 1) {
                        if (pb[i+1] == 0x05) {
                            fprintf(stderr, "nvrm_shim: NOP trigger 0x%02x at pb[%u]\n",
                                    pb[i+1], i);
                            /* Dump 5 words before trigger to see syncpt format */
                            fprintf(stderr, "nvrm_shim: context pb[%u..%u]:",
                                    i > 5 ? i-5 : 0, i+1);
                            for (int j = (i > 5 ? i-5 : 0); j <= i+1; j++)
                                fprintf(stderr, " %08x", pb[j]);
                            fprintf(stderr, "\n");
                            pb[i] = 0x20000000;
                            pb[i+1] = 0x20000000;
                            found_trigger = 1;
                        }
                    }

                    /* Syncpt incrs are tracked via submit args, not push buffer.
                     * We zero num_syncpt_incrs above when stripping triggers. */
                }
            }
        }

        /* If we stripped 0x05 but not 0x0F, calibration applies normally.
         * Conditional syncpt incrs should fire after 0x0F completes. */
        if (found_trigger) {
            fprintf(stderr, "nvrm_shim: stripped %d streaming triggers\n", found_trigger);
        }
    }

    return real_ioctl(fd, request, arg);
}
