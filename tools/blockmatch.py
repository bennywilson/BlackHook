"""Block matching against the captured colour frames.

The only ground truth independent of the velocity buffer: displacement measured
from the images. The decode, warp test and scale fit all start from the buffer
itself.

Block matching rather than optical flow: the subjects are large, smooth,
low-texture objects (a weapon viewmodel, the side of a train), where Farneback
reports near zero whatever the truth (the aperture problem). Block matching
measures a region's displacement directly, assuming the region is locally
rigid.

Both matchers work on the SAD volume: for every integer shift, the mean
absolute difference between the current frame and the shifted previous frame,
box-filtered over a window. The integer minimum is refined to sub-pixel with a
parabola through it and its neighbours on each axis; much of the real motion is
under a pixel.

The returned displacement is the offset from a pixel in frame N back to where
that surface was in frame N-1, as in the decode.
"""
import numpy as np

try:
    import cv2
except ImportError:  # pragma: no cover - cv2 is required by every caller
    cv2 = None


def _parabolic1(sm1, s0, sp1):
    """Scalar form: sub-pixel offset of the minimum of a parabola through
    three equally-spaced samples. 0 on a degenerate (flat/non-convex) triple."""
    denom = sm1 - 2.0 * s0 + sp1
    if denom <= 1e-9:
        return 0.0
    return float(np.clip(0.5 * (sm1 - sp1) / denom, -1.0, 1.0))


def _parabolic(sm1, s0, sp1):
    """Array form of _parabolic1, applied elementwise."""
    denom = sm1 - 2.0 * s0 + sp1
    out = np.zeros_like(s0, dtype=np.float32)
    ok = denom > 1e-9
    out[ok] = 0.5 * (sm1[ok] - sp1[ok]) / denom[ok]
    return np.clip(out, -1.0, 1.0)


def _sad_map(cur, padded, search, dx, dy, window):
    """Box-filtered |cur - prev shifted by (dx, dy)|, at full resolution.

    Shift (dy, dx) samples prev at (y+dy, x+dx), so the minimising shift is the
    current-to-previous displacement. `padded` is prev edge-replicated by
    `search` on all sides, so each shift is a slice.
    """
    h, w = cur.shape
    y0, x0 = search + dy, search + dx
    shifted = padded[y0:y0 + h, x0:x0 + w]
    return cv2.boxFilter(np.abs(cur - shifted), -1, (window, window), normalize=True)


def coarse_shift(cur, prev, mask=None, max_shift=400):
    """Integer global displacement, with no search radius limit.

    The other matchers here refine within +-`search` pixels (14 by default). On
    a title moving 60 px per frame their minimum sits on the edge of the box and
    the result is a small, wrong number: against a capture moving 27 px,
    check_decode.py once reported 1.68 px. Phase correlation fails differently:
    it measures translation only, so a camera flying forward reads as ~0.2 px.

    This uses normalised cross-correlation of a crop over a wide window, which
    has no radius limit and reports its own match quality. Returns
    (dx, dy, score); below ~0.5, do not trust the answer.
    """
    h, w = cur.shape
    if mask is not None and mask.any():
        # Centre the template on the scored region, not the frame: the written
        # velocity region is often foliage at the frame edges, whose WPO motion
        # rides on the camera's. Centred on the frame, it measures the
        # background, and the residual exceeds the refinement window.
        ys_, xs_ = np.where(mask)
        cy, cx = int(ys_.mean()), int(xs_.mean())
        hy, hx = int(h * 0.22), int(w * 0.20)
        y0, y1 = max(0, cy - hy), min(h, cy + hy)
        x0, x1 = max(0, cx - hx), min(w, cx + hx)
    else:
        y0, y1 = int(h * 0.30), int(h * 0.75)
        x0, x1 = int(w * 0.30), int(w * 0.70)
    template = cur[y0:y1, x0:x1]
    pad = min(max_shift, x0, y0, w - x1, h - y1)
    if pad < 4 or template.size == 0:
        return 0, 0, 0.0
    ys, ye, xs, xe = y0 - pad, y1 + pad, x0 - pad, x1 + pad
    result = cv2.matchTemplate(prev[ys:ye, xs:xe].astype(np.float32),
                               template.astype(np.float32), cv2.TM_CCOEFF_NORMED)
    _, score, _, loc = cv2.minMaxLoc(result)
    return (xs + loc[0]) - x0, (ys + loc[1]) - y0, float(score)


