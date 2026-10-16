"""Writes a manifest.json for a dump captured before the hook wrote one.

`mvtools.load_dump()` refuses a dump without manifest.json; this reconstructs
one from the dump's meta.txt, meta_depth.txt and frames.csv. It records only
what those files hold. Fields the hook observes while capturing (the build,
start and seal times, fence coverage, why the burst ended) are omitted or
marked, and `reconstructed` at the top says the manifest was assembled after
the fact.

Usage:  python legacy_meta.py <dump_dir>

Writes <dump_dir>/manifest.json (via a .tmp file and a rename). Refuses if a
manifest.json already exists.
"""
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mvtools  # noqa: E402


# The per-surface facts from capture.cpp's `kCaptureFacts`: how the hook works,
# the same for every dump.
STATIC_FACTS = {
    "velocity": {"trigger": "render_target_to_shader_resource", "arbitration": "first_in_frame",
                 "file_pattern": "vel_%05d.bin"},
    "depth": {"trigger": "depth_write_to_readable", "arbitration": "last_before_surface",
              "file_pattern": "depth_%05d.bin"},
    "color": {"trigger": "present_back_buffer", "arbitration": "first_in_frame",
              "file_pattern": "color_%05d.bin"},
}


def _layout_surface(meta, prefix, plane_prefix=None):
    """One surfaces[name] entry for the layouts[0] block, from meta's flat keys."""
    fmt = meta.get(f"{prefix}_format")
    if fmt is None:
        return None
    bytes_ = meta[f"{prefix}_bytes"]
    subresources = meta[f"{prefix}_subresources"]
    if plane_prefix is None:
        # Single-plane surface (velocity, colour): flat width/height/row_pitch
        # keys, no plane index.
        planes = [{
            "index": 0, "offset": 0,
            "width": meta[f"{prefix}_width"], "height": meta[f"{prefix}_height"],
            "row_pitch": meta[f"{prefix}_row_pitch"], "format": fmt,
        }]
    else:
        planes = []
        i = 0
        while f"{plane_prefix}{i}_format" in meta:
            planes.append({
                "index": i,
                "offset": meta[f"{plane_prefix}{i}_offset"],
                "width": meta[f"{plane_prefix}{i}_width"],
                "height": meta[f"{plane_prefix}{i}_height"],
                "row_pitch": meta[f"{plane_prefix}{i}_row_pitch"],
                "format": meta[f"{plane_prefix}{i}_format"],
            })
            i += 1
    return {"format": fmt, "bytes": bytes_, "subresources": subresources, "planes": planes}


def build_manifest(dump_dir):
    meta_path = os.path.join(dump_dir, "meta.txt")
    if not os.path.exists(meta_path):
        raise ValueError(f"{dump_dir} has no meta.txt - nothing to backfill from")

    meta = {}
    for name in ("meta.txt", "meta_depth.txt"):
        path = os.path.join(dump_dir, name)
        if not os.path.exists(path):
            continue
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                meta[k] = int(v)

    has_depth = "depth_format" in meta
    velocity_count = len(mvtools.frame_indices(dump_dir))
    color_count = sum(1 for n in os.listdir(dump_dir) if n.startswith("color_") and n.endswith(".bin"))
    depth_count = sum(1 for n in os.listdir(dump_dir) if n.startswith("depth_") and n.endswith(".bin"))

    surfaces = {}
    for name, prefix, plane_prefix, present in (
            ("velocity", "velocity", None, True),
            ("color", "color", None, True),
            ("depth", "depth", "depth_plane", has_depth)):
        obj = {"present": present, "required": name != "depth", **STATIC_FACTS[name]}
        if not present:
            obj["reason"] = "never_identified"
        if name == "color":
            obj["source"] = {"kind": "known_by_construction",
                              "mechanism": "IDXGISwapChain3::GetBuffer(GetCurrentBackBufferIndex())"}
        else:
            obj["source"] = {"kind": "identified"}
        # Old dumps recorded no fence coverage; none is reported.
        obj["coverage_note"] = "not recorded - this dump predates per-surface fence coverage tallies"
        surfaces[name] = obj

    layouts_surfaces = {}
    vel_layout = _layout_surface(meta, "velocity")
    if vel_layout:
        layouts_surfaces["velocity"] = vel_layout
    color_layout = _layout_surface(meta, "color")
    if color_layout:
        layouts_surfaces["color"] = color_layout
    if has_depth:
        depth_layout = _layout_surface(meta, "depth", plane_prefix="depth_plane")
        if depth_layout:
            layouts_surfaces["depth"] = depth_layout

    manifest = {
        "schema_version": 1,
        "reconstructed": {
            "by": "tools/legacy_meta.py",
            "reason": "this dump predates manifest.json (docs/REFACTOR_PLAN.md Phase 4); every field "
                       "below was derived from meta.txt/meta_depth.txt/frames.csv rather than recorded "
                       "live by the capture that produced these blobs",
        },
        "producer": {"tool": "mv_hook", "commit": None, "configured_utc": None},
        "session": {
            "process": None,
            "started_utc": None,
            "sealed_utc": None,
            "profile": "default",
            "back_buffer": {"width": meta["color_width"], "height": meta["color_height"],
                             "format": meta["color_format"]},
        },
        "engine": (
            {"major": meta["engine_version_major"], "minor": meta["engine_version_minor"],
             "source": "MV_ENGINE_VERSION"}
            if "engine_version_major" in meta else None),
        "burst": {
            "requested_frames": None,
            "recorded_frames": velocity_count,
            "drained_frames": velocity_count,
            "ended_because": "unknown_pre_manifest_capture",
        },
        "surfaces": surfaces,
        "layouts": [{"epoch": 0, "surfaces": layouts_surfaces}],
        "frames": {
            "file": "frames.csv",
            "columns": ["capture_index", "game_frame_index"],
            "note": "2-column legacy format",
        },
        "integrity": {
            # The capture that produced these blobs finished; "sealed" means
            # that here.
            "sealed": True,
            "required_surfaces": ["velocity", "color"] + (["depth"] if has_depth else []),
            "dropped_frames_backpressure": None,
            "frames_without_velocity": None,
            "incomplete_frames_dropped": None,
            "slots_missed_busy": None,
            "writes_dropped": None,
        },
    }
    return manifest


def main():
    if len(sys.argv) != 2:
        print("usage: python legacy_meta.py <dump_dir>", file=sys.stderr)
        return 1
    dump_dir = sys.argv[1]
    out_path = os.path.join(dump_dir, "manifest.json")
    if os.path.exists(out_path):
        print(f"refusing to overwrite existing {out_path} - this is a one-time backfill, "
              f"delete it first if you want to regenerate", file=sys.stderr)
        return 1

    manifest = build_manifest(dump_dir)
    fd, tmp_path = tempfile.mkstemp(dir=dump_dir, prefix="manifest.json.", suffix=".tmp")
    with os.fdopen(fd, "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    os.replace(tmp_path, out_path)  # atomic on the same filesystem, same guarantee as the hook's own write
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
