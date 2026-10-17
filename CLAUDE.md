# Conventions

## Comments and docs

- Say why, not what. A comment records a constraint, a trap, or the engine or
  D3D12 behaviour the code exists for. Drop one that restates the code.
- No history: no milestones, "used to", "previously", or names of deleted
  code. That belongs in commit messages.
- Open with the fact. Keep it short; past a few lines, the detail belongs in
  a commit message, the README, or `profiles/ue5.json`.
- Cut filler and roundabout openings such as "What to look for is" or "It
  should be noted that". Make the subject the thing the sentence is about:
  "`profiles/ue5.json` defines what the hook searches for."
- Use the active voice and name who acts: "The hook writes the manifest after
  the burst drains." Keep the passive only when the actor is unknown or
  irrelevant.
- Stimulus first. Lead with the condition, then name who acts and what it
  does: "If a loaded profile has the same name as an embedded one, it
  replaces it." A conditional must read as conditional.
- Write for a graphics programmer. Skip what they already know.
- State each fact once, where it is defined, and point to it elsewhere.
- Don't enumerate what the code owns, such as a list of optional fields.
  Name where it lives so the comment stays true as the code grows.
- One term per concept, using the project's names: embedded and loaded
  profiles, shortlist, survivors, structural gate, behaviour gate, trigger
  edge.
- Name the thing. Avoid "these", "here" or "it" when the referent is more
  than a line away.
- Match the code. If it tests `>=`, write "at least".
- Say which layer a name belongs to: UE `DepthStencilTargetable`, D3D12
  `ALLOW_DEPTH_STENCIL`. Cite engine behaviour or mark it unverified.
- Backtick identifiers and paths: `SurfaceIdentifier`, `hook/src/capture.h`.
- Say where a measurement came from. Public files name no games: write "on
  a tested title" and keep the title and numbers in `notes/` (git-excluded).
- Plain, full sentences. No shorthand fragments, no dash asides, no "not X,
  but Y" framing, no intensifiers ("exactly", "genuinely", "deliberately",
  "silently").
- Group related fields under a header comment. Leave a blank line between
  the header's summary and the first field's comment.
- Wrap at 80 columns. No trailing whitespace.

The README describes the project as it is now, not how it got there.

## Verifying a change

- `tools/run_testhost_suite.ps1` must pass; add `-Overlay` for overlay changes.
- A change that should not alter captured data must pass
  `-Baseline <mv_hook.dll built from the previous commit>` (EQUIVALENT).
- Build with `/W4` and no warnings.
