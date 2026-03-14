# Master Plan — Android 9 on Xiaomi Mi Pad 1 (Tegra K1)

This document is the overall project roadmap for bringing Android 9 (Pie) to the Xiaomi Mi Pad 1st Generation (mocha), which is powered by the NVIDIA Tegra K1 (T124) SoC with a Kepler-based GK20A GPU.

---

## Table of Contents

1. [Context](#context)
2. [Available Resources](#available-resources)
3. [Project Phases](#project-phases)
   - [Phase 1: HWC2 (CRITICAL)](#phase-1-hwc2-critical)
   - [Phase 2: NvOs Shim (HIGH)](#phase-2-nvos-shim-high)
   - [Phase 3: Kernel Port Preparation (MEDIUM)](#phase-3-kernel-port-preparation-medium)
   - [Phase 4: Kernel Port to 3.18 (MEDIUM)](#phase-4-kernel-port-to-318-medium)
   - [Phase 5: Future Kernel Steps (LOW)](#phase-5-future-kernel-steps-low)
4. [Constraints](#constraints)
5. [Risk Matrix](#risk-matrix)
6. [Success Criteria](#success-criteria)

---

## Context

### Device Specifications
- **Device:** Xiaomi Mi Pad 1st Generation (codename: mocha)
- **SoC:** NVIDIA Tegra K1 (T124)
- **GPU:** NVIDIA GK20A (Kepler architecture)
- **Target OS:** Android 9 (Pie)
- **Current Kernel:** R24.1 BSP based on Linux 3.10.108
- **Working Userspace:** R24.1 proprietary blobs (the ONLY working proprietary set for T124)

### Core Problem

Android 9 removed support for Hardware Composer 1 (HWC1) and requires Hardware Composer 2 (HWC2). The standard Android `hwc2on1` adapter, which wraps HWC1 drivers for HWC2 compatibility, has critical fence file descriptor leaks when used with NVIDIA's HWC1 driver. These leaks cause system crashes, making the device unusable on Android 9 without a proper solution.

The only viable path forward is to implement a native HWC2 HAL that directly targets the R24.1 kernel APIs, bypassing the problematic adapter entirely.

---

## Available Resources

### Source Code (JXD S192 Leak)

A significant leak of NVIDIA source code provides invaluable reference material:

- **Full HWC1 source:** `nvhwc.c`, `nvfb.c`, and related composition logic
- **Full gralloc source:** Memory allocation and buffer management
- **Full libnvdc source:** Display controller interface
- **Full libnvgr source:** Graphics resource management
- **Partial libnvrm source:** `nvrm_channel_linux.c` for resource manager channel operations
- **All API headers:** `nvdc.h`, `nvrm_*.h`, `nvsync.h`, `nvgr*.h`
- **Kernel UAPI headers:** `tegra_dc_ext.h`, `nvmap.h`, `nvhost_ioctl.h`

### Kernel Resources

- **Smoke-kernel-mocha:** Older 3.10.24 kernel for Mi Pad 1 (reference only)
- **SmokeR24.1-kernel:** Current 3.10.108 R24.1 BSP kernel (active development target)

### Proprietary Blobs

- **proprietary_vendor_nvidia/shield/:** NVIDIA Shield tablet blobs (R24.1 era)
- **android_vendor_xiaomi_mocha/:** Xiaomi Mi Pad specific blobs

---

## Project Phases

### Phase 1: HWC2 (Priority: CRITICAL)

**Goal:** Replace the `hwc2on1` adapter with a native HWC2 HAL implementation.

**Why This is Critical:**
The fence fd leak in the hwc2on1 adapter causes system instability and crashes on Android 9. A native HWC2 implementation is the only way to achieve reliable display output.

**Approach:**

1. **Write new HWC2 HAL** targeting the R24.1 kernel API directly
2. **Header Adaptations:** Three specific changes are needed between HWC1-era headers and HWC2-era kernel headers:
   - `timestamp`: `struct timespec` → `struct tegra_timespec`
   - `flip_3`: `reserved1` field → `flags` field
   - `Event types`: Sequential enum values → Bit mask values
3. **Port Composition Logic:** Adapt window assignment, bandwidth negotiation, and layer handling from the HWC1 source
4. **Fence Lifecycle Management:** Implement strict fence management from day one to prevent resource leaks
5. **Incremental Development:**
   - Start with all-CLIENT composition mode (GPU composition only)
   - Add hardware overlay support incrementally
   - Add 2D/VIC scratch blit capabilities last

**Dependencies:**
- R24.1 kernel headers
- gralloc blob
- libnvblit and libnvddk_* blobs

**Estimated Scope:** ~3000-5000 lines of C/C++

**See Also:** `docs/graphics/hwc2-implementation-plan.md`

---

### Phase 2: NvOs Shim (Priority: HIGH)

**Goal:** Create an open-source NvOs compatibility layer.

**Why This is Needed:**
No newer BSP is available from NVIDIA for the T124. The R24.1 blobs may have bionic ABI compatibility issues with Android Pie's newer libc. An open-source shim provides control and debuggability.

**Approach:**

1. **Write thin shim library** exporting the same symbols as `libnvos.so`
2. **Function Mapping:** Approximately 100 NvOs* functions need to be mapped to POSIX or bionic equivalents
3. **Incremental Testing:**
   - Test with libnvrm first (lowest dependency)
   - Then test with gralloc
   - Finally test with OpenGL/GLES libraries

**Dependencies:** None (pure userspace, no kernel dependencies)

**See Also:** `docs/blobs/nvos-shim-design.md`

---

### Phase 3: Kernel Port Preparation (Priority: MEDIUM)

**Goal:** Prepare for upgrading the kernel from 3.10 to 3.18.

**Why This is Important:**
Linux 3.10 is End-of-Life (EOL) and receives no security updates. The plan is to move to newer kernels in small, manageable steps: 3.18 → 4.4 → 4.9.

**Approach:**

1. **Download R28 kernel source** (4.4-based) from NVIDIA's nv-tegra git repository
2. **Generate diffs** of NVIDIA drivers between R24 (3.10) and R28 (4.4)
3. **Analyze API changes** that NVIDIA made during their own kernel porting efforts
4. **Design compat layer structure** for handling kernel API differences
5. **Identify android-3.18** as the base for the first port

**Dependencies:** Phase 1 and 2 should be stable before beginning kernel porting

**See Also:** `docs/kernel-porting/port-strategy-3.10-to-3.18.md`

---

### Phase 4: Kernel Port to 3.18 (Priority: MEDIUM)

**Goal:** Port ALL NVIDIA kernel components from R24.1 (3.10.108) to android-3.18.

**Why 3.18:**
It is the closest Long-Term Support (LTS) kernel to 3.10, minimizing the API delta while providing a modernized base.

**Approach:**

Seven sub-phases in strict dependency order:

#### Phase 4.0: Base Preparation
- Set up android-3.18 tree
- Verify sync framework compatibility

#### Phase 4.1: Foundation
- mach-tegra board support
- Clock framework (clk)
- Pin control (pinctrl)
- Device Tree Sources (DTS)

#### Phase 4.2: Memory Subsystem
- IOMMU/SMMU drivers
- nvmap (NVIDIA memory allocator)
- Memory controller

#### Phase 4.3: Platform Support
- DVFS (Dynamic Voltage and Frequency Scaling)
- EDP (Electrical Design Point) management
- PMC (Power Management Controller)
- Power gating
- Thermal management (soctherm)

#### Phase 4.4: Host1x and Sync Framework
- nvhost (Host1x command submission)
- Host1x DRM
- Sync framework integration

#### Phase 4.5: Display
- tegra_dc (Display Controller)
- Panel drivers
- HDMI/DSI output

#### Phase 4.6: GPU
- nvgpu/gk20a driver (Kepler GPU support)

#### Phase 4.7: Peripherals
- USB
- I2C
- SPI
- MMC/SD
- Sound (ALSA)
- Camera (ISP)

**Key Design Principles:**
- Modular structure with `compat/` shim headers for API differences
- Validate that R24.1 userspace blobs work unchanged (no wrapper needed if drivers are ported correctly)
- Approximately 1480 NVIDIA-specific files to port

**See Also:** `docs/kernel-porting/r24-component-inventory.md`

---

### Phase 5: Future Kernel Steps (Priority: LOW)

**Goal:** Continue kernel modernization beyond 3.18.

**Planned Path:** 3.18 → 4.4 → 4.9

#### 4.4 Kernel
- Use R28 kernel as direct reference
- T124 was officially dropped by NVIDIA at R28, but T210 (Jetson TX1) drivers serve as a template
- Many driver structures remain similar

#### 4.9 Kernel
- Use R32 kernel as reference
- Significant driver restructuring occurred by this release
- More challenging port but better long-term support

**Approach:**
Each step uses the same `compat/` layer approach — update shim headers for the new kernel API while preserving blob compatibility.

---

## Constraints

1. **Single Working Proprietary Stack:** Only R24.1 blobs are known to work. There is no alternative source for the GPU driver.

2. **No NVIDIA Open GPU Module Support:** NVIDIA's open-source GPU kernel modules (starting at R515+) do NOT support T124/Kepler. This path is not viable.

3. **No Mesa/Nouveau:** Mesa open-source drivers and the Nouveau kernel driver are not being considered for this project due to complexity and feature gaps.

4. **T124 Officially Dropped:** NVIDIA dropped T124 support from L4T after R24. There is no official NVIDIA support for newer kernels.

5. **Blob Compatibility Requirement:** All kernel porting must preserve R24.1 userspace blob compatibility. The userspace is fixed; only the kernel can change.

---

## Risk Matrix

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| HWC2 fence management bugs | Medium | High (system crash) | Start with all-CLIENT mode, add overlays incrementally, strict code review of fence lifecycle |
| NvOs shim ABI mismatch | Medium | High (blob crashes) | Test each blob individually, use strace/ltrace to identify symbol issues |
| Kernel 3.18 sync framework incompatible | Low | Critical | android-3.18 preserves staging sync framework, verify before extensive porting |
| GPU driver fails on 3.18 | Medium | Critical | Port GPU components last, can fall back to 3.10 kernel if needed |
| 2D/VIC blobs fail with ported nvhost | Low | Medium | nvhost core API is stable, test with simple blit operations first |
| Undocumented NvOs functions in blobs | Medium | Medium | Use `nm --dynamic` on each blob to discover all imported symbols |
| Kernel port timeline overrun | Medium | Medium | Modular sub-phases allow incremental validation, can stop at any stable point |
| Android 9 compatibility issues | Low | High | Use LineageOS 16.0 as base, leverage community patches |

---

## Success Criteria

### Phase 1 Success
- Android 9 boots to UI with working display
- No fence-related crashes during normal operation
- Video playback functional (hardware overlays working)
- GPU composition stable

### Phase 2 Success
- All R24.1 blobs load and function correctly with open-source NvOs shim
- No performance degradation compared to proprietary NvOs
- All system services start successfully

### Phase 3 Success
- Complete compat layer design documented
- R28 kernel diff fully analyzed
- Clear porting strategy defined for each driver component

### Phase 4 Success
- System boots on 3.18 kernel with R24.1 userspace
- All hardware functional: display, GPU, audio, USB, WiFi, Bluetooth
- Performance comparable to 3.10 kernel
- No regressions in stability

### Phase 5 Success
- System boots on 4.4 kernel
- All hardware functional
- Path to 4.9 validated

---

## Document Information

- **Project:** SmokeR24.1-kernel
- **Device:** Xiaomi Mi Pad 1 (mocha)
- **Target:** Android 9 (Pie) on Tegra K1
- **Last Updated:** 2026-03-15

---

*This is a living document. As the project progresses, phases may be refined, risks reassessed, and success criteria updated.*