def global_shift(cur, prev, mask, search=14, max_samples=20000, seed=0, coarse=True):
    """One rigid displacement of the masked region, to sub-pixel precision.

    Ground truth for a region that is one rigid object, and the "best global
    translation" control in warp_validate.py: if one shift explains the frame as
    well as the per-pixel field, the field has shown nothing per-pixel.

    Scores the masked pixels only, not their bounding box: a weapon viewmodel
    fills ~8% of the frame but its box far more, and the static background in
    the box pulls the match toward (0, 0).

    The masked pixels are subsampled to `max_samples` (fixed seed): every pixel
    at every shift is ~10^9 operations per pair, and 20k samples localise the
    minimum as well.

    Returns (dx, dy), or None if the mask is too small or too near the border.
    """
    h, w = cur.shape
    # Centre the refinement window on a coarse estimate, so `search` limits the
    # precision of the answer, not its size. Without it, anything faster than
    # `search` would come back as ~0.
    cdx = cdy = 0
    if coarse:
        cdx, cdy, score = coarse_shift(cur, prev, mask)
        if score < 0.5:
            cdx = cdy = 0  # no trustworthy coarse estimate; fall back to centred
    # Exclude pixels whose search window would leave the image.
    margin = search + 1 + max(abs(cdx), abs(cdy))
    if 2 * margin >= min(h, w):
        return None
    inside = np.zeros_like(mask, dtype=bool)
    inside[margin:h - margin, margin:w - margin] = True
    ys, xs = np.where(mask & inside)
    if ys.size < 64:
        return None
    if ys.size > max_samples:
        pick = np.random.default_rng(seed).choice(ys.size, max_samples, replace=False)
        ys, xs = ys[pick], xs[pick]

    ref = cur[ys, xs]
    n = 2 * search + 1
    scores = np.empty((n, n), dtype=np.float64)
    for iy, dy in enumerate(range(-search, search + 1)):
        for ix, dx in enumerate(range(-search, search + 1)):
            scores[iy, ix] = np.abs(ref - prev[ys + dy + cdy, xs + dx + cdx]).mean()

    iy, ix = np.unravel_index(np.argmin(scores), scores.shape)
    dy, dx = iy - search, ix - search
    # Refine only where the minimum is inside the window. On the edge, the true
    # displacement is outside it.
    if 0 < ix < n - 1:
        dx += _parabolic1(scores[iy, ix - 1], scores[iy, ix], scores[iy, ix + 1])
    if 0 < iy < n - 1:
        dy += _parabolic1(scores[iy - 1, ix], scores[iy, ix], scores[iy + 1, ix])
    return float(dx + cdx), float(dy + cdy)


