# Camera Stack Bring-up on Mi Pad 1 (Tegra K1) — Article Series Outline

Working language: English. Audience: kernel / Android-HAL / Vulkan engineers familiar
with embedded camera pipelines. Goal: document the full story of bringing up an
open-source camera stack on a Tegra K1 device, end-to-end (kernel V4L2 → Vulkan HAL).

Two articles, each split into chapters by topic. Chapter notes are working tezisy,
not final prose. Each chapter lists the source-of-truth files / commits / branches
so the article can be expanded directly against the code.

---

## Article 1 — Kernel side

**Branch of record:** `SmokeR24.1-stable` (camera-relevant work also on
`debug/fbconsole`, `feature/fm-v4l2-bringup`, `feature/isp-reprocess`).
**Repo:** SmokeR24.1-kernel (this tree).

### Ch. 1 — Context and the fork in the road

- Target: Xiaomi Mi Pad 1 (codename `mocha`), Tegra K1 (T124), LineageOS 14.1
  (Android 7.1.2). 1536×2048 portrait display; rotation sensors broken on the
  unit; no adb — deployment via custom HTTP/kexec flow.
- Sensors: IMX179 rear (8 MP, RGGB, CSI-A 4-lane), OV5693 front (5 MP, BGGR,
  CSI-E 1-lane), AD5823 VCM. See `reference_camera_hardware.md`.
- Stock camera stack: NVIDIA closed HAL (`camera.tegra.so`) sitting on
  `libnvmm_camera_v3.so` (NvCameraCore) on top of `libnvisp_v3.so`. Closed
  end-to-end; the only published Tegra source from NVIDIA stops at
  `libnvodm_imager` / kernel glue.
- The fork-in-the-road paragraph: a brief account of trying the stock-blob
  path first (custom HAL3 wrapping NvCameraCore via dlopen — repo
  `custom_tegra_camera`). ISP accepted the buffer and reported completion,
  but output stayed uninitialised (~0xFF). Stock pipeline routes pixels
  through a DZ (digital-zoom / scaler) component that we never finished
  re-binding. We pivoted to a fully open path: own V4L2 driver in kernel +
  own HAL on Vulkan. Memory: `project_camera_stock_blobs.md`,
  `project_isp_processframe.md` (full RE for completeness).
- Goals for the open stack: Camera2 (HAL 3.4) compliance, no closed blobs
  on the data path, reasonable performance (≥20 fps at 1080p), zero-copy
  end-to-end.

### Ch. 2 — V4L2 driver bring-up

- Kernel media subsystem layout on T124: VI (Video Input) → CSI (MIPI receiver)
  → ISP. Path used for our stack: VI/CSI capture only, ISP unused on the
  kernel side (will be replaced by Vulkan compute in userspace).
- Sensor drivers: IMX179 + OV5693 + AD5823 VCM as V4L2 subdevs on
  `i2c@7000c500` (adapter 2). I2C addresses, MCLK assignments (CAM_MCLK
  24 MHz for IMX179, MCLK2 for OV5693).
- CSI port mapping on T124 — important and undocumented:
  - IMX179 → CSI-A+B pins → CILA+CILB → PP_A → VI_CSI_0
    (`DPD = CSIA+CSIB`)
  - OV5693 → CSI-E pins → CILE → PP_B → VI_CSI_1 (`DPD = CSIE`, reg1 bit12)
- GPIO map (CAM_RSTN, CAM2_PWDN, CAM2_RSTN, CAM_AF_PWDN — AD5823 enable owned
  by IMX179) → `reference_camera_hardware.md`.
- DTS bindings — what's mocha-specific. Pin groups, power supplies, regulator
  rails, the readout_orientation=180 quirk on OV5693 (sensor reads inverted;
  VI is expected to handle the flip).
- Sensor mode tables: how V4L2 framesizes / framerates compose to a mode
  table. The trade-off between maintaining many entries vs sensor-side
  PLL/line-time math.
