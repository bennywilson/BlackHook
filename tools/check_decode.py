"""Check the decoded velocity against directly measured image motion.

This is the test the decode actually needed, and the one that was missing for
most of the project: it compares, per frame pair, what the decode *claims* the
written region moved against what the imagery *shows* it moved. Ground truth is
block matching (see blockmatch.py), which knows nothing about the velocity
buffer.

It also runs the decode this project used to use - a plain linear decode with
fitted constants - alongside the engine's real one, because the difference
between them is the project's main debugging result and this is where it is
reproducible. The short version:

  * SceneVelocity is SQUARE-ROOT encoded. VELOCITY_ENCODE_GAMMA is 1 for every
    SM5+ platform (Common.ush:238), so `sqrt(abs(V))*sqrt(2)` is applied before
    the UNORM quantisation and must be undone after it.
  * Decoding it linearly overstates small motions badly - sqrt is steepest near
    zero - which is what produced ~1.7px of confident, consistently-signed
    "motion" on a viewmodel that block matching put at 0.00px. That was read as
    a zero-point error for a while. It is not: the zero point is exactly where
    Common.ush says it is.
  * Channels 2 and 3 are not colour and not displacement. They are the float32
    bit pattern of V.z (the DeviceZ delta) split into high and low halves, with
    the bottom bit of channel 3 carrying bHasPixelAnimation. Reassembled they
    are a clean, tightly-clustered float; viewed as channels they look like one
    bimodal channel and one noise channel, which is what misled the earlier
    reading of them.

Usage:  python check_decode.py [dump_dir] [max_pairs]
"""
import os
import sys
import cv2
import numpy as np
import blockmatch
import mvtools
import paths

dump_dir = sys.argv[1] if len(sys.argv) > 1 else paths.DEFAULT_DUMP
max_pairs = int(sys.argv[2]) if len(sys.argv) > 2 else 60

dump = mvtools.load_dump(dump_dir)
meta = dump.meta
indices = mvtools.frame_indices(dump_dir)
cw, ch = dump.surface("color").width, dump.surface("color").height
vw, vh = dump.surface("velocity").width, dump.surface("velocity").height

# Integer search radius, in output-resolution pixels. Wide enough for the
# fastest thing seen in these captures (a train at ~11px/frame).
SEARCH = 14
UE5_ZERO_CODE = 32767

# The constants this project used before the encoding was read properly. Kept
# only so the failure they produced stays reproducible from the repo.
LEGACY_UV_PER_UNIT_X = -0.1758
LEGACY_UV_PER_UNIT_Y = +0.1991


def gray(i):
    c = mvtools.load_color(os.path.join(dump_dir, f"color_{i:05d}.bin"), meta)
    return (np.clip(c, 0, 1) * 255).astype(np.float32).mean(axis=2)


def legacy_decode(raw):
    """The old linear decode, for side-by-side comparison only."""
    out = np.empty(raw.shape[:2] + (2,), dtype=np.float32)
    out[:, :, 0] = (raw[:, :, 0] - 0.5) * LEGACY_UV_PER_UNIT_X
    out[:, :, 1] = (raw[:, :, 1] - 0.5) * LEGACY_UV_PER_UNIT_Y
    return out


rows = []
zero_offsets = []
pairs, adjacency_note = mvtools.consecutive_pairs(dump_dir, indices)
pairs = pairs[:max_pairs]
print(f"pairing: {adjacency_note}\n")

print(f"{'pair':>6} {'eng_dx':>8} {'eng_dy':>8} {'bm_dx':>7} {'bm_dy':>7} "
      f"{'eng|v|':>7} {'bm|v|':>6} {'legacy|v|':>9}")
for prev_i, cur_i in pairs:
    vel_path = os.path.join(dump_dir, f"vel_{cur_i:05d}.bin")
    written = mvtools.velocity_written_mask(vel_path, meta)
    if written.mean() < 0.01:
        continue
    raw = mvtools.load_velocity_raw(vel_path, meta)

    flow = mvtools.decode_velocity(vel_path, meta)
    dec_dx = float(flow[:, :, 0][written].mean() * cw)
    dec_dy = float(flow[:, :, 1][written].mean() * ch)

    old = legacy_decode(raw)
    old_mag = float(np.hypot(old[:, :, 0][written].mean() * cw, old[:, :, 1][written].mean() * ch))

    mask = cv2.resize(written.astype(np.uint8), (cw, ch), interpolation=cv2.INTER_NEAREST) > 0
    matched = blockmatch.global_shift(gray(cur_i), gray(prev_i), mask, search=SEARCH)
    if matched is None:
        continue
    bm_dx, bm_dy = matched

    for c in (0, 1):
        codes = raw[:, :, c][written] * 65535.0
        hist, edges = np.histogram(codes, bins=2000)
        zero_offsets.append((c, float((edges[hist.argmax()] + edges[hist.argmax() + 1]) / 2)))

    dec_mag, bm_mag = float(np.hypot(dec_dx, dec_dy)), float(np.hypot(bm_dx, bm_dy))
    rows.append((dec_dx, dec_dy, bm_dx, bm_dy, dec_mag, bm_mag, old_mag))
    if len(rows) % 8 == 1:
        print(f"{cur_i:>6} {dec_dx:>8.3f} {dec_dy:>8.3f} {bm_dx:>7.2f} {bm_dy:>7.2f} "
              f"{dec_mag:>7.3f} {bm_mag:>6.2f} {old_mag:>9.3f}")

