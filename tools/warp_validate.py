"""Warp-and-difference validation of the extracted motion field.

Takes frame N-1, warps it by the motion field captured at frame N, and
compares the result against the real frame N. If the vectors are correct (and
correctly decoded, signed and scaled) the warped image should match frame N
noticeably better than the un-warped frame N-1 does.

Only pixels where the velocity buffer was actually written are scored - UE5
writes SceneVelocity for dynamic objects only, leaving static geometry to be
reconstructed from camera matrices by the consuming shader. Scoring the
unwritten region would be measuring something this buffer never claimed to
contain.

WHAT THIS TEST IS WORTH, stated up front because it is easy to over-read.

Two things changed since the version of this script that produced the numbers
quoted in earlier drafts, and both change how much the result means:

  * The decode constants are no longer fitted to this test. They are now read
    out of Common.ush (see mvtools.py). The multiplier sweep in step 1 used to
    be circular - it recovered the value it had itself produced - and is now an
    independent check: nothing downstream tuned it, so a peak at 1.0 is real
    evidence that the source-derived decode is right.

  * There are now control conditions. "Warping by this field reduces error" is
    a weak claim on its own: a single rigid shift would do that too on a scene
    dominated by one large moving object. Step 2 scores the real field against
    a sign flip, against the best possible single global translation, and
    against a deliberately mismatched frame's field. The per-pixel field has
    to beat all three before "per-pixel correct" is a fair description.

It still scores only the ~8-13% of pixels the velocity buffer wrote, using a
mask derived from that same buffer. It is not a full-screen result.
"""
import os
import sys
import cv2
import numpy as np
import blockmatch
import mvtools
import paths

dump_dir = sys.argv[1] if len(sys.argv) > 1 else paths.DEFAULT_DUMP
dump = mvtools.load_dump(dump_dir)
meta = dump.meta
indices = mvtools.frame_indices(dump_dir)
cw, ch = dump.surface("color").width, dump.surface("color").height

grid_x, grid_y = np.meshgrid(np.arange(cw, dtype=np.float32), np.arange(ch, dtype=np.float32))

# How far ahead the mismatched-frame control reaches for a wrong field. Far
# enough that it is genuinely a different moment, close enough that it is still
# the same shot with a similar written region.
MISMATCH_OFFSET = 5


def to_display(color):
    return mvtools.to_display(color)


def warp(prev_img, flow_uv, scale=1.0):
    """Sample prev_img at each pixel offset by the (scaled) flow."""
    fu = cv2.resize(flow_uv[:, :, 0], (cw, ch), interpolation=cv2.INTER_LINEAR)
    fv = cv2.resize(flow_uv[:, :, 1], (cw, ch), interpolation=cv2.INTER_LINEAR)
    # cv2.remap requires contiguous float32 maps.
    map_x = np.ascontiguousarray(grid_x + scale * fu * cw, dtype=np.float32)
    map_y = np.ascontiguousarray(grid_y + scale * fv * ch, dtype=np.float32)
    return cv2.remap(prev_img, map_x, map_y, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)


def shift(prev_img, dx, dy):
    """Warp by one rigid translation, same sampling convention as warp()."""
    map_x = np.ascontiguousarray(grid_x + dx, dtype=np.float32)
    map_y = np.ascontiguousarray(grid_y + dy, dtype=np.float32)
    return cv2.remap(prev_img, map_x, map_y, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)


def masked_mae(a, b, mask):
    return float(np.abs(a[mask] - b[mask]).mean())


def load_pair(prev_i, cur_i, flow_from=None):
    """(prev, cur, flow, mask) for a pair, or None if nothing was written.

    flow_from lets the mismatched-frame control take its field from a different
    frame than the one being scored, while keeping the scored mask the same -
    otherwise the control would be scoring a different set of pixels too and
    the comparison would not be like for like.
    """
    prev = to_display(mvtools.load_color(os.path.join(dump_dir, f"color_{prev_i:05d}.bin"), meta))
    cur = to_display(mvtools.load_color(os.path.join(dump_dir, f"color_{cur_i:05d}.bin"), meta))
    vel_path = os.path.join(dump_dir, f"vel_{cur_i:05d}.bin")
    written = mvtools.velocity_written_mask(vel_path, meta)
    flow = mvtools.decode_velocity(os.path.join(
        dump_dir, f"vel_{(flow_from if flow_from is not None else cur_i):05d}.bin"), meta)
    # Unwritten pixels hold 0, which decodes to a large bogus displacement -
    # zero them so "no data" warps as "no motion" rather than smearing static
    # geometry across the frame.
    flow[~written] = 0.0
    mask = cv2.resize(written.astype(np.uint8), (cw, ch), interpolation=cv2.INTER_NEAREST) > 0
    if mask.sum() == 0:
        return None
    return prev, cur, flow, mask


all_pairs, adjacency_note = mvtools.consecutive_pairs(dump_dir, indices)
if not all_pairs:
    sys.exit("no adjacent frame pairs in this dump")
