"""Checks the decode on a synthetic velocity grid of WPO-like values.

World-position-offset motion (wind-animated foliage) is not camera motion, and
no image-matching reference here measures it reliably. The encode does not
care where motion came from, so this tests the decode on such values directly.

`mv_testhost.exe --wpo-encode-test <dir>` renders a known (Vx, Vy, Vz) grid
through the real `EncodeVelocityToTexture()` / `VELOCITY_ENCODE_DEPTH` path
(testhost/src/wpo_encode_test.cpp) and writes the texture plus a meta.txt
giving its layout. This script regenerates the grid and runs the mvtools
decode on the texture. The grid size comes from meta.txt; the value ranges
below must match the shader's by hand. It checks:

  1. The decoded value is close to the true value (informative: bounded by
     UNORM16 quantization).
  2. The decoded value matches decode(round_to_unorm16(encode(true value))) to
     near float32 precision, apart from channel 3's bottom bit, which holds
     bHasPixelAnimation. This is the pass/fail gate: a decode bug away from the
     near-zero values real captures mostly hold, or a GPU rounding differently
     from round-to-nearest, shows up here.

Usage:  python wpo_synthetic_test.py [dump_dir]
"""
import os
import sys

import numpy as np

import mvtools
import paths

dump_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(paths.DUMPS, "wpo_test_out")

# Must match the lerp ranges in testhost/src/wpo_encode_test.cpp's PSMain;
# nothing checks that they agree. They stay inside the encoding's |V| = 2.008
# ceiling, so nothing saturates and the np.clip() calls below are no-ops.
VX_RANGE = (-2.0, 2.0)
VY_RANGE = (-2.0, 2.0)
VZ_RANGE = (-1e-5, 1e-5)

vel_path = os.path.join(dump_dir, "wpo_vel_00000.bin")
meta_path = os.path.join(dump_dir, "meta.txt")
if not (os.path.exists(vel_path) and os.path.exists(meta_path)):
    sys.exit(f"no {vel_path} / meta.txt - run mv_testhost.exe --wpo-encode-test \"{dump_dir}\" first")
# The fixture's own key=value layout file. It is not a capture dump, which is all
# `load_dump()` reads; parsed into the dict the mvtools decode functions take.
with open(meta_path) as f:
    meta = {k: int(v) for k, v in (line.strip().split("=", 1) for line in f if "=" in line)}

w, h = meta["velocity_width"], meta["velocity_height"]
print(f"dump: {w}x{h}, format {meta['velocity_format']}, "
      f"engine {meta.get('engine_version_major', '?')}.{meta.get('engine_version_minor', '?')}")

# The true grid. pos.xy in the shader is the pixel centre (x+0.5, y+0.5), so
# uv = (pos.xy - 0.5) / (W, H) = (x, y) / (W, H).
xs = np.arange(w, dtype=np.float64) / w
ys = np.arange(h, dtype=np.float64) / h
uv_x, uv_y = np.meshgrid(xs, ys)  # (h, w)

true_vx = VX_RANGE[0] + (VX_RANGE[1] - VX_RANGE[0]) * uv_x
true_vy = VY_RANGE[0] + (VY_RANGE[1] - VY_RANGE[0]) * uv_y
true_vz = VZ_RANGE[0] + (VZ_RANGE[1] - VZ_RANGE[0]) * (uv_x * uv_y)

px, py = np.meshgrid(np.arange(w, dtype=np.uint32), np.arange(h, dtype=np.uint32))
true_anim = ((px ^ py) & 1).astype(bool)

# --- what the GPU wrote, through the mvtools decode ----------------------
decoded_clip = mvtools.decode_velocity_clip(vel_path, meta)  # (h, w, 2)
decoded_vz, decoded_anim = mvtools.decode_velocity_depth(vel_path, meta)

# --- gate 1 (informative): decoded vs the true, unquantized value ---------
raw_err_x = np.abs(decoded_clip[:, :, 0] - true_vx)
raw_err_y = np.abs(decoded_clip[:, :, 1] - true_vy)
print(f"\ndecoded vs true (unquantized, informative only):")
print(f"  X: max err {raw_err_x.max():.5f}, mean {raw_err_x.mean():.5f}")
print(f"  Y: max err {raw_err_y.max():.5f}, mean {raw_err_y.mean():.5f}")


def encode_xy(v):
    g = np.sign(v) * np.sqrt(np.abs(v)) * (2.0 / np.sqrt(2.0)) if mvtools.ENCODE_GAMMA else v
    return g * mvtools.ENCODE_SCALE + mvtools.ENCODE_ZERO


