"""Render the captured motion field as a video alongside the gameplay.

Three panels: the game's own back buffer, the extracted SceneVelocity field on
its own, and the field composited over the frame. Panel 2 is deliberately
shown raw rather than only as an overlay. SceneVelocity holds velocity for
*dynamic and world-position-offset geometry only* - 11-13% of pixels in the
station capture (the train, background characters and signage, and the weapon
viewmodel), 7-9% in the corridor capture where the viewmodel is the only thing
moving, ~21% in the Skyrunner flight captures. Static-geometry motion is
reconstructed from camera matrices inside the consuming TSR/motion-blur shader
and is genuinely not in this buffer, so a mostly-black panel is the honest
picture of its contents rather than a bug in the visualisation.

The hue-for-direction colouring here is the optical-flow (Middlebury)
convention, imported by us. Unreal draws nothing like it, so this view has no
engine ground truth to be checked against - the in-game overlay deliberately
shows the raw channel mapping instead. See DEBUGGING.md.

Usage:  python make_video.py <dump_dir> [out.mp4] [--preview] [--still N]
        [--still N]             write one PNG of frame N (or the middle frame if
                                N is omitted) instead of a video - this is what
                                produces the README's lead figure
"""
import os
import sys

import cv2
import numpy as np

import mvtools
import paths

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = [a for a in sys.argv[1:] if a.startswith("--")]

dump_dir = args[0] if args else paths.DEFAULT_DUMP
out_path = args[1] if len(args) > 1 else paths.out_path("motion_field.mp4")
preview_only = "--preview" in flags
still_index = None
still_only = "--still" in sys.argv
if still_only:
    i = sys.argv.index("--still")
    if i + 1 < len(sys.argv) and not sys.argv[i + 1].startswith("--"):
        still_index = int(sys.argv[i + 1])
        if str(still_index) in args:
            args.remove(str(still_index))

dump = mvtools.load_dump(dump_dir)
meta = dump.meta
indices = mvtools.frame_indices(dump_dir)
cw, ch = dump.surface("color").width, dump.surface("color").height
vw, vh = dump.surface("velocity").width, dump.surface("velocity").height

# Target a fixed panel width rather than a fixed downscale ratio, so panel
# (and text) size stays legible regardless of capture resolution. 640 was
# chosen to match this project's native ~1708px-wide captures, which used a
# fixed 0.36 ratio (-> ~615px panels); a 960px capture at that same ratio
# gives ~345px panels, small enough that label text visibly pixelates.
target_panel_width = 640
scale = min(1.0, target_panel_width / cw)
pw, ph = max(1, int(cw * scale)), max(1, int(ch * scale))


def colour_wheel(size):
    """Legend: hue = direction of motion, brightness = magnitude."""
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    cx = cy = (size - 1) / 2.0
    dx, dy = xx - cx, yy - cy
    r = np.sqrt(dx * dx + dy * dy) / cx
    ang = np.arctan2(dy, dx)
    hsv = np.zeros((size, size, 3), np.float32)
    hsv[:, :, 0] = (ang + np.pi) / (2 * np.pi) * 179.0
    hsv[:, :, 1] = 255.0
    hsv[:, :, 2] = np.clip(r, 0, 1) * 255.0
    bgr = cv2.cvtColor(hsv.astype(np.uint8), cv2.COLOR_HSV2BGR)
    bgr[r > 1] = 0
    return bgr


def label(img, text, colour=(255, 255, 255)):
    # Font size is derived from the panel's own width rather than fixed, so it
    # still fits at a low capture resolution. 0.58 @ 2px was tuned against this
    # project's native-resolution captures (~615px-wide panels); at 960x540
    # panels are only ~345px wide and the fixed size ran text off the edge.
    w = img.shape[1]
    font_scale = max(0.30, min(0.58, w / 615.0 * 0.58))
    thickness = 2 if font_scale >= 0.45 else 1
    bar_h = max(20, int(round(30 * font_scale / 0.58)))
    cv2.rectangle(img, (0, 0), (w, bar_h), (0, 0, 0), -1)
    cv2.putText(img, text, (10, bar_h - 8), cv2.FONT_HERSHEY_SIMPLEX, font_scale, colour, thickness)
    return img


def caption_bar(width, text):
    bar = np.zeros((26, width, 3), np.uint8)
    cv2.putText(bar, text, (10, 18), cv2.FONT_HERSHEY_SIMPLEX, 0.45, (185, 185, 185), 1)
    return bar


print(f"dump:   {dump_dir}  ({len(indices)} frames)")