print(f"pairing: {adjacency_note}")

# Probe and scored sets are DISJOINT. Previously the probe pairs were a subset
# of the scored ones, so the multiplier was chosen on data it was then scored
# on - a small leak, but the kind that quietly turns a validation back into a
# fit. Every 7th pair probes; the rest are scored and never touched by step 1.
probe_pairs = all_pairs[::7][:15]
probe_set = set(probe_pairs)
scored_pairs = [p for p in all_pairs if p not in probe_set]
print(f"{len(all_pairs)} adjacent pairs: {len(probe_pairs)} held out to probe the "
      f"multiplier, {len(scored_pairs)} scored\n")

# --- Step 1: sweep the decode scale ----------------------------------------
print("Sweeping the decode multiplier on the held-out probe pairs")
print("(mean absolute error over written pixels, lower is better)")
print("1.0 = the constants transcribed from Common.ush, unmodified. Nothing")
print("downstream was fitted to this sweep, so where it peaks is evidence.\n")

multipliers = [round(m, 2) for m in np.arange(0.4, 1.65, 0.1)]
cached = [c for c in (load_pair(a, b) for a, b in probe_pairs) if c is not None]
if not cached:
    sys.exit("no probe pairs with written velocity")

baseline = float(np.mean([masked_mae(p, c, m) for p, c, _, m in cached]))
print(f"  {'multiplier':>12}   {'MAE':>9}   {'vs baseline':>12}")
print(f"  {'0 (no warp)':>12}   {baseline:9.5f}   {'--':>12}")
results = []
for mult in multipliers:
    score = float(np.mean([masked_mae(warp(p, f, mult), c, m) for p, c, f, m in cached]))
    results.append((score, mult))
    print(f"  {mult:>12}   {score:9.5f}   {(baseline - score) / baseline * 100:+11.1f}%")

best_score, best_mult = min(results)
print(f"\nBest multiplier: {best_mult} (MAE {best_score:.5f} vs baseline {baseline:.5f})")
if best_score >= baseline:
    print("WARNING: no multiplier beats the un-warped baseline, so this sweep says")
    print("  NOTHING about the decode scale. When the true motion is smaller than the")
    print("  blur a resample introduces, every multiplier loses to not warping at all")
    print("  and the sweep just ranks them by how little they move the image - which is")
    print("  why the optimum here is the smallest value tried rather than an interior")
    print("  peak. Read that as 'this capture has no motion to reconstruct', not as a")
    print("  measurement. Recapture with real motion in frame.")

# The scored run below deliberately uses 1.0, not best_mult. Scoring at the
# swept optimum would reintroduce exactly the circularity this split removes.
SCALE = 1.0
print(f"Scoring below uses multiplier {SCALE} (the unmodified engine decode), "
      f"not the swept optimum.\n")

# --- Step 2: the real field against three controls -------------------------
print("Full validation on the scored pairs, with controls")
print("  per-pixel  - the decoded SceneVelocity field, as-is")
print("  sign flip  - the same field negated; must get clearly WORSE")
print("  global     - the single best rigid translation for the masked region,")
print("               found by block matching; an upper bound on what any")
print("               rigid-motion model could achieve on this pair")
print("  mismatched - the field from frame N+%d applied to the N-1 -> N pair;"
      % MISMATCH_OFFSET)
print("               must get clearly worse\n")

index_set = set(indices)
rows = []
for prev_i, cur_i in scored_pairs:
    loaded = load_pair(prev_i, cur_i)
    if loaded is None:
        continue
    prev, cur, flow, mask = loaded

    base = masked_mae(prev, cur, mask)
    per_pixel = masked_mae(warp(prev, flow, SCALE), cur, mask)
    flipped = masked_mae(warp(prev, -flow, SCALE), cur, mask)

    g = blockmatch.global_shift(
        (cur * 255).mean(axis=2).astype(np.float32),
        (prev * 255).mean(axis=2).astype(np.float32), mask, search=14)
    global_mae = masked_mae(shift(prev, *g), cur, mask) if g is not None else float("nan")

    mismatch = float("nan")
    other = cur_i + MISMATCH_OFFSET
    if other in index_set:
        alt = load_pair(prev_i, cur_i, flow_from=other)
        if alt is not None:
            mismatch = masked_mae(warp(prev, alt[2], SCALE), cur, mask)

    rows.append((cur_i, base, per_pixel, flipped, global_mae, mismatch))

if not rows:
    sys.exit("no scorable pairs")

