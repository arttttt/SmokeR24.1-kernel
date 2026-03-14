# Reverse Engineering Feasibility Assessment

This document provides a per-blob assessment of reverse engineering difficulty, open-source alternatives, and recommendations for the Tegra K1 graphics stack.

## Summary Table

| Blob | Size | RE Difficulty | Open Source Alternative | Recommendation |
|------|------|--------------|------------------------|----------------|
| `libnvdc.so` | ~5KB | **FULL SOURCE EXISTS** | Use JXD source | Use source, adapt headers for R24.1 |
| `libnvos.so` | 46KB | Easy | Write shim | Write open-source shim (see `nvos-shim-design.md`) |
| `libnvgr.so` | 22KB | **FULL SOURCE EXISTS** | Use JXD source | Use source |
| `gralloc.tegra.so` | 34-55KB | **FULL SOURCE EXISTS** | Use JXD source | Use source, adapt for R24.1 |
| `hwcomposer.tegra.so` | 278-373KB | **FULL SOURCE EXISTS** | Write HWC2 | Write new HWC2 (see `hwc2-implementation-plan.md`) |
| `libnvrm.so` | 71KB | Medium | Write by headers+kernel | 2-3 weeks; full API headers + open kernel drivers |
| `libnvrm_graphics.so` | 46KB | Medium | Write by headers | 1-2 weeks; layout computation + chip capabilities |
| `libnvblit.so` | 47KB | Medium-Hard | GPU fallback (GLES) | Use blob OR replace with GLES (loses VPR access) |
| `libnvddk_2d_v2.so` | 50KB | Hard | GPU fallback | Use blob; reversing host1x cmd stream is hard |
| `libnvddk_vic.so` | 47KB | Hard | GPU fallback | Use blob; VIC cmd format partially in envytools |
| `libnvwsi.so` | 38KB | Medium | Not needed with blob GL | Not needed if using proprietary GL stack |
| `libglcore.so` | 11.5MB | **IMPOSSIBLE** | Nouveau+Mesa (not considered) | Use blob |
| `libcgdrv.so` | 3.2MB | **IMPOSSIBLE** | Mesa NIR (not considered) | Use blob |
| `libEGL_tegra.so` | ~100KB | **IMPOSSIBLE** | Mesa EGL (not considered) | Use blob |
| `libGLESv2_tegra.so` | ~100KB | **IMPOSSIBLE** | Mesa GLES (not considered) | Use blob |

---

## Detailed Assessment

### Full Source Available (Immediate Use)

#### libnvdc.so (~5KB)
- **Status**: Full source code available in JXD leak
- **Complexity**: Low - simple display controller interface
- **Action**: Adapt source headers for R24.1 compatibility
- **Time estimate**: 1-2 days

#### libnvgr.so (22KB)
- **Status**: Full source code available in JXD leak
- **Complexity**: Low - gralloc wrapper utilities
- **Action**: Compile from source with minor adaptations
- **Time estimate**: 1-2 days

#### gralloc.tegra.so (34-55KB)
- **Status**: Full source code available in JXD leak
- **Complexity**: Medium - buffer allocation logic
- **Action**: Adapt for R24.1 kernel interfaces
- **Time estimate**: 3-5 days

#### hwcomposer.tegra.so (278-373KB)
- **Status**: Full source code available in JXD leak (HWC 1.x)
- **Complexity**: High - full display stack
- **Action**: Write new HWC2 implementation using source as reference
- **Time estimate**: 2-3 weeks

---

### Write from Headers/Documentation

#### libnvrm.so (71KB)
- **Reverse engineering difficulty**: Medium
- **Available resources**:
  - Complete C headers with function signatures
  - Open-source kernel drivers (nvmap, nvhost)
  - Sample code in JXD leak
- **Key challenges**:
  - Understanding syncpoint wait semantics
  - Channel submission format details
  - Error handling paths
- **Open source alternative**: Can be written from headers + kernel code
- **Time estimate**: 2-3 weeks
- **Recommendation**: Worth attempting - core infrastructure

#### libnvrm_graphics.so (46KB)
- **Reverse engineering difficulty**: Medium
- **Available resources**:
  - C headers for surface layout functions
  - Chip capability queries documented
- **Key challenges**:
  - Surface layout computation algorithms
  - Compression tag management
