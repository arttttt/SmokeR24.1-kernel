# Kernel Port Strategy: 3.10 → 3.18

## Rationale for 3.18

Selecting kernel 3.18 as the target port version offers several advantages:

1. **LTS Availability** — 3.18 is the closest LTS kernel to 3.10 (3.14 and 3.16 were not LTS releases)
2. **Android Support** — Android common kernel 3.18 exists and is well-tested
3. **Sync Framework** — Still located in `drivers/staging/android/`, maintaining compatibility with R24.1 userspace
4. **API Stability** — No radical API breaks in DRM, DMA, or IOMMU that appear in 4.x kernels
5. **Incremental Path** — Provides a stepping stone: 3.10 → 3.18 → 4.4 → 4.9
6. **NVIDIA Gap** — NVIDIA has no official L4T release with 3.18; they jumped directly from 3.10 (R24) to 4.4 (R28)

---

## Base Kernel Selection

**Recommended:** `android-3.18` kernel tree (NOT mainline 3.18)

The Android common kernel includes:
- Staging drivers (sync framework)
- Android-specific patches
- Better device support for mobile SoCs

---

## Reference Material

The R28 (4.4) kernel source serves as a valuable reference, showing how NVIDIA adapted their drivers for kernel API changes. The diff between R24 (3.10) and R28 (4.4) NVIDIA drivers acts as a "cheat sheet" for required adaptations.

---

## Porting Phases

Porting must follow dependency order. Each phase builds upon previous phases.

### Phase 0: Base Preparation

**Prerequisites:**
- Clone android-3.18 kernel tree
- Verify sync framework API compatibility
- Verify DMA/IOMMU API compatibility
- Create `compat/` header directory for API shims

**Deliverable:** Bootable base kernel with minimal tegra support

---

### Phase 1: Foundation (Boot Requirement)

These components are required for basic boot.

#### 1.1: arch/arm/mach-tegra/
- Adapt to 3.18 ARM platform APIs
- Update board support files
- Modify early initialization

#### 1.2: drivers/clk/tegra/
- Clock framework changes
- `CLK_IS_ROOT` deprecated
- `clk_register()` signature changes

#### 1.3: drivers/pinctrl/tegra/
- Pinctrl API is stable
- Minimal changes expected

#### 1.4: Device Tree Files
- Add T124 DTS/DTSI files
- Adapt to 3.18 DT bindings
- Update pinmux and clock references

---

### Phase 2: Memory (All Drivers Depend on This)

#### 2.1: drivers/iommu/tegra-smmu.c
- `iommu_ops` expanded in 3.18
- Additional callback functions required
- Map/unmap signature changes

#### 2.2: drivers/video/tegra/nvmap/
- DMA mapping API changes
- `dma_attrs` transition begins
- Cache management updates

#### 2.3: drivers/platform/tegra/mc/
- Memory controller driver
- Bandwidth management APIs

---

### Phase 3: Platform (Power/Clock Management)

#### 3.1: drivers/platform/tegra/ Core
- DVFS implementation
- EDP management
- PMC (Power Management Controller)

#### 3.2: drivers/platform/tegra/powergate/
- genpd API expanded
- Power domain definitions
- Domain state tracking

#### 3.3: Thermal and Frequency
- soctherm.c — Thermal zones
- tegra_cl_dvfs.c — Closed-loop DVFS

---

### Phase 4: Host1x & Sync (Display and GPU Depend on This)

#### 4.1: drivers/video/tegra/host/
- nvhost core driver
- Channel management
- Syncpoint infrastructure

#### 4.2: drivers/gpu/host1x/
- Host1x DRM integration
- Hardware abstraction layer

#### 4.3: Sync Framework Validation
- Fence API verification
- Timeline management
- Cross-kernel compatibility

---

### Phase 5: Display (Visual Output)

#### 5.1: drivers/video/tegra/dc/
- Display controller driver
- Window management
- Output drivers (HDMI, DSI, eDP)

#### 5.2: Panel Drivers
- Board-specific panel initialization
- Backlight control
- Touch integration

---

### Phase 6: GPU (OpenGL)

#### 6.1: drivers/gpu/nvgpu/gk20a/
- GPU driver port
- Power management (rail/clock gating)
- Channel submission

**Note:** GPU can be deferred if only display is required initially.

---

