/*
 * ptrace-based .so injector for Android.
 *
 * Injects a shared library into a running process by:
 * 1. ptrace attach
 * 2. Find dlopen in target process
 * 3. Allocate memory via mmap syscall
 * 4. Write library path
 * 5. Call dlopen(path, RTLD_NOW)
 * 6. Detach
 *
 * Usage: isp_injector <pid> <path_to_so>
 *
 * Build with NDK:
 *   armv7a-linux-androideabi19-clang -pie -o isp_injector isp_injector.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <asm/ptrace.h>

/* PTRACE constants for Android NDK */
#ifndef PTRACE_PEEKDATA
#define PTRACE_PEEKDATA 2
#define PTRACE_POKEDATA 5
#define PTRACE_CONT     7
#define PTRACE_GETREGS  12
#define PTRACE_SETREGS  13
#define PTRACE_ATTACH   16
#define PTRACE_DETACH   17
#endif

#define ARM_r0  uregs[0]
#define ARM_r1  uregs[1]
#define ARM_r2  uregs[2]
#define ARM_r3  uregs[3]
#define ARM_r4  uregs[4]
#define ARM_r5  uregs[5]
#define ARM_r6  uregs[6]
#define ARM_r7  uregs[7]
#define ARM_r8  uregs[8]
#define ARM_r9  uregs[9]
#define ARM_r10 uregs[10]
#define ARM_fp  uregs[11]
#define ARM_ip  uregs[12]
#define ARM_sp  uregs[13]
#define ARM_lr  uregs[14]
#define ARM_pc  uregs[15]
#define ARM_cpsr uregs[16]

/* Find base address of a library in target process */
static unsigned long find_lib_base(pid_t pid, const char *libname) {
    char maps[64];
    snprintf(maps, sizeof(maps), "/proc/%d/maps", pid);
    FILE *f = fopen(maps, "r");
    if (!f) return 0;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, libname) && strstr(line, "r-xp")) {
            unsigned long base;
            sscanf(line, "%lx-", &base);
            fclose(f);
            return base;
        }
    }
    fclose(f);
    return 0;
}

/* Find function address in target process by computing offset */
static unsigned long find_remote_func(pid_t pid, const char *libname,
                                       void *local_func) {
    unsigned long local_base = find_lib_base(getpid(), libname);
    unsigned long remote_base = find_lib_base(pid, libname);
    if (!local_base || !remote_base) return 0;
    return remote_base + ((unsigned long)local_func - local_base);
}

/* Read/write data to target process memory */
static int ptrace_read(pid_t pid, unsigned long addr, void *buf, size_t len) {
    size_t i;
    long *dst = (long *)buf;
    for (i = 0; i < len; i += sizeof(long)) {
        *dst = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + i), NULL);
        if (*dst == -1 && errno) return -1;
        dst++;
    }
    return 0;
}

static int ptrace_write(pid_t pid, unsigned long addr, const void *buf, size_t len) {
    size_t i;
    const long *src = (const long *)buf;
    for (i = 0; i < len; i += sizeof(long)) {
        if (ptrace(PTRACE_POKEDATA, pid, (void *)(addr + i), (void *)*src) < 0)
            return -1;
        src++;
    }
    return 0;
}

/* Call a function in the target process.
 * Sets LR=0 so function returns to addr 0 → SIGSEGV.
 * We catch the SIGSEGV and read r0 for return value. */
