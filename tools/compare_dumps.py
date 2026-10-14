"""Are two capture dumps equivalent?

The regression oracle for the Surface migration (docs/REFACTOR_PLAN.md): every
phase that is supposed to change no behaviour is verified by capturing a dump
before and after and running this.

WHAT IS AND IS NOT REPRODUCIBLE, because it decides how this compares:

A dump is NOT byte-identical run to run, and no refactor will make it so. Which
frames land depends on slot contention - all four ring slots being in flight
makes the hook skip a frame by design (see g_noFreeSlot in capture.cpp) - and
that is a function of GPU/CPU timing, not of the code under test. Two runs of
mv_testhost at 240 frames were measured at 84 and 86 captured frames.

What IS reproducible is the CONTENT of a given game frame. Measured across two
runs sharing 61 game frames: velocity, depth and colour all byte-identical, 61
of 61, for all three. So the comparison keys on the game frame index that
frames.csv records - not on the capture index, which is just a counter of what
was drained and means nothing across runs.

Consequence worth stating: this proves equivalence on the frames both runs
happened to capture. A phase that broke capture for, say, only the first frame
after a reopen could hide in the frames one run missed. The intersection size is
therefore printed and gated - a comparison over a handful of frames is reported
as WEAK rather than passed.

Usage:
    python compare_dumps.py <before_dir> <after_dir>
    python compare_dumps.py <before_dir> <after_dir> --min-common 40

Exit code 0 if equivalent, 1 if not, 2 if the comparison was too weak to mean
anything.
"""
import argparse
import hashlib
import json
import os
import sys

def load_manifest(dump_dir):
    """manifest.json as a dict, or None. Written only when a burst completes."""
    path = os.path.join(dump_dir, "manifest.json")
    if not os.path.exists(path):
        return None
    with open(path) as f:
        return json.load(f)


def surfaces(*manifests):
    """(name, file prefix) for every surface either manifest lists, from its file_pattern."""
    out = {}
    for m in manifests:
        for name, s in (m or {}).get("surfaces", {}).items():
            out.setdefault(name, s["file_pattern"].split("_%")[0])
    return sorted(out.items())


def load_frame_map(dump_dir):
    """game frame index -> capture index.

    Inverted deliberately. The capture index is a counter of what this run
    managed to drain and is not comparable across runs; the game frame index is
    the engine's own clock and is.
    """
    path = os.path.join(dump_dir, "frames.csv")
    if not os.path.exists(path):
        return None
    out = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(",")
            if len(parts) < 2:
                continue
            capture, frame = int(parts[0]), int(parts[1])
            out[frame] = capture
    return out


def blob_hash(dump_dir, prefix, capture_index):
    path = os.path.join(dump_dir, f"{prefix}_{capture_index:05d}.bin")
    if not os.path.exists(path):
        return None, 0
    with open(path, "rb") as f:
        data = f.read()
    return hashlib.sha256(data).hexdigest(), len(data)


# manifest.json fields that differ between two runs of the SAME build, so
# comparing them would make the oracle fail on every run (measured: it did,
# from Phase 4 until this list existed). Two kinds:
#   - provenance: which build/profile, and when. Differs by design across the
#     change under test; shown, not judged. A profile that changes behaviour
#     shows up in the blobs and in the fields that ARE compared.
#   - timing: how many frames the writer drained vs dropped under backpressure
#     is GPU/CPU timing, the same reason frames are keyed on game frame index.
RUN_VARYING = {
    "producer.commit", "producer.configured_utc", "producer.built",
    "session.started_utc", "session.sealed_utc",
    "session.profile", "session.profile_source",
    "burst.drained_frames",
    "integrity.dropped_frames_backpressure", "integrity.slots_missed_busy",
}


def flatten(meta, prefix=""):
    """{"a": {"b": 1}} -> {"a.b": 1}. Lists stay whole values."""
    out = {}
    for key, value in meta.items():
        path = f"{prefix}{key}"
        if isinstance(value, dict) and value:
            out.update(flatten(value, path + "."))
        else:
            out[path] = value
    return out


