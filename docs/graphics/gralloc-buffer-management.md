# Tegra K1 Gralloc & Buffer Management

## Overview

This document describes the NVIDIA Tegra K1 gralloc (graphics allocator) implementation, including buffer structures, fence management, and the relationship between user-space handles and kernel-side buffer objects.

## Source Location

```
/vendor_nvidia_jxd_src/tegra/graphics-partner/android/gralloc/
```

## NvNativeHandle Structure

The `NvNativeHandle` is the core buffer handle structure used throughout the Tegra graphics stack. It extends Android's `native_handle_t` with NVIDIA-specific fields.

```c
struct NvNativeHandleRec {
    native_handle_t     Base;           // Android native handle base
    int                 MemId;          // Shared memory handle ID
    int                 SurfMemFd;      // Surface memory file descriptor
    NvU32               Magic;          // NVGR_HANDLE_MAGIC (0xDAFFCAFF)
    pid_t               Owner;          // Owning process ID
    NvNativeHandle     *hSelf;          // Self pointer for validation
    NvNativeBufferType  Type;           // SINGLE, STEREO, or YUV
    NvU32               SurfCount;      // Number of surfaces (max 3)
    NvRmSurface         Surf[NVGR_MAX_SURFACES];  // Surface descriptors
    NvGrBuffer         *Buf;            // Kernel-side shared data pointer
    NvU8               *Pixels;         // CPU-mapped pixel pointer
};
```

### Buffer Types

```c
typedef enum {
    NvNativeBufferType_Single = 0,      // Standard single buffer
    NvNativeBufferType_Stereo,          // Stereo (3D) buffer
    NvNativeBufferType_YUV              // YUV video buffer
} NvNativeBufferType;
```

## NvRmSurface Structure

The `NvRmSurface` structure fully exposes surface layout information (not opaque):

```c
typedef struct NvRmSurfaceRec {
    NvU32 Width;                    // Surface width in pixels
    NvU32 Height;                   // Surface height in pixels
    NvColorFormat ColorFormat;      // Pixel format
    NvRmSurfaceLayout Layout;       // Layout: Pitch / Tiled / Blocklinear
    NvU32 Pitch;                    // Row pitch in bytes
    NvRmMemHandle hMem;             // Memory handle
    NvU32 Offset;                   // Offset within memory allocation
    void* pBase;                    // CPU-mapped base address
    NvRmMemKind Kind;               // Memory kind (compression)
    NvU32 BlockHeightLog2;          // Block height parameter (blocklinear)
    NvDisplayScanFormat DisplayScanFormat;  // Progressive/interlaced
    NvU32 SecondFieldOffset;        // Offset to second field (interlaced)
} NvRmSurface;
```

### Surface Layouts

```c
typedef enum {
    NvRmSurfaceLayout_Pitch = 0,    // Linear/pitch layout
    NvRmSurfaceLayout_Tiled,        // 16x16 tiled layout
    NvRmSurfaceLayout_Blocklinear   // Block linear layout (compressed)
} NvRmSurfaceLayout;
```

## NvGrBuffer (Kernel-Side Shared Data)

The `NvGrBuffer` structure resides in shared memory and is accessible from both user space and kernel:

```c
struct NvGrBuffer {
    NvU32 Magic;                    // NVGR_BUFFER_MAGIC (0xB00BD00D)
    NvU32 State;                    // Buffer state flags
    NvGrLock Lock;                  // Lock state with mutex
    NvRmFence Fences[NVGR_MAX_FENCES];  // Fence array (up to 6)
    NvU32 WriteCount;               // Write operation counter
    NvU32 CompressionFlag;          // Compression state
    NvGrStereoInfo StereoInfo;      // Stereo buffer info
    NvU64 VideoTimestamp;           // Video timestamp
    NvGrRect SourceCrop;            // Source crop rectangle
    // Additional fields...
};
```

### Fence Array in Buffer

```c
typedef struct NvRmFenceRec {
    NvU32 SyncPointID;              // Syncpoint identifier
    NvU32 Value;                    // Syncpoint value
} NvRmFence;
```

Each buffer can have up to 6 fences tracking different operations (read, write, etc.).

## Fence API (nvsync.h)

The nvsync library provides Android fence file descriptor management:

```c
// Check fence validity
static inline int nvsync_is_valid(int fd) {
    return fd >= 0;
}

// Wait on fence with timeout (milliseconds)
int nvsync_wait(int fd, int timeout);

// Close fence file descriptor
void nvsync_close(int fd);

// Duplicate fence (creates new fd referring to same sync object)
int nvsync_dup(const char* name, int fd);

// Merge two fences (creates new fd)
int nvsync_merge(const char* name, int fd1, int fd2);

// Convert Android fence fd to NvRmFence array
int nvsync_to_fence(int fd, NvRmFence* fences, int numFences);

// Create Android fence fd from NvRmFence array
int nvsync_from_fence(const char* name, NvRmFence* fences, int numFences);
```

### Fence Lifecycle

1. **Creation**: Fence created via `nvsync_from_fence()` or received from producer
2. **Waiting**: Consumer waits via `nvsync_wait()` before accessing buffer
3. **Closing**: Consumer closes via `nvsync_close()` after wait completes
4. **Duplication**: Use `nvsync_dup()` when multiple consumers need same fence

## Buffer Fence Management API

