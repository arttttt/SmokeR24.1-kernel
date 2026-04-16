# HWC2 vs HWC3 — API Shape and Migration Notes

## Overview

HWC3 is the **AIDL-based Composer HAL** introduced in **Android 13 (T)**. The composition model (Layer / Display / validate → present, DEVICE vs CLIENT composition, fence discipline) is unchanged from HWC2. What changed is the transport, the command format, and a handful of new capabilities tied to platform features that landed in Android 13+ (HDR refinements, VRR, display decorations).

For a Tegra K1 native HWC effort targeting LineageOS 15.1 (Android 8.1) as the near-term base, HWC3 is not a near-term target. It is, however, an input into the **internal architecture**: the HWC core should be structured so the same backend can later be exposed over an AIDL shim without rewriting the composition logic.

This document catalogs the differences so that the HWC2 implementation does not accidentally couple itself to the HIDL transport.

## Interface Files

| Generation | Transport | Interface file (AOSP path) |
|---|---|---|
| HWC2 (HIDL 2.1) | HIDL | `hardware/interfaces/graphics/composer/2.1/IComposer.hal` |
| HWC2.2/2.3/2.4 | HIDL | `hardware/interfaces/graphics/composer/2.{2,3,4}/IComposer.hal` |
| HWC3 (AIDL) | AIDL | `hardware/interfaces/graphics/composer3/aidl/android/hardware/graphics/composer3/IComposer.aidl` |

HWC3 entry point is `android.hardware.graphics.composer3.IComposer`, with `IComposerClient` and `IComposerCallback` as companion interfaces. HIDL 2.4 remains a supported transport for devices that launched before Android 13; new platform features only land in AIDL.

## API Differences

### Command Submission

**HWC2.x (HIDL 2.1+)** — per-frame commands travel as an opaque serialized blob through an `MQDescriptorSync` (fast message queue). Opcodes are defined in `hardware/interfaces/graphics/composer/2.1/IComposerClient.hal` under `enum Command`. The HAL parses the blob byte-by-byte.

**HWC3 (AIDL)** — per-frame commands are passed as typed parcelable arrays:

```aidl
CommandResultPayload[] executeCommands(in DisplayCommand[] commands);
```

`DisplayCommand` carries `long display`, lifecycle flags, client target metadata, validate/present parameters, and an array of `LayerCommand`. `LayerCommand` is a struct with all per-layer properties as explicit fields:

```aidl
parcelable LayerCommand {
    long layer;
    @nullable Buffer buffer;                      // buffer + acquire fence + slot
    @nullable ParcelableBlendMode blendMode;
    @nullable Color color;                        // solid-color layers
    @nullable ParcelableComposition composition;  // CLIENT/DEVICE/CURSOR/...
    @nullable ParcelableDataspace dataspace;
    @nullable Rect displayFrame;
    @nullable PlaneAlpha planeAlpha;
    @nullable ParcelableTransform transform;
    @nullable FRect sourceCrop;
    @nullable int[] visibleRegion;
    @nullable int[] damageRegion;
    @nullable int zOrder;
    @nullable PerFrameMetadata[] perFrameMetadata;
    @nullable PerFrameMetadataBlob[] perFrameMetadataBlob;
    @nullable LayerBrightness brightness;
    @nullable WhitePointNits whitePointNits;
}
```

**Consequence for implementation.** In HWC2 your shim owns the opcode parser; in HWC3 Binder delivers already-typed objects. Keep the parser out of your core. The core should accept something like `struct LayerRequest { /* fields as above */ };` and not care which transport produced it.

### Result Payloads

**HWC2** — results are gathered through follow-up calls: `getChangedCompositionTypes`, `getDisplayRequests`, `getReleaseFences`, `getPresentFence`, plus the per-opcode reply buffer.

**HWC3** — `executeCommands` returns a single typed array:

```aidl
union CommandResultPayload {
    Error error;
    ChangedCompositionTypes changedCompositionTypes;
    DisplayRequest displayRequest;
    PresentFence presentFence;
    ReleaseFences releaseFences;
    PresentOrValidate.Result presentOrValidateResult;
    ClientTargetProperty clientTargetProperty;
}
```

One round-trip instead of many. Fewer Binder crossings, lower latency.

### Fence Types

Both use `sync_file` file descriptors internally. HWC3 wraps them in `ParcelableFileDescriptor` instead of raw HIDL `handle`. Ownership semantics are unchanged: one acquire consume per layer, unique release fd per layer, one present fd per frame.

### Display Configuration

**HWC2** — attributes are queried individually via `getDisplayAttribute(display, config, attribute, ...)` with `attribute ∈ {WIDTH, HEIGHT, VSYNC_PERIOD, DPI_X, DPI_Y, CONFIG_GROUP}`.

**HWC3** — `getDisplayConfigurations(display, maxFrameIntervalNs)` returns `DisplayConfiguration[]` as whole structs:

```aidl
parcelable DisplayConfiguration {
    int configId;
    int width;
    int height;
    @nullable DisplayConfiguration.Dpi dpi;
    int configGroup;
    int vsyncPeriod;
    @nullable VrrConfig vrrConfig;
}
```