arr = np.array([r[1:] for r in rows], dtype=np.float64)
names = ["no warp (baseline)", "per-pixel field", "sign-flipped", "best global shift", "mismatched frame"]
print(f"  pairs scored: {len(rows)}\n")
print(f"  {'condition':>20}   {'mean MAE':>9}   {'vs baseline':>12}   {'better than baseline':>20}")
base_col = arr[:, 0]
for i, name in enumerate(names):
    col = arr[:, i]
    ok = np.isfinite(col)
    if not ok.any():
        print(f"  {name:>20}   {'n/a':>9}   {'n/a':>12}   {'n/a':>20}")
        continue
    delta = (base_col[ok].mean() - col[ok].mean()) / base_col[ok].mean() * 100
    wins = int((col[ok] < base_col[ok]).sum())
    print(f"  {name:>20}   {col[ok].mean():9.5f}   {delta:+11.1f}%   "
          f"{wins:>10}/{int(ok.sum()):<9}")

# Explicit verdicts, so the reader does not have to do the comparison. Each of
# these is a claim the numbers above either support or do not.
print("\nControl verdicts:")
pp = arr[:, 1]


def verdict(label, col, should_be_worse_than_per_pixel=True):
    ok = np.isfinite(col) & np.isfinite(pp)
    if not ok.any():
        print(f"  {label}: no data")
        return
    d = (col[ok].mean() - pp[ok].mean()) / pp[ok].mean() * 100
    if should_be_worse_than_per_pixel:
        good = d > 5.0
        print(f"  {label}: {d:+.1f}% vs the per-pixel field  -> "
              f"{'PASS (clearly worse, as required)' if good else 'FAIL (not clearly worse)'}")
    else:
        print(f"  {label}: {d:+.1f}% vs the per-pixel field")


verdict("sign flip        ", arr[:, 2])
verdict("mismatched frame ", arr[:, 4])
gl = arr[:, 3]
ok = np.isfinite(gl) & np.isfinite(pp)
if ok.any():
    d = (gl[ok].mean() - pp[ok].mean()) / pp[ok].mean() * 100
    print(f"  best global shift: {d:+.1f}% vs the per-pixel field")
    if d > 5.0:
        print("      -> the per-pixel field beats the best rigid translation, so it is")
        print("         carrying information a single global shift cannot express.")
    else:
        print("      -> the per-pixel field does NOT clearly beat a single rigid shift.")
        print("         On a scene dominated by one large rigid object these are nearly")
        print("         the same hypothesis, so this test cannot separate them here.")
        print("         That is a limitation of the footage, not a pass.")

# --- Step 3: write visual evidence ----------------------------------------
# "A number in a log is not evidence" - dump the actual images so the result
# can be seen rather than trusted.
#
# This picks the pair with the LARGEST un-warped error, i.e. the most motion in
# the capture. That is the most legible pair, not a typical one - do not
# describe it as representative.
best_pair = max(rows, key=lambda r: r[1])[0]
loaded = load_pair(best_pair - 1, best_pair)
prev, cur, flow, mask = loaded
warped = warp(prev, flow, SCALE)

diff_before = np.abs(prev - cur).mean(axis=2)
diff_after = np.abs(warped - cur).mean(axis=2)
# Crop to the region that actually has velocity, so the comparison is legible
ys, xs = np.where(mask)
y0, y1 = max(ys.min() - 20, 0), min(ys.max() + 20, ch)
x0, x1 = max(xs.min() - 20, 0), min(xs.max() + 20, cw)

boost = 6.0
panels = [
    ("frame N-1", (prev[y0:y1, x0:x1, ::-1] * 255)),
    ("frame N", (cur[y0:y1, x0:x1, ::-1] * 255)),
    ("N-1 warped by MV", (warped[y0:y1, x0:x1, ::-1] * 255)),
    ("|N-1 - N| x6", np.dstack([np.clip(diff_before[y0:y1, x0:x1] * boost, 0, 1) * 255] * 3)),
    ("|warped - N| x6", np.dstack([np.clip(diff_after[y0:y1, x0:x1] * boost, 0, 1) * 255] * 3)),
]
imgs = []
for label, img in panels:
    img = img.astype(np.uint8).copy()
    cv2.putText(img, label, (10, 26), cv2.FONT_HERSHEY_SIMPLEX, 0.65, (0, 255, 0), 2)
    imgs.append(img)
montage = np.hstack(imgs)
out_path = sys.argv[2] if len(sys.argv) > 2 else paths.out_path("warp_validation.png")
# Clamp the figure width. At 1707x1067 a five-panel montage is 8535px wide and
# 14MB, which is not a sensible thing to commit to a repository and is wider
# than anything will ever display it. The panels stay legible at half that.
MAX_FIGURE_WIDTH = 4200
if montage.shape[1] > MAX_FIGURE_WIDTH:
    scale = MAX_FIGURE_WIDTH / montage.shape[1]
    montage = cv2.resize(montage, (MAX_FIGURE_WIDTH, int(montage.shape[0] * scale)),
                         interpolation=cv2.INTER_AREA)
cv2.imwrite(out_path, montage)
row = next(r for r in rows if r[0] == best_pair)
print(f"\n  wrote {out_path} (pair {best_pair-1} -> {best_pair}, "
      f"MAE {row[1]:.5f} -> {row[2]:.5f})")
