# Conventions

## Comments and docs

- Say why, not what. A comment records a constraint, a trap, or the engine or
  D3D12 behaviour the code exists for. Drop one that restates the code.
- No history: no milestones, "used to", "previously", or names of deleted
  code. That belongs in commit messages.
- Open with the fact. Keep it short; past a few lines, the detail belongs in
  a commit message, the README, or `profiles/ue5.json`.
- State each fact once, where it is defined, and point to it elsewhere.
- Backtick identifiers and paths: `SurfaceIdentifier`, `hook/src/capture.h`.
- Say where a measurement came from ("on Skyrunner, 50% vs 0%").
- Plain sentences. No dash asides, no "not X, but Y" framing, no intensifiers
  ("exactly", "genuinely", "deliberately", "silently").

The README describes the project as it is now, not how it got there.

## Verifying a change

- `tools/run_testhost_suite.ps1` must pass; add `-Overlay` for overlay changes.
- A change that should not alter captured data must pass
  `-Baseline <mv_hook.dll built from the previous commit>` (EQUIVALENT).
- Build with `/W4` and no warnings.