def decode_xy(enc):
    v = (enc - mvtools.ENCODE_ZERO) / mvtools.ENCODE_SCALE
    if mvtools.ENCODE_GAMMA:
        v = v * np.abs(v) * 0.5
    return v


# --- gate 2 (pass/fail): decoded vs decode(round_to_unorm16(encode(true))) -
expected_encoded_x = np.clip(encode_xy(true_vx), 0.0, 1.0)
expected_encoded_y = np.clip(encode_xy(true_vy), 0.0, 1.0)
expected_quant_x = np.round(expected_encoded_x * 65535.0) / 65535.0
expected_quant_y = np.round(expected_encoded_y * 65535.0) / 65535.0
expected_decoded_x = decode_xy(expected_quant_x)
expected_decoded_y = decode_xy(expected_quant_y)

gate_err_x = np.abs(decoded_clip[:, :, 0] - expected_decoded_x)
gate_err_y = np.abs(decoded_clip[:, :, 1] - expected_decoded_y)

# The decode curve is v*|v|*0.5, whose slope grows with |v|, so one UNORM16
# step is a larger decode error near V = +-2 than near zero. The tolerance is
# therefore per pixel: how far the decode moves when that pixel's quantized
# encode moves one step, computed numerically, times 3 to allow for a GPU
# rounding mode other than round-to-nearest. A decode bug misses by far more.
#
# Widening the sweep past |V| = 2.008 would need a one-sided perturbation: at a
# saturated pixel the +step clips back to 1.0 and the tolerance becomes zero.
QUANT_STEP = 1.0 / 65535.0
tol_x = 3.0 * np.abs(decode_xy(np.clip(expected_quant_x + QUANT_STEP, 0.0, 1.0)) - expected_decoded_x)
tol_y = 3.0 * np.abs(decode_xy(np.clip(expected_quant_y + QUANT_STEP, 0.0, 1.0)) - expected_decoded_y)

# Channel 3's bottom bit holds bHasPixelAnimation (VELOCITY_Z_LOW_MASK =
# 0xFFFE), so the reconstructed V.z has it cleared, as
# `mvtools.depth_encoding()` expects for 5.7.
true_vz_bits = true_vz.astype(np.float32).view(np.uint32)
expected_vz_bits = true_vz_bits & np.uint32(0xFFFFFFFE)
expected_decoded_z = expected_vz_bits.view(np.float32)
gate_err_z = np.abs(decoded_vz - expected_decoded_z)

anim_mismatches = int(np.sum(decoded_anim != true_anim)) if decoded_anim is not None else None

x_over = gate_err_x > tol_x
y_over = gate_err_y > tol_y
print(f"\ndecoded vs decode(round_to_unorm16(encode(true))) - the pass/fail gate:")
print(f"  X:  max err {gate_err_x.max():.3e} ({int(x_over.sum())} pixel(s) over their own tolerance)")
print(f"  Y:  max err {gate_err_y.max():.3e} ({int(y_over.sum())} pixel(s) over their own tolerance)")
print(f"  Vz: max err {gate_err_z.max():.3e}, mean {gate_err_z.mean():.3e}")
if anim_mismatches is not None:
    print(f"  bHasPixelAnimation: {anim_mismatches} / {true_anim.size} pixels mismatched")
else:
    print("  bHasPixelAnimation: not decoded (engine version has no such bit) - unexpected for this dump")

# Vz is reassembled bit for bit, so its tolerance is a fixed epsilon: the
# shader lerps in float32 and this script in float64, which can differ by about
# 1e-12 on a ~1e-5 value. A reassembly bug (wrong shift or mask, swapped halves)
# misses by far more.
VZ_TOLERANCE = 1e-9
ok = (not x_over.any() and not y_over.any() and gate_err_z.max() < VZ_TOLERANCE and anim_mismatches == 0)

print(f"\n{'PASS' if ok else 'FAIL'}: mvtools.py's decode functions "
      f"{'correctly invert' if ok else 'do NOT correctly invert'} the real GPU-written encode "
      f"on values representative of WPO pixels (swept across the encoding's nonlinear range, "
      f"to |V| = 2.0 against its 2.008 ceiling).")
if ok:
    print("This does not mean WPO pixels in a real capture decode to physically correct velocities - "
          "only that nothing about this project's decode logic is specific to static-camera-predictable "
          "motion.")
sys.exit(0 if ok else 1)
