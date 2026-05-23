# Bump tinyhal to audio HAL API 3.0 + proper FM AudioPatch

Status: planned, not started. Larger refactor than [per-device volume](per-device-volume.md); do it after the per-device volume work lands.

## Context

The in-tree mocha tinyhal declares `AUDIO_DEVICE_API_VERSION_2_0` in `adev_open()`. API 3.0 added four new entry points on `struct audio_hw_device` that AudioFlinger relies on for **codec-to-codec hardware audio paths** — paths where audio flows entirely inside the codec from an input port to an output port without ever passing through AudioFlinger's stream mixer:

- `create_audio_patch(dev, num_sources, sources[], num_sinks, sinks[], *handle)`
- `release_audio_patch(dev, handle)`
- `get_audio_port(dev, *port)`
- `set_audio_port_config(dev, *config)`

FM radio on mocha is exactly that kind of path. BCM4354 → RT5671 AIF4 → DAC1 → speaker amps. No PCM stream is open. AudioFlinger never sees the FM audio, never knows the current FM volume, never calls `out_set_volume()` for FM. Same story for any other future FM-style path (analog FM or DAB on a different chip, a DSP echo loopback, BT SCO codec-to-codec routing, etc).

The current workaround for FM is the `fm_volume` setParameters side-channel: FmService reads `STREAM_MUSIC` and forwards into the HAL via `AudioManager.setParameters("fm_volume=X")`, which adev_set_parameters intercepts and turns into a `DAC1 Playback Volume` write. It works but it's structurally wrong:

- App has to know about the codec-side volume knob.
- The side-channel is bypass for AudioFlinger and isn't visible in `dumpsys audio` / patches.
- Other Android audio framework features that expect AudioPatches (force-use, audio focus ducking effects targeting the route, AudioPortConfig query) don't see FM at all.
- Volume re-sync logic in `FmService` (postDelayed + onAudioPatchListUpdate) is reinventing what AudioFlinger would do for free with a real AudioPatch.

Stock NV's `nvaudio.primary.tegra.so` (the closed source HAL we replaced) shipped API 3.0 patch support and used it. QCom's `hardware/qcom/audio` has full API 3.0. Amlogic, Allwinner, MediaTek HALs all have it. We're an outlier.

## Goal

Implement API 3.0 on tinyhal so that FM (and any future codec-internal route) can be modelled as a proper AudioPatch:

```
AudioPatch:
  sources: [ AudioDevicePort { type: DEVICE_IN_FM_TUNER, ext_device: ... } ]
  sinks:   [ AudioDevicePort { type: DEVICE_OUT_SPEAKER  | DEVICE_OUT_WIRED_HEADPHONE, ext_device: ... } ]
```

When AudioFlinger creates this patch, the HAL applies the FM mixer route on the codec (the work we already do today via the `fm_in` device path), and AudioFlinger thereafter calls `out_set_volume()` on the patch's sink port whenever STREAM_MUSIC volume changes. Per-device volume mapping (see [per-device-volume.md](per-device-volume.md)) means the right codec mixer ctl is written automatically.

Net effect: delete the `fm_volume` setParameters bridge in audio_hw.c, delete the `<stream name="fm_volume">` named hw stream in audio.mocha.xml, delete `forwardFmVolumeToHal()` + receiver + delayed/onPatchListUpdate hooks in FmService, delete the `audio_policy.conf` AUDIO_DEVICE_IN_FM_TUNER attached_input_devices entry (or restructure how it surfaces ports), and rely on the framework's patch + per-device volume.

## Plan

This is a real refactor, not a small change. Outline:

### Phase 1: bump version, stub patch API

- In `device/xiaomi/mocha/audio/Android.mk` (or wherever the HAL gets its CFLAGS): add `-DAUDIO_DEVICE_API_VERSION_3_0`.
- In `device/xiaomi/mocha/audio/audio_hw.c::adev_open()`:
    - Change `version` from `AUDIO_DEVICE_API_VERSION_2_0` to `..._3_0`.
    - Wire up the four new ops to stub functions that return `-ENOSYS`.
- Verify the build still produces a working `audio.primary.tegra.so` and AudioFlinger still loads it (`dumpsys audio` shows `module 10` etc). FM works on the old side-channel exactly as before — nothing yet uses the new ops.

### Phase 2: implement get_audio_port + set_audio_port_config

These tell AudioFlinger which audio device ports the HAL knows about and let it query/set their format. Both need to enumerate from our `audio_policy.conf` device list and from device_table[] in configmgr. Probably:

- `get_audio_port(port)` — fill in `port->type` (DEVICE), `port->id`, `port->name` from our device_table; `port->ext.device.type` from the AUDIO_DEVICE_OUT_* / IN_* mask; supported sample rates/formats/channels from the corresponding `<stream>` in audio.mocha.xml.
- `set_audio_port_config(config)` — apply gain etc. For now, just succeed without doing much if the requested config matches what we already have.

### Phase 3: implement create_audio_patch + release_audio_patch

