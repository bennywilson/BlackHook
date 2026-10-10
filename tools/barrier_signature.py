"""How distinctive is the velocity buffer's barrier pattern, really?

The project's identification argument has two halves, and only one of them was
ever tested. The descriptor filter (1212x760, R16G16B16A16_UNORM) picks a
resource; the barrier analysis then shows that resource behaves exactly as
SceneVelocity should - two transitions per frame, strictly alternating
RENDER_TARGET <-> ALL_SHADER_RESOURCE, no UAV barriers, no state-continuity
violations. Clean, and circular: the barrier analysis only ever ran on
resources that had already passed the descriptor filter, so it could confirm
"the resource I picked behaves right" but never "nothing else does".

The hook now profiles every resource the identify pass walks past (see
DumpBarrierProfiles in hook/src/d3d12_hook.cpp) and writes them to
temp\\logs\\mv_barrier_profile.log. This reads that file and asks the question the
other way round: of all the resources in the frame, how many exhibit the
velocity buffer's signature?

If the answer is one, or a handful, then runtime behaviour alone comes close to
identifying the velocity buffer - no RenderDoc capture needed to crib a
descriptor from, and the technique transfers to a title you do not own. If the
answer is dozens, the descriptor filter is doing the real work and the barrier
pattern is corroboration rather than identification. Either result is worth
having; only one of them was assumed before.

Usage:  python barrier_signature.py [path-to-mv_barrier_profile.log]
"""
import os
import sys
from collections import namedtuple

import paths

path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(paths.LOGS, "mv_barrier_profile.log")
if not os.path.exists(path):
    sys.exit(f"{path} not found - run the game with the hook attached for at least 2000 frames "
             f"(the survey dumps itself once, automatically, at that point)")

Row = namedtuple("Row", "ptr width height format flags transitions frames twoPerFrame "
                        "violations rtToSrv srvToRt otherPairs uav")

rows = []
for line in open(path):
    line = line.strip()
    # The hook's own log lines carry a "[timestamp] " prefix; the header starts '#'.
    if "] " in line:
        line = line.split("] ", 1)[1]
    if not line or line.startswith("#"):
        continue
    parts = line.split()
    if len(parts) != 13:
        continue
    try:
        rows.append(Row(*[int(p) for p in parts]))
    except ValueError:
        continue

if not rows:
    sys.exit(f"no profile records parsed from {path}")

print(f"{len(rows)} resources profiled\n")

# The signature, stated as testable predicates rather than prose.
#
# Deliberately NOT part of it: anything about size or format. The whole point
# is to find out how far behaviour alone gets you, so bringing the descriptor
# back in through the side door would just restate the circularity.
def is_two_cycle(r):
    """Exactly two transitions per frame, every frame it appears."""
    return r.frames >= 10 and r.twoPerFrame == r.frames


def is_strict_alternation(r):
    """Only ever RT<->ALL_SHADER_RESOURCE, and never loses state continuity."""
    return r.otherPairs == 0 and r.violations == 0 and r.rtToSrv > 0 and r.srvToRt > 0


def no_uav_barriers(r):
    return r.uav == 0


stages = [
    ("seen in >=10 frames", lambda r: r.frames >= 10),
    ("  + exactly 2 transitions every frame", is_two_cycle),
    ("  + only RT <-> ALL_SHADER_RESOURCE, no continuity violations", is_strict_alternation),
    ("  + no UAV barriers", no_uav_barriers),
]

survivors = rows
print("Narrowing by behaviour alone:")
print(f"  {'all profiled resources':<62} {len(rows):>6}")
for label, predicate in stages:
    survivors = [r for r in survivors if predicate(r)]
    print(f"  {label:<62} {len(survivors):>6}")

print()
if not survivors:
    print("Nothing matched the full signature. Either the capture was too short for")
    print("resources to establish a pattern, or the pattern differs on this title.")
    sys.exit(0)

print(f"{len(survivors)} resource(s) match the SceneVelocity barrier signature on behaviour alone:\n")
print(f"  {'ptr':>16} {'size':>12} {'fmt':>5} {'flags':>6} {'frames':>7} {'transitions':>12}")
for r in sorted(survivors, key=lambda r: -r.frames):
    print(f"  {r.ptr:>16} {str(r.width) + 'x' + str(r.height):>12} {r.format:>5} "
          f"{r.flags:>6} {r.frames:>7} {r.transitions:>12}")

print("\nInterpretation:")
if len(survivors) == 1:
    print("  One. Runtime barrier behaviour alone identifies this resource, without")
    print("  needing the descriptor filter at all - meaning the same approach would")
    print("  work on a title with no RenderDoc capture to crib a descriptor from.")
else:
    print(f"  {len(survivors)}. Behaviour narrows the field from {len(rows)} to {len(survivors)}, which is a")
    print("  large reduction but not an identification on its own. Check whether the")
    print("  survivors share a descriptor - if they do, behaviour plus one descriptor")
    print("  predicate is still much weaker prior knowledge than the full signature.")

# How much of the reduction came from behaviour vs. how much was already
# implied by size? Worth knowing: if every 2-cycle resource is also the only
# one at that resolution, the two filters are not independent evidence.
sizes = {}
for r in survivors:
    sizes.setdefault((r.width, r.height, r.format), []).append(r)
if len(sizes) > 1:
    print(f"\n  The survivors span {len(sizes)} distinct descriptors, so behaviour is not")
    print("  simply re-deriving the descriptor filter.")
