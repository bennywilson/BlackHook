"""Decode vs measured motion, compared over a REGION rather than per pixel.

WHY THIS EXISTS, AND WHY IT IS NOT JUST check_decode.py AGAIN

`derive_scale.py` regresses decoded against measured displacement at every
confidently-matched pixel. That is the strongest form of the test when it works,
and on the second title in this project it does not work - not because the
decode is wrong but because the *reference* is unusable there:

  * the written region is wind-animated foliage, thin high-frequency structure
    with background visible through it, so a 17px correlation window routinely
    spans two surfaces moving differently and returns a compromise;
  * the title applies heavy motion blur, which makes consecutive frames look
    more similar at smaller offsets and biases any matcher low;
  * per-pixel matching reports slope ~0.7 against a correct decode, and filtering
    harder makes it worse, not better (slope peaks at 0.73 keeping the top 50%
    of matches by confidence and falls to 0.39 keeping the top 3%). A reference
    that degrades as you demand more of it is not measuring what you think.

This tool asks a weaker question that the same footage can actually answer: over
one region, does the decode predict the displacement that region visibly
underwent? It matches a large crop by normalised cross-correlation, which has no
small search radius to be blind above and reports its own match quality, and
compares that against the median decoded displacement over the same pixels.

It cannot show the field is correct *per pixel* - only per region, per frame.
That is a real limitation and it is the reason this file does not replace
derive_scale.py. What it does show, on footage where the per-pixel route is
compromised, is whether the decode has the right scale and sign at all - and
because it works across a wide magnitude range, it also discriminates the
square-root encoding from a linear one, which is the question a single frame
cannot answer.

Usage:  python region_match.py [dump_dir]
"""
import os
import sys

import numpy as np

import blockmatch
import mvtools
import paths

try:
    import cv2
except ImportError:  # pragma: no cover
    cv2 = None

# The fitted constants this project used before the encoding was read out of
# Common.ush - kept only as a control. They are linear, so if the stored
# encoding really is square-root these should track badly at large
# displacements and acceptably at small ones. See DEBUGGING.md.
LEGACY_UV_PER_UNIT_X = -0.1758
LEGACY_UV_PER_UNIT_Y = +0.1991

# Below this normalised-cross-correlation score the match is not trustworthy and
# the pair is skipped rather than contributing a number. Stated rather than
# silently tolerated: the whole point of this file is that a reference which
# fails quietly is worse than no reference.
MIN_SCORE = 0.70

dump_dir = sys.argv[1] if len(sys.argv) > 1 else paths.DEFAULT_DUMP
dump = mvtools.load_dump(dump_dir)
meta = dump.meta
cw, ch = dump.surface("color").width, dump.surface("color").height


def gray(index):
    color = mvtools.load_color(os.path.join(dump_dir, f"color_{index:05d}.bin"), meta)
    return (np.clip(color, 0, 1) * 255).astype(np.float32).mean(axis=2)


def legacy_uv(path):
    raw = mvtools.load_velocity_raw(path, meta)
    out = np.empty(raw.shape[:2] + (2,), dtype=np.float32)
    out[:, :, 0] = (raw[:, :, 0] - 0.5) * LEGACY_UV_PER_UNIT_X
    out[:, :, 1] = (raw[:, :, 1] - 0.5) * LEGACY_UV_PER_UNIT_Y
    return out


def crop_box(mask, h, w):
    """A crop centred on the written region, big enough to match reliably.

    Centred on the mask rather than on the frame because that is where the
    pixels under test are, and left large so the normalised cross-correlation
    has plenty of structure to lock onto.
    """
    ys, xs = np.where(mask)
    if ys.size < 1000:
        return None
    cy, cx = int(ys.mean()), int(xs.mean())
    hy, hx = int(h * 0.22), int(w * 0.20)
    y0, y1 = max(0, cy - hy), min(h, cy + hy)
    x0, x1 = max(0, cx - hx), min(w, cx + hx)
    if (y1 - y0) < 64 or (x1 - x0) < 64:
        return None
    return y0, y1, x0, x1


def match_crop(cur, prev, box, max_shift=400):
    """Where did this crop of `cur` come from in `prev`? Returns (dx, dy, ncc).

    Plain template matching over a wide window: no small search radius to be
    blind above, and it reports its own match quality so a bad match can be
    discarded instead of quietly contributing a number.
    """
    h, w = cur.shape
    y0, y1, x0, x1 = box
    template = cur[y0:y1, x0:x1]
    pad = min(max_shift, x0, y0, w - x1, h - y1)
    if pad < 8:
        return 0.0, 0.0, 0.0
    ys, ye, xs, xe = y0 - pad, y1 + pad, x0 - pad, x1 + pad
    result = cv2.matchTemplate(prev[ys:ye, xs:xe], template, cv2.TM_CCOEFF_NORMED)
    _, score, _, loc = cv2.minMaxLoc(result)
    return float((xs + loc[0]) - x0), float((ys + loc[1]) - y0), float(score)