wheel = colour_wheel(max(46, ph // 7))

panel_titles = ("game frame", "extracted motion field", "composited")
caption = ("SceneVelocity holds dynamic and world-position-offset geometry only; static-geometry "
           "motion is reconstructed by the consuming shader and is not in this buffer")


def fields_for(index):
    """(uv, written_mask) for one frame, in UV units."""
    vel_path = os.path.join(dump_dir, f"vel_{index:05d}.bin")
    flow = mvtools.decode_velocity(vel_path, meta)
    written = mvtools.velocity_written_mask(vel_path, meta)
    uv = flow.copy()
    uv[~written] = 0.0
    return uv, written


# One magnitude scale for the whole clip, so brightness is comparable frame to
# frame rather than silently renormalising per frame - but derived from this
# capture instead of hardcoded, because captures can differ by more than an
# order of magnitude in displacement and a constant tuned for one renders the
# other as a flat white or a flat black.
probe = indices[::max(1, len(indices) // 6)][:6]
mags = []
for index in probe:
    uv, written = fields_for(index)
    px = uv[:, :, 0] * vw
    py = uv[:, :, 1] * vh
    m = np.sqrt(px * px + py * py)
    mags.append(np.percentile(m[written], 99) if written.any() else 0.0)
MAX_PIXELS = float(max(np.median(mags), 1e-3))
print(f"scale:  brightness saturates at {MAX_PIXELS:.1f} px/frame "
      f"(p99 over {len(probe)} probe frames, fixed for the whole clip)")


def render(index):
    color = mvtools.load_color(os.path.join(dump_dir, f"color_{index:05d}.bin"), meta)
    uv, written = fields_for(index)
    bgr = (mvtools.to_display(color)[:, :, ::-1] * 255).astype(np.uint8)

    field_rgb = mvtools.flow_to_rgb(uv, meta, max_pixels=MAX_PIXELS)
    # Force unwritten pixels to actual black rather than leaving them at
    # flow_to_rgb's brightness floor. The floor exists so slow-moving written
    # pixels do not vanish; applied to unwritten ones it paints "no data" in the
    # same colour vocabulary as data, and which pixels the engine wrote is
    # exactly what this panel is for.
    field_rgb[~written] = 0

    # Velocity is at render resolution (DRS); the back buffer is at output
    # resolution - resize up before compositing or laying out.
    field_full = cv2.resize(field_rgb, (cw, ch), interpolation=cv2.INTER_NEAREST)
    mask_full = cv2.resize(written.astype(np.uint8), (cw, ch), interpolation=cv2.INTER_NEAREST) > 0
    composited = bgr.copy()
    composited[mask_full] = field_full[mask_full]

    coverage = written.mean() * 100.0
    p1 = label(cv2.resize(bgr, (pw, ph)), panel_titles[0])
    p2 = label(cv2.resize(field_full, (pw, ph)), f"{panel_titles[1]} ({coverage:.1f}% written)")
    p3 = label(cv2.resize(composited, (pw, ph)), panel_titles[2])

    # Drop the legend into the panel that carries the field.
    s = wheel.shape[0]
    y0, x0 = ph - s - 8, pw - s - 8
    p2[y0:y0 + s, x0:x0 + s] = np.maximum(p2[y0:y0 + s, x0:x0 + s], wheel)
    cv2.putText(p2, "dir", (x0, y0 - 5), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (200, 200, 200), 1)

    frame = np.vstack([p1, p2, p3])
    # Caption on its own bar so it stays readable over bright scene content.
    return np.vstack([frame, caption_bar(frame.shape[1], caption)])


if still_only:
    index = still_index if still_index is not None else indices[len(indices) // 2]
    if index not in indices:
        print(f"frame {index} is not in this dump; frames are {indices[0]}..{indices[-1]}")
        sys.exit(1)
    still_path = out_path if out_path.lower().endswith(".png") else paths.out_path("motion_field_still.png")
    cv2.imwrite(still_path, render(index))
    print(f"wrote {still_path} (frame {index})")
    sys.exit(0)

if preview_only:
    for index in indices[:2]:
        cv2.imwrite(paths.out_path(f"preview_{index:05d}.png"), render(index))
    print("wrote preview PNGs")
    sys.exit(0)

writer = cv2.VideoWriter(out_path, cv2.VideoWriter_fourcc(*"mp4v"), 60, (pw, ph * 3 + 26))
for index in indices:
    writer.write(render(index))
writer.release()
print(f"wrote {out_path} ({len(indices)} frames)")