def _dense_residual(cur, prev, search, window):
    """Per-pixel displacement field, sub-pixel, from the SAD volume.

    The scale fit needs displacement measured at each pixel, so the regression
    against the decoded field varies per pixel on both axes.

    `window` is the correlation window: large enough for a smooth surface to
    localise, small enough that two objects moving differently rarely share
    one. `search` bounds the displacement; anything faster is pinned to the
    window edge and flagged.

    Returns (flow_xy, confidence, clipped):
      flow_xy    (h, w, 2) float32, pixels, current -> previous
      confidence SAD margin between the best and second-best distinct shift;
                 low means an ambiguous match (flat or repeating region)
      clipped    True where the residual is on the edge of the search window:
                 pinned, not measured. Returned because, with a coarse
                 pre-shift, the total can exceed the window and only the
                 residual can be pinned.
    """
    if cv2 is None:
        raise RuntimeError("blockmatch requires opencv-python")
    cur = np.asarray(cur, dtype=np.float32)
    prev = np.asarray(prev, dtype=np.float32)
    h, w = cur.shape
    padded = cv2.copyMakeBorder(prev, search, search, search, search, cv2.BORDER_REPLICATE)

    # Two streaming passes instead of a full SAD volume, which at these
    # resolutions would be gigabytes.
    #
    # Pass 1: running argmin over shifts.
    best = np.full((h, w), np.inf, dtype=np.float32)
    best_dx = np.zeros((h, w), dtype=np.int16)
    best_dy = np.zeros((h, w), dtype=np.int16)
    for dy in range(-search, search + 1):
        for dx in range(-search, search + 1):
            sad = _sad_map(cur, padded, search, dx, dy, window)
            better = sad < best
            best = np.where(better, sad, best)
            best_dx[better] = dx
            best_dy[better] = dy

    # Pass 2: the three SAD samples each pixel's sub-pixel fit needs, and the
    # best score outside a 3x3 neighbourhood of the winner (the ambiguity
    # margin). Both need pass 1's winner.
    s_xm = np.full((h, w), np.nan, dtype=np.float32)
    s_xp = np.full((h, w), np.nan, dtype=np.float32)
    s_ym = np.full((h, w), np.nan, dtype=np.float32)
    s_yp = np.full((h, w), np.nan, dtype=np.float32)
    second = np.full((h, w), np.inf, dtype=np.float32)
    for dy in range(-search, search + 1):
        for dx in range(-search, search + 1):
            sad = _sad_map(cur, padded, search, dx, dy, window)
            same_y, same_x = best_dy == dy, best_dx == dx
            np.copyto(s_xm, sad, where=same_y & (best_dx == dx + 1))
            np.copyto(s_xp, sad, where=same_y & (best_dx == dx - 1))
            np.copyto(s_ym, sad, where=same_x & (best_dy == dy + 1))
            np.copyto(s_yp, sad, where=same_x & (best_dy == dy - 1))
            far = (np.abs(best_dy - dy) > 1) | (np.abs(best_dx - dx) > 1)
            second = np.where(far & (sad < second), sad, second)

    # A minimum on the edge of the window is pinned, not refinable: its outer
    # neighbour was never evaluated (NaN here).
    ddx = np.where(np.isnan(s_xm) | np.isnan(s_xp), 0.0,
                   _parabolic(np.nan_to_num(s_xm), best, np.nan_to_num(s_xp)))
    ddy = np.where(np.isnan(s_ym) | np.isnan(s_yp), 0.0,
                   _parabolic(np.nan_to_num(s_ym), best, np.nan_to_num(s_yp)))

    flow = np.empty((h, w, 2), dtype=np.float32)
    flow[:, :, 0] = best_dx + ddx
    flow[:, :, 1] = best_dy + ddy
    clipped = (np.abs(best_dx) >= search) | (np.abs(best_dy) >= search)
    return flow, (second - best).astype(np.float32), clipped


def dense(cur, prev, search=8, window=17, levels=4):
    """Per-pixel displacement field, coarse-to-fine over an image pyramid.

    A single level searches +-`search` around zero, so `search` limits how far a
    pixel can move. On a title moving 15-70 px per frame every fast pixel is
    pinned and discarded, and the scale fit sees only the slowest 1% (slope
    0.73 where a correct decode gives 1.000). A single global pre-shift does not
    fix it: a pixel moving differently from the scene, which is what velocity
    is written for, is still pinned. Warping by the upsampled flow at each level
    gives every pixel its own coarse estimate.

    Reach is about search * (2^levels - 1): ~120 px at the defaults, with the
    same sub-pixel precision at level 0.

    Returns (flow_xy, confidence, clipped) as the single-level matcher does;
    `clipped` refers to the residual at the finest level.
    """
    if cv2 is None:
        raise RuntimeError("blockmatch requires opencv-python")
    cur = np.asarray(cur, dtype=np.float32)
    prev = np.asarray(prev, dtype=np.float32)

    pyr_cur, pyr_prev = [cur], [prev]
    for _ in range(max(0, levels - 1)):
        if min(pyr_cur[-1].shape) < 4 * window:
            break  # below this the correlation window covers the whole image
        pyr_cur.append(cv2.pyrDown(pyr_cur[-1]))
        pyr_prev.append(cv2.pyrDown(pyr_prev[-1]))

    flow = None
    confidence = clipped = None
    for level in range(len(pyr_cur) - 1, -1, -1):
        c, p = pyr_cur[level], pyr_prev[level]
        h, w = c.shape
        if flow is None:
            flow = np.zeros((h, w, 2), dtype=np.float32)
        else:
            # Upsample the coarser level's answer and double it: one pixel of
            # displacement at half resolution is two at full.
            flow = cv2.resize(flow, (w, h), interpolation=cv2.INTER_LINEAR) * 2.0

        # Warp prev by the estimate so far, so the search only has to find what
        # is left over. Sampling at (x + flow) matches _sad_map's convention,
        # where the minimising shift is the current -> previous displacement.
        gx, gy = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
        warped = cv2.remap(p, gx + flow[:, :, 0], gy + flow[:, :, 1],
                           cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)

        residual, confidence, clipped = _dense_residual(c, warped, search, window)
        flow = flow + residual

    return flow, confidence, clipped
