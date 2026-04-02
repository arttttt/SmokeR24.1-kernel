# Stock MIUI FM Radio Dump — Xiaomi Mi Pad 1 (mocha)

Captured 2026-04-03 from stock MIUI KitKat (Android 4.4.4) with working FM radio.

## Files

| File | Description |
|------|-------------|
| `BCM4350C0.hcd` | BT/FM patchram firmware (41KB). BCM4354 uses BCM4350 C0 silicon revision |
| `bluetooth.default.so` | Broadcom Bluedroid HAL with full FM stack (BTA FM). From `/system/lib/hw/` |
| `libbt-vendor.so` | BT vendor HAL. From `/vendor/lib/` |
| `com.broadcom.bt.jar` | Broadcom FM framework JAR. From `/system/framework/` |
| `FM.apk` | MIUI FM Radio app (`com.miui.fm`). From `/system/app/` |
| `bt_vendor.conf` | BT vendor config: UART `/dev/ttyTHS2`, firmware path `/etc/firmware/` |
| `bt_stack.conf` | BT stack config with trace levels |
| `bt_did.conf` | BT Device ID config |
| `fm_logcat_full.txt` | Full logcat capture of FM radio enable + tune session |
| `bluetooth-default-so-fm-symbols.txt` | Extracted FM function symbols from bluetooth.default.so |
| `system-properties.txt` | BT/FM related system properties |

## Stock FM Architecture

```
com.miui.fm/FmActivity (PID 4197, uid 10040)
  ↓ bind (FmLocalService in :remote process, PID 4213)
com.broadcom.bt.jar / FmProxy
  ↓ bind to bluetooth process
com.android.bluetooth / FmService (PID 4040, uid 1002 = bluetooth)
  ↓ JNI (FmServiceJni)
bluetooth.default.so (Broadcom BTA FM stack)
  ↓ HCI VSC 0xFC15
libbt-vendor.so → /dev/ttyTHS2 (UART) → BCM4354 firmware
  ↓ I2S audio (direct, not through Tegra AHUB)
RT5671 AIF4 → headphones/speakers
```

## FM Init Sequence (from logcat)

1. `FmActivity.onCreate()` → starts `FmLocalService` in `:remote` process
2. `FmProxy` binds to `com.broadcom.fm.fmreceiver.FmService` (lives in `com.android.bluetooth`)
3. `turnFmOn(8850)` — 88.50 MHz default frequency
4. `FM_CHIP_ON` → `processEnableChip()` → `RADIO_STATE_CHANGED state=14`
5. `FM_ON` → `bluedroid get_fm_interface` → `enableFmNative(functionalityMask=0)`
6. `FM_SET_AUDIO_PATH(1)` → `setAudioPathNative(audioPath=1)` (headphones)
7. `FM_SET_VOLUME(0→187→51)` — volume ramp
8. `FM_TUNE_RADIO` → `tuneNative(freq=8850)` → result: freq=8850, rssi=124, snr=2
9. `nvaudio_fm: Set fm volume to 51=256x0.2`

## Key Observations

- FM lives inside `com.android.bluetooth` process (not a separate service)
- FM works WITHOUT enabling BT from UI — `RADIO_STATE_CHANGED state=14` activates radio part of chip independently
- No `/dev/fm*` or `/dev/radio*` device nodes — purely userspace HCI
- Audio routed through NVIDIA audio HAL `nvaudio_fm` module (volume control via `ro.audio.fm_scale`)
- `bta_path=2` for FM volume = `FM_AUDIO_I2S` path
- RSSI=124, SNR=2 at 88.50 MHz — chip is receiving
