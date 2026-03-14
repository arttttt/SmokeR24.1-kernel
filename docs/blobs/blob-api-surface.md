# Blob API Surface — Functions, Structures, and Kernel Interfaces

This document details the complete API surface exposed by the major proprietary blobs, including function signatures, device nodes, and kernel ioctl interfaces.

---

## libnvrm.so — Resource Manager

The Resource Manager is the core interface between userspace and the Tegra kernel drivers. It provides memory management, channel submission, and synchronization primitives.

### Device Nodes
- `/dev/nvmap` — Memory allocation and management
- `/dev/nvhost-ctrl` — Host1x control operations

### Key API Functions (from headers)

#### Device Management
```c
NvError NvRmOpen(NvRmDeviceHandle *pHandle, NvU32 DeviceId);
NvError NvRmInit(NvRmDeviceHandle hDevice);
void NvRmClose(NvRmDeviceHandle hDevice);
```

#### Memory Management
```c
NvError NvRmMemHandleAlloc(NvRmDeviceHandle hDevice,
                           const NvRmMemHandle *hMems,
                           NvU32 Count,
                           NvU32 Alignment,
                           NvRmMemKind Kind,
                           NvRmMemCompressionTags CompressionTags,
                           NvU32 Size,
                           NvU32 Coherency,
                           NvRmMemHandle *hMem);

void NvRmMemHandleFree(NvRmMemHandle hMem);
NvError NvRmMemPin(NvRmMemHandle hMem);
void NvRmMemUnpin(NvRmMemHandle hMem);
void *NvRmMemMap(NvRmMemHandle hMem, NvU32 Offset, NvU32 Size, NvU32 Flags);
void NvRmMemUnmap(NvRmMemHandle hMem, void *pPtr, NvU32 Size);
NvU32 NvRmMemGetAddress(NvRmMemHandle hMem);
void NvRmMemRead(NvRmMemHandle hMem, NvU32 Offset, void *pDst, NvU32 Size);
void NvRmMemWrite(NvRmMemHandle hMem, NvU32 Offset, const void *pSrc, NvU32 Size);
```

#### DMA-BUF Import/Export
```c
NvError NvRmMemHandleFromFd(NvRmDeviceHandle hDevice, int fd, NvRmMemHandle *hMem);
int NvRmMemGetFd(NvRmDeviceHandle hDevice, NvRmMemHandle hMem);
```

#### Channel Operations
```c
NvError NvRmChannelOpen(NvRmDeviceHandle hDevice,
                        NvRmChannelHandle *phChannel,
                        NvRmModuleID ModuleID,
                        NvRmChannelConfig *pConfig);

void NvRmChannelClose(NvRmChannelHandle hChannel);

NvError NvRmChannelSubmit(NvRmChannelHandle hChannel,
                          const NvRmCommandBuffer *pCommandBuffers,
                          NvU32 NumCommandBuffers,
                          const NvRmSyncPointDescriptor *pSyncPoints,
                          NvU32 NumSyncPoints,
                          const NvRmFence *pFences,
                          NvU32 NumFences,
                          NvRmFence *pFence);
```

#### Synchronization
```c
NvError NvRmChannelSyncPointWaitmexTimeout(NvRmChannelHandle hChannel,
                                           NvU32 SyncPointID,
                                           NvU32 Threshold,
                                           NvU32 *pActualValue,
                                           NvRmTimeVal Timeout,
                                           NvU32 Flags);

NvU32 NvRmChannelSyncPointRead(NvRmChannelHandle hChannel, NvU32 SyncPointID);
```

#### Chip Capabilities
```c
NvError NvRmChipGetCapabilityU32(NvRmDeviceHandle hDevice,
                                 NvRmChipCapability Capability,
                                 NvU32 *pValue);

NvError NvRmSurfaceGetDefaultLayout(NvRmDeviceHandle hDevice,
                                    NvRmSurfaceLayout Layout,
                                    NvBool *pIsDefault);
```

#### Fence Operations
```c
NvError NvRmFenceGetFromFile(NvRmDeviceHandle hDevice, int fd, NvRmFence *pFence);
NvError NvRmFencePutToFile(NvRmDeviceHandle hDevice, const NvRmFence *pFence, int *pFd);
```

