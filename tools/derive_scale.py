"""Test the SceneVelocity decode against measured motion, per pixel.

Regresses per pixel, over the written region, against block matching
(blockmatch.py). Three questions:

  1. What is the exponent? Fit  measured_px = k * |code_offset|^p  in log-log.
     p ~ 1 means the encoding is linear in the stored code; p ~ 2 means the code
     is a square root of the displacement (VELOCITY_ENCODE_GAMMA on). The
     engine source says 2 (Common.ush:2062); fitting it checks what the
     shipping shaders did.

  2. Is the decode unbiased? Regress measured against
     `mvtools.decode_velocity`. A correct decode gives slope 1, intercept 0.
     Nothing here feeds back into the constants.

  3. How does a linear decode compare? The same regression with linear
     constants.

Needs a capture with real motion; on a near-static one the log-log fit has no
range, and the script says so.

Usage:  python derive_scale.py [dump_dir] [stride]
"""
import os
import sys
import cv2
import numpy as np
import blockmatch
import mvtools
import paths

dump_dir = sys.argv[1] if len(sys.argv) > 1 else paths.DEFAULT_DUMP
stride = int(sys.argv[2]) if len(sys.argv) > 2 else 3

dump = mvtools.load_dump(dump_dir)
meta = dump.meta
indices = mvtools.frame_indices(dump_dir)
cw, ch = dump.surface("color").width, dump.surface("color").height

# Block matching. SEARCH bounds the measurable displacement; WINDOW is the
# correlation window. MIN_CONFIDENCE drops pixels whose SAD minimum is not
# distinct (flat walls, sky, smooth surfaces), where optical flow would report
# a confident zero.
SEARCH = 12
WINDOW = 17
MIN_CONFIDENCE = 1.5

LEGACY_UV_PER_UNIT_X = -0.1758
LEGACY_UV_PER_UNIT_Y = +0.1991


def gray(i):
    c = mvtools.load_color(os.path.join(dump_dir, f"color_{i:05d}.bin"), meta)
    return (np.clip(c, 0, 1) * 255).astype(np.float32).mean(axis=2)


# Dense block matching is the slow part: 2*(2*SEARCH+1)^2 full-resolution
# passes per pair. A dozen pairs already give hundreds of thousands of pixels;
# more add minutes and no useful precision.
MAX_PAIRS = 12

pairs, adjacency_note = mvtools.consecutive_pairs(dump_dir, indices)
pairs = pairs[::stride][:MAX_PAIRS]
print(f"pairing: {adjacency_note}")
print(f"{len(pairs)} pairs after stride {stride} (capped at {MAX_PAIRS})\n")

dec_x, dec_y, meas_x, meas_y, code_x, code_y = [], [], [], [], [], []

for prev_i, cur_i in pairs:
    vel_path = os.path.join(dump_dir, f"vel_{cur_i:05d}.bin")
    written = mvtools.velocity_written_mask(vel_path, meta)
    if written.mean() < 0.005:
        continue
    raw = mvtools.load_velocity_raw(vel_path, meta)
    flow_uv = mvtools.decode_velocity(vel_path, meta)

    # Compared at output resolution, where the images are. The velocity buffer
    # is resampled, nearest-neighbour: bilinear at the edge of a written region
    # would blend in the cleared value and show motion that is not there.
    def up(a, interp=cv2.INTER_NEAREST):
        return cv2.resize(a, (cw, ch), interpolation=interp)

    mask = up(written.astype(np.uint8)) > 0
    if mask.sum() < 5000:
        continue

    measured, confidence, clipped = blockmatch.dense(
        gray(cur_i), gray(prev_i), search=SEARCH, window=WINDOW)
    good = mask & (confidence > MIN_CONFIDENCE)
    # Drop matches pinned to the edge of the search window: they are clipped,
    # not measured. `blockmatch` reports this itself; testing |flow| < SEARCH
    # instead would drop every pixel faster than the window, on a title moving
    # 15-70 px per frame nearly all of them.
    good &= ~clipped
    if good.sum() < 2000:
        continue

    dec_x.append(up(flow_uv[:, :, 0])[good] * cw)
    dec_y.append(up(flow_uv[:, :, 1])[good] * ch)
    meas_x.append(measured[:, :, 0][good])
    meas_y.append(measured[:, :, 1][good])
    code_x.append((up(raw[:, :, 0])[good] - mvtools.ENCODE_ZERO) * 65535.0)
    code_y.append((up(raw[:, :, 1])[good] - mvtools.ENCODE_ZERO) * 65535.0)

if not dec_x:
    sys.exit("no pairs with enough confidently-matched written pixels; recapture with real motion")

dec_x, dec_y = np.concatenate(dec_x), np.concatenate(dec_y)
meas_x, meas_y = np.concatenate(meas_x), np.concatenate(meas_y)
code_x, code_y = np.concatenate(code_x), np.concatenate(code_y)
print(f"{len(dec_x)} confidently-matched written pixels pooled across pairs")
p99 = max(np.percentile(np.abs(meas_x), 99), np.percentile(np.abs(meas_y), 99))
print(f"measured displacement: |x| p50={np.percentile(np.abs(meas_x),50):.2f}px "
      f"p99={np.percentile(np.abs(meas_x),99):.2f}px, "
      f"|y| p50={np.percentile(np.abs(meas_y),50):.2f}px "
      f"p99={np.percentile(np.abs(meas_y),99):.2f}px")

