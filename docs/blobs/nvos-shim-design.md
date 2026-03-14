# NvOs Open-Source Shim Design

## Problem Statement

The R24.1 proprietary blobs were compiled against bionic from the Android 5.x-6.x era (2014-2015). The target system uses bionic from Android 9 (Pie, 2018). No newer BSP exists to obtain a compatible `libnvos.so`.

### Potential ABI Issues

1. **pthread_mutex_t size** - May have changed between bionic versions
2. **dlopen behavior** - Dynamic linking semantics may differ
3. **TLS layout** - Thread-local storage layout changes
4. **File descriptor flags** - Default flags for open() may differ
5. **Error code mappings** - errno values may have shifted

## Approach

Write a thin open-source library (`libnvos_shim.so`) that:
1. Exports the same symbols as the original `libnvos.so`
2. Links against current bionic (Android 9)
3. Maintains binary compatibility at the ABI level

---

## Known NvOs Function Categories

### 1. Mutex Operations

```c
typedef struct NvOsMutexRec *NvOsMutexHandle;

NvError NvOsMutexCreate(NvOsMutexHandle *mutex);
void NvOsMutexLock(NvOsMutexHandle mutex);
void NvOsMutexUnlock(NvOsMutexHandle mutex);
void NvOsMutexDestroy(NvOsMutexHandle mutex);
```

**Implementation notes**:
- `NvOsMutexHandle` is likely a pointer to a wrapper structure
- Must handle recursive mutexes if required by blobs
- Map `NvError` appropriately for mutex creation failures

### 2. Semaphore Operations

```c
typedef struct NvOsSemaphoreRec *NvOsSemaphoreHandle;

NvError NvOsSemaphoreCreate(NvOsSemaphoreHandle *sem, NvU32 value);
NvError NvOsSemaphoreWait(NvOsSemaphoreHandle sem);
NvError NvOsSemaphoreWaitTimeout(NvOsSemaphoreHandle sem, NvU32 msec);
void NvOsSemaphoreSignal(NvOsSemaphoreHandle sem);
void NvOsSemaphoreDestroy(NvOsSemaphoreHandle sem);
```

**Implementation notes**:
- Use `sem_init`, `sem_wait`, `sem_timedwait`, `sem_post`, `sem_destroy`
- Timeout in milliseconds requires conversion to timespec

### 3. Thread Operations

```c
typedef struct NvOsThreadRec *NvOsThreadHandle;
typedef void (*NvOsThreadFunction)(void *args);

NvError NvOsThreadCreate(NvOsThreadFunction func, void *args, NvOsThreadHandle *thread);
void NvOsThreadJoin(NvOsThreadHandle thread);
void NvOsThreadYield(void);
void NvOsThreadSetLowPriority(void);
```

**Implementation notes**:
- Map to `pthread_create`, `pthread_join`, `sched_yield`
- Low priority may map to `SCHED_IDLE` or nice value adjustment

### 4. Memory Operations

```c
void *NvOsAlloc(size_t size);
void *NvOsRealloc(void *ptr, size_t size);
void NvOsFree(void *ptr);
void NvOsMemcpy(void *dest, const void *src, size_t size);
void NvOsMemset(void *s, int c, size_t size);
int NvOsMemcmp(const void *s1, const void *s2, size_t size);

// Physical memory mapping
typedef NvU64 NvOsPhysAddr;
typedef struct NvOsMemRec *NvOsMemHandle;

void *NvOsPhysicalMemMap(NvOsPhysAddr phys, size_t size, NvU32 flags, NvOsMemHandle *mem);
NvError NvOsPhysicalMemUnmap(void *ptr, size_t size);
```

**Implementation notes**:
- Direct mapping to `malloc`, `realloc`, `free`, `memcpy`, `memset`, `memcmp`
- Physical memory mapping uses `mmap` with `/dev/mem` or device-specific interfaces

### 5. File Operations