#### Module Information
```c
NvU32 NvRmModuleGetNumInstances(NvRmDeviceHandle hDevice, NvRmModuleID ModuleID);
int NvRm_MemmgrGetIoctlFile(NvRmDeviceHandle hDevice);
```

---

## Kernel Ioctl Interface

The following ioctl commands are used by libnvrm.so (extracted from `nvrm_channel_linux.c` source):

### NVMAP ioctls (device: `/dev/nvmap`)
| Command | Code | Description |
|---------|------|-------------|
| NVMAP_IOC_CREATE | 'N' << 8 \| 0 | Create a new memory handle |
| NVMAP_IOC_ALLOC | 'N' << 8 \| 3 | Allocate memory for a handle |
| NVMAP_IOC_FREE | 'N' << 8 \| 4 | Free a memory handle |
| NVMAP_IOC_PIN_MULT | 'N' << 8 \| 8 | Pin multiple handles |
| NVMAP_IOC_UNPIN_MULT | 'N' << 8 \| 9 | Unpin multiple handles |
| NVMAP_IOC_CACHE | 'N' << 8 \| 12 | Cache maintenance operations |
| NVMAP_IOC_GET_FD | 'N' << 8 \| 13 | Export handle to DMA-BUF fd |
| NVMAP_IOC_FROM_FD | 'N' << 8 \| 14 | Import handle from DMA-BUF fd |
| NVMAP_IOC_MMAP | 'N' << 8 \| 15 | Map handle into process space |
| NVMAP_IOC_PARAM | 'N' << 8 \| 17 | Get handle parameters |

### NVHOST Control ioctls (device: `/dev/nvhost-ctrl`)
| Command | Code | Description |
|---------|------|-------------|
| NVHOST_IOCTL_CTRL_GET_VERSION | 'H' << 8 \| 0 | Get driver version |
| NVHOST_IOCTL_CTRL_SYNCPT_READ | 'H' << 8 \| 1 | Read syncpoint value |
| NVHOST_IOCTL_CTRL_SYNCPT_READ_MAX | 'H' << 8 \| 2 | Read max syncpoint value |
| NVHOST_IOCTL_CTRL_SYNCPT_INCR | 'H' << 8 \| 3 | Increment syncpoint |
| NVHOST_IOCTL_CTRL_SYNCPT_WAITEX | 'H' << 8 \| 6 | Wait for syncpoint (extended) |
| NVHOST_IOCTL_CTRL_SYNCPT_WAITMEX | 'H' << 8 \| 9 | Wait for syncpoint (multi-extended) |
| NVHOST_IOCTL_CTRL_SYNC_FENCE_CREATE | 'H' << 8 \| 13 | Create sync fence |
| NVHOST_IOCTL_CTRL_MODULE_MUTEX | 'H' << 8 \| 14 | Module mutex operations |
| NVHOST_IOCTL_CTRL_MODULE_REGRDWR | 'H' << 8 \| 15 | Module register read/write |

### NVHOST Channel ioctls (devices: `/dev/nvhost-*`)
| Command | Code | Description |
|---------|------|-------------|
| NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD | 'H' << 8 \| 0 | Set nvmap file descriptor |
| NVHOST_IOCTL_CHANNEL_SET_TIMEOUT_EX | 'H' << 8 \| 10 | Set timeout (extended) |
| NVHOST_IOCTL_CHANNEL_GET_SYNCPOINTS | 'H' << 8 \| 1 | Get available syncpoints |
| NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT | 'H' << 8 \| 12 | Get specific syncpoint |
| NVHOST_IOCTL_CHANNEL_SUBMIT | 'H' << 8 \| 2 | Submit command buffer (32-bit) |
| NVHOST_IOCTL_CHANNEL_SUBMIT_64 | 'H' << 8 \| 18 | Submit command buffer (64-bit) |
| NVHOST_IOCTL_CHANNEL_GET_TIMEDOUT | 'H' << 8 \| 3 | Get timed out status |
| NVHOST_IOCTL_CHANNEL_GET_WAITBASE | 'H' << 8 \| 4 | Get waitbase |
| NVHOST_IOCTL_CHANNEL_GET_CLK_RATE | 'H' << 8 \| 5 | Get clock rate |
| NVHOST_IOCTL_CHANNEL_SET_CLK_RATE | 'H' << 8 \| 6 | Set clock rate |
| NVHOST_IOCTL_CHANNEL_SET_PRIORITY | 'H' << 8 \| 8 | Set channel priority |
| NVHOST_IOCTL_CHANNEL_MODULE_REGRDWR | 'H' << 8 \| 11 | Module register read/write |
| NVHOST_IOCTL_CHANNEL_GET_MODMUTEX | 'H' << 8 \| 13 | Get module mutex |
| NVHOST_IOCTL_CHANNEL_READ_3D_REG | 'H' << 8 \| 14 | Read 3D register |
| NVHOST_IOCTL_CHANNEL_SET_CTXSWITCH | 'H' << 8 \| 15 | Set context switch parameters |

