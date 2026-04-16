# Shared-Core Architecture for HWC2 + HWC3

## Goal

Structure the Tegra K1 HWC so a single backend serves two transports:

- **HWC2 (HIDL)** — Android 8.1 through 12, and Lineage 15.1–19.1. This is the near-term target.
- **HWC3 (AIDL)** — Android 13+. Not needed now, but the code should be shaped so an AIDL shim is a few hundred lines of field copying, not a rewrite.

The composition model, the display backend, the fence discipline, and the planner are identical between HWC2 and HWC3. Only the transport boundary differs. Isolate the transport.

## Reference Implementation

The canonical example of this layering is Google's `hardware/google/graphics/common/libhwc2.1/` (used by Exynos and Pixel). The same pattern is followed by `external/drm_hwcomposer/` starting with its 2023 AIDL migration. In both cases the internal `Device` / `Display` / `Layer` classes know nothing about which transport is active; HIDL and AIDL interface services are built as thin adapters on top.

We follow the same layering.

## Layer Diagram

```
┌──────────────────────────────────────────────────────────────┐
│              Android Framework (SurfaceFlinger)              │
└──────────────────────────────────────────────────────────────┘
              │                                      │
              │ HIDL composer@2.1..2.4                │ AIDL composer3 (Android 13+)
              ▼                                      ▼
┌─────────────────────────┐      ┌─────────────────────────────┐
│  hwc2_shim/             │      │  hwc3_shim/                 │
│    Hwc2Device.cpp       │      │    Hwc3Composer.cpp         │
│    Hwc2DisplayAdapter   │      │    CommandDispatcher.cpp    │
│    Hwc2LayerAdapter     │      │    Hwc3CallbackBridge       │
│    command buffer parse │      │    AIDL ↔ core type mapping │
└────────────────────────┬┘      └┬────────────────────────────┘
                         │        │
                         ▼        ▼
              ┌──────────────────────────┐
              │  hwc_core/  (NO API-version types) │
              │    ComposerCore                    │
              │    HwDisplay / HwLayer             │
              │    FenceManager                    │
              │    CompositionPlanner              │
              │    TegraDisplayBackend             │
              │    CompositionEngine (2D/VIC/GL)   │
              └──────────────────────────┘
                         │
                         ▼
              Tegra K1 HW (/dev/tegra_dc_*, VIC, 2D, DC planes)
```

## Hard Rules for the Core

1. **No `hwc2_*` types in `hwc_core/`.** Not `hwc2_composition_t`, not `hwc2_error_t`, not `hwc2_layer_t`. The core uses its own enums and numeric layer IDs.
2. **No AIDL types in `hwc_core/`.** Not `aidl::android::hardware::graphics::composer3::Composition`, not `ParcelFileDescriptor`. Raw `int` fds, `android::Fence` where helpful.
3. **No HIDL Binder types in `hwc_core/`.** Not `hidl_vec`, not `hidl_handle`, not `MQDescriptorSync`.
4. **Core types are a superset.** If HWC3 has a field HWC2 lacks (e.g. `LayerBrightness`), the core type has it. The HWC2 shim just ignores it; the HWC3 shim fills it in.

## Core Types

Define these in `hwc_core/HwcTypes.h`:

```cpp
namespace hwc_core {

enum class Composition {
    Invalid = 0,
    Client,          // HWC2 FRAMEBUFFER / HWC3 CLIENT — GPU composition by SF
    Device,          // HWC2 DEVICE    / HWC3 DEVICE  — hardware overlay
    SolidColor,      // HWC2 SOLID_COLOR
    Cursor,          // HWC2 CURSOR
    Sideband,        // HWC2 SIDEBAND (we don't support it, but keep the enum)
    DisplayDecoration, // HWC3-only (stub)
};

enum class BlendMode {
    Invalid = 0,
    None,
    Premultiplied,
    Coverage,
};

enum class Transform : uint32_t {
    None   = 0,
    FlipH  = 1 << 0,
    FlipV  = 1 << 1,
    Rot90  = 1 << 2,
    Rot180 = FlipH | FlipV,
    Rot270 = Rot90 | Rot180,
};

enum class Error {
    None = 0,
    BadDisplay,
    BadLayer,
    BadConfig,
    BadParameter,
    HasChanges,
    NoResources,
    NotValidated,
    Unsupported,
};

struct Rect { int32_t left, top, right, bottom; };
struct FRect { float left, top, right, bottom; };
struct Color { float r, g, b, a; };  // HWC3 uses float; HWC2 uint8_t — convert in shim
struct Region { std::vector<Rect> rects; };

struct LayerRequest {
    // Populated by shim from HIDL command buffer or AIDL LayerCommand.
    // Any nullopt field means "unchanged since previous frame".
    std::optional<buffer_handle_t>   buffer;
    std::optional<int>               acquireFenceFd;   // owned: core consumes
    std::optional<BlendMode>         blendMode;
    std::optional<Color>             solidColor;
    std::optional<Composition>       composition;
    std::optional<int32_t>           dataspace;        // HAL_DATASPACE_*
    std::optional<Rect>              displayFrame;
    std::optional<float>             planeAlpha;
    std::optional<Transform>         transform;
    std::optional<FRect>             sourceCrop;
    std::optional<Region>            visibleRegion;
    std::optional<Region>            damageRegion;
    std::optional<int32_t>           zOrder;
    // HWC3-only fields, ignored by HWC2 shim:
    std::optional<float>             brightness;       // LayerBrightness
    std::optional<float>             whitePointNits;
};

struct PresentResult {
    int presentFenceFd = -1;                // owned: caller takes
    std::vector<std::pair<uint64_t, int>> releaseFences;  // layerId → fd
    std::vector<std::pair<uint64_t, Composition>> changedCompositionTypes;
    Error error = Error::None;
};

} // namespace hwc_core
```

These types mirror HWC3's `LayerCommand` shape deliberately. The HWC2 shim does `LayerRequest → command buffer ops`; the HWC3 shim does `LayerCommand → LayerRequest` as one-to-one field copies.

## Core Interface

```cpp
namespace hwc_core {

class ComposerCore {
public:
    static std::unique_ptr<ComposerCore> Create();

    // Device-level
    std::vector<uint64_t> GetDisplays() const;
    void SetHotplugCallback(std::function<void(uint64_t, bool connected)>);
    void SetVsyncCallback(std::function<void(uint64_t, int64_t timestamp)>);
    void SetRefreshCallback(std::function<void(uint64_t)>);

    // Display-level
    uint64_t CreateLayer(uint64_t display, Error* out);
    Error    DestroyLayer(uint64_t display, uint64_t layer);
    Error    SetClientTarget(uint64_t display, buffer_handle_t target,
                             int acquireFenceFd, int32_t dataspace,
                             const Region& damage);
    Error    SetLayer(uint64_t display, uint64_t layer, const LayerRequest& req);

    Error ValidateDisplay(uint64_t display,
                          std::vector<std::pair<uint64_t, Composition>>* changed,
                          std::vector<std::pair<uint64_t, uint32_t>>* requests);
    Error AcceptDisplayChanges(uint64_t display);
    PresentResult PresentDisplay(uint64_t display);

    // Configuration
    std::vector<uint32_t> GetDisplayConfigs(uint64_t display);
    Error GetDisplayAttribute(uint64_t display, uint32_t config,
                              DisplayAttribute attr, int32_t* value);
    Error SetActiveConfig(uint64_t display, uint32_t config);
    Error SetPowerMode(uint64_t display, PowerMode mode);
    Error SetVsyncEnabled(uint64_t display, bool enabled);
};

} // namespace hwc_core
```

Nothing HWC2-specific, nothing HWC3-specific. Everything in native Android types or `hwc_core` types.

## HWC2 Shim

```cpp
// hwc2_shim/Hwc2Device.cpp
class Hwc2Device {
    static int32_t Open(const struct hw_module_t* module,
                        const char* name, struct hw_device_t** dev);

    // hwc2_device_t function pointer table
    static hwc2_function_pointer_t GetFunction(hwc2_device_t*,
                                                int32_t descriptor);

    // Translate HWC2 calls → core
    Error validateDisplay(hwc2_display_t d, uint32_t* outNumTypes,
                          uint32_t* outNumRequests) {
        auto result = core_->ValidateDisplay(d, &changed_, &requests_);
        *outNumTypes = changed_.size();
        *outNumRequests = requests_.size();
        return ToHwc2Error(result);
    }

    std::unique_ptr<hwc_core::ComposerCore> core_;
};
```