```c
typedef struct NvOsFileRec *NvOsFileHandle;

typedef enum {
    NVOS_OPEN_READ = 0x01,
    NVOS_OPEN_WRITE = 0x02,
    NVOS_OPEN_CREATE = 0x04,
    NVOS_OPEN_APPEND = 0x08
} NvOsOpenFlags;

typedef enum {
    NVOS_SEEK_SET,
    NVOS_SEEK_CUR,
    NVOS_SEEK_END
} NvOsSeekEnum;

NvError NvOsFopen(const char *path, NvU32 flags, NvOsFileHandle *file);
void NvOsFclose(NvOsFileHandle file);
size_t NvOsFread(NvOsFileHandle file, void *ptr, size_t size);
size_t NvOsFwrite(NvOsFileHandle file, const void *ptr, size_t size);
NvError NvOsFseek(NvOsFileHandle file, NvS64 offset, NvOsSeekEnum whence);
NvError NvOsFflush(NvOsFileHandle file);
```

**Implementation notes**:
- Map to `open`, `close`, `read`, `write`, `lseek`, `fsync`
- Flag values need to be mapped to POSIX O_* constants

### 6. Configuration/Property Operations

```c
typedef struct NvOsConfigRec *NvOsConfigHandle;

NvError NvOsConfigGetState(NvOsConfigHandle config, const char *name, NvU32 *value);
NvError NvOsConfigSetState(NvOsConfigHandle config, const char *name, NvU32 value);
```

**Implementation notes**:
- Likely wraps Android property system
- Use `property_get` and `property_set` from bionic

### 7. Time Operations

```c
NvU32 NvOsGetTimeMS(void);
NvU64 NvOsGetTimeUS(void);
void NvOsSleepMS(NvU32 msec);
void NvOsWaitUS(NvU32 usec);
```

**Implementation notes**:
- Use `clock_gettime(CLOCK_MONOTONIC, ...)` for time
- Use `usleep` or `nanosleep` for sleep
- Microsecond wait may need busy-loop for precision

### 8. Debug/Logging Operations

```c
void NvOsDebugPrintf(const char *format, ...);
void NvOsDebugVprintf(const char *format, va_list ap);
void NvOsBreakPoint(const char *file, NvU32 line, const char *condition);
```

**Implementation notes**:
- Map to Android logging: `ALOGD`, `ALOGV`, etc.
- Breakpoint may trigger `raise(SIGTRAP)` or `__builtin_trap()`

### 9. System Information

```c
typedef struct {
    NvU32 osType;
    NvU32 versionMajor;
    NvU32 versionMinor;
    NvU32 versionPatch;
    char versionString[64];
} NvOsOsInfo;

NvError NvOsGetOsInformation(NvOsOsInfo *info);
NvU32 NvOsPageSize(void);
```

**Implementation notes**:
- Return Android as OS type
- Version from build properties
- Page size from `sysconf(_SC_PAGESIZE)`

### 10. Shared Library Operations

```c
typedef struct NvOsLibraryRec *NvOsLibraryHandle;

NvError NvOsLibraryLoad(const char *name, NvOsLibraryHandle *library);
NvError NvOsLibraryGetSymbol(NvOsLibraryHandle library, const char *symbol, void **address);
void NvOsLibraryUnload(NvOsLibraryHandle library);
```

**Implementation notes**:
- Map to `dlopen`, `dlsym`, `dlclose`
- Handle RTLD_NOW vs RTLD_LAZY appropriately

### 11. Atomic Operations

```c
NvS32 NvOsAtomicCompareExchange32(NvS32 *pTarget, NvS32 oldValue, NvS32 newValue);
NvS32 NvOsAtomicExchange32(NvS32 *pTarget, NvS32 value);
NvS32 NvOsAtomicExchangeAdd32(NvS32 *pTarget, NvS32 value);
```

**Implementation notes**:
- Use GCC builtins: `__sync_val_compare_and_swap`, `__sync_lock_test_and_set`, `__sync_fetch_and_add`
- Or C11 `atomic_compare_exchange_strong`, etc.

---

## Implementation Guidelines

### Handle Type Definitions

```c
// Internal structures - exact sizes need to be determined
typedef struct NvOsMutexRec {
    pthread_mutex_t mutex;
} NvOsMutexRec;

typedef struct NvOsSemaphoreRec {
    sem_t sem;
} NvOsSemaphoreRec;

typedef struct NvOsThreadRec {
    pthread_t thread;
} NvOsThreadRec;

typedef struct NvOsFileRec {
    int fd;
} NvOsFileRec;

typedef struct NvOsLibraryRec {
    void *handle;
} NvOsLibraryRec;
```