def main():
    if cv2 is None:
        raise SystemExit("region_match requires opencv-python")

    indices = mvtools.frame_indices(dump_dir)
    pairs, note = mvtools.consecutive_pairs(dump_dir, indices)
    print(f"pairing: {note}\n")

    rows = []
    print(f"{'pair':>10} {'measured dx':>12} {'dy':>7} {'ncc':>6} | "
          f"{'engine dx':>10} {'dy':>7} | {'legacy dx':>10} {'dy':>7}")
    for prev_i, cur_i in pairs:
        vel = os.path.join(dump_dir, f"vel_{cur_i:05d}.bin")
        written = mvtools.velocity_written_mask(vel, meta)
        mask = cv2.resize(written.astype(np.uint8), (cw, ch), interpolation=cv2.INTER_NEAREST) > 0
        if mask.mean() < 0.02:
            continue

        # The crop is chosen here, not inside blockmatch, because BOTH sides of
        # the comparison have to be computed over the same pixels. Measuring a
        # centre crop and then averaging the decode over the whole frame is not
        # a scale measurement - under camera rotation, foliage at the frame edge
        # moves several times as far as the middle, so the two numbers describe
        # different parts of the image and their ratio is meaningless. The first
        # version of this file did exactly that and reported slope 0.73 where a
        # like-for-like comparison of the same footage gives 1.04.
        box = crop_box(mask, ch, cw)
        if box is None:
            continue
        y0, y1, x0, x1 = box
        dx, dy, score = match_crop(gray(cur_i), gray(prev_i), box)
        if score < MIN_SCORE:
            continue

        engine = mvtools.decode_velocity(vel, meta)
        legacy = legacy_uv(vel)
        vh, vw = written.shape
        sub = (slice(y0 * vh // ch, y1 * vh // ch), slice(x0 * vw // cw, x1 * vw // cw))
        inside = written[sub]
        if inside.sum() < 500:
            continue
        edx = float(np.median(engine[:, :, 0][sub][inside]) * cw)
        edy = float(np.median(engine[:, :, 1][sub][inside]) * ch)
        ldx = float(np.median(legacy[:, :, 0][sub][inside]) * cw)
        ldy = float(np.median(legacy[:, :, 1][sub][inside]) * ch)
        rows.append((dx, dy, edx, edy, ldx, ldy))
        if len(rows) <= 12:
            print(f"{prev_i:>4}->{cur_i:<4} {dx:>12.1f} {dy:>7.1f} {score:>6.3f} | "
                  f"{edx:>10.2f} {edy:>7.2f} | {ldx:>10.2f} {ldy:>7.2f}")

    if len(rows) < 6:
        print(f"\nonly {len(rows)} pairs matched above ncc {MIN_SCORE}; not enough to report a "
              f"scale. This means the reference could not measure this footage, NOT that the "
              f"decode is wrong - the two look identical from here and must not be conflated.")
        return

    a = np.array(rows)
    span = float(np.abs(a[:, 0]).max())
    print(f"\n{len(rows)} pairs matched above ncc {MIN_SCORE}; "
          f"measured |dx| spans 0..{span:.0f} px")

    if span < 2.0:
        print("WARNING: this capture has no dynamic range. Slopes below are not meaningful.")
        return

    print("\nslope of measured against decoded (1.000 = correct), and correlation:")
    for label, xi, yi in (("engine (sqrt, Common.ush)", 2, 3), ("legacy (linear, fitted)", 4, 5)):
        sx = np.polyfit(a[:, xi], a[:, 0], 1)[0]
        sy = np.polyfit(a[:, yi], a[:, 1], 1)[0]
        rx = np.corrcoef(a[:, xi], a[:, 0])[0, 1]
        ry = np.corrcoef(a[:, yi], a[:, 1])[0, 1]
        print(f"  {label:<28} X: slope {sx:6.3f}  r {rx:+.3f}    Y: slope {sy:6.3f}  r {ry:+.3f}")

    # The magnitude sweep is what separates the two encodings. A linear decode
    # of square-root-encoded data does not fail by a constant factor - it fails
    # progressively worse as displacement grows, because that is where the two
    # curves diverge. So a decode whose ratio is FLAT across the range is the
    # right one, and this is the check a single frame cannot do.
    print("\nratio measured/decoded by displacement band (flat = correct encoding):")
    mag = np.abs(a[:, 0])
    edges = np.percentile(mag, [0, 33, 66, 100])
    print(f"{'band (px)':>16} {'n':>4} {'engine':>9} {'legacy':>9}")
    for lo, hi in zip(edges[:-1], edges[1:]):
        sel = (mag >= lo) & (mag <= hi)
        if sel.sum() < 2:
            continue
        with np.errstate(divide="ignore", invalid="ignore"):
            eng = np.nanmedian(np.where(np.abs(a[sel, 2]) > 0.5, a[sel, 0] / a[sel, 2], np.nan))
            leg = np.nanmedian(np.where(np.abs(a[sel, 4]) > 0.5, a[sel, 0] / a[sel, 4], np.nan))
        print(f"{lo:>7.1f}-{hi:<8.1f} {sel.sum():>4} {eng:>9.3f} {leg:>9.3f}")
    print("\nRead the lowest band with care: template matching resolves whole pixels only,\n"
          "so at a few px the quantisation dominates and BOTH decodes read low. That is the\n"
          "reference running out of precision, not a property of either decode - which is why\n"
          "it shows up in both columns at once. The discrimination lives in the upper bands,\n"
          "where a linear decode of square-root-encoded data must diverge and a correct one\n"
          "must stay flat.")


if __name__ == "__main__":
    main()