- FPS investigation: target vs measured.
  - IMX179 1080p30 = 31 fps (clean), 720p30 = 31 fps, 3264×2448@30 = 5 fps
    (DMA bandwidth ceiling on T124, ~16 MB/frame).
  - IMX179 720p60 stays at ~30 fps — mode table not switching; calls out the
    need for a dedicated high-FPS mode entry.
  - OV5693 limited by 1-lane CSI-E: 1080p30 = 20 fps, 720p120 = 30 fps
    (mode table switches but lane bandwidth caps).
  - Memory: `project_camera_fps_modes.md`.
- AD5823 focuser as a `v4l-subdev`. V4L2_CID_FOCUS_ABSOLUTE, calibrated
  positions (infinity=140, macro=640), I2C 0x0C. The IMX179 driver owns
  CAM_AF_PWDN — turning the rear sensor off also kills the focuser.
- V4L2 control plumbing: V4L2_CID_EXPOSURE_ABSOLUTE, V4L2_CID_GAIN (Q8 fixed-
  point on IMX179, commit `dc519b07bd5` on `feature/fm-v4l2-bringup`).
  How frame_length / coarse_time interplay drives effective FPS.
- Kconfig / DTS gate: V4L2 stack is enabled/disabled via Kconfig + DTS, never
  reverted as commits — so the stock-blob path can be revived as a build-time
  option. Memory rule: `feedback_v4l2_config_only.md`.

### Ch. 3 — nvmap foreign DMA-BUF import (the kernel patch that unlocked zero-copy)

- The problem: Tegra's `nvmap` is the only allocator that NVIDIA's Vulkan
  driver and gralloc both accept. V4L2 capture buffers are exported as
  generic DMA-BUFs (`V4L2_MEMORY_DMABUF` + EXPBUF). Stock nvmap rejects
  foreign DMA-BUFs (`NVMAP_IOC_FROM_FD` returns the fd as-is only if it's
  already an nvmap fd) — confirmed absent from NVIDIA's L4T R32.x–R36.x.
- The patch (branch `debug/fbconsole`): teach `nvmap` to wrap any foreign
  DMA-BUF into a synthetic nvmap handle. Files: `nvmap_dmabuf.c`,
  `nvmap_alloc.c`, `nvmap_handle.c`, `nvmap_priv.h`. Memory:
  `project_nvmap_foreign_dmabuf.md`.
- The userspace consequence: a V4L2 dmabuf fd can now be promoted to an
  nvmap fd (`NVMAP_IOC_FROM_FD` → handle → `NVMAP_IOC_GET_FD`) and imported
  into Vulkan as an `OPAQUE_FD` external memory handle. Capture writes
  land directly in Vulkan-allocated memory — no `VIDIOC_DQBUF` copy.
- Measured impact: V4L2 upload step 26 ms → 0 ms; total frame
  44 ms → 28 ms (~35 fps headroom). The single biggest perf step in the
  whole project.
