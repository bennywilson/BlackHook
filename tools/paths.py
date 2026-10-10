"""Where everything this repo generates lives: <repo>/temp/, gitignored.

Mirrors the hook's OutputDir() (hook/src/logging.h), which bakes the same
<repo>/temp path in at CMake configure time:

    temp/build/   CMake build tree
    temp/logs/    mv_hook.log, mv_barrier_profile.log, other mv_*.log
    temp/dumps/   mv_dump/ (the hook's default dump), <game>_<stamp>/ runs
    temp/out/     ad-hoc tool output (videos, previews, montages)

results/ is NOT under here on purpose: it holds committed validation output.

Kept free of numpy so tools that don't decode anything can import it.
"""
import os

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
TEMP = os.path.join(REPO, "temp")
LOGS = os.path.join(TEMP, "logs")
DUMPS = os.path.join(TEMP, "dumps")
DEFAULT_DUMP = os.path.join(DUMPS, "mv_dump")
OUT = os.path.join(TEMP, "out")


def out_path(name):
    """temp/out/<name>, creating temp/out/ if needed."""
    os.makedirs(OUT, exist_ok=True)
    return os.path.join(OUT, name)