# Below a couple of pixels of real motion the capture sits inside block
# matching's noise, and every fit returns slope ~0 and R^2 ~0: a fact about the
# footage, not the decode. Warn instead of reporting it as a constant.
DEGENERATE = p99 < 2.0
if DEGENERATE:
    print("\n" + "=" * 72)
    print("WARNING: this capture has no dynamic range to regress against.")
    print(f"  99th-percentile measured motion is {p99:.2f}px, comparable to block")
    print("  matching's own precision. Slopes and exponents below are NOT")
    print("  meaningful and must not be quoted. Recapture with real motion in")
    print("  frame. The RMS comparison at the end is still informative, since it")
    print("  does not require the ground truth to vary.")
    print("=" * 72)


def fit(x, y):
    """Least squares y = a*x + b, with R^2."""
    a, b = np.polyfit(x, y, 1)
    pred = a * x + b
    ss_res = float(((y - pred) ** 2).sum())
    ss_tot = float(((y - y.mean()) ** 2).sum())
    return float(a), float(b), (1 - ss_res / ss_tot if ss_tot > 0 else float("nan"))


# --- 1. the exponent -------------------------------------------------------
print("\n1. Encoding exponent (does not assume the answer)")
print("   fitting  log|measured_px| = p * log|code_offset| + log k")
print("   p ~ 1 => linear encoding;  p ~ 2 => sqrt encoding (VELOCITY_ENCODE_GAMMA)")
for name, code, meas in (("X", code_x, meas_x), ("Y", code_y, meas_y)):
    sel = (np.abs(code) > 20) & (np.abs(meas) > 0.05)
    if sel.sum() < 500:
        print(f"   axis {name}: too few pixels with measurable motion ({sel.sum()})")
        continue
    lc, lm = np.log(np.abs(code[sel])), np.log(np.abs(meas[sel]))
    if np.ptp(lc) < 1.0:
        print(f"   axis {name}: code offsets span too little range "
              f"({np.exp(np.ptp(lc)):.1f}x) to fit an exponent - needs faster motion")
        continue
    p, logk, r2 = fit(lc, lm)
    verdict = "sqrt/gamma" if abs(p - 2.0) < 0.4 else ("linear" if abs(p - 1.0) < 0.3 else "neither")
    if r2 < 0.3:
        verdict = "UNINFORMATIVE (R^2 too low to distinguish)"
    print(f"   axis {name}: p = {p:.3f}   R^2 = {r2:.3f}   (n={sel.sum()})  -> {verdict}")

# --- 2. is the engine decode unbiased? -------------------------------------
print("\n2. Engine decode vs measured (slope 1.000, intercept 0 = correct)")
for name, dec, meas in (("X", dec_x, meas_x), ("Y", dec_y, meas_y)):
    a, b, r2 = fit(dec, meas)
    print(f"   axis {name}: measured = {a:.3f} * decoded {b:+.3f} px   R^2 = {r2:.3f}")

# --- 3. the legacy linear decode, same test --------------------------------
print("\n3. Legacy linear decode vs measured, for comparison")
leg_x = (code_x / 65535.0 + mvtools.ENCODE_ZERO - 0.5) * LEGACY_UV_PER_UNIT_X * cw
leg_y = (code_y / 65535.0 + mvtools.ENCODE_ZERO - 0.5) * LEGACY_UV_PER_UNIT_Y * ch
for name, leg, meas in (("X", leg_x, meas_x), ("Y", leg_y, meas_y)):
    a, b, r2 = fit(leg, meas)
    print(f"   axis {name}: measured = {a:.3f} * decoded {b:+.3f} px   R^2 = {r2:.3f}")

print("\nRMS error against measured displacement, per pixel:")
for name, dec, leg, meas in (("X", dec_x, leg_x, meas_x), ("Y", dec_y, leg_y, meas_y)):
    print(f"   axis {name}: engine {np.sqrt(((dec - meas) ** 2).mean()):7.3f} px   "
          f"legacy {np.sqrt(((leg - meas) ** 2).mean()):7.3f} px")

# --- scatter plot ----------------------------------------------------------
try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    print("\n(matplotlib not installed - skipping the scatter plot)")
    sys.exit(0)

fig, axes = plt.subplots(1, 2, figsize=(11, 5))
n = min(len(dec_x), 40000)
sub = np.random.default_rng(0).choice(len(dec_x), n, replace=False)
for ax, name, dec, leg, meas in (
        (axes[0], "X", dec_x, leg_x, meas_x), (axes[1], "Y", dec_y, leg_y, meas_y)):
    ax.scatter(meas[sub], leg[sub], s=1, alpha=0.12, color="tab:red", label="legacy linear decode")
    ax.scatter(meas[sub], dec[sub], s=1, alpha=0.12, color="tab:blue", label="engine decode")
    lim = float(np.percentile(np.abs(meas), 99.5)) * 1.4 + 0.5
    ax.plot([-lim, lim], [-lim, lim], "k--", lw=1, label="y = x (correct)")
    ax.set_xlim(-lim, lim)
    ax.set_ylim(-lim, lim)
    ax.set_xlabel(f"block-matched displacement, {name} (px)")
    ax.set_ylabel(f"decoded displacement, {name} (px)")
    ax.set_title(f"axis {name}")
    leg_h = ax.legend(markerscale=8, fontsize=8, loc="upper left")
    for h in leg_h.legend_handles:
        h.set_alpha(1.0)
fig.suptitle("SceneVelocity decode vs measured image motion, per pixel")
fig.tight_layout()
# run_validation.py passes its tag in MV_RESULT_TAG, so the figure lands in the
# same results/<tag>/ folder as the report.
out = os.path.join(paths.results_dir(os.environ.get("MV_RESULT_TAG", "")), "decode_scatter.png")
fig.savefig(out, dpi=110)
print(f"\nwrote {os.path.normpath(out)}")
