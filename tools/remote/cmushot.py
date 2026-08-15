#!/usr/bin/env python3
"""The screenshot the panel actually shows.

screencap captures the composed frame BEFORE the display controller's
colour pipeline -- the CMU sits on the way to the panel, past anything
readable -- so an ordinary screenshot never shows night light or any other
colour correction this hardware applies. There is no hardware path around
that on this controller: the CMU's output goes only to DSI.

But the pipeline's arithmetic is known exactly (LUT1 256x12bit degamma,
3x3 CSC in signed Q1.8, LUT2 with the proven two-segment indexing:
v<512 direct, then step 8), so this script reads the LIVE tables out of
the hardware and applies them to the screencap byte for byte. The output
PNG is what the panel shows, and differences can be judged numerically
instead of by eye.

One blind spot: on a scene that has sat still for a second, the
flattener hands the whole frame to the GPU and SurfaceFlinger bakes any
active colour transform into it with a shader -- the matrix leaves the
hardware (the composer restores the boot state) and screencap misses
the GL tint too. Identity tables under an active correction therefore
mean "capture during a live scene": touch the screen and shoot again.

Usage: cmushot.py [out.png]     (device via adb; needs cmutest on it)
"""
import subprocess
import sys
import io

import numpy as np
from PIL import Image

CMUTEST = "/data/local/tmp/cmutest"


def adb(*args, binary=False):
    r = subprocess.run(["adb", *args], capture_output=True, check=True)
    return r.stdout if binary else r.stdout.decode()


def read_pipeline():
    vals = [int(x) for x in
            adb("shell", CMUTEST, "dump").split()]
    if len(vals) != 1 + 9 + 256 + 960:
        raise SystemExit(f"unexpected dump length {len(vals)}")
    enable = vals[0]
    csc = np.array(vals[1:10], dtype=np.int32)
    # Q1.8, ten bits, two's complement: the upper half of the raw
    # encoding is negative.
    csc = np.where(csc > 511, csc - 1024, csc)
    lut1 = np.array(vals[10:266], dtype=np.int32)
    lut2 = np.array(vals[266:1226], dtype=np.int32)
    return enable, csc, lut1, lut2


def lut2_index(v12):
    """The hardware's two-segment map from a 12-bit value to a slot."""
    return np.where(v12 < 512, v12, 512 + ((v12 - 512) >> 3))


def apply_pipeline(rgb, csc, lut1, lut2):
    lin = lut1[rgb]  # (h, w, 3) of 12-bit linear light

    m = csc.reshape(3, 3)  # rows: R, G, B outputs; k{in}{out} order
    out = np.einsum('ij,hwj->hwi', m, lin.astype(np.int64)) >> 8
    out = np.clip(out, 0, 4095).astype(np.int32)

    return lut2[lut2_index(out)].astype(np.uint8)


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "cmushot.png"

    shot = Image.open(io.BytesIO(
        adb("exec-out", "screencap", "-p", binary=True))).convert("RGB")
    rgb = np.asarray(shot)

    enable, csc, lut1, lut2 = read_pipeline()
    if not enable:
        print("CMU disabled: the screencap already is what the panel shows")
        shot.save(out_path)
        return

    identity = np.array_equal(csc, [256, 0, 0, 0, 256, 0, 0, 0, 256])
    if identity:
        print("note: identity matrix -- if a correction is active, it is "
              "living in the GL-flattened static frame right now; touch "
              "the screen and capture again to see the hardware tint")

    shown = apply_pipeline(rgb, csc, lut1, lut2)
    Image.fromarray(shown).save(out_path)

    delta = shown.astype(np.int32) - rgb.astype(np.int32)
    print(f"csc rows: {csc.reshape(3, 3).tolist()}")
    for ch, name in enumerate("RGB"):
        d = delta[..., ch]
        print(f"{name}: mean {d.mean():+.2f}  min {d.min():+d} "
              f"max {d.max():+d}")
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
