"""Run every offline check against a dump and write results/validation.txt.

The point of this script is reproducibility. The validation numbers quoted in
README.md and DEBUGGING.md were, for most of this project's life, produced by
script revisions that were never committed - at one point the figure in
results/ had panel labels that matched no code in the repo. A reader had no way
to check any of it without a capture of their own, and neither did I after the
capture that produced them was overwritten.

So: one command, every tool, output captured verbatim into results/, including
the failures. If a number appears in the write-up it should be findable here.

Usage:  python run_validation.py [dump_dir] [tag]

`tag` names the title the dump came from and is appended to every output file
(results/validation_<tag>.txt, warp_validation_<tag>.png, ...). It exists
because this project now covers more than one game, and the first runs against
a second title overwrote the first title's committed results - the same class
of loss as the capture that was overwritten and could not be regenerated, and
harder to notice, because the file still exists and still looks plausible.
Omit it for the original results/validation.txt.
"""
import os
import subprocess
import sys

import paths
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
RESULTS = os.path.normpath(os.path.join(TOOLS, "..", "results"))

dump_dir = sys.argv[1] if len(sys.argv) > 1 else paths.DEFAULT_DUMP
tag = sys.argv[2] if len(sys.argv) > 2 else ""
suffix = f"_{tag}" if tag else ""
os.makedirs(RESULTS, exist_ok=True)
out_path = os.path.join(RESULTS, f"validation{suffix}.txt")
figure_path = os.path.join(RESULTS, f"warp_validation{suffix}.png")
# Tools that write their own figures read this rather than taking a path.
os.environ["MV_RESULT_TAG"] = tag

STEPS = [
    ("inspect_velocity.py", ["inspect_velocity.py", dump_dir],
     "What is in the capture at all: coverage, motion magnitude, and the\n"
     "reassembled V.z from channels 2/3."),
    ("check_decode.py", ["check_decode.py", dump_dir],
     "The decode against independently measured image motion (block matching),\n"
     "with the project's old linear decode alongside for comparison."),
    ("derive_scale.py", ["derive_scale.py", dump_dir],
     "Per-pixel regression of decoded against block-matched displacement, plus\n"
     "a fit of the encoding exponent that does not assume the answer."),
    ("region_match.py", ["region_match.py", dump_dir],
     "The same comparison per REGION rather than per pixel, against wide-range\n"
     "cross-correlation of the game's own frames. Weaker than derive_scale.py by\n"
     "construction, but it is the one that works on footage where per-pixel\n"
     "matching is defeated by motion blur and thin foliage - and its magnitude\n"
     "sweep is what discriminates the square-root encoding from a linear one."),
    ("warp_validate.py", ["warp_validate.py", dump_dir, figure_path],
     "Warp frame N-1 by frame N's field and score it, against three controls:\n"
     "a sign flip, the best possible single rigid translation, and a\n"
     "deliberately mismatched frame's field."),
]


def main():
    with open(out_path, "w", encoding="utf-8") as out:
        out.write("SceneVelocity extraction - offline validation\n")
        out.write("=" * 72 + "\n")
        out.write(f"generated: {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
        out.write(f"dump:      {dump_dir}\n")
        out.write(f"title:     {tag or '(untagged)'}\n")
        out.write("command:   python tools/run_validation.py <dump_dir> [tag]\n")
        out.write("\nThis file is generated verbatim from the committed tools. Nothing here\n")
        out.write("is hand-edited, including the parts that fail.\n")

        failures = 0
        for name, argv, blurb in STEPS:
            header = f"\n\n{'=' * 72}\n{name}\n{'=' * 72}\n{blurb}\n{'-' * 72}\n"
            out.write(header)
            print(f"running {name} ...", flush=True)
            proc = subprocess.run(
                [sys.executable] + argv, cwd=TOOLS, capture_output=True, text=True)
            out.write(proc.stdout)
            if proc.stderr.strip():
                out.write("\n--- stderr ---\n")
                out.write(proc.stderr)
            if proc.returncode != 0:
                failures += 1
                out.write(f"\n[exit code {proc.returncode}]\n")
        out.write(f"\n\n{'=' * 72}\n{len(STEPS) - failures}/{len(STEPS)} tools ran to completion.\n")

    print(f"\nwrote {out_path}")


if __name__ == "__main__":
    main()