def diff_metadata(before_meta, after_meta):
    """Returns (problems, run_varying): human-readable lines for differences
    that count against equivalence, and for those that don't (RUN_VARYING)."""
    problems, varying = [], []
    if before_meta is None or after_meta is None:
        problems.append(
            f"manifest.json missing: before={before_meta is not None} after={after_meta is not None}"
            " (written when a burst SEALS - a short testhost run never gets there)")
        return problems, varying
    # A surface entry with present:false describes data that does not exist -
    # e.g. normals, which a newer hook lists as "not_enabled" and an older one
    # does not list at all. Neither side has blobs for it, so it is not dump
    # content; shown, not compared. A PRESENT surface is compared in full.
    for side, meta in (("before", before_meta), ("after", after_meta)):
        for name, surface in list(meta.get("surfaces", {}).items()):
            if isinstance(surface, dict) and not surface.get("present", False):
                varying.append(f"  surfaces.{name}: not present in {side} "
                               f"({surface.get('reason', 'no reason given')}) - not compared")
    def absent(meta, key):
        parts = key.split(".")
        if len(parts) < 2 or parts[0] != "surfaces":
            return False
        surface = meta.get("surfaces", {}).get(parts[1])
        return surface is None or (isinstance(surface, dict) and not surface.get("present", False))
    b_flat, a_flat = flatten(before_meta), flatten(after_meta)
    for key in sorted(set(b_flat) | set(a_flat)):
        if absent(before_meta, key) and absent(after_meta, key):
            continue
        b, a = b_flat.get(key), a_flat.get(key)
        if b != a:
            (varying if key in RUN_VARYING else problems).append(f"  {key}: {b!r} -> {a!r}")
    return problems, varying


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("before")
    parser.add_argument("after")
    parser.add_argument(
        "--min-common", type=int, default=25,
        help="fewest shared game frames for the comparison to mean anything (default 25)")
    args = parser.parse_args()

    for d in (args.before, args.after):
        if not os.path.isdir(d):
            print(f"FAIL - not a directory: {d}")
            return 1

    print(f"before: {args.before}")
    print(f"after:  {args.after}")
    print()

    # --- metadata: must match exactly, outside RUN_VARYING -----------------
    before_manifest, after_manifest = load_manifest(args.before), load_manifest(args.after)
    meta_problems, meta_varying = diff_metadata(before_manifest, after_manifest)
    if meta_problems:
        print("METADATA DIFFERS:")
        for p in meta_problems:
            print(p)
    else:
        print("metadata: identical")
    if meta_varying:
        print("run-varying (provenance/timing - shown, not compared):")
        for p in meta_varying:
            print(p)

    # --- frame maps ---------------------------------------------------------
    before_map = load_frame_map(args.before)
    after_map = load_frame_map(args.after)
    if before_map is None or after_map is None:
        print("FAIL - frames.csv missing; there is no way to line up frames between runs "
              "without it, and comparing by capture index would compare different game frames.")
        return 1

    common = sorted(set(before_map) & set(after_map))
    print(f"frames: before={len(before_map)} after={len(after_map)} common={len(common)}")
    if common:
        print(f"        game frame range: before {min(before_map)}..{max(before_map)}, "
              f"after {min(after_map)}..{max(after_map)}")
    print()

    # --- content on the intersection ----------------------------------------
    content_ok = True
    any_surface = False
    for name, prefix in surfaces(before_manifest, after_manifest):
        identical = differing = missing_before = missing_after = 0
        first_diff = None
        for frame in common:
            hb, nb = blob_hash(args.before, prefix, before_map[frame])
            ha, na = blob_hash(args.after, prefix, after_map[frame])
            if hb is None and ha is None:
                continue
            if hb is None:
                missing_before += 1
                continue
            if ha is None:
                missing_after += 1
                continue
            if hb == ha:
                identical += 1
            else:
                differing += 1
                if first_diff is None:
                    first_diff = (frame, nb, na)
        if identical == differing == missing_before == missing_after == 0:
            continue  # surface not present in either dump
        any_surface = True

        status = "ok" if (differing == 0 and missing_before == 0 and missing_after == 0) else "DIFFERS"
        print(f"{name:9s} {status:8s} identical={identical:4d} differing={differing:4d} "
              f"only-in-after={missing_before:3d} only-in-before={missing_after:3d}")
        if first_diff:
            frame, nb, na = first_diff
            print(f"          first difference at game frame {frame}: "
                  f"{nb} bytes before, {na} bytes after")
        if status == "DIFFERS":
            content_ok = False

    if not any_surface:
        print("FAIL - no surface blobs found in either dump")
        return 1

    print()
    # --- verdict ------------------------------------------------------------
    if meta_problems or not content_ok:
        print("RESULT: DIFFERENT - this change altered the dump.")
        return 1
    if len(common) < args.min_common:
        print(f"RESULT: WEAK - equivalent on {len(common)} shared game frames, which is fewer "
              f"than --min-common={args.min_common}. That is not enough to conclude much; "
              f"re-run with more frames rather than treating this as a pass.")
        return 2
    print(f"RESULT: EQUIVALENT - metadata identical (outside {len(meta_varying)} run-varying "
          f"field(s) above), and all {len(common)} shared game frames byte-identical across "
          f"every surface.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
