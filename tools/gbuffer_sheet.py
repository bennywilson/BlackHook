"""Contact sheets of the 8-bit and 10-bit surfaces, raw channels, no decode.

    python gbuffer_sheet.py <dump_dir> [--frames 6] [--out sheet.png]
        One row per frame: colour | normals | specular RGB | specular alpha
        (whichever of normals and specular the dump has), frames spread evenly.
    python gbuffer_sheet.py --snapshots <snapshot_*.bin ...> [--scale 2] [--out snaps.png]
        MV_SNAPSHOT candidates: one row each, RGB | alpha (stretched), with
        per-channel statistics. How GBufferB was told from GBufferC.

numpy only (no OpenCV); writes the PNG itself. Default output: temp/out/.
"""
import argparse
import os
import re
import struct
import sys
import zlib

import numpy as np

import mvtools
from paths import out_path

R10G10B10A2 = 24
BGRA = {87, 90, 91}  # B8G8R8A8_UNORM, _TYPELESS, _UNORM_SRGB; RGBA otherwise


def to_rgb_alpha(tight, width, height, fmt):
    """(h, w*4) bytes -> (rgb uint8 (h, w, 3), alpha uint8 (h, w))."""
    if fmt == R10G10B10A2:
        px = tight.view(np.uint32).reshape(height, width)
        rgb = (np.stack([(px >> s) & 0x3FF for s in (0, 10, 20)], -1) * (255 / 1023)).astype(np.uint8)
        return rgb, ((px >> 30) * 85).astype(np.uint8)
    p = tight.reshape(height, width, 4)
    return (p[..., [2, 1, 0]] if fmt in BGRA else p[..., :3]), p[..., 3]


def stretch(a):
    lo, hi = int(a.min()), int(a.max())
    v = ((a.astype(np.float32) - lo) / max(1, hi - lo) * 255).astype(np.uint8)
    return np.repeat(v[..., None], 3, -1)


def fit(img, w, h):
    ys = (np.arange(h) * img.shape[0] / h).astype(int)
    xs = (np.arange(w) * img.shape[1] / w).astype(int)
    return img[ys][:, xs]


def write_png(path, rgb):
    rgb = np.ascontiguousarray(rgb)
    h, w, _ = rgb.shape
    raw = b"".join(b"\x00" + rgb[y].tobytes() for y in range(h))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def surface_image(dump, name, index):
    s = dump.surface(name)
    tight = mvtools._rows(os.path.join(dump.dir, s.file_pattern % index), s.width, s.height, s.row_pitch, 4)
    return to_rgb_alpha(tight, s.width, s.height, s.format)


def dump_sheet(dump_dir, count):
    dump = mvtools.load_dump(dump_dir)
    gbuffers = [n for n in ("normals", "specular") if dump.has(n)]
    if not gbuffers:
        sys.exit(f"{dump_dir}: neither normals nor specular was captured")
    have = lambda n: {int(f[len(n) + 1:len(n) + 6]) for f in os.listdir(dump_dir) if f.startswith(n + "_")}
    frames = sorted(set.intersection(*(have(n) for n in gbuffers)))
    picks = sorted({frames[round(i * (len(frames) - 1) / max(1, count - 1))] for i in range(min(count, len(frames)))})
    ref = dump.surface(gbuffers[0])
    w, h = ref.width // 2, ref.height // 2
    rows = []
    for i in picks:
        tiles = [surface_image(dump, "color", i)[0]]
        for n in gbuffers:
            rgb, alpha = surface_image(dump, n, i)
            tiles += [rgb, stretch(alpha)] if n == "specular" else [rgb]
        rows.append(np.pad(np.concatenate([fit(t, w, h) for t in tiles], 1), ((0, 6), (0, 0), (0, 0))))
    return np.concatenate(rows, 0), f"frames {picks}: colour | " + " | ".join(
        "specular RGB | specular alpha" if n == "specular" else n for n in gbuffers)


def snapshot_sheet(files, scale):
    rows, notes = [], []
    for f in files:
        m = re.search(r"_(\d+)x(\d+)_fmt(\d+)_pitch(\d+)\.bin$", f)
        if m is None:
            sys.exit(f"{f}: not an MV_SNAPSHOT file name")
        w, h, fmt, pitch = map(int, m.groups())
        rgb, alpha = to_rgb_alpha(mvtools._rows(f, w, h, pitch, 4), w, h, fmt)
        rows.append(np.pad(np.concatenate([rgb, stretch(alpha)], 1)[::scale, ::scale], ((0, 4), (0, 0), (0, 0))))
        stats = " | ".join(f"{c} mean={rgb[..., k].mean():6.1f} sd={rgb[..., k].std():5.1f}" for k, c in enumerate("RGB"))
        values = np.unique(alpha)
        notes.append(f"{os.path.basename(f)[:44]}  {stats} | A {len(values)} distinct {values[:8].tolist()}")
    return np.concatenate(rows, 0), "\n".join(notes)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("inputs", nargs="+", help="a dump directory, or snapshot files with --snapshots")
    parser.add_argument("--snapshots", action="store_true")
    parser.add_argument("--frames", type=int, default=6)
    parser.add_argument("--scale", type=int, default=2, help="--snapshots: keep every Nth pixel")
    parser.add_argument("--out")
    args = parser.parse_args()
    if args.snapshots:
        image, note = snapshot_sheet(args.inputs, args.scale)
    else:
        image, note = dump_sheet(args.inputs[0], args.frames)
    path = args.out or out_path("snapshots.png" if args.snapshots else "gbuffer_sheet.png")
    write_png(path, image)
    print(note)
    print(path)


if __name__ == "__main__":
    main()