### Phase 7: Peripherals (Per-Device)

Port based on target device requirements:

| Subsystem | Priority | Files |
|-----------|----------|-------|
| USB | High | ehci-tegra.c, xhci-tegra.c, phy-tegra-usb.c |
| I2C | High | i2c-tegra.c |
| SPI | Medium | spi-tegra114.c |
| MMC | High | sdhci-tegra.c |
| Sound | Medium | sound/soc/tegra*/ |
| Camera | Low | drivers/media/platform/tegra/ |
| Network | Medium | bcmdhd, tegra_hv_net.c |

---

## Kernel Subsystem Changes: 3.10 → 3.18

| Subsystem | Change | Impact |
|-----------|--------|--------|
| Clock framework | `CLK_IS_ROOT` deprecated, `clk_register` expanded | mach-tegra clocks |
| IOMMU | `iommu_ops` expanded | tegra-smmu.c |
| DMA mapping | `dma_attrs` transition begins | nvmap |
| Power domains | genpd API expanded | powergate |
| IRQ | `IRQF_DISABLED` removed | All drivers (trivial fix) |
| Staging/sync | Still in staging, minor signature changes | **CRITICAL** — verify |
| Platform device | Stable | Compatible |
| DRM | Atomic modesetting added | No impact (nvgpu uses proprietary DRM) |
| OF/Device tree | API expanded | Compatible |
| Regulator | Stable | Minimal changes |
| Pinctrl | Stable | Minimal changes |

---

## Modular Structure for Portability

Recommended directory structure for the ported drivers:

```
nvidia-tegra-compat/
├── Kconfig
├── Makefile
├── nvmap/              # From drivers/video/tegra/nvmap/
├── nvhost/             # From drivers/video/tegra/host/
├── nvgpu/              # From drivers/gpu/nvgpu/
├── dc/                 # From drivers/video/tegra/dc/
├── platform/           # From drivers/platform/tegra/
├── media/              # From drivers/media/platform/tegra/
├── sound/              # From sound/soc/tegra*/
├── include/            # All NVIDIA headers
└── compat/             # API compatibility shims
    ├── sync_compat.h   # Sync framework differences
    ├── dma_compat.h    # DMA mapping API differences
    ├── iommu_compat.h  # IOMMU API differences
    └── clk_compat.h    # Clock framework differences
```

### Compatibility Layer Benefits

The `compat/` layer allows migration to newer kernels by updating only the shim headers:

```c
// compat/sync_compat.h
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 18, 0)
// 3.10 API
#define sync_fence_create(name, pt) sync_fence_create(name, pt)
#else
// 3.18+ API
#define sync_fence_create(name, pt) sync_fence_create(name, pt)
#endif
```

---

## Wrapper Policy

**Important:** Wrappers are **NOT** needed for userspace blobs if NVIDIA kernel drivers are ported directly from R24.1.

All blobs communicate with the R24.1 kernel API. If the kernel drivers remain the same (just ported to a new kernel version), the API remains compatible.

### When Wrappers Are Needed

- Modifying kernel driver behavior
- Adding new features
- Changing IOCTL interfaces

### When Wrappers Are NOT Needed

- Direct port of R24.1 drivers
- No API modifications
- Only kernel-internal API adaptations

---

## Testing Checkpoints

| Phase | Test | Success Criteria |
|-------|------|------------------|
| 1 | Early boot | Kernel reaches initramfs |
| 2 | Memory init | nvmap loads, allocations work |
| 3 | Platform | DVFS, thermal, powergate functional |
| 4 | Host1x | Syncpoints, channels operational |
| 5 | Display | Framebuffer visible |
| 6 | GPU | OpenGL test passes |
| 7 | Peripherals | Device-specific validation |

---

## Timeline Estimate

| Phase | Duration | Cumulative |
|-------|----------|------------|
| 0 | 1 week | 1 week |
| 1 | 2 weeks | 3 weeks |
| 2 | 2 weeks | 5 weeks |
| 3 | 2 weeks | 7 weeks |
| 4 | 2 weeks | 9 weeks |
| 5 | 2 weeks | 11 weeks |
| 6 | 3 weeks | 14 weeks |
| 7 | 2 weeks | 16 weeks |

**Total estimated time:** 4 months for full port with GPU

**Display-only estimate:** 2.5 months (phases 0-5)
