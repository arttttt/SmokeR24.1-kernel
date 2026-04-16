# drm-hwcomposer as Reference for Tegra K1 HWC

## Summary

The upstream **[drm-hwcomposer](https://gitlab.freedesktop.org/drm-hwcomposer/drm-hwcomposer)** project is a HWC2/HWC3 implementation over Linux DRM/KMS, developed primarily by Collabora and ChromeOS. It cannot be ported as-is to Tegra K1 because the R24.1 kernel has no DRM/KMS driver for the display controller — only the proprietary `tegra_dc_ext_*` ioctl path.

However, drm-hwcomposer is **architecturally valuable as a reference**. Its HWC2 entry layer, its composition planner, its fence discipline, and its backend abstraction patterns all apply directly to our own HWC even though the DRM backend itself does not. This document catalogs what to reuse and what to discard.

## Why Direct Port Is Blocked

Three hard blockers exist on the R24.1 kernel + proprietary NVIDIA userland stack:

### 1. No DRM/KMS in the kernel

`SmokeR24.1-kernel/drivers/gpu/drm/` contains `i915`, `nouveau`, `omapdrm`, `radeon`, etc. — but no `tegra/`. Mainline Linux gained `drivers/gpu/drm/tegra/` well after the R24 branch forked, and it was never backported. The only userspace interface to the Tegra K1 display controller is `/dev/tegra_dc_0` and `/dev/tegra_dc_ctrl` with `tegra_dc_ext_*` ioctls (see [display-controller-interface.md](display-controller-interface.md)).

drm-hwcomposer's `drm/DrmDevice.cpp::Init()` hard-requires:
- `drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)` — returns error on failure
- `drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)` — returns error on failure
- `drmSetMaster(fd)` + `drmIsMaster()`
- `DRM_CAP_ADDFB2_MODIFIERS` — graceful fallback

The entire `DrmAtomicStateManager::CommitFrame()` is built on `drmModeAtomicCommit()` with property-API. Legacy `drmModeSetCrtc` is not used anywhere in the critical path. Without `/dev/dri/cardN` backed by an atomic-capable driver, the project does not initialize.

### 2. Buffer Handle Mismatch

drm-hwcomposer's path is:

```
gralloc handle → BufferInfoGetter → PRIME fd → drmPrimeFDToHandle() → drmModeAddFB2() → plane FB_ID
```

Our `nvgr` gralloc operates on `NvRmMemHandle` / `NvRmSurface` (103 references in nvgr sources), not DMA-BUF handles. The kernel `drivers/video/tegra/nvmap/nvmap_dmabuf.c` can export nvmap buffers as dma-buf, but that alone is insufficient — drm-hwcomposer needs `drmModeAddFB2`, which does not exist without blocker #1.

### 3. No NVIDIA BufferInfo Backend

`bufferinfo/legacy/` contains six backends: `Libdrm`, `Minigbm`, `MaliHisi`, `MaliMeson`, `MaliMediatek`, `Imagination`. No Tegra/NVIDIA. Writing one is possible but insufficient on its own (blocker #2).

## What's Reusable

Roughly 30–40% of the ~15 000-line codebase transfers cleanly. What survives and what to copy:

### Fully Reusable: `hwc2_device/` (~2 000 lines)

The HWC2 API-facing layer is backend-agnostic:

| File | Purpose | Reusability |
|---|---|---|
| `DrmHwcTwo.cpp/.h` | `hwc2_device_t` implementation, `getFunction()` table, device lifecycle | Copy structure; swap DRM backend calls for core calls |
| `HwcDisplay.cpp/.h` | validate/present/accept flow, changed-types tracking, layer Z-order management | Copy nearly verbatim |
| `HwcLayer.cpp/.h` | Per-layer state: buffer, blend, transform, plane_alpha, source_crop, display_frame, visible_region, damage_region | Copy nearly verbatim |

This layer contains the subtle correctness bits: `BAD_LAYER`/`BAD_DISPLAY` error paths, accept-before-present guards, changed-type bookkeeping, double-validate protection. Reimplementing these from scratch wastes 1–2 weeks.

### Architectural Reference: Backend Pattern

**`backend/Backend.cpp`, `backend/BackendClient.cpp`, `backend/BackendManager.cpp`** — composition planning. The validate flow is:

```
validateDisplay()
    ├── Backend::ValidateDisplay(display)
    │   ├── propose initial composition (all DEVICE)
    │   ├── ATOMIC_TEST_ONLY commit
    │   ├── if OK → done
    │   ├── if fail → demote highest-cost layer to CLIENT
    │   └── repeat until TEST_ONLY succeeds
    └── record changed composition types
```

The TEST_ONLY probe is an atomic-KMS feature. On Tegra we don't have it, but the **pattern** ports: a "dry-run" of `nvhwc_assign_windows()` that reports whether the proposed window assignment will fit within DC constraints (window count, scaling limits, rotation capability per ver4 chip — see `hwc1-architecture.md` § "Display Capabilities by Chip"). If it doesn't fit, demote layers and retry.

`BackendClient` is the degenerate case — everything goes to CLIENT composition. Useful as a starting point for phase 1 of the implementation.

### Architectural Reference: Plan Object

**`compositor/DrmKmsPlan.cpp/.h`** — an immutable `Plan` describes one frame's composition. Construction is separate from execution. Useful pattern: build a `TegraPlan { window_assignments[4], scratch_ops[], client_target_slot }` in validate, execute it in present. Keeps the `nvfb_post()` call clean of planning logic.

### Architectural Reference: Fence Discipline

**`drm/DrmAtomicStateManager.cpp` + `utils/UniqueFd.h`** — not the `IN_FENCE_FD`/`OUT_FENCE_PTR` usage (we don't have those), but the **discipline**:

- Every `acquire_fence` consumed exactly once (wait + close)
- Every per-layer `release_fence` is a unique `dup` of the present fence
- Exactly one `present_fence` per `presentDisplay()`
- `UniqueFd` RAII wrapper — never leak fds

The `PresentTrackerThread` that waits on the previous present fence asynchronously is a nice pattern too; we already have `nvsync_wait` to build an equivalent.

### Architectural Reference: VSync Worker

**`drm/VSyncWorker.cpp/.h`** — a dedicated thread, blocking on the vsync signal source, dispatching callbacks. Source-agnostic in shape: on DRM it reads via `drmHandleEvent`, on us it reads via `TEGRA_DC_EXT_EVENT_VBLANK` from `/dev/tegra_dc_ctrl`. The threading / callback-dispatch structure ports directly.

### Architectural Reference: BufferInfoGetter Abstraction

**`bufferinfo/BufferInfoGetter.cpp/.h`** — pluggable gralloc backend pattern. If our HWC ever needs to support more than one gralloc (say, proprietary `nvgr` + a future open-source one), this abstraction is the right shape. For a single `nvgr`, it's overkill; a direct `NvGrModule*` call from `HwcLayer::SetBuffer()` is fine.

## What to Discard

| Component | Why discard |
|---|---|
| `drm/DrmDevice.cpp` | Direct libdrm open + caps — nonexistent in our world |
| `drm/DrmAtomicStateManager.cpp` | Atomic commit path — replace with `tegra_dc_ext` flip ioctl |
| `drm/DrmFbImporter.cpp` | PRIME fd → GEM handle → FB ID — replace with `NvGrBuffer → nvfb_buffer` |
| `drm/DrmPlane/Crtc/Connector/Mode/Property/Encoder.cpp` | All KMS resource enumeration |
| `bufferinfo/legacy/BufferInfo*.cpp` | All six are DRM-specific grallocs |
| `utils/properties.h` DRM-specific parts | Property name strings are KMS properties |

## One-to-One Mapping

```
drm-hwcomposer          →  Tegra HWC core (proposed)
────────────────────────────────────────────────────────
DrmDevice               →  TegraDevice          (opens /dev/tegra_dc_ctrl)
DrmCrtc                 →  TegraDisplay         (primary/external)
DrmPlane                →  TegraWindow          (4 per display on K1)
DrmConnector            →  TegraOutput          (DSI/HDMI)
DrmMode                 →  TegraMode            (pixclk + porches)
DrmAtomicStateManager   →  TegraFlipCommitter   (FLIP3/FLIP4 ioctl)
DrmFbImporter           →  NvGrFbImporter       (buffer_handle_t → nvfb_buffer)
DrmPropertyBlobManager  →  (not needed — no blob properties)
VSyncWorker             →  TegraVSyncWorker     (reads DC event mask)
backend/Backend         →  PlanningBackend      (layer-to-window assignment)
compositor/DrmKmsPlan   →  TegraPlan            (immutable plan object)
hwc2_device/DrmHwcTwo   →  Hwc2Device           (copy nearly verbatim)
hwc2_device/HwcDisplay  →  HwDisplay            (copy nearly verbatim)
hwc2_device/HwcLayer    →  HwLayer              (copy nearly verbatim)
utils/UniqueFd          →  UniqueFd             (copy verbatim)
```

## drmfb-composer as an Alternative Template

**[me176c-dev/drmfb-composer](https://github.com/me176c-dev/drmfb-composer)** is a minimalist composer (~1 000 lines) that gives up overlay composition entirely: every layer goes to CLIENT, and a single FB_TARGET plane is programmed to scan out SurfaceFlinger's GLES output. It's simpler to read, but it:

1. Still requires `/dev/dri/cardN` (legacy KMS path — blocker #1 remains).
2. Discards 75% of Tegra K1's display hardware (the 4 DC windows, VIC, cursor).

Not useful as a runtime base. Mildly useful as a compact reference for "HWC2 absolute minimum".

## Estimated Savings

Reusing drm-hwcomposer as pattern source (not runtime code) saves roughly **30–40%** of the implementation effort — primarily:

- HWC2 API correctness (2 weeks saved)
- Backend/Plan architecture (1 week saved)
- VSyncWorker + fence discipline scaffolding (1 week saved)

The remainder — `TegraDisplayBackend`, `CompositionEngine` dispatch, `NvGrFbImporter`, `CompositionPlanner` for 4-window K1 constraints — is still the bulk of the work, and that's ours alone.

## Related Documents

- [HWC2 Implementation Plan](hwc2-implementation-plan.md) — concrete phased plan
- [Shared-Core Architecture](shared-core-hwc2-hwc3.md) — how the core is layered
- [Display Controller Kernel Interface](display-controller-interface.md) — `tegra_dc_ext_*` reference
- [Composition Engines](composition-engines.md) — 2D/VIC/GL dispatch