### Error Code Mapping

```c
typedef enum {
    NvSuccess = 0,
    NvError_NotImplemented = 0x80000001,
    NvError_NotSupported = 0x80000002,
    NvError_NotInitialized = 0x80000003,
    NvError_BadParameter = 0x80000004,
    NvError_Timeout = 0x80000005,
    NvError_InsufficientMemory = 0x80000006,
    NvError_ReadOnlyAttribute = 0x80000007,
    NvError_InvalidState = 0x80000008,
    NvError_InvalidAddress = 0x80000009,
    NvError_InvalidSize = 0x8000000A,
    NvError_BadValue = 0x8000000B,
    NvError_AlreadyAllocated = 0x8000000C,
    NvError_Busy = 0x8000000D,
    NvError_ResourceError = 0x8000000E,
    NvError_AccessDenied = 0x8000000F,
    // ... more error codes
} NvError;
```

Map errno values appropriately:
- `ENOMEM` → `NvError_InsufficientMemory`
- `EINVAL` → `NvError_BadParameter`
- `EACCES` → `NvError_AccessDenied`
- `ETIMEDOUT` → `NvError_Timeout`
- `EBUSY` → `NvError_Busy`

---

## Testing Strategy

### Phase 1: Basic Functionality
1. Build shim library
2. Replace `libnvos.so` with `libnvos_shim.so`
3. Test basic initialization

### Phase 2: libnvrm Testing
1. Load `libnvrm.so` (simplest NvOs dependency)
2. Test device open/close
3. Test memory allocation
4. Test channel operations

### Phase 3: Graphics Stack
1. Test `gralloc.tegra.so`
2. Test buffer allocation and mapping
3. Verify GPU-accessible memory

### Phase 4: HWC
1. Test `hwcomposer.tegra.so`
2. Verify display initialization
3. Test layer composition

### Phase 5: Full GL Stack
1. Test EGL initialization
2. Test GLES context creation
3. Run graphics benchmarks

### Debugging Tools
- `strace` - Trace system calls
- `ltrace` - Trace library calls
- `LD_DEBUG=libs` - Debug dynamic linking
- `logcat` - Android system logs

---

## Risk Assessment

### Risk: Undocumented Functions

Some blobs may use undocumented internal NvOs functions not present in headers.

**Discovery method**:
```bash
# Extract all NvOs* symbols imported by each blob
for blob in *.so; do
    echo "=== $blob ==="
    nm -D "$blob" | grep "U NvOs" | sort | uniq
done
```

**Mitigation**:
- Implement discovered functions as needed
- Use `dlopen`/`dlsym` fallback for unknown functions
- Log unknown function calls for analysis

### Risk: Structure Size Mismatches

If `NvOsMutexHandle` or other handle types have different sizes in the shim vs. original, memory corruption will occur.

**Detection**:
- Run with AddressSanitizer if possible
- Watch for segfaults in mutex operations
- Check for heap corruption patterns

**Mitigation**:
- Determine exact structure sizes from blob analysis
- Use padding fields to match expected sizes
- Consider binary-compatible structure layouts

### Risk: Threading Model Differences

Bionic threading changes between versions may affect behavior.

**Mitigation**:
- Test thoroughly with multi-threaded workloads
- Verify mutex/semphore behavior under contention
- Check thread priority handling

---

## Build Configuration

```makefile
# Android.bp or Android.mk snippet

cc_library_shared {
    name: "libnvos_shim",
    srcs: [
        "nvos_mutex.c",
        "nvos_semaphore.c",
        "nvos_thread.c",
        "nvos_memory.c",
        "nvos_file.c",
        "nvos_time.c",
        "nvos_debug.c",
        "nvos_library.c",
        "nvos_atomic.c",
    ],
    cflags: [
        "-Wall",
        "-Werror",
        "-fvisibility=hidden",
    ],
    export_include_dirs: ["include"],
    shared_libs: [
        "libc",
        "liblog",
    ],
}
```

---

## Conclusion

The NvOs shim is a critical component for Android version compatibility. With careful attention to ABI compatibility and thorough testing, it should be possible to replace the proprietary `libnvos.so` with an open-source implementation that works with current bionic.
