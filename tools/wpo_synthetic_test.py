"""Checks decode correctness on a synthetic velocity grid representative of
WPO (world-position-offset) pixels - motion that is not pure camera motion,
which is exactly the population wind-animated foliage belongs to and which no
image-matching reference in this project resolves reliably either.

Companion to mv_testhost.exe --wpo-encode-test <dir>, which renders a known
(Vx, Vy, Vz) grid through the real EncodeVelocityToTexture()/
VELOCITY_ENCODE_DEPTH path (transcribed exactly from Common.ush, in
testhost/src/wpo_encode_test.cpp) and dumps the result in the same
meta.txt/*.bin shape a real capture uses. This script regenerates the same
grid independently and runs the UNMODIFIED, already-shipping mvtools decode
functions against the dump, then checks two things.

The grid dimensions come from the dump's own meta.txt, so those cannot drift.
The VALUE ranges below (VX_RANGE/VY_RANGE/VZ_RANGE) are a hand-kept duplicate
of the lerp ranges in wpo_encode_test.cpp's PSMain, and nothing checks that
the two still agree - the dump records the encoded result, not the formula
that produced it. Change one side and you must change the other, or this
script silently compares against the wrong "truth" and reports a failure that
is really a fixture mismatch. What it checks:

  1. The decoded value is close to the true value used to drive the shader
     (informative - bounded by UNORM16 quantization, reported but not the
     pass/fail gate, since the gate below is the tighter, better-founded one).
  2. The decoded value matches decode(round_to_unorm16(encode(true value)))
     to near float32 precision - i.e. mvtools' decode function correctly
     inverts whatever value a real GPU actually wrote, modulo only the
     documented, deliberate loss of channel 3's bottom bit to
     bHasPixelAnimation. This is the actual pass/fail gate: if the real
     hardware quantizes differently than the round-to-nearest model here
     (a genuinely interesting finding), or if mvtools' decode has a bug that
     only shows up away from the near-zero, static-geometry-dominated values
     real captures mostly contain, this is where it would show up.

Usage:  python wpo_synthetic_test.py [dump_dir]
"""
import os
import sys

import numpy as np

import mvtools
import paths

dump_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(paths.DUMPS, "wpo_test_out")

# Must match the PSMain lerp ranges in testhost/src/wpo_encode_test.cpp
# exactly - this is a from-scratch re-derivation of what SHOULD have been
# encoded, not a read of anything the shader computed. See the module
# docstring: this duplication is unchecked, so keep the two in step by hand.
#
# Note these stay INSIDE the encoding's |V| = 2.008 ceiling (2.0 is the
# largest magnitude produced), so no pixel in this grid saturates. The
# np.clip() calls below are therefore no-ops on the current ranges and exist
# only so widening them later does not silently produce nonsense.
VX_RANGE = (-2.0, 2.0)
VY_RANGE = (-2.0, 2.0)
VZ_RANGE = (-1e-5, 1e-5)

dump = mvtools.load_dump(dump_dir)
meta = dump.meta
vel_path = os.path.join(dump_dir, "wpo_vel_00000.bin")
if not os.path.exists(vel_path):
    sys.exit(f"no {vel_path} - run mv_testhost.exe --wpo-encode-test \"{dump_dir}\" first")

w, h = dump.surface("velocity").width, dump.surface("velocity").height
print(f"dump: {w}x{h}, format {meta['velocity_format']}, "
      f"engine {meta.get('engine_version_major', '?')}.{meta.get('engine_version_minor', '?')}")

# The independently-known true grid. pos.xy in the shader is the D3D12 pixel
# CENTER (x+0.5, y+0.5), so uv = (pos.xy - 0.5) / (W, H) == (x, y) / (W, H)
# exactly - no off-by-half here to get wrong.
xs = np.arange(w, dtype=np.float64) / w
ys = np.arange(h, dtype=np.float64) / h
uv_x, uv_y = np.meshgrid(xs, ys)  # (h, w)

