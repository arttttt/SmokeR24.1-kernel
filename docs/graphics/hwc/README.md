# Hardware Composer — Documentation Index

This section covers everything related to the Hardware Composer HAL on Tegra K1 (T124): analysis of the existing HWC1 blob, the HWC2 implementation plan, upstream reference projects, and the kernel-level backend the HWC uses.

Gralloc is covered separately in [`../gralloc-buffer-management.md`](../gralloc-buffer-management.md) — it is a buffer-management concern, not a composer concern, even though HWC consumes gralloc buffers.

## Contents

### Existing Stack Analysis

- **[HWC1 Architecture](hwc1-architecture.md)** — Full anatomy of the proprietary NVIDIA HWC1 from the JXD source leak: file map, composition flow (prepare/set), data structures (`nvhwc_context`, `nvhwc_display`, `nvfb_window`), chip-version capability matrix, linked libraries.
- **[HWC1 vs HWC2](hwc1-vs-hwc2.md)** — Detailed API and fence-semantics comparison. Explains why the `hwc2on1` adapter leaks fds against the NVIDIA HWC1 blob and why a native HWC2 is required.

### Implementation Plan

- **[HWC2 Implementation Plan](hwc2-implementation-plan.md)** — Original from-scratch phased plan: file structure, R24.1 kernel API adaptations (tegra_timespec, flip_3 flags, event bitmasks), incremental bring-up order.
- **[drm-hwcomposer Soft Fork + Abstraction Plan](drm-hwcomposer-fork-plan.md)** — **Chosen strategy.** Fork upstream, introduce `HwcDisplayPipeline` abstraction, keep DRM backend as reference, add Tegra backend. Reuses ~3000 lines of proven core, writes ~1100 lines new. First frame in ~2 weeks.
- **[Shared-Core Architecture for HWC2 + HWC3](shared-core-hwc2-hwc3.md)** — How to structure the backend so a future HWC3 (AIDL) shim is a few hundred lines, not a rewrite. Reference: Google's `libhwc2.1`.

### Future: HWC3

- **[HWC2 vs HWC3](hwc2-vs-hwc3.md)** — AIDL composer3 introduced in Android 13: typed `DisplayCommand[]` replacing HIDL command buffer, `CommandResultPayload` unified result, VRR / HDR / brightness / display decorations — and what stays identical (fence discipline, composition model).

### External Reference

- **[drm-hwcomposer as Reference](drm-hwcomposer-reference.md)** — Why the upstream [drm-hwcomposer](https://gitlab.freedesktop.org/drm-hwcomposer/drm-hwcomposer) cannot be ported as-is (no DRM/KMS in R24.1 kernel, no PRIME path), and which of its ~15 000 lines are reusable as architectural patterns. See [fork plan](drm-hwcomposer-fork-plan.md) for how these findings are operationalized.

### Backend (Kernel & Engines)

- **[Display Controller Kernel Interface](display-controller-interface.md)** — `tegra_dc_ext_*` ioctl reference for `/dev/tegra_dc_ctrl` and `/dev/tegra_dc_N`. Flip3/Flip4 structures, event bitmasks (R24.1 format), pixel formats, blend modes, output types.
- **[Composition Engines](composition-engines.md)** — GLComposer / GLDrawTexture / Gralloc-VIC-2D compositor priority, selection logic, scratch buffer system, capability flags (`NVCOMPOSER_CAP_*`), and the impact of running without 2D/VIC hardware engines (DRM video, decompression, DIDIM, idle power).

## Reading Order for New Contributors

1. [HWC1 Architecture](hwc1-architecture.md) — understand the status quo
2. [Display Controller Kernel Interface](display-controller-interface.md) — understand the backend
3. [HWC1 vs HWC2](hwc1-vs-hwc2.md) — understand why migration is needed
4. [HWC2 Implementation Plan](hwc2-implementation-plan.md) — understand the target
5. [drm-hwcomposer Reference](drm-hwcomposer-reference.md) — understand what to borrow from upstream
6. [drm-hwcomposer Fork + Abstraction Plan](drm-hwcomposer-fork-plan.md) — the chosen implementation strategy
7. [Shared-Core Architecture](shared-core-hwc2-hwc3.md) — layering that keeps HWC3 affordable
8. [Composition Engines](composition-engines.md) — dispatch behavior for `TegraCompositor`
9. [HWC2 vs HWC3](hwc2-vs-hwc3.md) — future transport
