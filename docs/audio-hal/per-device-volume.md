# Per-device hardware volume in tinyhal (Problem A)

Status: planned, not started.

## Context

Mocha ships a Wolfson/Cirrus-derived **tinyhal** as `audio.primary.tegra.so`. The HAL is the standard Android `audio_hw_device` (API 2.0) for Tegra+RT5671 with TFA9890 stereo speaker amps. tinyhal already exposes `out_set_volume(stream, l, r)` and `set_hw_volume(stream, l_pc, r_pc)`, and it writes any mixer controls declared as `<ctl function="leftvol|rightvol">` inside a `<stream>` tag in `audio.mocha.xml`.

Today **no `<stream>` declares those**. As a result:

- `out_set_volume()` is invoked but `set_hw_volume()` finds no mapping and no-ops.
- AudioFlinger detects that the HAL didn't do hardware attenuation and falls back to **software scaling** of the PCM samples before they reach the HAL output stream.
- Music playback still has working volume — just in software, ahead of the codec.

This works fine for music, but the architecture has two real limitations:

1. The same `<stream>` would map to one set of volume controls no matter which device is currently routed (speaker vs wired headphone). Mocha has very different physical paths — speaker goes via DAC2 → Mono DAC mix → TFA9890 stereo amp, headphone goes via DAC1 + DAC2 → HPO MIX → headphone jack. Each has its own preferred attenuation knob: `DAC2 Playback Volume` (0–175) for speaker, `HP Playback Volume` (0–39) for headphone.
2. Software scaling burns CPU on a constrained device and degrades signal-to-noise compared to hardware attenuation on a TLV-curve mixer ctl.

Independently, the FM volume bridge we built (`fm_volume` setParameters side-channel writing `DAC1 Playback Volume`) does its own per-channel mixer write. That precedent shows we already know how to drive the codec hardware volume; we just don't drive it for normal music playback.

## Goal

Move STREAM_MUSIC attenuation off AudioFlinger's software scaler and onto codec mixer controls, **picking the right control based on the currently routed device**. Music volume continues to behave identically from the user's point of view; it just runs on the codec's TLV-curved hardware volume.

## Plan

Extend tinyhal config to support `<volume>` mappings inside each `<device>` block, keyed by device direction:

```xml
<device name="speaker">
    <volume function="leftvol"  name="DAC2 Playback Volume" index="0"/>
    <volume function="rightvol" name="DAC2 Playback Volume" index="1"/>
    <path name="on"> ... </path>
</device>

<device name="headphone">
    <volume function="leftvol"  name="HP Playback Volume" index="0"/>
    <volume function="rightvol" name="HP Playback Volume" index="1"/>
    <path name="on"> ... </path>
</device>
```

Then make `set_hw_volume(stream, l_pc, r_pc)` look up the active output device for that stream and write to that device's volume control(s). If the device has no `<volume>` declared, fall through to the existing stream-level `<ctl function="leftvol|rightvol">` mapping (so existing behaviour is preserved for any stream where per-device mapping doesn't make sense — e.g., the `fm_volume` named stream we already use).

### Files to touch

In `device/xiaomi/mocha/configmgr/audio_config.c`:

- `struct device` (around `parse_device_start`) — add `struct stream_control volume_left, volume_right`, default to `{ .id = UINT_MAX }`.
- `elem_table[]` — allow `e_elem_stream_ctl` as a valid subelement of `e_elem_device` (it currently only nests under `e_elem_stream`).
- `parse_stream_ctl_start()` — when inside a `<device>` context, write to the device's `volume_left`/`volume_right` instead of the stream's. Currently the function unconditionally uses `state->current.stream->controls.volume_{left,right}`; needs a branch on `state->current.elem`.
- `set_hw_volume(stream, l_pc, r_pc)` — before doing the existing stream-level writes, iterate `stream->current_devices`, find each device's `<volume>` (if any), and call `set_vol_ctl()` on it. Per-channel index handling (left → device->volume_left, right → device->volume_right) is the same as the existing stream-level branch.

In `device/xiaomi/mocha/audio/audio.mocha.xml`:

- Add `<volume function="leftvol" name="DAC2 Playback Volume" index="0"/>` and the right-channel counterpart inside `<device name="speaker">`. Same for `<device name="headphone">` with `HP Playback Volume`. Range attributes can be omitted — `set_vol_ctl()` already reads the codec's TLV min/max if `min`/`max` are absent.
- `<device name="headset">` shares the headphone jack so it should reuse the same `HP Playback Volume` mapping.

In `device/xiaomi/mocha/audio/audio_hw.c`:

- No behavioural change. `out_set_volume()` already calls `set_hw_volume()` — that callee is what we're extending.

### Tests

After landing:

1. Music to speaker — volume keys should write `DAC2 Playback Volume`. Verify with `tinymix_mocha -D 1 get` for the ctl between keypresses.
2. Music to wired headphone — volume keys should write `HP Playback Volume`. Verify same way.
3. AudioFlinger software attenuation should drop — `dumpsys media.audio_flinger` should no longer show the per-track `L dB`/`R dB` columns lowering on volume change for STREAM_MUSIC.
4. FM volume bridge keeps working — the `fm_volume` named hw stream's mapping is unaffected.
5. Headset insert/remove during music playback — volume control should follow the active device's mapping.

## Risks / unknowns

- Volume curve change. AudioFlinger's software scaler is a linear PCM multiplier (after `audio_track_cblk` scaling). Codec hardware volume on RT5671 is TLV-curved (logarithmic). User-perceived steps will look different at the same UI position. Probably fine but worth listening to.
- AudioFlinger may still apply some software scaling on top (it can if `HAL doesn't fully cover the range`). Need to verify the HAL returns the correct values from `out_set_volume()` and that AudioFlinger believes the hardware did the work.
- Volume on dual-output routing (speaker + a2dp dual_audio). When both devices are active, which mapping wins? Probably need to apply both. Easy with the iteration-over-`current_devices` design.
- Headphone has DAC1 in its path (via `HPO MIX DAC1 Switch`), and DAC1 also carries FM. If FM is playing on the speaker via DAC1 and the user plugs in headphones, the HP Playback Volume mapping applies on top. Need to make sure that doesn't double-attenuate the FM signal. May want a special-case: when FM HW direct is active, the per-device volume should track headphone path; the `fm_volume` setParameters is what carries the master FM gain. Worth thinking through.

## Out of scope

- A2DP, USB audio HALs are separate HAL modules; not affected by this work.
- AudioFlinger / framework changes are unnecessary — the entire fix lives in tinyhal + XML.
- This change does NOT remove the `fm_volume` setParameters side-channel; that stays as the FM-specific bridge until the API 3.0 / AudioPatch work lands. See [api-3.0-audio-patch.md](api-3.0-audio-patch.md).

## Related memory

- `feedback_tinyhal_stereo_volume_index.md` — already learned: `SOC_DOUBLE_TLV` codec controls need explicit `index="0"`/`index="1"` per channel.
- `feedback_fm_volume_route_settle.md` — re-sync timing pattern that applies whenever the route changes.
- `project_fm_radio.md` — `fm_volume` bridge is the existing precedent for codec-side volume writes.