true_vx = VX_RANGE[0] + (VX_RANGE[1] - VX_RANGE[0]) * uv_x
true_vy = VY_RANGE[0] + (VY_RANGE[1] - VY_RANGE[0]) * uv_y
true_vz = VZ_RANGE[0] + (VZ_RANGE[1] - VZ_RANGE[0]) * (uv_x * uv_y)

px, py = np.meshgrid(np.arange(w, dtype=np.uint32), np.arange(h, dtype=np.uint32))
true_anim = ((px ^ py) & 1).astype(bool)

# --- what the real GPU actually wrote, decoded by the unmodified, shipping
# decode functions --------------------------------------------------------
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


# --- gate 2 (the actual pass/fail check): decoded vs
# decode(round_to_unorm16(encode(true))) -----------------------------------
expected_encoded_x = np.clip(encode_xy(true_vx), 0.0, 1.0)
expected_encoded_y = np.clip(encode_xy(true_vy), 0.0, 1.0)
expected_quant_x = np.round(expected_encoded_x * 65535.0) / 65535.0
expected_quant_y = np.round(expected_encoded_y * 65535.0) / 65535.0
expected_decoded_x = decode_xy(expected_quant_x)
expected_decoded_y = decode_xy(expected_quant_y)

gate_err_x = np.abs(decoded_clip[:, :, 0] - expected_decoded_x)
gate_err_y = np.abs(decoded_clip[:, :, 1] - expected_decoded_y)

# The decode curve is v*|v|*0.5, whose slope grows with |v| - so one UNORM16
# quantization step produces MORE decode-space error at the buffer's extreme
# end (V near +-2) than near zero. A flat epsilon calibrated near zero fails
# there for a reason that has nothing to do with decode correctness: it is a
# property of the encoding's own shape, the same nonlinearity this project's
# sqrt-vs-linear write-up (mvtools.py's header comment) is about. The
# per-pixel tolerance below is one quantization step's worth of decode-space
# movement at THAT pixel's own value, not a constant - computed numerically
# (perturb the quantized encode by one step, see how far decode moves) rather
# than via the closed-form derivative, so it needs no separate derivation to
# trust. A 3x safety factor allows for the real GPU's rounding mode
# disagreeing with the round-to-nearest model above without hiding an actual
# decode bug, which would miss by much more than one quantization step.
#
# Caveat if the sweep is ever widened past the |V| = 2.008 ceiling: at a pixel
# whose encode saturates to exactly 1.0, the +QUANT_STEP perturbation clips
# back to 1.0 and the tolerance collapses to zero, demanding bit-exactness
# there. That is harmless today (nothing saturates - see VX_RANGE above) but
# would need a one-sided perturbation to stay meaningful.
QUANT_STEP = 1.0 / 65535.0
tol_x = 3.0 * np.abs(decode_xy(np.clip(expected_quant_x + QUANT_STEP, 0.0, 1.0)) - expected_decoded_x)
tol_y = 3.0 * np.abs(decode_xy(np.clip(expected_quant_y + QUANT_STEP, 0.0, 1.0)) - expected_decoded_y)

# Channel 3's bottom bit is deliberately stolen for bHasPixelAnimation
# (VELOCITY_Z_LOW_MASK = 0xFFFE), so the reconstructed V.z has that bit
# cleared regardless of what it held - not a bug, the documented convention
# mvtools.depth_encoding() already encodes for (5, 7).
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

# Vz's decode is a bit-exact reassembly rather than a nonlinear curve, so its
# tolerance is not value-dependent the way X/Y's is. It is not zero either:
# the shader computes Vz by lerping in float32 while the line above lerps in
# float64 and casts, so the two can differ by an ULP or so of a ~1e-5 value
# (~1e-12) with the decode still perfectly correct. A fixed epsilon a few
# orders above that is the right model - tight enough that a genuine
# reassembly bug (wrong shift, wrong mask, swapped halves) misses by far more.
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
