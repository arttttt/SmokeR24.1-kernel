# NVIDIA Tegra K1 Proprietary Blob Inventory

This document catalogs all proprietary binary blobs required for graphics support on the NVIDIA Tegra K1 (T124) platform, comparing sources from two vendor trees.

## Vendor Sources

1. **NVIDIA Shield blobs** (`/proprietary_vendor_nvidia/shield/`)
   - Newer revision
   - Larger HWC binary
   - Additional HAL modules (HDMI CEC, memtrack, Vulkan)

2. **Xiaomi Mi Pad blobs** (`/android_vendor_xiaomi_mocha/`)
   - Older but stable revision
   - Smaller footprint
   - Simpler dependency chain

## HAL Modules

| File | Mocha Size | Shield Size | Description |
|------|-----------|-------------|-------------|
| `hwcomposer.tegra.so` | 278,788 | 373,648 | Hardware Composer HAL - composes layers for display |
| `gralloc.tegra.so` | 34,288 | 55,256 | Graphics memory allocator - allocates GPU-accessible buffers |
| `hdmi_cec.tegra.so` | — | 18,168 | HDMI Consumer Electronics Control (Shield only) |
| `memtrack.tegra.so` | — | 18,152 | Memory tracking for Android (Shield only) |
| `vulkan.tegra.so` | — | 18,272 | Vulkan graphics API support (Shield only) |

## Core Graphics Libraries

| File | Size | Description | Source Available? |
|------|------|-------------|-------------------|
| `libnvrm.so` | 71,464 | Resource Manager - core kernel interface | Headers only |
| `libnvrm_graphics.so` | 46,676 | RM graphics extension - surface layouts | Headers only |
| `libnvos.so` | 46,748 | OS abstraction layer - POSIX wrappers | No (shim required) |
| `libnvdc.so` | ~5,000 | Display controller interface | **FULL SOURCE** |
| `libnvgr.so` | 22,104 | Gralloc wrapper utilities | **FULL SOURCE** |
| `libnvblit.so` | 47,264 | 2D blitter abstraction layer | No |
| `libnvddk_2d_v2.so` | 50,788 | 2D DDK driver (host1x gr2d) | No |
| `libnvddk_vic.so` | 47,196 | VIC DDK driver (video/image compositor) | No |
| `libnvwsi.so` | 38,496 | Window system integration | No |
| `libnvglsi.so` | 210,916 | OpenGL system integration | No |
| `libglcore.so` | 11,507,833 | OpenGL core implementation | No |
| `libcgdrv.so` | 3,232,068 | Cg shader compiler driver | No |
| `libnvhwc_service.so` | 38,548 | HWC background service | No |
| `libnvcms.so` | 22,108 | Content management system | No |
| `libnvrm_gpu.so` | 84,208 | RM GPU management extension | No |
| `libnvrmapi_tegra.so` | 42,788 | RM Tegra-specific API | No |

## EGL/OpenGL Libraries

| File | Description |
|------|-------------|
| `libEGL_tegra.so` | EGL main entry point |
| `libGLESv1_CM_tegra.so` | OpenGL ES 1.1 implementation |
| `libGLESv2_tegra.so` | OpenGL ES 2.0 implementation |
| `libEGL_tegra_impl.so` | EGL implementation details |
| `libGLESv1_CM_tegra_impl.so` | GLES 1.1 implementation details |
| `libGLESv2_tegra_impl.so` | GLES 2.0 implementation details |

## Binary Characteristics

All libraries share the following properties:
- **Architecture**: ARM 32-bit ELF (armeabi-v7a)
- **Strip status**: Stripped (no debug symbols)
- **Linker**: `/system/bin/linker` (32-bit)
- **No 64-bit support**: No `lib64/` directories present

## Important Note on libnvdc.so

The `libnvdc.so` library is **not found as a separate blob** in either vendor tree. Display controller functions are integrated directly into `hwcomposer.tegra.so`. However, **full source code exists** in the JXD leak and can be used to understand or replace this functionality.

## Dependency Graph

```
                    Android Framework
                           |
        +------------------+------------------+
        |                  |                  |
   SurfaceFlinger       Camera HAL        Media Codec
        |                  |                  |
        v                  v                  v
   hwcomposer.tegra.so  libnvcms.so     libnvomx.so (not listed)
        |                                     |
        v                                     v
   gralloc.tegra.so                    libnvmm*.so (not listed)
        |                                     |
        v                                     v
   libnvgr.so                          libnvos.so
        |                  +------------------+
        v                  v                  |
   libnvrm.so        libnvblit.so            |
        |                  |                  |
        v                  v                  v
   libnvrm_graphics.so  libnvddk_2d_v2.so   libnvddk_vic.so
        |                                     |
        v                                     v
   libnvdc.so (source)                  libnvwsi.so
        |                                     |
        v                                     v
   /dev/nvhost-*                       libnvglsi.so
        |                                     |
        v                                     v
   Kernel Drivers                     libglcore.so
        |                              /  |  \
        v                             /   |   \
   Tegra DRM/KMS                   EGL  GLES  libcgdrv.so
   (open source)                   implementations
```

## Key Observations

1. **Open Source Opportunities**: `libnvdc.so`, `libnvgr.so`, and `gralloc.tegra.so` have full source available from the JXD leak.

2. **Shim Required**: `libnvos.so` needs an open-source shim for Android version compatibility (see `nvos-shim-design.md`).

3. **GPU Stack**: The OpenGL stack (`libglcore.so` + `libcgdrv.so` + EGL/GLES) totals ~15MB of proprietary code and is impractical to reverse engineer.

4. **2D/VIC Acceleration**: The DDK libraries (`libnvddk_2d_v2.so`, `libnvddk_vic.so`) provide hardware-accelerated 2D operations but are opaque binaries.
