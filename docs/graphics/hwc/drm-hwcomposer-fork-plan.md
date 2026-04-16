# drm-hwcomposer Soft Fork + Abstraction Plan

## Strategy

Fork [drm-hwcomposer](https://gitlab.freedesktop.org/drm-hwcomposer/drm-hwcomposer), **keep the DRM backend alive**, introduce a `HwcDisplayPipeline` abstraction one level above the existing `DrmDisplayPipeline`, add a Tegra implementation as a second backend selected at build time.

This approach beats both alternatives considered:

| Approach | Why not chosen |
|---|---|
| **Absolute minimum HWC2 from scratch** (~900 lines) | Throws away proven ChromeOS-validated fence discipline and HWC2 correctness; reimplementing that is the expensive part, not the backend |
| **Hard fork, delete `drm/` entirely** | Loses upstream updates forever; no path to contribute abstraction back; breaks DRM target we might want as fallback |
| **Soft fork + abstraction** (chosen) | Reuses ~3000 lines of battle-tested core, keeps DRM backend as reference/regression-check, opens a plausible upstream PR path, build-time selection keeps both targets sane |

## What's Reused vs. Written

### Reused verbatim (~3000 lines, from upstream)

| Component | Purpose |
|---|---|
| `hwc2_device/DrmHwcTwo.{cpp,h}` | HWC2 entry, `getFunction` table, device lifecycle, callbacks |
| `hwc2_device/HwcDisplay.{cpp,h}` | validate → accept → present flow, changedTypes bookkeeping |
| `hwc2_device/HwcLayer.{cpp,h}` | All 13 layer setters, per-layer state |
| `hwc2_device/HwcDisplayConfigs.{cpp,h}` | Config enumeration and active config |
| `utils/UniqueFd.h`, `Worker.{cpp,h}`, `autolock.h`, `log.h`, `sync.{cpp,h}`, `properties.h` | RAII fd wrapper, thread/worker abstraction, sync helpers |
| `backend/Backend.{cpp,h}`, `BackendClient.{cpp,h}`, `BackendManager.{cpp,h}` | Base class + all-CLIENT fallback + registry |
| `bufferinfo/BufferInfoGetter.{cpp,h}` | Pluggable gralloc scaffold (already abstract in upstream) |
| `compositor/FlatteningController.{cpp,h}` (optional) | GPU composition flattening optimization |

### Discarded (compiled into DRM target only)

```
drm/DrmDevice.cpp                 (3500+ lines total in drm/)
drm/DrmAtomicStateManager.cpp
drm/DrmFbImporter.cpp
drm/DrmPlane.cpp, DrmCrtc.cpp, DrmConnector.cpp, DrmMode.cpp
drm/DrmProperty.cpp, DrmEncoder.cpp, DrmUnique.h
drm/VSyncWorker.cpp
bufferinfo/legacy/BufferInfo*.cpp (6 impls, ~600 lines — all DRM-tied)
compositor/DrmKmsPlan.cpp         (~300 lines — becomes DrmPlan : Plan)
```

### Written new (~1100 lines Tegra-specific)

| Component | Est. size | Purpose |
|---|---|---|
| `tegra/TegraHwcDisplayPipeline.{cpp,h}` | ~400 | `HwcDisplayPipeline` impl over `tegra_dc_ext` |
| `tegra/TegraCompositor.{cpp,h}` | ~300 | `Compositor` impl: Plan → `TEGRA_DC_EXT_FLIP3` |
| `tegra/TegraPlan.{cpp,h}` | ~150 | Immutable frame description: `nvfb_window[4]` + client-target slot |
| `tegra/TegraVSyncSource.{cpp,h}` | ~100 | DC event-mask reader (`TEGRA_DC_EXT_EVENT_VBLANK`), callback dispatch |
| `bufferinfo/BufferInfoNvGr.{cpp,h}` | ~100 | `BufferInfoGetter` impl via `libnvgr` API |
| `Android.bp`, service entry | ~50 | Build system, HIDL service registration |

## Phase Plan

| # | Phase | Duration | Done when |
|---|---|---|---|
| 0 | **Fork baseline** | 1 day | Fork on a stable tag, branch `tegra-backend`, existing DRM target builds on Lineage 15.1 against Tegra-unrelated device (sanity check) |
| 1 | **DRM touch-points audit** | 1 day | Grep-driven document of every place in `hwc2_device/`, `backend/`, `compositor/` that accesses `DrmDevice`/`DrmConnector`/`DrmCrtc`/`DrmPlane` types directly |
| 2 | **Design abstraction headers** | 1–2 days | Draft `HwcDisplayPipeline`, `Compositor`, `VSyncSource`, `Plan` interfaces. No implementations yet. Review against audit |
| 3 | **Extract DRM impl behind interface** | 3–5 days | `DrmHwcDisplayPipeline : HwcDisplayPipeline`, `DrmCompositor : Compositor`, `DrmPlan : Plan`. Upstream DRM target works without regressions. Tests green |
| 4 | **Tegra backend skeleton (stubs)** | 3–5 days | `TegraHwcDisplayPipeline` / `TegraCompositor` / `TegraPlan` / `TegraVSyncSource` compile. HWC loads. Present returns UNSUPPORTED but framework doesn't crash |
| 5 | **First frame (all-CLIENT)** | 3–5 days | `TegraCompositor::ExecutePlan` issues `TEGRA_DC_EXT_FLIP3` for the client target on window 0. VSync works. Fence discipline validated with `lsof -p $(pgrep surfaceflinger)` under load |
| 6 | **Overlay planning (phase 2)** | 2–3 weeks | `TegraPlan` extended to describe multi-window assignment. `Backend` demotes layers to CLIENT on overflow. RGBA layers without transform go to DEVICE |
| 7 | **Upstream PR** | in parallel | PR #1: abstraction + DRM migration (no Tegra code). PR #2 (optional): Tegra backend as an example consumer |

**To first frame: ~2 weeks.**
**To production overlay support: ~5–6 weeks.**

## Abstraction Interface Design

```cpp
// include/hwc_core/HwcDisplayPipeline.h
namespace hwc_core {

enum class ConnectorType { Unknown, DSI, HDMI, DP, LVDS, Virtual };

struct HwcMode {
    uint32_t id;
    uint32_t width;
    uint32_t height;
    uint32_t vsync_period_ns;
    uint32_t dpi_x;
    uint32_t dpi_y;
    uint32_t config_group;
};

struct HwcPlane {
    uint32_t id;
    uint32_t type;              // PRIMARY / OVERLAY / CURSOR
    uint32_t supported_formats; // bitmask
    uint32_t caps;              // scaling, rotation, YUV, tiled
};

class Plan {
public:
    virtual ~Plan() = default;
    virtual size_t GetLayerCount() const = 0;
    virtual const PlannedLayer& GetLayer(size_t idx) const = 0;
    // Backend-specific fields accessed via downcast in backend code only
};

class Compositor {
public:
    virtual ~Compositor() = default;
    virtual int TestPlan(const Plan& plan) = 0;
    virtual int ExecutePlan(const Plan& plan, int* out_present_fd) = 0;
};

class VSyncSource {
public:
    virtual ~VSyncSource() = default;
    using Callback = std::function<void(int64_t timestamp_ns)>;
    virtual int Enable(Callback cb) = 0;
    virtual int Disable() = 0;
};

class HwcDisplayPipeline {
public:
    virtual ~HwcDisplayPipeline() = default;

    virtual uint32_t      GetDisplayId() const = 0;
    virtual std::string   GetName() const = 0;
    virtual ConnectorType GetConnectorType() const = 0;
    virtual bool          IsConnected() const = 0;

    virtual std::vector<HwcMode> GetModes() const = 0;
    virtual HwcMode              GetActiveMode() const = 0;
    virtual int                  SetActiveMode(uint32_t mode_id) = 0;

    virtual Compositor&  GetCompositor() = 0;
    virtual VSyncSource& GetVSyncSource() = 0;

    virtual int SetPowerMode(PowerMode mode) = 0;

    virtual size_t GetPlaneCount() const = 0;
    virtual const HwcPlane& GetPlane(size_t idx) const = 0;
};

} // namespace hwc_core
```

### DRM Implementation Layering

```cpp
// drm/DrmHwcDisplayPipeline.h
class DrmHwcDisplayPipeline : public hwc_core::HwcDisplayPipeline {
    std::unique_ptr<DrmConnector>   connector_;
    std::unique_ptr<DrmCrtc>        crtc_;
    std::unique_ptr<DrmCompositor>  compositor_;   // wraps DrmAtomicStateManager
    std::unique_ptr<DrmVSyncSource> vsync_;
    // + existing fields migrated from old DrmDisplayPipeline

    // HwcDisplayPipeline API — delegate to existing DRM types
};
```

The existing `DrmDisplayPipeline` code is not deleted — it moves one level down and is owned by `DrmHwcDisplayPipeline`. The upstream DRM build target continues to work exactly as before.

### Tegra Implementation

```cpp
// tegra/TegraHwcDisplayPipeline.h
class TegraHwcDisplayPipeline : public hwc_core::HwcDisplayPipeline {
    UniqueFd dc_fd_;                 // /dev/tegra_dc_0
    UniqueFd dc_ctrl_fd_;            // /dev/tegra_dc_ctrl
    NvGrModule* gralloc_;
    std::unique_ptr<TegraCompositor>  compositor_;
    std::unique_ptr<TegraVSyncSource> vsync_;
    TegraDisplayCaps caps_;          // from TEGRA_DC_EXT_GET_CAP_INFO
    std::vector<HwcPlane> planes_;   // 4 windows on K1
};

class TegraCompositor : public hwc_core::Compositor {
    UniqueFd& dc_fd_;

    int TestPlan(const Plan& plan) override {
        // Validate window count, pitch alignment, scaling limits, rotation caps
        // from nvhwc.c::nvhwc_assign_windows logic — no ioctl, pure validation
    }

    int ExecutePlan(const Plan& plan, int* out_present_fd) override {
        const auto& tp = static_cast<const TegraPlan&>(plan);
        struct tegra_dc_ext_flip_3 flip = {};
        flip.win = reinterpret_cast<__u64>(tp.Windows());
        flip.win_num = tp.WindowCount();
        flip.flags = 0;
        flip.post_syncpt_fd = -1;
        if (ioctl(dc_fd_.Get(), TEGRA_DC_EXT_FLIP3, &flip) < 0) return -errno;
        *out_present_fd = flip.post_syncpt_fd;
        return 0;
    }
};
```

## Build-Time Backend Selection

Build-time selection (not runtime env var) — upstream-style, matches how `BufferInfoGetter` backends are already selected:

```bp
// Android.bp
soong_config_module_type {
    name: "hwcomposer_cc_defaults",
    module_type: "cc_defaults",
    config_namespace: "hwcomposer",
    variables: ["backend"],
    properties: ["srcs", "cflags", "shared_libs"],
}

soong_config_string_variable {
    name: "backend",
    values: ["drm", "tegra"],
}

hwcomposer_cc_defaults {
    name: "hwcomposer_backend_defaults",
    soong_config_variables: {
        backend: {
            drm: {
                cflags: ["-DHWC_BACKEND_DRM=1"],
                srcs: ["drm/**/*.cpp", "bufferinfo/legacy/*.cpp"],
                shared_libs: ["libdrm"],
            },
            tegra: {
                cflags: ["-DHWC_BACKEND_TEGRA=1"],
                srcs: ["tegra/**/*.cpp", "bufferinfo/BufferInfoNvGr.cpp"],
                shared_libs: ["libnvgr"],
            },
        },
    },
}

cc_library_shared {
    name: "hwcomposer.tegra",
    defaults: ["hwcomposer_backend_defaults"],
    srcs: [
        "hwc2_device/*.cpp",
        "utils/*.cpp",
        "backend/*.cpp",
        "bufferinfo/BufferInfoGetter.cpp",
        "compositor/FlatteningController.cpp",
    ],
}
```

Device `BoardConfig.mk`:

```make
SOONG_CONFIG_NAMESPACES += hwcomposer
SOONG_CONFIG_hwcomposer += backend
SOONG_CONFIG_hwcomposer_backend := tegra
```

In `DrmHwcTwo::Init()`:

```cpp
#if defined(HWC_BACKEND_TEGRA)
    pipelines_ = TegraHwcDisplayPipeline::EnumerateAll();
#elif defined(HWC_BACKEND_DRM)
    pipelines_ = DrmHwcDisplayPipeline::EnumerateAll();
#else
    #error "No HWC backend selected"
#endif
```

## DRM Touch-Points Requiring Refactor

Concrete list of places in upstream that directly access DRM types and must be migrated behind the interface. Count: ~40–60 call sites, 2–3 days of careful refactor.

| File / expression | Change |
|---|---|
| `HwcDisplay::pipeline_` (`DrmDisplayPipeline` by value) | `std::unique_ptr<HwcDisplayPipeline>` |
| `HwcDisplay::AtomicCommit()` → `pipeline_.atomic_state_manager->ExecuteAtomicCommit(...)` | `pipeline_->GetCompositor().ExecutePlan(plan, &out_fd)` |
| `HwcDisplay::ValidateStagedComposition()` → `DrmAtomicStateManager::Commit(TEST_ONLY)` | `pipeline_->GetCompositor().TestPlan(plan)` |
| `HwcLayer::PopulateLayerData()` → `buffer_info_.fb_id` (DRM handle) | Generic `FbHandle` / union, accessed via getter |
| `HwcDisplayConfigs::Update()` → `DrmConnector::UpdateModes()` | `pipeline_->GetModes()` |
| `backend/Backend.cpp::ValidateLayers()` → `CrtcPlanner::CreateInstance(drm_device)` | `plan_builder_->BuildPlan(layers, pipeline)` |
| `compositor/DrmKmsPlan` | Renamed to `DrmPlan : Plan`, and an abstract `Plan` base split out |
| `hwc2_device/DrmHwcTwo::Init()` → `DrmDevice::CreateInstances()` | Pipeline factory via `#ifdef` (above) |
| `HwcDisplay::SetPowerMode()` → `connector_->SetDpmsMode()` | `pipeline_->SetPowerMode()` |
| `HwcDisplay::VSyncCallback` registration → `DrmVSyncWorker::Init()` | `pipeline_->GetVSyncSource().Enable(cb)` |

## Pitfalls

1. **`BufferInfo` struct contains DRM concepts.** Upstream has `uint32_t prime_fds[4]`, `uint32_t modifiers`, `uint32_t fb_id` as direct fields. Keep them for DRM backend; add a backend-tagged union for `TegraSurface { NvRmSurface surfs[3]; uint32_t nvgr_memfd; }`. Read through a getter in `HwcLayer`, not direct field access — this is the only refactor that touches `HwcLayer` beyond trivial.
2. **Tests `drm/test/*` are DRM-specific.** Keep them compiling for the DRM target only. `utils/test/*` and `bufferinfo/test/*` partially port. Don't try to universalize upstream's test framework — it's coupled to DRM mocks.
3. **`libdrm` dependency must be conditional.** Code in `drm/` uses `<xf86drm.h>` — never compiles in Tegra target. Compile-time backend selection (above) is essential; runtime dispatch would force linking against libdrm always.
4. **Pick a release tag, not HEAD.** Upstream moves. Fork at a stable tag (`v3.0.0` or latest release). Rebase onto newer tags later, once; don't continuously chase HEAD.
5. **Fence source differs.** Upstream uses `OUT_FENCE_PTR` atomic property — fence fd appears after commit. Tegra's `TEGRA_DC_EXT_FLIP3` returns fence in `post_syncpt_fd` after ioctl returns. Both produce sync_file fds — the `Compositor::ExecutePlan(plan, &out_fd)` interface hides the mechanism difference.
6. **C++ standard.** Upstream uses C++17 (`std::optional`, structured bindings, fold expressions). Verify Lineage 15.1 `BoardConfig.mk` / `Android.mk` compiler flags — should be fine on clang 5+, but check.
7. **HIDL service wrapper.** Upstream ships a HIDL `composer@2.1-service` wrapper. Reuse it as-is — it's generic HWC2 bridging.

## Upstream PR Strategy

Split into two PRs, land independently.

### PR #1 — Abstraction + DRM Migration

**Scope.** Introduce `HwcDisplayPipeline`, `Compositor`, `VSyncSource`, `Plan` interfaces. Migrate existing DRM implementation behind them. No functional changes. DRM target works identically.

**Pitch.** "Introduce pluggable display backend abstraction. DRM backend migrated behind the new interface. Enables vendor-specific backends (e.g., legacy downstream kernels without DRM) without forking. Existing DRM target is binary-identical."

**Diff size.** ~800–1200 lines, mostly moving code + thin wrappers.

**Likelihood of acceptance.** Medium-to-high. drm-hwcomposer already moved toward abstraction with `BufferInfoGetter`; adding pluggable pipeline is a logical continuation. Collabora/ChromeOS may push back if they see it as unnecessary complexity — argument is "other platforms exist beyond mainline-DRM" + showing a working consumer helps.

**Forums.** Mesa mailing list (`mesa-dev`), drm-hwcomposer GitLab issue tracker, XDC (X.Org Developer Conference) BOF if timing aligns.

### PR #2 — Tegra K1 Backend (Optional)

**Scope.** Add `tegra/` directory with Tegra K1 pipeline. Documents why (legacy L4T R24 kernel without DRM/KMS on T124).

**Likelihood.** Low. Too narrow — Tegra K1 is a 2014 chip, LineageOS-only userland, very small user base. Upstream will likely say "keep it in your fork".

**Fallback.** That's fine. Your fork carries the Tegra backend using the abstraction landed in PR #1, and rebases cleanly on every upstream release.

## Summary

- **Reused lines: ~3000** (HWC2 API + backend + utils — battle-tested in ChromeOS production)
- **Written lines: ~1100** (Tegra backend only)
- **Refactor overhead: ~1 week** (one-time abstraction work, pays off forever)
- **First frame: ~2 weeks**
- **Production-ready with overlay: ~5–6 weeks**
- **Upstream contribution path: clean** — PR #1 stands on its own architectural merit

This is the best ROI among the three options evaluated.

## Related Documents

- [drm-hwcomposer as Reference](drm-hwcomposer-reference.md) — why direct port is blocked, which parts are reusable (this plan operationalizes those findings)
- [Shared-Core Architecture](shared-core-hwc2-hwc3.md) — layering that also keeps HWC3 migration affordable
- [HWC2 Implementation Plan](hwc2-implementation-plan.md) — the original from-scratch plan (superseded by this one for the backend question; phased bring-up steps still apply)
- [Composition Engines](composition-engines.md) — 2D/VIC/GL dispatch for `TegraCompositor`
- [Display Controller Kernel Interface](display-controller-interface.md) — `tegra_dc_ext_*` reference