This is the meat. When AudioFlinger asks for a patch (e.g., FM_TUNER → SPEAKER):

- Validate the sources/sinks are device ports we know about (FM_TUNER as a source, SPEAKER as a sink).
- Apply the source device's mixer path (= the existing `fm_in` `<path name="on">`).
- Apply the sink device's mixer path (= the existing `<device name="speaker">` `<path name="on">`, including its per-device volume mapping from [per-device-volume.md](per-device-volume.md)).
- Assign a patch handle (just an incrementing int wrapped in a struct), store the source/sink ids and applied paths against that handle in an adev-scoped map.
- Return the handle.

`release_audio_patch(handle)`:
- Look up the patch.
- Apply the source device's `<path name="off">` (where it exists) and the sink device's `<path name="off">`.
- Free the entry.

Per-device volume changes via `out_set_volume()` of the sink port will then naturally drive the codec hardware volume on FM playback. The fm_in path's tear-down handles the FM-specific mixer ctls (DAC1 L/R Mux, Mono mix L1/R1, FM Switch) just like it does today.

### Phase 4: switch FmService to AudioPatch and remove the side-channel

In `arttttt/android_packages_apps_FMRadio`:

- Update `createAudioPatch()`: source `AudioDevicePort` type=DEVICE_IN_FM_TUNER, sink accepts BOTH DEVICE_OUT_SPEAKER and DEVICE_OUT_WIRED_HEADPHONE/HEADSET. Currently we only accept the wired headset/headphone, which limits the AudioPatch to headphones-only.
- Update `isPatchMixerToEarphone()` to recognize a valid sink for either speaker or headphone — currently it only counts the wired sinks.
- Remove `forwardFmVolumeToHal()`, `mVolumeReceiver`, `registerVolumeChangedReceiver()`, etc. AudioFlinger now writes the codec via `out_set_volume()` on the patch sink.

In `device/xiaomi/mocha/audio/audio_hw.c`:

- Remove the `fm_volume` key handling in `adev_set_parameters()`.

In `device/xiaomi/mocha/audio/audio.mocha.xml`:

- Remove the `<stream name="fm_volume" type="hw">` block.

In `device/xiaomi/mocha/overlay/packages/apps/FMRadio/res/values/config.xml` (new):

- Override `config_useSoftwareRenderingForAudio` to `false`, so the app prefers AudioPatch over software render. (Today the upstream FMRadio sets it to `true`, forcing software render, which is why we needed the side-channel in the first place.)

### Phase 5: tests + cleanup

- All FM scenarios from [project_fm_radio memory](../../.../memory/project_fm_radio.md) — single power-up, source switch, headset insert/remove during playback, volume keys from system bar, BT toggle while FM is up, etc.
- AudioFlinger patch list (`dumpsys media.audio_flinger -p`) should now show the FM patch.
- Verify `out_set_volume()` is invoked on volume changes (logcat tinyhal at V level).

## Risks / unknowns

- API 3.0 also changed `adev_open_output_stream` / `adev_open_input_stream` signatures (added `address` param). Need to handle both call sites. Existing `#ifdef AUDIO_DEVICE_API_VERSION_3_0` blocks at audio_hw.c lines 627, 1460, 2347, 2437 hint at this — they cover the trivial parts (frame_size accessor, address arg) but not the patch API.
- The `audio_patch_handle_t` lifecycle. AudioFlinger may release a patch handle the HAL never created if a session crashes. Probably need to be lenient on release of unknown handles.
- AudioFlinger uses `set_audio_port_config()` for gain settings on individual ports (per-port gain, separate from stream volume). Our codec doesn't really have per-port gain controls in that model — each path has its own mixer chain. May need to map port-config-gain to the per-device volume ctls we declared. Or just return success and ignore.
- Once we get rid of `fm_volume` setParameters, the per-device music volume (problem A) is no longer optional — without it, FM volume via AudioPatch would also be software-scaled, defeating the purpose. So [per-device-volume.md](per-device-volume.md) is a hard prerequisite of phases 3+.
- The `config_useSoftwareRenderingForAudio` flag in FMRadio's overlay change may need additional FmService cleanup if there are codepaths that assume software render is always the fallback. Read `FmService.startPatchOrRender` carefully before flipping.

## Out of scope

- Replacing the rest of FmService's audio behaviour (audio focus, mute, etc) — those are orthogonal.
- AudioPatches with mixer sources (i.e. patches from an AudioMixPort, not a DEVICE port). AudioFlinger uses these for stream → device routing internally; we don't need to handle them differently from API 2.0 behaviour because AudioFlinger will still call `adev_open_output_stream` for those.

## Related memory

- `project_fm_radio.md` — current FM bridge architecture and the things this work would simplify.
- `feedback_fm_volume_route_settle.md` — the timing logic this work makes obsolete.
- `feedback_tinyhal_stereo_volume_index.md` — same `index="0"`/`"1"` channel mapping rule applies after the refactor.