The HIDL `composer@2.1` service (and @2.2/2.3/2.4) sits one level further out, translating HIDL command-buffer opcodes into `hwc2_device_t` calls — but we can skip the intermediate `hwc2_device_t` struct entirely and have the HIDL service call into `ComposerCore` directly. Fewer layers, less boilerplate. The HIDL command buffer parser lives in `hwc2_shim/CommandBufferParser.cpp`.

## HWC3 Shim (Future)

```cpp
// hwc3_shim/Hwc3Composer.cpp (stub — to be written when targeting Android 13+)
class Hwc3ComposerClient : public aidl::android::hardware::graphics::composer3::BnComposerClient {
    ndk::ScopedAStatus executeCommands(
            const std::vector<DisplayCommand>& commands,
            std::vector<CommandResultPayload>* results) override {
        for (const auto& dc : commands) {
            for (const auto& lc : dc.layers) {
                auto req = LayerCommandToLayerRequest(lc);  // field copying
                core_->SetLayer(dc.display, lc.layer, req);
            }
            if (dc.validateDisplay) {
                // → core_->ValidateDisplay()
                // → pack ChangedCompositionTypes into results
            }
            if (dc.presentDisplay || dc.presentOrValidateDisplay) {
                auto r = core_->PresentDisplay(dc.display);
                // → pack PresentFence, ReleaseFences into results
            }
        }
        return ndk::ScopedAStatus::ok();
    }

    std::unique_ptr<hwc_core::ComposerCore> core_;  // same instance as HWC2 shim
};
```

## Enum Translation Tables

Keep these in `hwc2_shim/TypeConversions.cpp` and `hwc3_shim/TypeConversions.cpp` — one file per direction each. No magic, just tables.

```cpp
// hwc2_shim/TypeConversions.cpp
int32_t ToHwc2Composition(Composition c) {
    switch (c) {
        case Composition::Client:     return HWC2_COMPOSITION_CLIENT;
        case Composition::Device:     return HWC2_COMPOSITION_DEVICE;
        case Composition::SolidColor: return HWC2_COMPOSITION_SOLID_COLOR;
        case Composition::Cursor:     return HWC2_COMPOSITION_CURSOR;
        case Composition::Sideband:   return HWC2_COMPOSITION_SIDEBAND;
        default:                      return HWC2_COMPOSITION_INVALID;
    }
}
```

## Shared Components

- **FenceManager** — identical code for both paths. Enforces one-consume-per-acquire, unique-release-per-layer, one-present-per-frame. Uses only `int` fds and `sync_wait`.
- **CompositionPlanner** — ports `nvhwc_assign_windows()` from `nvhwc.c`. Operates on core types.
- **TegraDisplayBackend** — `tegra_dc_ext_*` ioctl wrapper. No knowledge of HWC API version.
- **CompositionEngine** — dispatches to `libnvblit` / `libnvddk_vic` / GLES fallback. See `composition-engines.md`.

## Process / Service Layout

One HAL binary. In `manifest.xml` declare the HIDL service (`composer@2.1`) now. When Android 13 ever matters, add the AIDL service to the same binary; both shims wrap the same `ComposerCore` singleton.

```
system/vendor/bin/hw/android.hardware.graphics.composer-service.tegra
    ├── main.cpp           — registers both HIDL and AIDL services (AIDL optional at build time)
    ├── hwc2_shim/
    ├── hwc3_shim/         (empty until needed)
    └── hwc_core/
```

## What You Save

For HWC2 today: ~2500–3500 lines of backend + 500–800 lines of HWC2 shim.

For HWC3 later: if the core is clean, the AIDL shim is **~400–600 lines** (AIDL type imports + command dispatch + field copying). No composition logic moves. No fence logic moves. No ioctl code moves.

Without this discipline, the shim tends to be 2000–4000 lines of interleaved logic and transport glue, and the HWC3 migration becomes a full rewrite.

## Related Documents

- [HWC2 vs HWC3](hwc2-vs-hwc3.md) — API differences catalog
- [HWC2 Implementation Plan](hwc2-implementation-plan.md) — phased plan, file layout
- [drm-hwcomposer as Reference](drm-hwcomposer-reference.md) — reusable patterns from upstream project