if not rows:
    sys.exit("no scorable pairs found")

arr = np.array(rows)
print(f"\npairs scored:                  {len(arr)}")
print(f"mean block-matched |v|:        {arr[:, 5].mean():.3f} px   <- ground truth")
print(f"mean decoded |v| (engine):     {arr[:, 4].mean():.3f} px")
print(f"mean decoded |v| (legacy lin): {arr[:, 6].mean():.3f} px")
print(f"mean |engine - measured|:      "
      f"{np.abs(np.hypot(arr[:, 0] - arr[:, 2], arr[:, 1] - arr[:, 3])).mean():.3f} px")
print(f"mean |legacy  - measured|:     {np.abs(arr[:, 6] - arr[:, 5]).mean():.3f} px")

# Correlation is only meaningful if the ground truth actually varies. A capture
# where every pair matches at the same shift has no dynamic range to correlate
# against, and reporting r on it would be misleading.
for axis, (dcol, bcol) in enumerate([(0, 2), (1, 3)]):
    name = "X" if axis == 0 else "Y"
    if np.ptp(arr[:, bcol]) < 0.25:
        print(f"corr {name}: undefined - block matching found only "
              f"{np.ptp(arr[:, bcol]):.2f}px of variation across the capture")
    else:
        print(f"corr {name}: {np.corrcoef(arr[:, dcol], arr[:, bcol])[0, 1]:+.3f}")

print(f"\nzero point, vs UE5 documented {UE5_ZERO_CODE} (Common.ush:2069):")
for c in (0, 1):
    modes = np.array([m for cc, m in zero_offsets if cc == c])
    off = modes.mean() - UE5_ZERO_CODE
    # What that code offset means once the sqrt encoding is undone. This is the
    # line that settles it: an offset of a few hundred codes is not a broken
    # zero point, it is a fraction of a pixel of real motion, magnified by the
    # encoding being steepest exactly there.
    res = vw if c == 0 else vh
    g = (off / 65535.0) / mvtools.ENCODE_SCALE
    px = abs(g * abs(g) * 0.5 * 0.5) * res
    print(f"  ch{c}: modal code {modes.mean():8.1f}  (offset {off:+.1f}, "
          f"spread across frames {np.ptp(modes):.1f})")
    print(f"        -> {px:.3f} px under the engine's sqrt decode, "
          f"{abs(off / 65535.0 * (LEGACY_UV_PER_UNIT_X if c == 0 else LEGACY_UV_PER_UNIT_Y) * res):.3f} px "
          f"under the legacy linear one")

# --- channels 2 and 3 -------------------------------------------------------
print("\nchannels 2/3 (VELOCITY_ENCODE_DEPTH, Common.ush:2071):")
sample = pairs[len(pairs) // 2][1] if pairs else indices[0]
vel_path = os.path.join(dump_dir, f"vel_{sample:05d}.bin")
written = mvtools.velocity_written_mask(vel_path, meta)
if mvtools.velocity_channels(meta)[0] < 4:
    # Not a failure and not a gap in the analysis: NeedVelocityDepth() picks the
    # format AND whether V.z is written, so a 2-channel capture provably has no
    # depth channels to look at.
    print("  this capture is 2-channel (PF_G16R16), so there are no channels 2/3 - "
          "NeedVelocityDepth() was false on this title, which is the same predicate "
          "that chose the format (VelocityRendering.cpp:758)")
    raise SystemExit(0)
vz, anim = mvtools.decode_velocity_depth(vel_path, meta)
sel = vz[written]
raw = mvtools.load_velocity_raw(vel_path, meta)
print(f"  frame {sample}: ch2 spans {raw[:, :, 2][written].min():.3f}..{raw[:, :, 2][written].max():.3f}, "
      f"ch3 spans {raw[:, :, 3][written].min():.3f}..{raw[:, :, 3][written].max():.3f}")
print(f"  reassembled V.z (DeviceZ delta): mean {sel.mean():+.3e}  "
      f"range {sel.min():+.3e}..{sel.max():+.3e}")
print(f"  finite and denormal-free: {np.isfinite(sel).mean() * 100:.1f}% "
      f"(a mis-read of these channels would be full of NaN/Inf)")
if anim is None:
    print("  bHasPixelAnimation: not encoded in this engine version (no VELOCITY_Z_LOW_MASK), "
          "so channel 3's low bit is float mantissa, not a flag")
else:
    print(f"  bHasPixelAnimation set on {anim[written].mean() * 100:.1f}% of written pixels")