### Adding Fences to Buffer

```c
// Add NvRmFence to buffer for specific usage
int NvGrAddFence(NvNativeHandle* h, int usage, const NvRmFence* fence);

// Add Android fence fd to buffer
int NvGrAddFenceFd(NvNativeHandle* h, int usage, int fenceFd);
```

### Retrieving Fences from Buffer

```c
// Get NvRmFence array from buffer
int NvGrGetFences(NvNativeHandle* h, int usage, NvRmFence* fences, int* numFences);

// Get Android fence fd from buffer
int NvGrGetFenceFd(NvNativeHandle* h, int usage);
```

## Gralloc Module Extensions (NvGrModule)

The Tegra gralloc module extends the standard Android gralloc with additional operations:

### Fence Operations

```c
// Add NVIDIA fence to buffer
int (*add_nvfence)(gralloc_module_t const* module,
                   buffer_handle_t handle,
                   int usage,
                   const NvRmFence* fence);

// Get NVIDIA fences from buffer
int (*get_nvfences)(gralloc_module_t const* module,
                    buffer_handle_t handle,
                    int usage,
                    NvRmFence* fences,
                    int* numFences);

// Get Android fence fd
int (*get_fence)(gralloc_module_t const* module,
                 buffer_handle_t handle,
                 int usage);

// Add Android fence fd
int (*add_fence)(gralloc_module_t const* module,
                 buffer_handle_t handle,
                 int usage,
                 int fenceFd);
```

### Blit Operations

```c
// Clear/fill rectangle in buffer
int (*clear)(gralloc_module_t const* module,
             buffer_handle_t handle,
             const NvGrRect* rect,
             NvU32 color,
             int inFenceFd,
             int* outFenceFd);

// Blit between buffers
int (*blit)(gralloc_module_t const* module,
            buffer_handle_t src,
            const NvGrRect* srcRect,
            int srcFenceFd,
            buffer_handle_t dst,
            const NvGrRect* dstRect,
            int dstFenceFd,
            int transform,
            const NvPoint* dstOffset,
            int* outFenceFd);
```

### Buffer Decompression

```c
// Decompress lossless compressed buffer
int (*decompress_buffer)(gralloc_module_t const* module,
                         buffer_handle_t handle,
                         int fenceIn,
                         int* fenceOut);
```

### Property Override

```c
// Override gralloc property at runtime
int (*override_property)(gralloc_module_t const* module,
                         const char* name,
                         const char* value);
```

## Memory Heaps

### IOMMU Heap

Standard system memory allocated through the IOMMU for device access:
- Used for most graphics buffers
- Accessible by GPU, VIC, 2D engine, and display controller
- Supports all surface layouts

### VPR (Video Protected Region)

Secure memory region for DRM-protected content:
- Used for encrypted video playback
- Accessible by secure video decoder and display controller
- Cannot be accessed by GPU (for security)
- Requires special allocation flags

### ExternalCarveOut

Reserved memory region for specific use cases:
- Used for camera buffers, video encoder
- May have different caching attributes
- Limited size, managed carefully

## Surface Layout Details

### Pitch (Linear) Layout

```
Row 0: [pixel 0][pixel 1][pixel 2]...[pixel N]
Row 1: [pixel 0][pixel 1][pixel 2]...[pixel N]
...
```

- Simple row-major layout
- `Pitch` = bytes per row (may include padding)
- Compatible with all hardware units

### Tiled Layout

```
16x16 pixel tiles stored contiguously
```

- 16x16 pixel tiles for improved cache locality
- Used for render targets
- Required for certain GPU operations

### Blocklinear Layout

```
Compressed blocks with configurable block height
```

- Used for lossless compression
- `BlockHeightLog2` determines block size
- Saves memory bandwidth
- Requires decompression before display (if not supported by DC)

## Libraries

### Shared Libraries

| Library | Purpose |
|---------|---------|
| `liblog` | Android logging |
| `libcutils` | Android utilities |
| `libdl` | Dynamic linker |
| `libsync` | Fence synchronization |
| `libEGL` | EGL interface |
| `libnvgr` | NVIDIA gralloc core |
| `libnvos` | OS abstraction layer |
| `libnvrm` | Resource manager |
| `libnvrm_graphics` | Graphics resource manager |
| `libnvblit` | Blit operations |

### Static Libraries

| Library | Purpose |
|---------|---------|
| `libnvfxmath` | Fixed-point math utilities |

## Buffer Validation

### Magic Values

```c
#define NVGR_HANDLE_MAGIC   0xDAFFCAFF  // NvNativeHandle magic
#define NVGR_BUFFER_MAGIC   0xB00BD00D  // NvGrBuffer magic
```

### Validation Checks

1. **Handle validation**: Check `Magic` field
2. **Ownership validation**: Check `Owner` against current PID
3. **Self-pointer validation**: Check `hSelf` points to handle itself
4. **Buffer validation**: Check `Buf->Magic` matches expected value

## Best Practices

1. **Always use fence APIs**: Never manipulate fence fds directly; use nvsync functions
2. **Check buffer magic**: Validate handles before dereferencing
3. **Handle VPR correctly**: Protected content requires VPR heap and special handling
4. **Track surface layout**: Different hardware units have different layout requirements
5. **Manage compression**: Decompress buffers before display if DC doesn't support compressed formats