- **Open source alternative**: Can be written from headers
- **Time estimate**: 1-2 weeks
- **Recommendation**: Worth attempting - needed for graphics

---

### Opaque Binaries (Use or Fallback)

#### libnvos.so (46KB)
- **Reverse engineering difficulty**: Easy
- **Approach**: Write open-source shim
- **See**: `nvos-shim-design.md` for detailed design
- **Time estimate**: 1 week
- **Recommendation**: Write shim for Android version compatibility

#### libnvblit.so (47KB)
- **Reverse engineering difficulty**: Medium-Hard
- **Function**: Dispatches to 2D or VIC engines
- **Open source alternative**: Use GPU (GLES) for blits
- **Trade-off**: Loses VPR (Video Protected Region) access
- **Recommendation**: Use blob OR replace with GLES fallback

#### libnvddk_2d_v2.so (50KB)
- **Reverse engineering difficulty**: Hard
- **Function**: Generates host1x command streams for 2D engine
- **Open source alternative**: GPU fallback
- **Key challenge**: host1x command format not fully documented
- **Recommendation**: Use blob; reversing command stream is error-prone

#### libnvddk_vic.so (47KB)
- **Reverse engineering difficulty**: Hard
- **Function**: Generates VIC engine command buffers
- **Open source alternative**: GPU fallback
- **Resources**: Some VIC documentation in envytools project
- **Recommendation**: Use blob; partial documentation exists but incomplete

---

### GPU Stack (Use Blob Only)

#### libglcore.so (11.5MB)
- **Reverse engineering difficulty**: IMPOSSIBLE
- **Content**: Complete OpenGL implementation
- **Lines of code equivalent**: ~3-5 million
- **Open source alternative**: Nouveau + Mesa (not considered for this project)
- **Recommendation**: Use blob exclusively

#### libcgdrv.so (3.2MB)
- **Reverse engineering difficulty**: IMPOSSIBLE
- **Content**: Cg shader compiler
- **Complexity**: Compiler backend with GPU-specific optimizations
- **Open source alternative**: Mesa NIR (not considered)
- **Recommendation**: Use blob exclusively

#### EGL/GLES Libraries (~200KB total)
- **Reverse engineering difficulty**: IMPOSSIBLE
- **Content**: Khronos API implementations tied to libglcore
- **Open source alternative**: Mesa EGL/GLES (not considered)
- **Recommendation**: Use blob exclusively

---

## Key Insights

### GPU Stack Reality
The GPU driver stack (`libglcore.so` + `libcgdrv.so` + EGL/GLES) totals approximately **15MB of proprietary code**. This represents:
- Decades of NVIDIA driver development
- Highly optimized GPU command generation
- Proprietary shader compiler technology
- Hardware-specific workarounds and optimizations

**Reverse engineering this stack would require years of effort.** The Mesa/Nouveau alternative exists but is explicitly out of scope for this project.

### 2D/VIC Acceleration
The 2D and VIC DDK libraries (`libnvddk_2d_v2.so`, `libnvddk_vic.so`) provide hardware-accelerated:
- YUV-to-RGB conversion
- Scaling and rotation
- Deinterlacing
- Compositing operations

These are **opaque binaries** with partially documented command formats. The envytools project has made progress on VIC, but full documentation is not available.

### Strategic Approach

**Immediate wins** (use source):
1. `libnvdc.so` - Display controller
2. `libnvgr.so` - Gralloc utilities
3. `gralloc.tegra.so` - Buffer allocator
4. `hwcomposer.tegra.so` - Reference for HWC2

**Worth implementing**:
1. `libnvos.so` shim - Android compatibility
2. `libnvrm.so` - Core resource manager
3. `libnvrm_graphics.so` - Graphics extensions

**Use blobs for**:
1. GPU stack (libglcore, libcgdrv, EGL/GLES)
2. 2D/VIC DDK (unless GLES fallback acceptable)

---

## Conclusion

With full source available for `libnvdc`, `libnvgr`, `gralloc`, and `hwcomposer` — plus complete API headers for `libnvrm` — the project can achieve significant open-source coverage. The only truly opaque blobs are:

1. **GPU driver** (`libglcore`, `libcgdrv`) - must use blob
2. **2D/VIC engines** (`libnvddk_*`) - use blob or accept GLES fallback

This represents a **practical balance** between open-source implementation and proprietary blob usage.