Plus `DisplayConfiguration.VrrConfig` for Variable Refresh Rate hints.

### Variable Refresh Rate (VRR)

Entirely new in HWC3. Hooks:

- `notifyExpectedPresent(display, expectedPresentTime, frameIntervalNs)` — hint from SurfaceFlinger about upcoming presents.
- `IComposerCallback::onRefreshRateChangedDebug(display, vsyncPeriodNanos)`.
- `DisplayConfiguration.VrrConfig { minFrameIntervalNs, frameIntervalPowerHints[], notifyExpectedPresentConfig }`.

None of this is relevant for the DSI panel on mocha (fixed 60 Hz). Stub it out.

### HDR and Brightness

**HWC3 additions**:

- `DisplayBrightness` per-layer dimming via `LayerBrightness { float brightness; }`.
- `ClientTargetProperty.dimmingStage` — tells SF whether HAL or client applies luminance scaling.
- `LayerCommand.whitePointNits` — per-layer peak brightness in HDR content.
- `OverlayProperties` / `getOverlaySupport()` — queries which format/dataspace combinations can use hardware overlay.
- `getHdrConversionCapabilities` / `setHdrConversionStrategy` — SDR↔HDR conversion hints.

Tegra K1 display controller has no HDR pipeline. Return empty capability sets.

### Display Decorations

`DisplayDecorationSupport { PixelFormat format, AlphaInterpretation alphaInterpretation }` — lets a display advertise a hardware alpha mask for rounded corners / notch cutouts. Not applicable to mocha.

### Content Type

`ContentType { NONE, GRAPHICS, PHOTO, CINEMA, GAME }` — hint to the display hardware about content category (influences gamma / color processing on some panels). On mocha, stub to `NONE`.

### Power and Refresh

- `PowerMode` extended with `ON_SUSPEND` (HWC2 had it since 2.2, HWC3 keeps it).
- `RefreshRateConfigs` split into `FrameIntervalPowerHint[]`.

### Capabilities

```aidl
enum Capability {
    INVALID,
    SIDEBAND_STREAM,
    SKIP_CLIENT_COLOR_TRANSFORM,
    PRESENT_FENCE_IS_NOT_RELIABLE,
    SKIP_VALIDATE,
    BOOT_DISPLAY_CONFIG,
    HDR_OUTPUT_CONVERSION_CONFIG,
    REFRESH_RATE_CHANGED_CALLBACK_DEBUG,
    DISPLAY_IDLE_TIMER,
    LAYER_LIFECYCLE_BATCH_COMMAND,
}
```

`SKIP_VALIDATE` is the one that matters for perf — HAL tells SF "if configuration didn't change, skip validate before present". Implementable on our backend if layer state fingerprinting is cheap.

## Fence Semantics (No Change)

The "strict" fence model is identical between HWC2 and HWC3:

| Fence | Direction | Rule |
|---|---|---|
| Acquire (per layer) | SF → HAL | HAL MUST consume (wait + close) exactly once |
| Release (per layer) | HAL → SF | HAL MUST return unique fd per layer |
| Present (per frame) | HAL → SF | HAL MUST return one fd per `presentDisplay` |

This is precisely the contract the existing `hwc2on1` adapter violates with the NVIDIA HWC1 blob (see `hwc1-vs-hwc2.md` § "Why hwc2on1 Adapter Breaks with NVIDIA HWC1"). A native implementation gets this right for both HWC2 and HWC3 with the same `FenceManager`.

## What This Means for the Tegra K1 Effort

1. **Target HWC2 first.** Android 8.1 baseline demands it; the bulk of the work (display backend, composition engine, fence manager) is identical for HWC3.
2. **Keep the core transport-agnostic.** No `hwc2_*` types in the core. No HIDL-specific serialization in the core. See `shared-core-hwc2-hwc3.md`.
3. **Layer command model should already match HWC3's shape.** If your internal `LayerRequest` mirrors AIDL `LayerCommand`, the AIDL shim becomes ~500 lines of field copying.
4. **Fence discipline is the same.** The `FenceManager` from the HWC2 plan applies to HWC3 unchanged.
5. **Stub the HWC3-only features.** VRR, HDR, brightness, display decorations, content type — return defaults. They all have well-defined "none supported" states.

## References

- HWC3 AIDL definition — `hardware/interfaces/graphics/composer3/aidl/android/hardware/graphics/composer3/`
- HWC3 default implementation — `hardware/interfaces/graphics/composer/aidl/default/`
- HWC3 VTS — `hardware/interfaces/graphics/composer/aidl/vts/`
- Google reference shared-core HWC — `hardware/google/graphics/common/libhwc2.1/`
- `drm_hwcomposer` AIDL port — `external/drm_hwcomposer/` (post-2023)

## Related Documents

- [HWC1 vs HWC2](hwc1-vs-hwc2.md) — the previous API transition
- [HWC2 Implementation Plan](hwc2-implementation-plan.md) — concrete plan for mocha
- [Shared-Core Architecture](shared-core-hwc2-hwc3.md) — how to structure for both
