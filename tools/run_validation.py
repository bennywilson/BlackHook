"""Run every offline check against a dump and write results/<tag>/validation.txt.

One command runs every tool and writes its output, failures included, into
results/<tag>/, so every number in the README can be traced to a file.

Usage:  python run_validation.py [dump_dir] [tag]

`tag` names the folder under results/. Use the engine version (`ue5.7`) for a
tracked reference run, and any other tag for a local run that git ignores.
Without a tag, the output goes to results/untagged/.
"""
import os
import subprocess
import sys

import paths
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))

dump_dir = sys.argv[1] if len(sys.argv) > 1 else paths.DEFAULT_DUMP
tag = sys.argv[2] if len(sys.argv) > 2 else ""
results = paths.results_dir(tag)
out_path = os.path.join(results, "validation.txt")
figure_path = os.path.join(results, "warp_validation.png")
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
     "mismatched frame's field."),
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