### Device Nodes Opened
- `/dev/nvhost-ctrl` — Control interface
- `/dev/nvhost-display` — Display engine
- `/dev/nvhost-gr2d` — 2D graphics engine
- `/dev/nvhost-gr3d` — 3D graphics engine
- `/dev/nvhost-isp` — Image signal processor
- `/dev/nvhost-vi` — Video input
- `/dev/nvhost-mpe` — MPEG encoder
- `/dev/nvhost-msenc` — MSENC encoder
- `/dev/nvhost-tsec` — Security engine
- `/dev/nvhost-gpu` — GPU management
- `/dev/nvhost-vic` — VIC engine

---

## libnvos.so — OS Abstraction Layer

NvOs provides a platform abstraction layer wrapping POSIX/bionic functions. Approximately 100 functions are exported.

### Mutex Operations
```c
NvError NvOsMutexCreate(NvOsMutexHandle *mutex);
void NvOsMutexLock(NvOsMutexHandle mutex);
void NvOsMutexUnlock(NvOsMutexHandle mutex);
void NvOsMutexDestroy(NvOsMutexHandle mutex);
```
*Maps to: pthread_mutex_init, pthread_mutex_lock, pthread_mutex_unlock, pthread_mutex_destroy*

### Semaphore Operations
```c
NvError NvOsSemaphoreCreate(NvOsSemaphoreHandle *sem, NvU32 value);
NvError NvOsSemaphoreWait(NvOsSemaphoreHandle sem);
NvError NvOsSemaphoreWaitTimeout(NvOsSemaphoreHandle sem, NvU32 msec);
void NvOsSemaphoreSignal(NvOsSemaphoreHandle sem);
void NvOsSemaphoreDestroy(NvOsSemaphoreHandle sem);
```
*Maps to: sem_init, sem_wait, sem_timedwait, sem_post, sem_destroy*

### Thread Operations
```c
NvError NvOsThreadCreate(NvOsThreadFunction func, void *args, NvOsThreadHandle *thread);
void NvOsThreadJoin(NvOsThreadHandle thread);
void NvOsThreadYield(void);
void NvOsThreadSetLowPriority(void);
```
*Maps to: pthread_create, pthread_join, sched_yield, pthread_setschedparam*

### Memory Operations
```c
void *NvOsAlloc(size_t size);
void *NvOsRealloc(void *ptr, size_t size);
void NvOsFree(void *ptr);
void NvOsMemcpy(void *dest, const void *src, size_t size);
void NvOsMemset(void *s, int c, size_t size);
int NvOsMemcmp(const void *s1, const void *s2, size_t size);
void *NvOsPhysicalMemMap(NvOsPhysAddr phys, size_t size, NvU32 flags, NvOsMemHandle *mem);
```
*Maps to: malloc, realloc, free, memcpy, memset, memcmp, mmap*

### File Operations
```c
NvError NvOsFopen(const char *path, NvU32 flags, NvOsFileHandle *file);
void NvOsFclose(NvOsFileHandle file);
size_t NvOsFread(NvOsFileHandle file, void *ptr, size_t size);
size_t NvOsFwrite(NvOsFileHandle file, const void *ptr, size_t size);
NvError NvOsFseek(NvOsFileHandle file, NvS64 offset, NvOsSeekEnum whence);
NvError NvOsFflush(NvOsFileHandle file);
NvError NvOsConfigGetState(NvOsConfigHandle config, const char *name, NvU32 *value);
NvError NvOsConfigSetState(NvOsConfigHandle config, const char *name, NvU32 value);
```
*Maps to: open, close, read, write, lseek, fsync, property_get/property_set*