static long ptrace_call(pid_t pid, unsigned long func_addr,
                         long *args, int nargs, struct pt_regs *saved_regs) {
    struct pt_regs regs;
    memcpy(&regs, saved_regs, sizeof(regs));

    /* Set args in r0-r3 */
    if (nargs > 0) regs.ARM_r0 = args[0];
    if (nargs > 1) regs.ARM_r1 = args[1];
    if (nargs > 2) regs.ARM_r2 = args[2];
    if (nargs > 3) regs.ARM_r3 = args[3];

    /* Set PC to function address */
    if (func_addr & 1) {
        /* Thumb */
        regs.ARM_pc = func_addr & ~1;
        regs.ARM_cpsr |= (1 << 5);
    } else {
        regs.ARM_pc = func_addr;
        regs.ARM_cpsr &= ~(1 << 5);
    }

    /* LR = 0 → function returns to 0 → SIGSEGV */
    regs.ARM_lr = 0;

    if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) < 0) {
        printf("  ptrace_call: SETREGS failed: %s\n", strerror(errno));
        return -1;
    }
    if (ptrace(PTRACE_CONT, pid, NULL, NULL) < 0) {
        printf("  ptrace_call: CONT failed: %s\n", strerror(errno));
        return -1;
    }

    int status;
    waitpid(pid, &status, WUNTRACED);

    if (WIFSTOPPED(status)) {
        int sig = WSTOPSIG(status);
        printf("  ptrace_call: stopped by signal %d\n", sig);
    }

    /* Get return value from r0 */
    ptrace(PTRACE_GETREGS, pid, NULL, &regs);
    printf("  ptrace_call: ret r0=0x%lx pc=0x%lx\n", regs.ARM_r0, regs.ARM_pc);
    return regs.ARM_r0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <pid> <path_to_so>\n", argv[0]);
        return 1;
    }

    pid_t pid = atoi(argv[1]);
    const char *so_path = argv[2];

    printf("Injecting %s into pid %d\n", so_path, pid);

    /* Check library path exists */
    if (access(so_path, R_OK) < 0) {
        printf("Cannot access %s: %s\n", so_path, strerror(errno));
        return 1;
    }

    /* Attach */
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) < 0) {
        printf("PTRACE_ATTACH failed: %s\n", strerror(errno));
        return 1;
    }
    int status;
    waitpid(pid, &status, WUNTRACED);
    printf("Attached to %d\n", pid);

    /* Save registers */
    struct pt_regs saved_regs;
    ptrace(PTRACE_GETREGS, pid, NULL, &saved_regs);
    printf("Saved regs, PC=0x%lx SP=0x%lx\n", saved_regs.ARM_pc, saved_regs.ARM_sp);

    /* Find dlopen in target */
    /* On Android 4.4, dlopen is in /system/bin/linker */
    unsigned long remote_dlopen = find_remote_func(pid, "linker", (void *)dlopen);
    if (!remote_dlopen) {
        /* Try libc */
        remote_dlopen = find_remote_func(pid, "libc.so", (void *)dlopen);
    }
    if (!remote_dlopen) {
        /* Try libdl */
        remote_dlopen = find_remote_func(pid, "libdl.so", (void *)dlopen);
    }
    printf("Remote dlopen: 0x%lx\n", remote_dlopen);

    if (!remote_dlopen) {
        printf("Cannot find dlopen in target\n");
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return 1;
    }

    /* Write library path below stack pointer (safe scratch area) */
    unsigned long remote_mem = saved_regs.ARM_sp - 256;
    char path_buf[256];
    memset(path_buf, 0, sizeof(path_buf));
    strncpy(path_buf, so_path, sizeof(path_buf) - 1);
    ptrace_write(pid, remote_mem, path_buf, (strlen(so_path) / sizeof(long) + 2) * sizeof(long));
    printf("Wrote path '%s' to 0x%lx (below SP)\n", so_path, remote_mem);

    /* Call dlopen(path, flags)
     * Android 4.4 bionic: RTLD_NOW=0, RTLD_LAZY=1
     * But error says "invalid flags: 4" even when we pass 0.
     * The callee_addr might not be dlopen but a wrapper.
     * Try passing flags=1 (RTLD_LAZY) which is always valid. */
    long dlopen_args[2] = { (long)remote_mem, 1 /* RTLD_LAZY */ };
    printf("dlopen args: r0=0x%lx r1=%ld\n", dlopen_args[0], dlopen_args[1]);
    unsigned long dlopen_ret = ptrace_call(pid, remote_dlopen, dlopen_args, 2, &saved_regs);
    printf("dlopen returned: 0x%lx\n", dlopen_ret);

    if (!dlopen_ret) {
        /* Call dlerror to get error message */
        unsigned long remote_dlerror = find_remote_func(pid, "linker", (void *)dlerror);
        if (remote_dlerror) {
            long no_args[1] = {0};
            unsigned long err_ptr = ptrace_call(pid, remote_dlerror, no_args, 0, &saved_regs);
            if (err_ptr) {
                char errbuf[256];
                memset(errbuf, 0, sizeof(errbuf));
                ptrace_read(pid, err_ptr, errbuf, sizeof(errbuf));
                printf("dlerror: %s\n", errbuf);
            }
        }
        printf("dlopen failed\n");
    } else {
        printf("SUCCESS: library injected, handle=0x%lx\n", dlopen_ret);
        printf("Check /data/local/tmp/isp_inject.log for results\n");
    }

    /* Restore registers and detach */
    ptrace(PTRACE_SETREGS, pid, NULL, &saved_regs);
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    printf("Detached from %d\n", pid);

    return 0;
}