- Discussion points for the article:
  - Why nvmap is the gate at all on Tegra (vs Mesa/Linux's
    `VK_EXT_external_memory_dma_buf`, which doesn't exist on this driver).
  - The dual nature of the patch: trivial as a diff (~hundreds of lines),
    enormous as an unlock. NVIDIA never shipped this in 8+ years of L4T.
  - Compatibility: only affects ABI on the import path; no risk to
    existing nvmap users.

### Ch. 4 — Debug infrastructure (short chapter, working flow)

- No adb on the device — flashing the full stack on every iteration is
  prohibitive. Custom flow: `mocha-remote.sh` (HTTP server on device + SPA
  + kexec) lets us boot a freshly-built kernel + ramdisk in seconds without
  bootloader interaction. Memory: `project_remote_debug.md`,
  `project_debug_server_system.md`.
- fbconsole as a first-resort debug surface during early bring-up
  (branch `debug/fbconsole`) — visible kernel log on the display when
  the framework can't come up.
- Build chain: MCP-driven remote kernel-build server (Linux box on
  Tailscale). Profile `SmokeR24.1` for 24.1, `Stock` for the reference
  Smoke-kernel-mocha, `los-14.1` for Android 7.1.2 builds.
  Memory: `reference_build_server.md`.
- Mention but don't deep-dive — the article's spine is the camera stack,
  not the build harness.

---

## Article 2 — Userspace HAL

**Repo:** `/Users/artem/Projects/android-camera-hal` (not part of this tree).
**Branch of record:** `master`. Bring-up history split into Tier 1 → Tier 1.5
→ Tier 2 → Tier 3 → 3A refactor; each tier has a memory file.

### Ch. 1 — HAL architecture and why Vulkan-only

- HAL 3 entry points: `camera_module_t`, `camera3_device_t`,
  `processCaptureRequest` lifecycle. We claim
  `CAMERA_DEVICE_API_VERSION_3_4`, hardware level `LIMITED`. Memory:
  `project_hal34_compliance.md`.
- Backend history: started with three backends (CPU `ImageConverter`,
  GLES 3.1 compute, Vulkan compute) toggled by `persist.camera.isp_backend`.
  Tier 1.5 deleted CPU and GLES — Vulkan won on perf and on the only
  zero-copy path that actually worked on the K1 driver. Memory:
  `feedback_scope_first.md`, `project_next_zero_copy.md`.
- The contract surface today: `BACKWARD_COMPATIBLE` only, no RAW / no
  MANUAL_SENSOR / no reprocessing. Why each "no" — what unlocking it
  would cost in metadata and per-frame guarantees.
- The pieces that have a chapter of their own below: zero-copy I/O,
  Vulkan compute (demosaic + ISP shader), async pipeline + threads,
  3A coordinator, JSON tuning per sensor, NEON stats.
- A diagram is worth drawing: V4L2 ring → input DMA-BUF → Vulkan compute
  (demosaic) → mScratchImg → per-output blit (fragment ROP into gralloc /
  YUV encode / JPEG copy) → Camera3 framework with per-output release_fence.

### Ch. 2 — Vulkan compute (demosaic + ISP shader)

- Why Vulkan on Kepler/K1 at all: the only stack on this device where we
  have working compute + working dmabuf import + working release-fence
  semantics (via `VK_KHR_external_*_fd`). GLES couldn't reach gralloc
  zero-copy.
- Driver inventory: NVIDIA Vulkan 1.0 inside `libglcore.so` (~11 MB),
  thin shim `vulkan.tegra.so` (~18 KB) on top. Extensions actually
  exposed to app-level code (from `vkEnumerateDeviceExtensionProperties`
  on the cameraserver):
  - `VK_KHR_external_memory_fd`, `VK_KHR_external_fence_fd`,
    `VK_KHR_external_semaphore_fd` — full chain.
  - `VK_NV_glsl_shader` — accept GLSL source directly. We depend on
    this because SPIR-V on this driver is unstable.
  - `VK_KHR_dedicated_allocation`, `VK_KHR_push_descriptor`,
    `VK_KHR_sampler_ycbcr_conversion`.
- The asymmetry: `VK_ANDROID_native_buffer` is in the driver strings
  but `libvulkan.so` (the Android loader) filters it out of
  user-space queries — it's reserved for the platform HAL. So the
  classic `VkNativeBufferANDROID` import path is unusable from a HAL
  caller. Workaround = OPAQUE_FD over nvmap (next chapter).
  Memory: `reference_tegra_vulkan.md`.
- Demosaic shader (`DemosaicCompute.h`): McGuire / Malvar-He-Cutler 5×5,
  13 samples, branch-free conditional selects on the Bayer pattern.
  Replaces an earlier bilinear baseline. Includes optical-black subtract
  + dynamic-range rescale from the JSON tuning's
  `opticalBlack.manualBiasR`.
- Performance on GK20A (single SMX, 192 CUDA cores, 48 KB shared mem,
  small L1 tex cache, old compiler). Detailed perf findings live in
  `reference_tegra_k1_compute.md`; the article surfaces the key ones:
  - Shared `atomicAdd` on a single hot address is implemented in
    software (CAS loop) on Kepler — replaced by tree-reduce, saved
    ~18 ms on the demosaic during a fusion experiment.
  - Warp-aligned 2D iteration thrashed the ~12 KB L1 tex cache
    (Sobel footprint 20 → 60 rows). Flat-stride layout keeping the
    warp set compact on 2–3 rows wins despite an extra divmod.
  - Integer divmod is ~2–3 cycles on this driver (magic-number
    reciprocal). Do not refactor control flow to "save" it.
  - Fat fused shaders (demosaic + stats in one dispatch) cost
    +27 ms — register pressure + L2 pressure dominate. Separate
    dispatches and temporal subsample win.
  - Win: shared Bayer tile with cooperative load (20×20 halo,
    16×16 workgroup, one barrier). Demosaic 47 ms → 15 ms → 3.4 ms.
- A small detour on color: the NVIDIA `.isp` `ccMatrix` is column-major
  from our POV. Feeding it verbatim into a row-major shader produces
  magenta on neutrals. `SensorTuning::ccmForCctQ10` transposes on write.
  Memory: `reference_ccm_convention.md`. Worth a sidebar — it's a
  failure mode someone else will hit.

### Ch. 3 — Zero-copy I/O (V4L2 → Vulkan → gralloc)

- The input half: `V4L2_MEMORY_DMABUF` capture writes directly into
  Vulkan-allocated `VkBuffer`s. 4-slot ring. `vkGetMemoryFdKHR(OPAQUE_FD)`
  exports fds; HAL hands them to `V4l2Device::setDmaBufFds()`.
- The kernel side of this works only thanks to the nvmap-foreign-dmabuf
  patch (Article 1, Ch. 3). Short cross-reference here.
- The output half: writing into gralloc-backed surfaces. Two paths
  considered, only one works:
  - Compute store into a `VK_ANDROID_native_buffer` image → produces
    layout-swizzled garbage (the tiler isn't engaged). Same for
    `vkCmdCopyImage`/`CopyBufferToImage` targeting a gralloc image.
  - Fragment ROP: sample from `mScratchImg` via `sampler2D` with
    hardware bilinear, push-constant carries `cropXYWH + srcWH +
    outWH`, ROP writes into gralloc. Cleanly engages the tiler.
    Zero-copy zoom and cross-resolution come for free.
- Sync model:
  - Framework `acquire_fence` sync_fds → imported as binary
    `VkSemaphore` via `VK_KHR_external_semaphore_fd` and waited on
    by the GPU submit.
  - Per-output `release_fence` exported via
    `vkQueueSignalReleaseImageANDROID` and returned to the
    framework.
- `VK_ANDROID_native_buffer` cache trap (a "found this the hard way"
  sidebar): Camera2 / nvgralloc reuses the same `native_handle_t*`
  across different underlying allocations. A pointer-keyed `VkImage`
  cache silently renders into freed memory — symptom is alternating
  black-and-rendered frames at HAL frame rate. Fix: validate every
  cache hit with FNV-1a of the full handle data array +
  `fstat(handle->data[0]).st_ino`. Memory:
  `reference_anb_handle_reuse.md`.
- NV gralloc layout quirk worth mentioning:
  `GRALLOC_USAGE_HW_VIDEO_ENCODER` alone selects tiled NV12 with
  height padded to the next 1024 (1080 → 2048, ~12 MB/buf). Adding
  `SW_WRITE_OFTEN` forces linear (~3 MB/buf) and NVENC still reads it.
  Memory: `reference_tegra_gralloc.md`.

### Ch. 4 — Async pipeline (Tier 3)

- Pre-Tier-3 shape: everything ran on the binder thread —
  `processCaptureRequest` blocked on GPU drain + V4L2 DQBUF.
- Tier 3 split (multiple PRs, summarised here — the memory file
  `project_tier3_progress.md` has the chronology):
  - `RequestThread` — deep-copies request into `PipelineContext`,
    returns from `processCaptureRequest` in < 1 ms.
  - `CaptureThread` (under `BayerSource` / `V4l2Source`) — sole
    owner of V4L2 lifecycle. Build-once infrastructure across
    `configureStreams` calls when sensor mode is unchanged.
  - `PipelineThread` — owns the GPU dispatch, fence-fd `poll()`,
    `StatsProcessStage`, `ResultDispatchStage`.
  - `ResultThread` — owns Camera3 dispatch + tracker remove +
    `JpegWorker` FIFO gate. Final dispatch can hold while a BLOB is
    being encoded, without stalling sensor frames.
  - `JpegWorker` — async libjpeg encode. `PipelineContext.jpegPending`
    (atomic) is the FIFO gate. Encoded BLOB lands as the final partial
    result.
- The "produce-once / sample-many" invariant (PR 7): one demosaic per
  frame into `mScratchImg`; per-output blits/encodes/copies share a
  single `vkQueueSubmit`. Per-output `release_fence`s come from
  `vkQueueSignalReleaseImageANDROID` against the same queue.
- Start/stop ordering (this is genuinely tricky and a likely sidebar):
  - `stopWorkers()`: BayerSource → RequestThread → StatsWorker →
    PipelineThread → JpegWorker → ResultThread.
  - `startWorkers()`: BayerSource → StatsWorker → ResultThread →
    JpegWorker → PipelineThread → RequestThread.
  - Reasoning: each downstream consumer must be live before its
    producer pushes.

### Ch. 5 — 3A (`Ipa3A` coordinator + AE / AWB / AF)

- Architecture: `Ipa3A` owns three pure-return-style controllers
  (`AutoExposureController`, `AutoWhiteBalanceController`,
  `AutoFocusController`). Each takes typed input, returns a typed
  `*Result` POD, never reaches into collaborators. `Ipa3A` runs
  gating, dispatches AWB → AE → AF, routes results to backends
  (`IspPipeline::setWbGains` for AWB, `DelayedControls` for AE,
  `V4l2Device` focuser for AF).
- Partial-result emission: AWB = counter 1, AE = counter 2,
  final (output buffers + AF + base metadata) = counter 3.
  `PARTIAL_RESULT_COUNT = 3`. The framework union-merges, only the
  final partial carries `output_buffers`.
- AE: EV-space single-pole LPF (replaces an old multiplier-space
  cascade that wound up over setpoint crossings). Two-candidate ratio
  per tick — mean target vs highlight cap (IQM top 2 % of post-WB
  max-of-channels). 2 % absolute dead-band. Optional asymmetric LPF
  speed near setpoint (`close_speed_zone`), per-sensor knob.
  Manual / cold-start V4L2 triple computed by the static helper
  `AutoExposureController::parseManualSettings`. Memory:
  `project_tier3_progress.md` (AE section).
- AWB: two impls behind one interface (`Awb.h`).
  - `GrayWorldAwbController` (OV5693): gray-world over a 16×16
    `rgbMean` patch grid, 96-valid-patch confidence gate, symmetric
    EMA-relax to prior on gate failure.
  - `BayesianAwbController` (IMX179): RPi-style coarseSearch over
    calibrated `ctCurveR / ctCurveB` PWLs, lux-conditioned prior,
    `deltaLimit` per-zone clamp, `biasCT` synthetic anchor,
    off-curve fineSearch capped by `transversePos/transverseNeg`,
    IIR damping with cold-start snap. Manual presets via
    `bayes.modes[]`.
  - Calibration source on IMX179: `awb.v4.FusionLights` from the
    NVIDIA `.isp` (eight inner CT points 3500–5350 K, 2700 K /
    7000 K endpoints extrapolated). Pending: a real grey-card session.
  - Selection: `AwbFactory::createAwb` reads `active.awb.algorithm`
    from the per-sensor JSON. Misconfigured Bayes (`algorithm=bayes`
    but no `bayes` block) → `SensorTuning::load` flips back to gray-
    world; the factory logs a warning.
  - Memory: `project_awb_design.md`, `docs/awb-bayes.md` in the HAL.
- AF: CDAF coarse-fine. NEON `Σ(Gx² + Gy²) / Σ I²` per patch (sliding
  window optimisation tried twice on ARMv7 NEON and regressed — see
  `reference_tegra_k1_compute.md` for the register-budget reasoning).
  State machine `Idle → Coarse1 → [Coarse2] → Fine → Settle → Idle`.
  Continuous AF: focusMetric + centre 8×8 RGB-mean as scene-change
  gate, AE-converged precondition, 30-frame cooldown, panning lockout.
  AF reference projects when designing: `raspberrypi/libcamera`
  (gold), `libcamera/ipu3` (minimal CDAF), `intel/ipu6-camera-hal`
  (HAL-glue patterns only — closed algorithm). LineageOS is a known
  dead end. Memory: `reference_af_projects.md`.

### Ch. 6 — JSON tuning per sensor

- The premise: don't hard-code anything sensor-specific into the HAL.
  Every tunable lives under `/vendor/etc/camera/tuning/
  <lower(sensor)>_<lower(integrator)>.json`. Adding a new module = drop
  a JSON, no code change.
- Converter: `tools/isp_to_json.py` parses NVIDIA's `.isp` syntax
  (namespaced `path[i].sub = value;`, multi-line tuples, split lines).
  Splits output into `active` (paths the HAL consumes) and `reserved`
  (everything else, preserved 1:1 for future stages).
- Module-side data (physical size, focal length, min focus distance,
  bayer pattern, orientation) comes from auxiliary
  `tuning/_module_<sensor>_<integrator>.json` files — those facts
  live in the sensor datasheet and DT, not the `.isp`.
- HAL-specific knobs that don't exist in the `.isp` live in
  `active.hal_overrides` of the same per-module JSON
  (`ae.close_speed_zone` today). Preserve-verbatim across `.isp`
  regenerations.
- Consumer surface today:
  - `AutoFocusController` reads VCM positions, offsets, settle frames,
    sweep step / contrast / retrigger ratios from `af.*`. No compile-
    time fallbacks; missing key → branch disables itself
    (`feedback_no_silent_fallbacks` rule).
  - `SensorTuning::ccmForCctQ10` picks the nearest-CCT matrix from
    `colorCorrection.Set[]`. Pinned to 5000 K until AWB CCT estimate.
  - `DemosaicCompute.h` reads optical-black subtract from
    `opticalBlack.manualBiasR`.
  - `CameraStaticMetadata` reads `ANDROID_SENSOR_INFO_PHYSICAL_SIZE`,
    focal length, min focus distance from `module.*`.
- Reserved / not yet consumed (legitimate YAGNI): noise reduction
  v2/v6, lens shading, tone curves v2, sharpness filters, full AE
  (VFR tables, SmartTarget, MeanAlg), flicker, full AWB LUTs. Memory:
  `project_tier2_tuning.md`.

### Ch. 7 — NEON stats (a contrarian aside that shipped)

- Setup: Tier 3 needs per-frame stats (histogram + Sobel + RGB means
  on a patch grid). First attempt = GPU compute, fused into demosaic.
  Cost: +27 ms / frame on the K1.
- The pivot: a vectorised NEON pass over the raw Bayer slot produces
  the entire `IpaStats` layout (128-bin green-channel histogram +
  16×16 rgbMean + 16×16 Tenengrad) in ~8 ms on 720p — `vld2q_u16` +
  `vpadalq` + ±2-step same-parity Sobel lanes. Demosaic kept on the
  GPU; stats moved to CPU. Memory: `reference_tegra_k1_compute.md`
  ("CPU NEON" section).
- `StatsWorker` runs across `phaseCount = 2` submits to halve peak
  CPU per frame (~4 ms). The design deadline becomes
  `phaseCount × frame_period`. Trade-off: `phaseCount = 1` → fresh
  every frame, more CPU; higher → less CPU, slower stats.
- Sliding-window Sobel optimisation regressed twice on ARMv7 NEON.
  Worth a paragraph in the article — the failure is instructive:
  16 Q-regs total, the sliding ring forced 17 persistent across
  iterations, compiler spilled, latency doubled. Persistent vs
  transient register budget is the lesson, not raw load count.
- Spatial restrict (Sobel + greenSq computed only over the centre
  8×8 AF patches) gave the budget back: -75 % of Sobel work, no
  register-pressure regression. AF reads only the centre patches
  anyway.

### Ch. 8 — HAL 3.4 compliance (closing chapter)

- What we claim today: `CAMERA_DEVICE_API_VERSION_3_4`, hardware level
  `LIMITED`, `BACKWARD_COMPATIBLE` capability, no MANUAL_SENSOR /
  MANUAL_POST_PROCESSING / RAW / BURST_CAPTURE / REPROCESSING /
  DEPTH_OUTPUT / CONSTRAINED_HIGH_SPEED_VIDEO.
- Stream caps: PROCESSED = 2 (RGBA preview + YUV video), PROCESSED_STALLING
  = 1 (BLOB JPEG), RAW = 0, INPUT_STREAMS = 0.
- 3A contract: AE state honest (`INACTIVE / SEARCHING / CONVERGED /
  LOCKED`). AE_LOCK preserves converged target; EV compensation applies
  on both auto and locked paths. AF_REGIONS parsed into a patch grid
  `FocusRoi` (≥5×5 around the tap centre); same ROI drives NEON stats.
- What each "no" would cost to lift (short discussion):
  - RAW = mandatory `BLACK_LEVEL_PATTERN`, `WHITE_LEVEL`, color
    transforms, calibration matrices, NOISE_PROFILE + a RAW16 stream
    producer. Dropped — DNG isn't a use case worth the cost on mocha.
  - Reprocessing (`PRIVATE_REPROCESSING` / `YUV_REPROCESSING`) — ZSL
    attempt on 2026-04-26 was dropped (broke MediaRecorder thumbnail
    callback path; proper fix lives in V4L2 + ISP downscale path,
    not the HAL).
  - `MANUAL_SENSOR` — pre-capture-trigger contract + per-frame sensor
    settings round-trip not implemented.
- Open compliance gaps that aren't blocking apps: V4L2 fd reopened on
  every openDevice, NV21 output unverified, the persistent vertical
  seam at low exposures (Tegra/V4L2 hardware quirk).
- Memory: `project_hal34_compliance.md`.

### Closing — perf snapshot at the time of writing

- 1080p single-stream preview: ~20 fps, GPU-drain bound on
  `waitForPreviousFrame`. PR 4 (fence-fd in `poll()`) should collapse
  `wait` from ~55 ms to <10 ms and push to 28–30 fps.
- 720p multi-stream (preview + stats): GPU 4 ms (demosaic + blit),
  NEON stats 4 ms parallel, PERF post 7–14 ms steady, ~88 fps without
  vsync cap.
- Memory: `reference_perf_log.md` for the PERF log format and what
  each segment measures.

---

## Cross-cutting notes (apply to both articles)

- Cite memory files by name where appropriate so the working notes
  stay traceable. Final prose should not directly include
  memory-file URLs — translate the findings into article-style
  text, but keep the file pointer for the editor pass.
- Code citations: `path/to/file.cpp:line_number` form when discussing
  a specific function.
- Commit references: short SHA + branch when discussing a specific
  fix (e.g. "commit `38adbd9` on `master`").
- Avoid claiming the code currently matches the memory — memories are
  point-in-time observations. Verify against the current tree before
  asserting any file/line/function fact in published prose.
- Tegra-specific terminology: VI, CSI, ISP, nvmap, gralloc — all
  worth a one-sentence definition on first use; readers from outside
  the Tegra world will follow the article better.
