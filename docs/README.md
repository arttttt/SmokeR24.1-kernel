# Tegra K1 (T124) — Project Documentation

## Device: Xiaomi Mi Pad 1st Gen (mocha)
## SoC: NVIDIA Tegra K1 (T124), GPU: GK20A (Kepler)
## Target: Android 9 (Pie) on R24.1 BSP

---

## Documentation Structure

### Graphics & Display
- [HWC1 Architecture Analysis](graphics/hwc1-architecture.md) — Full analysis of the existing HWC1 implementation from JXD source leak
- [HWC2 Implementation Plan](graphics/hwc2-implementation-plan.md) — Plan for writing a native HWC2 to replace hwc2on1 adapter
- [HWC1 vs HWC2 API Differences](graphics/hwc1-vs-hwc2.md) — Detailed comparison of HAL versions and fence semantics
- [Display Controller Kernel Interface](graphics/display-controller-interface.md) — tegra_dc_ext ioctls, structs, and kernel UAPI
- [Gralloc & Buffer Management](graphics/gralloc-buffer-management.md) — NvNativeHandle, NvRmSurface, fence primitives
- [Composition Engines (2D/VIC/GL)](graphics/composition-engines.md) — What each engine provides, fallback behavior, losses without hardware engines

### Proprietary Blobs
- [Blob Inventory & Dependency Map](blobs/blob-inventory.md) — Complete list of all proprietary .so files, sizes, dependencies
- [Blob API Surface](blobs/blob-api-surface.md) — All known API functions, structures, and kernel interfaces used by blobs
- [Reverse Engineering Feasibility](blobs/reverse-engineering-feasibility.md) — Per-blob assessment of reversibility and open-source alternatives
- [nvos Shim Design](blobs/nvos-shim-design.md) — Plan for open-source NvOs* compatibility layer

### Kernel Porting
- [R24.1 Kernel Component Inventory](kernel-porting/r24-component-inventory.md) — Complete list of all NVIDIA-specific kernel components (~1480 files)
- [API Compatibility: R24.1 vs R28 (3.10 vs 4.4)](kernel-porting/api-compatibility-r24-r28.md) — UAPI breaking changes between kernel versions
- [Kernel Port Strategy: 3.10 → 3.18](kernel-porting/port-strategy-3.10-to-3.18.md) — Phased porting plan with compat layer approach
- [L4T Release History](kernel-porting/l4t-release-history.md) — Complete L4T version → kernel → SoC mapping

### Camera
- [Camera Stack Architecture](camera-reverse-engineering.md) — IMX179/OV5693, ISP, NVC stack *(existing)*
- [ISP Register Research](tegra_k1_isp_register_research_github.md) — ISP reverse engineering notes *(existing)*
- [V4L2 Camera Bringup](v4l2-camera-bringup.md) — V4L2 migration plan *(existing)*
- [ISP Profiles](camera-isp-profiles/) — Extracted ISP calibration data *(existing)*

### TrustZone & Secure OS
- [TrustZone & TLK Analysis](trustzone-tlk.md) — Full reverse-engineering of the stock TOS image: NVTOSP format, TLK memory layout, embedded TAs, boot protocol, PSCI implementation status and plan, Widevine L1 chain of trust, EKS analysis, OP-TEE porting assessment, and low-level sleep code source map

### U-Boot
- [MIPI DSI Display Issues](u-boot-mocha-dsi-issues.md) — Analysis of DSI bugs in U-Boot mocha display driver

### Plans
- [Master Plan](plans/master-plan.md) — Overall project roadmap and phase dependencies
