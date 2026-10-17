"""Where everything this repo generates lives: <repo>/temp/, gitignored.

The hook's `OutputDir()` (hook/src/logging.h) uses the same <repo>/temp, set at
CMake configure time:

    temp/build/   CMake build tree
    temp/logs/    mv_hook.log, mv_barrier_profile.log, other mv_*.log
    temp/dumps/   mv_dump/ (the hook's default dump), <game>_<stamp>/ runs
    temp/out/     tool output (videos, previews, contact sheets)

results/<tag>/ is outside temp/ and holds validation output. Git tracks only
the results/ue5.<minor>/ folders, one reference title per engine version, and
ignores every other tag. No numpy, so tools that decode nothing can import it.
"""
import os

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
TEMP = os.path.join(REPO, "temp")
LOGS = os.path.join(TEMP, "logs")
DUMPS = os.path.join(TEMP, "dumps")
DEFAULT_DUMP = os.path.join(DUMPS, "mv_dump")
OUT = os.path.join(TEMP, "out")
RESULTS = os.path.join(REPO, "results")


def results_dir(tag):
    """results/<tag>/, creating it if needed. An empty tag means "untagged"."""
    path = os.path.join(RESULTS, tag or "untagged")
    os.makedirs(path, exist_ok=True)
    return path


def out_path(name):
    """temp/out/<name>, creating temp/out/ if needed."""
    os.makedirs(OUT, exist_ok=True)
    return os.path.join(OUT, name)