### Time Operations
```c
NvU32 NvOsGetTimeMS(void);
NvU64 NvOsGetTimeUS(void);
void NvOsSleepMS(NvU32 msec);
void NvOsWaitUS(NvU32 usec);
```
*Maps to: clock_gettime(CLOCK_MONOTONIC), usleep/nanosleep*

### Debug/Logging
```c
void NvOsDebugPrintf(const char *format, ...);
void NvOsDebugVprintf(const char *format, va_list ap);
void NvOsBreakPoint(const char *file, NvU32 line, const char *condition);
```
*Maps to: ALOG, debuggerd integration*

### System Information
```c
NvError NvOsGetOsInformation(NvOsOsInfo *info);
NvU32 NvOsPageSize(void);
```

### Shared Library Loading
```c
NvError NvOsLibraryLoad(const char *name, NvOsLibraryHandle *library);
NvError NvOsLibraryGetSymbol(NvOsLibraryHandle library, const char *symbol, void **address);
void NvOsLibraryUnload(NvOsLibraryHandle library);
```
*Maps to: dlopen, dlsym, dlclose*

### Atomic Operations
```c
NvS32 NvOsAtomicCompareExchange32(NvS32 *pTarget, NvS32 oldValue, NvS32 newValue);
NvS32 NvOsAtomicExchange32(NvS32 *pTarget, NvS32 value);
NvS32 NvOsAtomicExchangeAdd32(NvS32 *pTarget, NvS32 value);
```
*Maps to: __sync_val_compare_and_swap, __sync_lock_test_and_set, __sync_fetch_and_add*

---

## libnvblit.so — 2D Blitter Abstraction

Dispatches blit operations to either the 2D engine or VIC based on operation type.

```c
NvError NvBlitInit(NvRmDeviceHandle hRm, NvBlitHandle *phBlit);
void NvBlitDeinit(NvBlitHandle hBlit);
NvError NvBlitSurface(NvBlitHandle hBlit,
                      const NvBlitSurface *pSrc,
                      const NvRect *pSrcRect,
                      NvBlitSurface *pDst,
                      const NvRect *pDstRect,
                      NvU32 flags);
```

Uses `NvRmChannel` for command submission and syncpoints for synchronization.

---

## libnvddk_2d_v2.so — 2D DDK Driver

Generates command buffers for the host1x 2D class (gr2d).

```c
NvError NvDdk2dOpen(NvRmDeviceHandle hRm, NvDdk2dHandle *phDdk2d);
void NvDdk2dClose(NvDdk2dHandle hDdk2d);
NvError NvDdk2dBlit(NvDdk2dHandle hDdk2d,
                    NvDdk2dSurface *pDst,
                    const NvRect *pDstRect,
                    NvDdk2dSurface *pSrc,
                    const NvRect *pSrcRect,
                    const NvDdk2dFixedRect *pSrcRectSub,
                    NvU32 flags,
                    const NvDdk2dBlend *pBlend,
                    const NvDdk2dColorKey *pColorKey);
NvError NvDdk2dClear(NvDdk2dHandle hDdk2d,
                     NvDdk2dSurface *pDst,
                     const NvRect *pDstRect,
                     NvU32 color);
```

---

## libnvddk_vic.so — VIC DDK Driver

Generates command buffers for the VIC (Video Image Compositor) engine.

```c
NvError NvDdkVicOpen(NvRmDeviceHandle hRm, NvDdkVicHandle *phVic);
void NvDdkVicClose(NvDdkVicHandle hVic);
NvError NvDdkVicProcess(NvDdkVicHandle hVic,
                        const NvDdkVicConfig *pConfig,
                        const NvDdkVicSurface *pInputs,
                        NvU32 numInputs,
                        const NvDdkVicSurface *pOutputs,
                        NvU32 numOutputs,
                        NvRmFence *pFence);
```

Supports: YUV conversion, scaling, rotation, and deinterlacing in hardware.

---

## Total Ioctl Surface Summary

The complete ioctl surface spans 86 distinct ioctl commands across:

| Subsystem | Prefix | Count |
|-----------|--------|-------|
| NVMAP | 'N' | ~15 |
| NVHOST Channel | 'H' | ~20 |
| NVHOST Control | 'H' | ~15 |
| GPU | 'G' | ~20 |
| Display | 'D' | ~10 |
| Display Control | 'C' | ~6 |

**Total: 86 distinct ioctl commands**
