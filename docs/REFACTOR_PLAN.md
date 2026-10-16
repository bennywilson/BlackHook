# Surface migration plan

> **Status: done** (branch `Clean-up`). Kept as the design record; the code
> differs from it where live evidence said so: every surface is a plain
> `SurfaceIdentifier` configured by profile data (rival adoption moved into the
> base class, no subclasses); `CaptureSystem` (sec 2.6) was never built -
> `capture.cpp` keeps file-scope state behind one entry point per event; a
> second optional surface, `specular` (GBufferB), was added; the surface names
> live in one table (`hook/src/capture.h`). Finding 1.2 (depth/GBuffer fence
> coverage not graded) and sec 6 #4 (`DrainSlot` on the render thread) remain
> open.

A design for moving the hook from three hardcoded, hand-triplicated capture
paths to a single `Surface` abstraction, without rewriting what works.

Naming, used consistently throughout and deliberately not "plane", "resource"
or "buffer" — all three already mean something else in this codebase
(`ID3D12Resource`, `D3D12_RESOURCE_DIMENSION_BUFFER`, and D3D12's own
depth/stencil subresource *planes*):

| term | meaning |
|---|---|
| **Surface** | one per-frame image the dump carries: velocity, depth, colour, normals |
| **SurfaceSpec** | data-only description of a surface. No state. JSON-expressible. |
| **SurfaceCapture** | one surface's per-slot capture state: readback buffer, footprints, desc, bytes |
| **SurfaceIdentifier** | the generic structural+behavioural search, parameterised by a `SurfaceSpec` |
| **SurfaceRegistry** | owns the specs and identifiers; one entry point from the barrier hook |
| **plane** | reserved for D3D12 subresource planes *inside* a Surface (depth plane 0 / stencil plane 1) |

Scope note, stated up front because it bounds every decision below: this is
**engine variants within D3D12/Unreal — 5.2 vs 5.7**. It is not cross-engine
portability. `SurfaceSpec`'s vocabulary is deliberately D3D12-shaped (DXGI
format enums, `D3D12_RESOURCE_FLAGS`, `D3D12_RESOURCE_STATES`) and its format
tables cite UE source. Anything that would also generalise to Vulkan or a
non-UE renderer is a cost paid now for a benefit that is not in scope.

---

## 1. Findings

Each claim from the prior analysis, checked against source. Three were wrong
or incomplete; those are marked. Additional findings follow.

### 1.1 Verified: `Slot` has five triplicated field sets — but **six** asymmetric fields, not four

`capture.cpp:55-90`. The triplication is exactly as described — five sets, each
appearing once per surface:

| set | lines |
|---|---|
| `ComPtr<ID3D12Resource> {velocity,color,depth}Buffer` | 57-59 |
| `vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> {…}Footprints` | 62-64 |
| `D3D12_RESOURCE_DESC {…}Desc` | 67-69 |
| `UINT64 {…}Bytes` | 70-72 |
| `UINT {…}Subresources` | 73-75 |

The asymmetric count is understated. Beyond `hasVelocity` (80), `hasDepth`
(81), `depthCopies` (82) and `velocitySubmitted` (86), two more fields are
velocity-only: `recordedList` (85) and `queueSubmissionsAtRecord` (89). Six
fields, of which four belong to velocity alone, one to depth alone, and none
to colour. Colour is the surface with no bookkeeping at all — see 1.8.

### 1.2 Verified, and the consequence is worse than stated

`recordedList` is set in exactly one place: `capture.cpp:878`, on the velocity
path. It is cleared at 762, 1104, 1134, 1162 and 1277. `NoteSubmission`
(1002-1029) matches incoming command lists against it and sets
`slot.velocitySubmitted`. The strong/weak/none grading (1216-1231) branches on
nothing else.

The claim says fence-coverage grading has never been evaluated for depth *or*
colour. Half of that is a gap and half is a non-issue, and the distinction
matters for the design:

- **Colour does not need it.** Its copy is recorded onto `slot->commandList`
  (1254-1260) and submitted by us (1268) immediately before `Signal` (1271).
  Coverage is by construction. A per-surface coverage field for colour would be
  a field that can only ever read "strong".
- **Depth does need it and does not have it.** Depth's copy rides the *game's*
  command list (`capture.cpp:967-973`), exactly like velocity's, and is covered
  by nothing that is ever checked. If a title's depth edge lands on a list
  submitted to a different queue, or submitted after our `Signal`, the depth
  readback is silently stale.

And the burst summary at 1288-1291 presents those three counters as a property
of the capture — "Only the first category is proof the readback is not stale."
It is proof about velocity only. That line currently over-claims.

### 1.3 Verified: the budget check is a hand-written three-term sum, after all three `MapOut` calls

`capture.cpp:592-614`. `MapOut` for velocity and colour at 596-597, depth at
603, then `const size_t total = velocityData.size() + colorData.size() +
depthData.size();` at 605 and `WriteQueueHasRoom(total)` at 606. Roughly 19 MB
of `memcpy` is performed and thrown away on a dropped frame (7.4 MB velocity at
1212×760 R16G16B16A16, 7.4 MB colour at 1707×1067 R10G10B10A2, 4.7 MB depth
across two planes — the extents from `DEBUGGING.md:297-300`).

Two things the claim does not say, both load-bearing:

- The check *is* currently sound despite looking racy. `WriteQueueHasRoom`
  releases `g_writeMutex` (324-328) before `EnqueueWrite` re-takes it (334),
  but the only producers are `DrainSlot`, `WriteMetadata` and
  `WriteDepthMetadata`, all on the render thread under `g_mutex`, and the
  consumer only ever *decrements* `g_queuedBytes` (288). Single-producer plus a
  monotonically-shrinking consumer makes the check conservative. That invariant
  is nowhere written down, and a fourth surface enqueued from any other thread
  breaks it silently.
- Depth's `EnqueueWrite` return value is discarded (624) while velocity's and
  colour's are checked (618, 620, 632). Harmless today only because the room
  check covered the total.

### 1.4 Verified: `DrainSlot` runs on the render thread inside the Present hook

`capture.cpp:1088`, inside `OnPresent`, called from `HookPresent` /
`HookPresent1` (`d3d12_hook.cpp:232, 248`) *before* chaining to the real
Present. The whole of `OnPresent` holds `g_mutex` (1039), which the barrier
hook also takes (`OnVelocityReadable` 783, `OnDepthReadable` 899) — so barrier
threads block behind the memcpy too.

Magnitude is larger than "~10MB/frame": the drain loop (1084-1091) can drain
**up to four slots in one Present**, so the worst case is ~78 MB of `memcpy`
inside a single Present hook.

### 1.5 **Partly refuted**: three reopen sites, but not all are descriptor-identity failures

There are exactly three call sites, which is right:

| site | trigger | kind |
|---|---|---|
| `d3d12_hook.cpp:219-220` | back-buffer size changed | **not** a descriptor-identity failure |
| `d3d12_hook.cpp:363` | `ValidateDepthCandidate` failed | descriptor identity |
| `d3d12_hook.cpp:406, 415` | `ValidateCandidate` failed | descriptor identity |

The first is a polled comparison of `DXGI_SWAP_CHAIN_DESC.BufferDesc`
(`d3d12_hook.cpp:200, 209-221` → `velocity_identify.cpp:687-711`), every 30
frames. The identified resource's own descriptor is explicitly *still valid* at
that point; `velocity_identify.cpp:706-709` says so in as many words ("its own
descriptor stays valid, which is why nothing else here notices"). Calling it a
descriptor-identity failure misreads the one reopen path that behaves
differently from the other two — and that difference is where finding 1.11
lives.

**"Nothing reopens on behavioural grounds" is verified, and is deliberate.**
`VelocityPassIsSilent` explicitly refuses to reidentify on silence
(`velocity_identify.cpp:950-957`, citing a 271,123-frame gap on a real title),
and the behavioural rival path *adopts* instead of reopening
(`d3d12_hook.cpp:427-437` → `velocity_identify.cpp:1035-1066`). So there is a
behavioural response to a lost resource; it just isn't a reopen. **Adopt and
reopen are different verbs and the target design must keep them apart.**

### 1.6 Verified: AMBIGUOUS is terminal, for both surfaces

Velocity: `velocity_identify.cpp:646-652` sets `g_decisionMade = true` and
returns without setting `g_identified`; `OnFrameForIdentification:885` gates
`Decide` on `!g_decisionMade`. Depth: identical shape at
`depth_identify.cpp:268-275`, gated at 366.

The inversion is worth naming. The *weaker* evidence state — no survivors at
all — retries forever with a widened window and a fresh measurement window
(`velocity_identify.cpp:575-625`, `depth_identify.cpp:218-226`). The
*stronger* state — two plausible candidates too close to separate — gives up
permanently. A settling window that happened to catch a menu produces "no
candidates" and recovers; one that caught two live buffers produces AMBIGUOUS
and never tries again, even though a longer window is exactly what would
separate them.

### 1.7 Verified, and it is a live bug, not just a coupling smell

`ReopenIdentification` calls `ForgetAllResourcesSeen()`
(`velocity_identify.cpp:927`); `ReopenDepthIdentification`
(`depth_identify.cpp:389-398`) does not.

But the claim understates it. `ReopenDepthIdentification` *clears* both
`g_shortlist` and `g_profiles` (396-397). The only thing that refills them is
`NoteResourceDescForDepth` (312-338), reached only from `TryIdentifyResource`
(`d3d12_hook.cpp:301`), which is gated on `TryMarkResourceSeen`
(`d3d12_hook.cpp:286` → `resource_tracking.cpp:104-107`) returning true — which
it never will for a resource already in the seen set.

So the standalone depth reopen at `d3d12_hook.cpp:363` does not "quietly depend
on its candidates already being in that shared set". It **empties the only
structure it could depend on and cannot repopulate it**, and depth is lost for
the rest of the session while the log line says "REOPENING the search". It
recovers only if the engine happens to allocate a brand-new depth resource
afterwards — which for a resolution change it might, which makes the failure
intermittent rather than reproducible.

### 1.8 Verified: no `hasColor`; presence implied by `MapOut` succeeding

`grep` finds no such field. `DrainSlot:596-600` returns false if either
velocity's or colour's `MapOut` fails.

The nuance is that this is currently *sound*, by an invariant nothing states: a
slot reaches `SlotState::Submitted` only at `capture.cpp:1278`, and `OnPresent`
returns early on every path that could leave colour unfilled — nine early
returns between 1119 and 1273 (1121, 1170, 1178, 1204, 1237, 1242, 1249, 1264,
1273). "Every Submitted slot has colour" is a real invariant enforced by nine
scattered `return`s instead of one field. That is a thing to make explicit in
the refactor, not a bug to fix.

### 1.9 Verified: the depth reopen cascade is hand-wired

Two sites, `d3d12_hook.cpp:220` and `:415`. Nothing in `velocity_identify.cpp`
declares that depth depends on it. `depth_identify.cpp` *does* know about
velocity — it includes the header and calls `IdentifiedVelocityExtent`
(`depth_identify.cpp:372`) — so the dependency is declared in the dependent, and
enforced in a third file that neither of them owns.

### 1.10 Verified: colour is read via `GetBuffer` and never identified

`capture.cpp:1167-1179`: `QueryInterface` to `IDXGISwapChain3`,
`GetCurrentBackBufferIndex` (1173), `GetBuffer` (1176). `colorDesc` is set from
`backBuffer->GetDesc()` at 1184 and stored at 1199. Confirmed, and the cited
line range is right.

Three further mechanical differences, all downstream of "known by
construction", and all load-bearing for §3.1:

1. **`GetBuffer` AddRefs** (1174-1175 comment), so colour needs none of the
   ref-holding, `MarkAsCandidate`/`ValidateCandidate` machinery that exists
   because velocity is a transient placed resource whose address can be
   recycled (`resource_tracking.cpp:42-62`).
2. **Its source state is a constant, not an observation.**
   `D3D12_RESOURCE_STATE_PRESENT` is hardcoded at 1259. For velocity and depth
   the state is passed through from the game's own barrier
   (`capture.h:32-34`) because we cannot know it. For the back buffer the
   swapchain contract tells us.
3. **Its copy rides our command list on a queue we submit to** (1254-1268),
   not the game's — so fence coverage is by construction (see 1.2).

### Additional findings

#### 1.11 `ReopenIdentification` does not clear the candidate set, so its own log line is false on the resize path

This is the most serious thing found.

`ReopenIdentification` (`velocity_identify.cpp:895-928`) clears `g_identified`,
`g_identifiedAlt`, the shortlist, the profiles, the resolution histogram and
the seen set. It does **not** clear `g_candidateResources`
(`resource_tracking.cpp:69`) — and `IsCandidate` (`d3d12_hook.cpp:366`) is what
the barrier hook actually gates capture on.

On the two `ValidateCandidate`-failure paths this is fine, because
`ValidateCandidate` erases the entry itself (`resource_tracking.cpp:173-176`)
before the reopen is called. On the **resize path** (`d3d12_hook.cpp:219`)
nothing erases it, and the resource's own descriptor is unchanged *by design*
(`velocity_identify.cpp:706-709`). So `ValidateCandidate` keeps passing,
`OnVelocityReadable` keeps copying from a texture the engine has abandoned, and
this runs for the whole duration of the "reopened" search — while
`velocity_identify.cpp:897-899` logs "Nothing is captured until a new decision
is reached."

When the new search does resolve, `MarkAsCandidate` (660) *adds* the new
resource without retiring the old, so both are candidates, and
`capture.cpp:794`'s `if (slot->hasVelocity) return;` makes the outcome
**first-edge-wins rather than correct-resource-wins**.

#### 1.12 Adoption is additive and never retires the previous target

`MarkAsCandidate` (`resource_tracking.cpp:134-139`) inserts; only
`ValidateCandidate` failure erases. With `kMaxAdoptions = 64`
(`velocity_identify.cpp:238`) the candidate set can reach 65 velocity
resources, all answering `IsCandidate` true. The adoption log line
(`velocity_identify.cpp:1065`) says "BOTH are now captured from, whichever
takes the edge"; the actual rule is whichever takes it *first in the frame*,
and the dump records nothing about which. For a same-format, same-extent
reallocation — every adoption observed so far, per the comment at 216-226 —
that is harmless. It is harmless by luck, and it should be recordable.

#### 1.13 `meta.txt` / `meta_depth.txt` are written once but describe a layout that can change

`g_metadataWritten` (`capture.cpp:1206`) and `g_depthMetadataWritten` (961) are
one-shot `exchange`s, and both sit **inside** the "layout changed → rebuild
footprints" branch (1185 and 936 respectively). The first layout change writes
the metadata; every later one rebuilds footprints and leaves the metadata
describing the old layout.

This target is a dynamic-resolution title — `DEBUGGING.md:297-300` records
velocity at 1212×760 "(render resolution, DRS-scaled)" against a 1707×1067 back
buffer. So this is not hypothetical. The downstream failure is silent:
`mvtools._rows` (`mvtools.py:68-81`) un-pads at whatever `row_pitch` the
metadata gave it and produces a plausible image at the wrong stride.

This is the single strongest argument for the manifest being written at burst
*end*, and for per-frame layout provenance rather than one global layout record.

#### 1.14 The completeness rule is evaluated against *live* identification state

`capture.cpp:1148-1152` gates on `IdentifiedDepthResource() != nullptr`,
re-evaluated on every Present. `OnFrameForDepthIdentification` runs every
Present too (`d3d12_hook.cpp:231, 247`), and a burst is 2400 frames
(`capture.cpp:30`).

So if depth reopens mid-burst (finding 1.7 — which is exactly when this
happens), `IdentifiedDepthResource()` becomes null, the gate stops requiring
depth, and subsequent frames are written *without* depth and *pass* the
homogeneity check. The comment at 1138-1147 says the check exists so that "a
dump with inconsistent frames" cannot happen. It can, and nothing in the dump
says which frames.

#### 1.15 Three surfaces, three different per-frame arbitration rules — and they live in `capture.cpp`

| surface | rule | site |
|---|---|---|
| velocity | first `RT→SRV` edge of the frame wins | `capture.cpp:794` |
| depth | last `DEPTH_WRITE→readable` edge *before velocity* wins, capped at 4, with a fallback branch for titles that order it the other way | `capture.cpp:908-926` |
| colour | the back buffer at Present | `capture.cpp:1167-1179` |

Depth's rule in particular is a real, title-dependent policy with a documented
fallback (914-926). It is data, and it belongs in `SurfaceSpec`, not in the
capture path.

#### 1.16 Two independent behaviour-profile maps walk every barrier, one under a global mutex

`velocity_identify.cpp:786-860` maintains `g_profiles` (up to 20,000 entries,
full state-pair histogram) and `depth_identify.cpp:340-362` maintains its own.
Both are called unconditionally from the barrier hook (`d3d12_hook.cpp:346-347`)
for every transition of every resource in the process. Velocity's takes
`g_profileMutex` on each one.

This is the actual hot-path cost, and a naive "one identifier per surface"
refactor multiplies it by the surface count. The registry must fan out from
**one** profile walk. Called out here because a design that gets this wrong
will look like the abstraction working.

#### 1.17 `BURST` has drifted, in three places, for one fact

`mvtools.py:507` is `BURST = 60` under the comment "must match `kCaptureFrames`
in hook/src/capture.cpp". `capture.cpp:30` is `kCaptureFrames = 2400`.
`README.md:37` says "F8 captures a 60-frame burst". Only the legacy
no-`frames.csv` fallback (`mvtools.py:550`) uses the constant, so today this
mis-slices old dumps only. It is a clean illustration of what workstream 2 is
for.

#### 1.18 There is no loader gate today

Seven scripts call `mvtools.load_meta(dump_dir)` and then index the dict by
hand: `check_decode.py:41`, `derive_scale.py:49`, `inspect_velocity.py:13`,
`make_video.py:47`, `region_match.py:63`, `warp_validate.py:43`,
`wpo_synthetic_test.py:59`. `load_meta` (`mvtools.py:41-65`) silently skips a
missing file and `int()`s every value, so "no depth in this session", "depth
file truncated" and "metadata from a different resolution than the frames" are
all the same object.

---

## 2. Target design

### 2.1 `SurfaceSpec` — data only

Every field is a scalar, an enum, a fixed array of scalars, or a name. Nothing
mutable, nothing that survives a call. That is what makes it JSON-expressible;
it is also what makes the profile-safety constraint a property of the type
rather than a review rule (§2.8).

```cpp
enum class SourceKind   : uint8_t { Identified, KnownByConstruction };
enum class KnownSource  : uint8_t { None, SwapChainBackBuffer };
enum class TriggerEdge  : uint8_t { RenderTargetToShaderResource, DepthWriteToReadable, PresentBackBuffer };
enum class Arbitration  : uint8_t { FirstInFrame, LastInFrame, LastBeforeSurface };
enum class AmbiguityRule: uint8_t { ScoreMargin, EdgeCountRatio };

struct FormatCandidate { DXGI_FORMAT format; int8_t score; const char* name; };
struct SignalWeight    { SignalId id; int8_t weight; };

struct SurfaceSpec
{
    const char*  name;                 // "velocity" | "depth" | "color" | "normals"
    SourceKind   source;
    KnownSource  known;                // meaningful only when source == KnownByConstruction

    // --- structural gate (inert when source == KnownByConstruction) -------
    FormatCandidate formats[8];  uint8_t formatCount;
    UINT  requiredFlags;               // e.g. ALLOW_RENDER_TARGET|ALLOW_UNORDERED_ACCESS
    UINT  forbiddenFlags;              // e.g. DENY_SHADER_RESOURCE
    UINT  exactFlagsBonus;             // Flags == this scores extra; 0 = no bonus
    uint16_t mipLevels, arraySize, sampleCount;   // 0 == "any"
    D3D12_TEXTURE_LAYOUT layout;
    UINT64 minWidth;  UINT minHeight;
    uint8_t maxBackBufferMultiple;     // velocity: 2. 0 == unbounded.

    // --- extent linkage ---------------------------------------------------
    const char* extentFrom;            // nullptr | "velocity"

    // --- behaviour --------------------------------------------------------
    TriggerEdge trigger;
    uint32_t  minFramesObserved;
    uint16_t  edgesPerFrameNum, edgesPerFrameDen;   // gate: edges*den >= frames*num
    uint64_t  settleFrames;
    SignalWeight signals[12];  uint8_t signalCount;
    AmbiguityRule ambiguityRule;  int16_t ambiguityValueX100;

    // --- rival adoption (consumed only by VelocitySurfaceIdentifier) ------
    uint32_t rivalStallFrames, rivalFramesBeforeAdopt, rivalMaxAdoptions, rivalMaxReports;

    // --- capture policy ---------------------------------------------------
    Arbitration arbitration;
    const char* arbitrateBefore;       // depth: "velocity"
    uint8_t maxCopiesPerFrame;
    bool required;                     // participates in whole-frame atomicity
    bool enabled;
};
```

Two things to note. The rival thresholds are *data* even though the rival state
machine is code — so a profile can retune adoption without a subclass. And the
two ambiguity rules cover both existing tiebreaks: velocity's `margin >= 2`
(`velocity_identify.cpp:644`) is `ScoreMargin, 200`; depth's `a.edges >
b.edges * 3/2` (`depth_identify.cpp:265`) is `EdgeCountRatio, 150`. Two
enumerators, both data — this is what makes depth need no subclass.

### 2.2 `SurfaceCapture` and the new `Slot`

```cpp
enum class Coverage : uint8_t { None, Weak, Strong, ByConstruction };

struct SurfaceCapture
{
    const SurfaceSpec* spec = nullptr;          // borrowed; session lifetime
    ComPtr<ID3D12Resource> readback;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    D3D12_RESOURCE_DESC desc{};
    UINT64  bytes = 0;
    UINT    subresources = 0;
    uint32_t layoutEpoch = 0;                   // bumped on every layout change
    uint8_t copies = 0;
    bool    present = false;                    // replaces hasVelocity/hasDepth; colour gets one too

    // per-surface fence coverage; replaces the singular recordedList
    IUnknown* recordedList = nullptr;
    uint64_t  queueSubmissionsAtRecord = 0;
    Coverage  coverage = Coverage::None;
};

struct Slot
{
    ComPtr<ID3D12CommandAllocator>    allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    std::vector<SurfaceCapture> surfaces;   // sized once; never resized after
    UINT64 fenceValue = 0;
    unsigned long long frameIndex = 0;
    uint32_t presentMask = 0;               // bit i == surfaces[i].present
    int  captureIndex = 0;
    SlotState state = SlotState::Free;
};
```

**Hot-path allocation discipline.** `surfaces` is sized exactly once, when the
registry is finalised (before hooks go live), to `registry.Count()`, and never
resized. The barrier path only writes into an existing element — no
`push_back`, no `reserve` on the hot path. Footprint vectors resize only inside
the existing "layout changed" branch (`capture.cpp:849`), which already
allocates today; no regression.

**Readback reuse** keeps the existing rule verbatim: the buffer is recreated
only when `SameLayout` fails, per surface, per slot. The one addition is
`layoutEpoch++` at that point, which is what makes finding 1.13 representable
in the manifest rather than silently wrong.

`recordedList` and `queueSubmissionsAtRecord` move *into* `SurfaceCapture`,
which is the whole fix for 1.2: depth gets graded because it has somewhere to
record it. Colour is set to `Coverage::ByConstruction` and never graded —
recorded as a distinct value rather than a "strong" that means something else.

### 2.3 `SurfaceIdentifier`

Everything currently in `velocity_identify.cpp` and `depth_identify.cpp` that
is not rival adoption becomes this one class, parameterised by a spec.

```cpp
enum class IdentifyState : uint8_t { Disabled, Searching, Selected, Ambiguous };
enum class ReopenReason  : uint8_t { DescriptorChanged, BackBufferResized, DependencyReopened };

class SurfaceIdentifier
{
public:
    explicit SurfaceIdentifier(const SurfaceSpec& spec, BarrierProfileStore& profiles) noexcept;
    virtual ~SurfaceIdentifier() = default;

    // hot path. noexcept, no allocation beyond the existing shortlist growth.
    void OnResourceDesc(ID3D12Resource*, const D3D12_RESOURCE_DESC&) noexcept;
    // returns true if this transition is this surface's trigger edge on the
    // selected resource, i.e. the caller should capture.
    bool OnTransition(ID3D12Resource*, uint64_t frame, int before, int after) noexcept;

    void OnFrame(uint64_t frame) noexcept;      // settle deadline, Decide()

    ID3D12Resource* Selected()  const noexcept;
    IdentifyState   State()     const noexcept;
    bool            Validate(ID3D12Resource*) const noexcept;
    bool            Extent(UINT64* w, UINT* h) const noexcept;
    void            Reopen(ReopenReason) noexcept;   // clears its OWN candidate set

protected:
    // The only two extension points.
    // Called when some OTHER resource takes this surface's trigger edge.
    // Return true to have the registry adopt it as an additional candidate.
    virtual bool OnUnmatchedEdge(ID3D12Resource*, uint64_t frame) noexcept { return false; }
    virtual void OnSelected(ID3D12Resource*, const D3D12_RESOURCE_DESC&) noexcept {}
};
```

Virtual dispatch only, return codes only, `noexcept` throughout — no
`std::function`, no exceptions, per the hot-path constraint.

Migrated wholesale into the base, parameterised by spec: the structural gate
(the two `StructurallyPlausible` functions collapse to one — compare
`velocity_identify.cpp:293-371` against `depth_identify.cpp:108-160`; they
differ only in the table and three predicates), the ref-holding shortlist, the
behaviour gate, signal collection and weighting, the survivor sort, the
ambiguity rule, extent linkage via `spec.extentFrom`, and the retry-on-empty
window reset (`velocity_identify.cpp:609-624`).

### 2.4 Subclass justification

The test: *if it could be expressed as data in a profile, it isn't a subclass.*

**`VelocitySurfaceIdentifier` — justified.** Rival adoption
(`velocity_identify.cpp:977-1094`) is a run-length state machine over
consecutive frames carrying five pieces of mutable cross-call state:
`g_rivalRunLength`, `g_lastRivalFrame`, `g_lastRivalResource`, `g_adoptions`,
`g_identifiedAlt`. No arrangement of scalars in a JSON file expresses "this
same resource has now carried the edge on 8 consecutive frames while the
selected one carried none". It overrides `OnUnmatchedEdge` and nothing else.
Its *thresholds* stay in the spec (§2.1), so the subclass owns only the state.

**`DepthSurfaceIdentifier` — not justified. Depth becomes pure spec.**
Everything depth does differently is data: the format table
(`depth_identify.cpp:45-52`), `requiredFlags = ALLOW_DEPTH_STENCIL` /
`forbiddenFlags = DENY_SHADER_RESOURCE` (57-58), `trigger =
DepthWriteToReadable`, `extentFrom = "velocity"` (197-202), `settleFrames = 60`
(82), and the ratio tiebreak (265) which becomes
`AmbiguityRule::EdgeCountRatio, 150`. **This is the proof point the whole
design rests on**, and Phase 7 is where it is settled honestly — if depth turns
out to need code, the right outcome is a `DepthSurfaceIdentifier` and a note
saying the generic base covers one surface of three.

**Colour — not a `SurfaceIdentifier` at all.** See §3.1. A trivial subclass
that "always resolves immediately" would have to fabricate a score, and that
fiction would reach the manifest.

**`NormalsSurfaceIdentifier` — not justified**, as far as can be determined
without runtime observation. Second proof point. See §3.2.

Net: **one subclass, one non-identifier adapter, two registry invariants.**

### 2.5 `SurfaceRegistry`

```cpp
struct SurfaceTrigger { int surfaceIndex; ID3D12Resource* resource; int stateAfter; };

class SurfaceRegistry
{
public:
    void Configure(const ProfileTable&);          // once, before hooks go live
    int  Count() const noexcept;
    const SurfaceSpec& Spec(int i) const noexcept;
    IdentifyState State(int i) const noexcept;

    // ONE hot-path entry point. Walks the shared profile store once, then
    // fans out to identifiers. Fills `out` with 0..n triggers; returns the count.
    int OnBarrierTransition(ID3D12Resource*, uint64_t frame, int before, int after,
                            SurfaceTrigger* out, int outCapacity) noexcept;
    void OnUavBarrier(ID3D12Resource*) noexcept;
    void OnFrame(uint64_t frame) noexcept;

    void ReopenCascade(const char* surfaceName, ReopenReason) noexcept;
    void NoteKnownSourceResource(ID3D12Resource*) noexcept;   // swapchain buffers

private:
    std::vector<SurfaceSpec> specs_;
    std::vector<std::unique_ptr<SurfaceIdentifier>> identifiers_;   // stable after Configure
    BarrierProfileStore profiles_;      // ONE store, shared by every identifier
    DependencyGraph     deps_;          // built from spec.extentFrom
    KnownSourceSet      knownSources_;  // resources obtained from IDXGISwapChain::GetBuffer
};
```

Five things this buys, each tied to a finding:

1. **One shared `BarrierProfileStore`** (fixes 1.16). The map lookup and mutex
   happen once per transition, not once per surface. The per-surface counters
   that actually differ (`rtToShaderResource` vs `depthWriteToReadable`) become
   a small fixed array indexed by `TriggerEdge`, computed once. Cost stays O(1)
   in the surface count.
2. **The reopen cascade is derived, not hand-wired** (fixes 1.9). `deps_` is
   built from `spec.extentFrom`; `ReopenCascade("velocity", …)` walks dependents
   transitively and reopens depth and normals. Adding a fifth surface that
   depends on velocity's extent requires no new call site.
3. **`ForgetAllResourcesSeen` and the identify budget become registry-owned**
   (fixes 1.7). The seen-set is a registry-level resource — it gates
   `NoteResourceDesc` for *every* surface — so the registry calls it exactly
   once per cascade. The current bug exists precisely because that call belongs
   to whichever identifier remembered to make it. **The fix is ownership, not
   another call site.**
4. **Candidate sets become per-identifier and are cleared by `Reopen`**
   (fixes 1.11 and 1.12). Today `g_candidateResources` is a process global that
   no reopen path clears, and adoption only ever inserts. Per-identifier
   ownership makes "reopening throws away the previous answer" true by
   construction rather than by remembering.
5. **Mutual exclusion and known-source exclusion** — two invariants normals
   forces into the design; see §3.3.

Results are returned as a POD array the caller acts on. No callbacks, no
`std::function`, nothing thrown.

### 2.6 `CaptureSystem` — ownership and lifetime

Replaces the file-scope globals in `capture.cpp`. One instance, constructed on
first `EnsureDevice`.

```cpp
class CaptureSystem
{
public:
    bool BeginBurst(int frames);
    void OnSurfaceReadable(const SurfaceTrigger&, ID3D12GraphicsCommandList*) noexcept;
    void OnPresent(IDXGISwapChain*) noexcept;
    void OnSubmission(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) noexcept;
    void EndBurst();          // seals and writes the manifest
};
```

| thing | owner | lifetime |
|---|---|---|
| `SurfaceSpec` | `ProfileTable`, loaded once at startup | process |
| `SurfaceIdentifier` | `SurfaceRegistry` | process; **reset**, never destroyed, on reopen |
| candidate refs (`ComPtr`) | the owning `SurfaceIdentifier` | until validate-fail or reopen |
| seen-set + identify budget | `SurfaceRegistry` | process; refilled by `ReopenCascade` |
| `SurfaceCapture.readback` | `Slot` | until layout change or shutdown |
| slot ring, fence, writer thread | `CaptureSystem` | process |
| manifest (in progress) | `CaptureSystem` | built incrementally, written at `EndBurst` |
| known-source resource set | `SurfaceRegistry`, fed by `CaptureSystem` | process |

Two ownership moves are the substance of the phase-8 bug fixes, and they are
worth stating as such rather than as refactoring: `ForgetAllResourcesSeen`
moves from "called by velocity's reopen" to "owned by the registry", and the
candidate set moves from a process global to per-identifier.

### 2.7 Whole-frame atomicity, made structural

Today: one hardcoded clause (`capture.cpp:1148-1165`) plus a three-term sum
(605). Both need editing for a fourth surface, and a forgotten edit degrades
silently.

Target, three mechanisms:

1. **A required-mask frozen at burst arm.** At `BeginBurst`, snapshot
   `requiredMask` = the bits of surfaces that are `spec.required &&
   spec.enabled` and either `KnownByConstruction` or currently `Selected`.
   Freeze it for the burst. `OnPresent` drops the slot unless
   `(slot.presentMask & requiredMask) == requiredMask`.
   Freezing is not cosmetic — it is the fix for finding 1.14. Today the rule is
   re-evaluated per frame against live identification state, so a mid-burst
   depth reopen produces a dump that is heterogeneous in fact and homogeneous
   by the rule's own test. A frozen mask means a mid-burst reopen ends the
   burst honestly instead.
2. **Size before map.** `DrainSlot` computes
   `for (s : slot.surfaces) if (s.present) total += s.bytes;` from the
   footprints, *before* any `Map` — the byte counts are already known from
   `GetCopyableFootprints`. One room check, then map and enqueue. Fixes 1.3.
3. **Partial enqueue not representable.** `EnqueueFrame(std::vector<WriteJob>)`
   appends all of a frame's blobs under a single `g_writeMutex` hold. Today
   each surface's `EnqueueWrite` call re-checks a 4096-byte threshold
   (`capture.cpp:347`) that can drop a blob individually — which is exactly the
   partial frame the room check exists to prevent. It cannot happen today only
   because of the undocumented single-producer invariant (1.3). Make it
   un-representable instead.

Adding a fifth surface then requires **zero** changes to atomicity code. A
`static_assert(kMaxSurfaces <= 32)` catches the only way to outgrow the mask.

### 2.8 What a profile may and may not do

A profile may **narrow or widen candidate sets** (drop a format entry, add one)
and **adjust weights and thresholds** (signal weights, settle frames,
ambiguity margin, rival thresholds, min extent). It may set `enabled: false`.

A profile may **never assert an identity**. This is enforced by the type, not
by review: `SurfaceSpec` has no field that can carry one. There is no
`resource`, no `exact_desc`, no `assume`, no `skip_gate`. The narrowest thing a
profile can express is a one-entry format table — which still has to pass the
behaviour gate and the extent test and win on score. It cannot short-circuit
the search; it can only describe what the search looks for.

The **one deliberate exception is colour**, and §3.1 argues why it is
principled rather than a backdoor.

---

## 3. Colour and normals

### 3.1 Colour: a "known source" surface, not a trivial identifier

**Decision: `SourceKind` is a first-class distinction in `SurfaceCapture` and
in the manifest. Colour gets no `SurfaceIdentifier` subclass and no adapter
pretending to be one.**

The argument from the code (finding 1.10):

- **There is nothing to identify.** The identity comes from the swapchain API
  contract: `GetCurrentBackBufferIndex()` + `GetBuffer()` return *the* buffer,
  by definition of the interface.
- **A trivial identifier would have to fake three mechanisms**: the
  ref-holding/validate dance (unnecessary — `GetBuffer` AddRefs), the
  passed-through source state (unnecessary — `PRESENT` is given by contract),
  and fence-coverage grading (meaningless — our own list, our own submit).
- **A trivial identifier would have to report a score**, and any score it
  reported would be a fiction sitting in the manifest next to velocity's real
  one, where a consumer could compare them. That is the regression: adding
  identification machinery where there is no identification to do makes the
  dump *less* honest, not more uniform.

What colour **does** share, and should: footprint derivation, readback
creation and reuse, layout-change revalidation and epoch bump, membership in
whole-frame atomicity, a slot in the surface vector, and manifest
representation. That is most of the value of the abstraction. Identification is
the one part it does not share, and that is correct.

Mechanically: `CaptureSystem::OnPresent` gains one explicit
`ResolveKnownSources(swapChain)` step. The existing `capture.cpp:1167-1210`
block moves there almost verbatim, with the footprint/readback half factored
into the shared `EnsureReadback(SurfaceCapture&, const D3D12_RESOURCE_DESC&)`
that every surface uses. Colour gains a `present` flag for the first time,
replacing the nine scattered early returns of finding 1.8 with a checked
invariant. The registry never dispatches to colour on the barrier path — the
fan-out loop is over `identifiers_`, and colour has no identifier — so it costs
literally nothing on the hot path.

**Why this is not a backdoor.** Four structural reasons, and they should be in
the header comment as well as here:

1. `KnownSource` is a **closed enum with one enumerator**. A profile selects an
   enumerator; it cannot describe one. Adding a second is a code change with a
   code review, not a config change.
2. The enumerator names an **API contract, not a resource**. It resolves at
   capture time through the swapchain the hook was handed. Not even the
   operator can point it somewhere else.
3. The manifest carries the distinction to the consumer (§4), with
   **non-uniform fields** — `known_by_construction` surfaces have no `score`
   key at all, so a tool that reads scores uniformly gets a `KeyError`, not a
   plausible number.
4. The rule, stated once: *a surface may be known by construction only if a
   documented API contract names the resource. Otherwise it must be
   identified.* Colour is the only surface in D3D12 that qualifies. There is no
   `GetVelocityBuffer()`, no `GetSceneDepth()`, no `GetGBufferA()`.

### 3.2 Normals: a `SurfaceSpec`, with the unknowns flagged

The analogy target is UE5's **GBufferA** (world-space normal), written in the
base pass. This project has already observed the relevant pass:
`DEBUGGING.md:1270-1276` records that the target's base pass binds **five
render targets** with velocity as `SV_Target4`. GBufferA is `SV_Target0` of
that same pass. That is unusually good starting evidence and it is why normals
is a credible second proof point rather than speculation.

Proposed spec, as it would appear in a profile:

```jsonc
"normals": {
  "source": "identified",
  "formats": [
    { "format": 24, "score": 4, "name": "R10G10B10A2_UNORM  (PF_A2B10G10R10 - UE5 default GBufferA)" },
    { "format": 10, "score": 3, "name": "R16G16B16A16_FLOAT (PF_FloatRGBA - high-precision GBuffer)" },
    { "format": 11, "score": 2, "name": "R16G16B16A16_UNORM (PF_A16B16G16R16 - COLLIDES with velocity, see below)" },
    { "format": 28, "score": 1, "name": "R8G8B8A8_UNORM     (low-precision GBuffer)" }
  ],
  "required_flags":  ["ALLOW_RENDER_TARGET"],
  "forbidden_flags": ["DENY_SHADER_RESOURCE"],
  "mips": 1, "array_size": 1, "samples": 1, "layout": "UNKNOWN",
  "extent_from": "velocity",
  "trigger": "render_target_to_shader_resource",
  "arbitration": "first_in_frame",
  "max_copies_per_frame": 1,
  "min_frames_observed": 30,
  "edges_per_frame_min": 0.5,
  "ambiguity": { "kind": "score_margin", "value": 2 },
  "required": false,
  "enabled": false
}
```

Every field is data. **No subclass proposed.**

The format list is offered as *candidates to verify*, not asserted. UE5's
GBuffer layout is controlled by `r.GBufferFormat`, and the mapping from that
cvar to a DXGI format has to be read out of the engine version in question the
same way velocity's was (`velocity_identify.cpp:29-45` cites
`FVelocityRendering::GetFormat`; depth's cites `D3D12RHI.cpp:117/127`). The
scores above encode "most likely first", not "known". Given this project's own
history with `R16G16B16A16_FLOAT` vs `_UNORM` (README, the format-enum bug),
these four entries should be re-derived from the 5.2 and 5.7 trees before
anyone runs a capture against them.

**What makes normals structurally distinguishable — the honest, layered answer:**

*Against velocity.* The format sets are disjoint under the current tables —
until they aren't. **`R16G16B16A16_UNORM` (11) appears in both**: it is
velocity's top-scoring format on this project's own target
(`velocity_identify.cpp:40`) and a plausible GBufferA format under a
high-precision `r.GBufferFormat`. Discriminators that survive the collision:
(a) velocity requires `ALLOW_UNORDERED_ACCESS` from `GetCreateFlags`
(`velocity_identify.cpp:47-51`) and GBufferA generally does not — but
"generally" is exactly the kind of claim this project has already been burned
by (README: Skyrunner's velocity *does* enter `UNORDERED_ACCESS`, falsifying an
Oxi "invariant"), so it must be scored evidence, not a gate; (b) both surfaces
are live simultaneously, so **mutual exclusion** applies — see §3.3.

*Against colour.* Same format is *likely*, not merely possible: this target's
back buffer **is** `R10G10B10A2_UNORM` (`DEBUGGING.md:299`). The distinguisher
is extent — normals sits at the scene extent via `extent_from: "velocity"`
(1212×760) while the back buffer is 1707×1067. But that separation is a
property of DRS being on. At 100% screen percentage with DRS off they coincide,
and a normals identifier could select the back buffer. Guard: **known-source
exclusion** — see §3.3.

*Cardinality:* 1.

**Unknowns, flagged rather than guessed. Each needs runtime observation of an
actual UE5 GBuffer pass:**

1. **Does GBufferA take its own `ResourceBarrier` call, or does the whole MRT
   set transition in one array?** If the latter, several surfaces trigger from
   one hook call, and the arbitration order *within* a barrier array is
   currently just array order (`d3d12_hook.cpp:336`). Needs a barrier trace.
2. **Is GBufferA transient/placed like velocity** (so it must be copied at the
   edge, README "Extraction") **or does it survive to Present?** Changes
   nothing about the spec but changes how alarming a missed edge is.
3. **What is `r.GBufferFormat` on the target**, and therefore which format entry
   actually fires.
4. **Does GBufferA's edge count per frame match velocity's?** Velocity is
   written twice — base pass and dedicated velocity pass
   (`DEBUGGING.md:1272-1276`) — and the dedicated pass binds only velocity and
   depth, so GBufferA should show *fewer* writes and a different pair histogram.
   Predicted from the disassembly already in `DEBUGGING.md`; not observed.
5. **Is Substrate on?** UE 5.5+/5.7 with Substrate replaces the classic GBuffer
   with a material buffer, and a standalone world-normal render target may not
   exist at all. Since scope is 5.2 vs 5.7, this is a live variant — and it is
   exactly what profiles are for: `ue5.7-substrate` sets
   `"normals": {"enabled": false, "reason": "substrate"}`.
6. **The normal encoding** (octahedral vs. direct, and whether it differs 5.2 →
   5.7) is a Python-side unknown. The manifest should carry
   `"encoding": "unknown"` rather than a guess, and the loader should refuse to
   decode rather than produce a plausible normal field.

### 3.3 Do these two prove out workstream 3?

**Mostly yes — with two forced changes, stated here rather than bolted on
later.**

**Colour proves the ownership half.** It exercises `SurfaceCapture`, readback
reuse, layout-epoch revalidation, atomicity membership and manifest
representation for a surface with no identifier at all — which proves
`SurfaceCapture` is not secretly "the output of a `SurfaceIdentifier`". *Forced
change:* `SourceKind` must exist in workstream 3's `SurfaceCapture` **and
schema from day one**. Retrofitting it means a schema version bump after dumps
exist.

**Depth proves the generic identifier on a different trigger edge and a
different tiebreak.** Normals is a materially harder test: it is the first case
where **two identifiers see the identical event stream** (`RT → SRV`) and must
not converge. That forces two invariants into `SurfaceRegistry`:

1. **Mutual exclusion.** At most one surface may hold a given resource. If a
   second identifier selects a resource already held, the registry refuses the
   selection and logs a collision, rather than silently double-capturing the
   same texture into two blobs. Not expressible in `SurfaceSpec` — it is a
   relation between surfaces.
2. **Known-source exclusion.** No identifier may select a resource obtained
   from `IDXGISwapChain::GetBuffer`. `CaptureSystem` already holds those
   pointers; feed them to `SurfaceRegistry::NoteKnownSourceResource` and have
   the structural gate reject membership. Note this is *not* a profile
   assertion — it is the same API contract that gives colour its identity,
   used as an exclusion rather than an assertion.

Neither forces a change to the data-only rule for `SurfaceSpec`, and neither
forces a new subclass. Net result: **one subclass (velocity/rival adoption),
one non-identifier adapter concept (colour), two registry invariants.** That is
the abstraction earning its keep rather than being asserted.

---

## 4. Schema

### 4.1 `manifest.json`, version 1

Written **atomically at burst end**: to `manifest.json.tmp`, then
`MoveFileExA(tmp, final, MOVEFILE_REPLACE_EXISTING)`, which is the Windows
atomic-rename primitive — a reader sees the old file or the whole new one,
never a torn one. It is enqueued as the **last `WriteJob` of the burst**, so
the writer thread's existing FIFO ordering guarantees every blob it references
is already on disk before the manifest naming them appears. No new
synchronisation.

```json
{
  "schema_version": 1,
  "producer": {
    "tool": "mv_hook",
    "commit": "0036149",
    "built": "2026-08-14T09:12:03Z"
  },
  "session": {
    "process": "Skyrunner-Win64-Shipping.exe",
    "started_utc": "2026-08-26T18:04:11Z",
    "sealed_utc": "2026-08-26T18:04:39Z",
    "profile": "ue5.2-generic",
    "back_buffer": { "width": 1707, "height": 1067, "format": 24 }
  },
  "engine": { "major": 5, "minor": 2, "source": "MV_ENGINE_VERSION" },

  "burst": {
    "requested_frames": 2400,
    "recorded_frames": 1187,
    "drained_frames": 1184,
    "ended_because": "velocity_pass_silent"
  },

  "surfaces": {
    "velocity": {
      "present": true,
      "source": {
        "kind": "identified",
        "state": "selected",
        "decided_at_frame": 91,
        "score": 14,
        "margin": 6,
        "survivors": 2,
        "unique_structural_match": false,
        "signals": [
          { "name": "format is a desktop velocity format",  "weight": 4, "fired": true },
          { "name": "at the modal render-target resolution", "weight": 3, "fired": true },
          { "name": "flags are exactly RT|UAV",              "weight": 2, "fired": false },
          { "name": "produced once per frame (one producer)","weight": 2, "fired": true },
          { "name": "never takes a UAV barrier",             "weight": 2, "fired": false },
          { "name": "never enters UNORDERED_ACCESS",         "weight": 2, "fired": false },
          { "name": "strict 2 events every frame (Oxi shape)","weight": 3, "fired": false },
          { "name": "no state-continuity violations",        "weight": 1, "fired": true }
        ],
        "adoptions": 3,
        "reopens": 0
      },
      "format": 11,
      "trigger": "render_target_to_shader_resource",
      "arbitration": "first_in_frame",
      "required": true,
      "file_pattern": "vel_%05d.bin",
      "coverage": { "strong": 1102, "weak": 82, "none": 0 },
      "encoding": {
        "kind": "ue5_scene_velocity",
        "gamma": "sqrt",
        "z_low_mask": 65535,
        "has_pixel_animation_bit": false
      }
    },

    "depth": {
      "present": true,
      "source": {
        "kind": "identified",
        "state": "selected",
        "decided_at_frame": 91,
        "score": 4,
        "margin": null,
        "survivors": 1,
        "unique_structural_match": true,
        "extent_from": "velocity",
        "edges_per_frame": 3.94,
        "reopens": 0
      },
      "format": 44,
      "trigger": "depth_write_to_readable",
      "arbitration": "last_before_surface",
      "arbitrate_before": "velocity",
      "max_copies_per_frame": 4,
      "required": true,
      "file_pattern": "depth_%05d.bin",
      "coverage": { "strong": 0, "weak": 1184, "none": 0 },
      "encoding": { "kind": "device_z", "reversed_z": true, "linearised": false }
    },

    "color": {
      "present": true,
      "source": {
        "kind": "known_by_construction",
        "mechanism": "IDXGISwapChain3::GetBuffer(GetCurrentBackBufferIndex())",
        "contract": "the swapchain names this resource; it is not inferred",
        "copy_source_state": "PRESENT",
        "back_buffer_index_varies": true
      },
      "format": 24,
      "trigger": "present_back_buffer",
      "arbitration": "first_in_frame",
      "required": true,
      "file_pattern": "color_%05d.bin",
      "coverage": { "by_construction": 1184 },
      "encoding": { "kind": "display_referred", "apply_gamma": false }
    },

    "normals": {
      "present": false,
      "reason": "not_enabled",
      "detail": "spec exists in profile ue5.2-generic but enabled=false; no identification was attempted"
    }
  },

  "layouts": [
    {
      "epoch": 0,
      "first_capture_index": 0,
      "surfaces": {
        "velocity": { "format": 11, "bytes": 7393248, "subresources": 1,
                      "planes": [ { "index": 0, "offset": 0, "width": 1212, "height": 760, "row_pitch": 9728, "format": 11 } ] },
        "depth":    { "format": 44, "bytes": 4669356, "subresources": 2,
                      "planes": [ { "index": 0, "offset": 0,       "width": 1212, "height": 760, "row_pitch": 4864, "format": 46 },
                                  { "index": 1, "offset": 3696640, "width": 1212, "height": 760, "row_pitch": 1280, "format": 49 } ] },
        "color":    { "format": 24, "bytes": 7375020, "subresources": 1,
                      "planes": [ { "index": 0, "offset": 0, "width": 1707, "height": 1067, "row_pitch": 6912, "format": 24 } ] }
      }
    },
    {
      "epoch": 1,
      "first_capture_index": 640,
      "reason": "velocity layout changed (dynamic resolution scaling)",
      "surfaces": {
        "velocity": { "format": 11, "bytes": 5910528, "subresources": 1,
                      "planes": [ { "index": 0, "offset": 0, "width": 1080, "height": 684, "row_pitch": 8704, "format": 11 } ] },
        "depth":    { "format": 44, "bytes": 4204800, "subresources": 2,
                      "planes": [ { "index": 0, "offset": 0,       "width": 1080, "height": 684, "row_pitch": 4352, "format": 46 },
                                  { "index": 1, "offset": 2976768, "width": 1080, "height": 684, "row_pitch": 1280, "format": 49 } ] },
        "color":    { "format": 24, "bytes": 7375020, "subresources": 1,
                      "planes": [ { "index": 0, "offset": 0, "width": 1707, "height": 1067, "row_pitch": 6912, "format": 24 } ] }
      }
    }
  ],

  "frames": {
    "file": "frames.csv",
    "columns": ["capture_index", "game_frame_index", "layout_epoch", "surfaces_mask", "coverage"],
    "surface_bit_order": ["velocity", "depth", "color", "normals"]
  },

  "integrity": {
    "sealed": true,
    "required_surfaces": ["velocity", "depth", "color"],
    "dropped_frames_backpressure": 3,
    "dropped_frames_incomplete": 12,
    "frames_without_velocity": 2,
    "slots_missed_busy": 41,
    "writes_dropped": 0
  }
}
```

Design notes on the schema itself:

- **`layouts` is an array of epochs**, not a single record. That is the fix for
  finding 1.13 carried into the data model: a mid-session DRS change adds an
  epoch instead of silently invalidating one global layout record.
- **`source` is deliberately non-uniform.** `identified` carries `score`,
  `margin`, `survivors`, `signals`. `known_by_construction` carries
  `mechanism` and `contract` and **has no `score` key at all**. A consumer
  cannot accidentally treat them uniformly; it gets a `KeyError`, which is the
  point.
- **`coverage` is per surface**, so 1.2's over-claim becomes visible. Note the
  realistic values above: depth shows `weak: 1184` because nothing today proves
  its list was submitted. Making that visible is the deliverable; making it
  *strong* is separate work.
- **`unique_structural_match`** preserves the distinction the logs already draw
  (`velocity_identify.cpp:673-682`, `depth_identify.cpp:294-307`) between "the
  only thing it could have been" and "the top of several, chosen on evidence".
  That distinction is currently only in a log file.
- **`encoding`** replaces the current arrangement where the decode constants
  live in `mvtools.py` and the engine version arrives via
  `engine_version_major/minor` (`capture.cpp:514-532`, consumed at
  `mvtools.py:241-268`). The dump carries what it means, not just what it is.
- **`normals.present: false` with a `reason`** is a first-class state. A
  consumer distinguishes `not_enabled`, `never_identified`, `ambiguous` and
  `substrate` without reading a log.

### 4.2 `frames.csv`

```
# capture_index,game_frame_index,layout_epoch,surfaces_mask,coverage
0,4181,0,7,strong
1,4182,0,7,strong
2,4183,0,7,weak
3,4185,0,7,strong
...
639,4831,0,7,strong
640,4833,1,7,strong
```

Same append-per-drained-frame mechanism as today (`capture.cpp:632-637`), three
columns wider. `surfaces_mask` is the bitmask in
`manifest.frames.surface_bit_order`; `7` = velocity|depth|colour present,
normals absent. `layout_epoch` indexes `manifest.layouts`. The column names are
in *both* the header comment and the manifest, so the loader validates
positions rather than assuming them.

Note the honest gaps this format already exposes: capture indices 2→3 map to
game frames 4183→4185, so that pair is not temporally adjacent and
`consecutive_pairs` drops it — the mechanism `mvtools.py:530-561` already
implements, now with the layout epoch alongside so a pair straddling an epoch
boundary can be dropped too. **That is a new correctness check** the current
format cannot express: warping frame 639 by a field decoded at epoch 0's
extent against frame 640 at epoch 1's extent is a silent error today.

### 4.3 The Python loader gate

One function, and it is the only door into a dump.

```python
SUPPORTED_SCHEMA = 1
_MIGRATIONS = {}          # older versions this build can still read

def load_dump(dump_dir, *, allow_unsealed=False) -> Dump: ...
```

`Dump` exposes `dump.surface("velocity")`, `dump.has("normals")`,
`dump.frames`, `dump.pairs()`, `dump.layout(epoch)`. Two surface dataclasses,
not one:

```python
@dataclass(frozen=True)
class IdentifiedSurface:   # has .score, .margin, .signals, .survivors, .state
@dataclass(frozen=True)
class KnownSurface:        # has .mechanism, .contract - and no .score attribute
```

so the manifest's non-uniformity survives into the loader instead of being
flattened by it. `dump.surface("color").score` raises `AttributeError` at the
type level.

Version-gate behaviour, exhaustively:

| condition | behaviour |
|---|---|
| no `manifest.json` | **Refuse.** If a `meta.txt` is present, say so by name: "this is a pre-manifest dump; re-capture, or use `tools/legacy_meta.py`." |
| `schema_version` key absent | **Refuse.** A manifest without a version is not a manifest. |
| `schema_version` > `SUPPORTED_SCHEMA` | **Refuse**, naming both numbers. Never best-effort — forward-compatible guessing is how a v2 semantic change silently mis-decodes. |
| `schema_version` < `SUPPORTED_SCHEMA`, in `_MIGRATIONS` | Accept, apply the migration, and record `dump.migrated_from`. |
| `schema_version` < `SUPPORTED_SCHEMA`, not in `_MIGRATIONS` | **Refuse**, naming the version and what would need adding. |
| `integrity.sealed` false or absent | **Refuse** unless `allow_unsealed=True`, which prints a loud multi-line banner naming the dump and every reason it is unsealed. |
| a *surface object* is missing required keys | Refuse **that surface**, record why in `dump.unusable["normals"]`; the rest of the dump loads. |
| `velocity` unusable | **Refuse the dump.** Everything downstream keys off velocity (`capture.cpp:1123-1137` already encodes this). |
| surface `present: false` | `dump.has(name)` is `False`; `dump.surface(name)` raises with the manifest's own `reason` string, not a `KeyError`. |
| surface `present: true` but a frame's blob is missing/short | that frame is excluded from `dump.frames`, and the count is reported in `dump.notes`. |
| `layouts` missing an epoch referenced by `frames.csv` | **Refuse the dump.** This is the 1.13 failure made loud. |

**A deliberate trade to state plainly:** because the manifest is written at
burst end, a crashed or force-quit session leaves an unsealed dump that the
loader refuses. That is a *change* — today a partial dump is silently readable,
which is how 1.13-style corruption reaches a result. `allow_unsealed` exists so
the frames are not lost, and it is loud on purpose.

**Migration of existing callers:** the seven call sites in 1.18 each collapse
from `meta = mvtools.load_meta(d)` + `meta["velocity_width"]` to
`dump = mvtools.load_dump(d)` + `dump.surface("velocity").width`.
`load_meta` is deleted in Phase 5, not deprecated — a second door defeats the
gate.

---

## 5. Migration sequence

Each phase is independently shippable and has a stated way to verify it changed
no behaviour. The ordering principle: **every phase that touches identification
lands on a codebase that already has a reference-dump oracle and a manifest.**

### Phase 0 — the oracle. *Needs care.*

`mv_testhost` already runs the capture path under the D3D12 debug layer with
four structural look-alikes, picks correctly every run, and already writes a
dump to `MV_DUMP_DIR`. What was missing is a way to compare two of them.

**Correction to an earlier draft of this plan**, which specified every phase
below as "golden dump byte-identical". That verification does not exist and no
refactor can create it. Which frames land depends on ring-slot contention -
four slots in flight makes the hook skip a frame *by design* (`g_noFreeSlot`,
`capture.cpp:804`) - and that is a function of GPU/CPU timing, not of the code
under test. Two runs of the current build at 240 frames captured 84 and 86
frames respectively.

What *is* reproducible is the content of a given **game frame**. Measured over
the 61 game frames those two runs shared: velocity, depth and colour all
byte-identical, 61 of 61. So `tools/compare_dumps.py` keys on the game frame
index `frames.csv` records - never the capture index, which is only a counter
of what was drained and means nothing across runs - and compares:

- metadata, which must match exactly (it reads `manifest.json` once that exists
  and `meta.txt`/`meta_depth.txt` before, so the oracle survives Phase 4);
- every surface blob, on the **intersection** of game frames.

The honest limit, printed on every run: this proves equivalence only on frames
both runs happened to capture. A regression confined to frames one run missed
could hide. So the shared-frame count is gated - below `--min-common` the tool
reports `WEAK` and exits 2 rather than passing.

Capture a reference dump before each phase:

```
set MV_AUTOCAPTURE=1
set MV_DUMP_DIR=%TEMP%\mv_ref_before
build\testhost\Release\mv_testhost.exe --hook <abs>\mv_hook.dll --frames 240
:: ...make the change, rebuild, capture to mv_ref_after...
python tools\compare_dumps.py %TEMP%\mv_ref_before %TEMP%\mv_ref_after
```

*Verification:* the tool is verified against planted faults - a single flipped
byte in one frame's velocity blob is localised to that surface and that game
frame, and a changed metadata value is reported as a key-level diff. A
comparison tool that can only ever return "equivalent" is worth nothing.

Without this, every phase below is unfalsifiable.

### Phase 1 — `Slot` → surface vector. *Small model, with review.*

Mechanical. Three specs hardcoded in C++, no JSON, no registry, no new
surfaces. Move the five triplicated field sets into `SurfaceCapture` and a
fixed-size-3 `std::vector`. Velocity/depth/colour code paths are otherwise
untouched, just indexed.

*Verify:* `compare_dumps.py` reports EQUIVALENT; `mv_hook.log` line-identical modulo
pointer values.

### Phase 2 — size-before-map and single-hold enqueue. *Small model, careful review.*

Fixes 1.3, and makes partial enqueue un-representable (§2.7).

*Verify:* `compare_dumps.py` reports EQUIVALENT. New unit test: with the write queue
artificially full, a frame is dropped and **nothing** for it reaches disk.

### Phase 3 — colour becomes a `SurfaceCapture`. *Small model.*

`source: known_by_construction`, a real `present` flag, nine early returns
collapse to one gate. Still no identifier, no registry.

*Verify:* `compare_dumps.py` reports EQUIVALENT. Fault-injection test: force `GetBuffer`
to fail and confirm nothing is written for that frame.

Colour is early precisely because it *removes* machinery rather than adding it.

### Phase 4 — `manifest.json` v1, written alongside the old files. *Needs care.*

Both `meta.txt`/`meta_depth.txt` and `manifest.json` are emitted. `load_dump`
is added alongside `load_meta`. One script — `inspect_velocity.py`, the
smallest — is switched.

*Verify:* an automated test asserting every manifest value equals its
`meta.txt` counterpart; `inspect_velocity.py` produces identical output through
both paths.

Schema decisions are permanent. This phase is where `SourceKind` and the
`layouts` epoch array must already be right (§3.3).

### Phase 5 — switch the remaining six scripts; delete `load_meta`. *Small model, strong oracle.*

Stop writing `meta.txt`/`meta_depth.txt`. Fix the `BURST` drift (1.17) by
deleting the constant.

*Verify:* `run_validation.py` reproduces `results/validation_skyrunner.txt`
**exactly** against an archived dump. The repo already contains that artefact,
which makes this an unusually strong regression test for a mechanical change.

### Phase 6 — extract `SurfaceIdentifier` from `velocity_identify.cpp`. *Needs care. Do not delegate.*

Velocity is its only user; `VelocitySurfaceIdentifier` holds rival adoption.
`depth_identify.cpp` is untouched.

*Verify:* `mv_testhost` picks the same resource out of four look-alikes every
run and still refuses to pick between two indistinguishable candidates; the
2000-frame barrier-profile dump is identical for an archived session.

### Phase 7 — depth becomes a `SurfaceSpec`; `depth_identify.cpp` is deleted. *Needs care.*

**This is where the abstraction is proven or it isn't.**

*Verify:* the same depth resource is selected on an archived session and the
`depth: SELECTED` line matches. If depth turns out to need code, the honest
outcome is a `DepthSurfaceIdentifier` and a note in this document saying the
generic base covers one surface of three — not a widened `SurfaceSpec` with a
depth-shaped hole in it.

### Phase 8 — `SurfaceRegistry`. *Needs care. Do not delegate.*

Shared profile store (1.16), derived reopen cascade (1.9), registry-owned
seen-set (1.7), per-identifier candidate sets (1.11, 1.12), mutual exclusion
and known-source exclusion (3.3), frozen required-mask (1.14).

**This is the one phase that deliberately changes behaviour**, so its
verification is positive tests rather than byte-identity:

- forcing a swapchain resize mid-session must *stop* capture until
  reidentification completes (today it does not — 1.11);
- forcing a depth-descriptor change must produce a depth answer again
  afterwards (today it cannot — 1.7);
- a mid-burst reopen must end the burst rather than silently drop a surface
  from later frames (today it does the latter — 1.14).

Write those three tests **before** the phase, or the bugs move into the
registry under nicer names.

### Phase 9 — JSON profiles. *Small model, with the negative test.*

Specs move to `profiles/ue5.json`. `ue5.2-generic` and `ue5.7-generic`.

*Verify:* `compare_dumps.py` reports EQUIVALENT under the profile that reproduces today's
hardcoded values. Negative test: a profile narrowing velocity's format list to
the wrong entry must produce "NO candidate", loudly, and must not fall back.

### Phase 10 — normals. *Needs care, and needs a running game.*

New spec, `enabled: false` by default, opt in with `MV_SURFACES=normals`.

*Verify:* with it disabled, `compare_dumps.py` reports EQUIVALENT. With it enabled on a
live UE5 title, read the `identify:` log and the mutual-exclusion log **before**
trusting a single byte. It is last because it is the only phase whose open
questions (§3.2) cannot be closed from source.

---

## 6. What this refactor would paper over

Flagged deliberately. Each is a real weakness that a tidy abstraction would
make less visible rather than fix.

1. **Finding 1.11 — the candidate set surviving a reopen.** Phase 8 fixes it
   *only if it is treated as a bug fix with its own test*. Written as a
   straight port, the same hole moves into `SurfaceRegistry` and acquires a
   better name. This is the single most important item in this section.

2. **The strong/weak/none coverage numbers over-claim, and per-surface
   coverage makes that visible without making it better.** The realistic
   manifest values in §4.1 show depth at `weak: 1184` — which is honest, and
   which will look like a regression to anyone reading only the summary line.
   It is not a regression; it is the first time the number has been measured.
   Measuring it is worth doing *before* designing anything around depth's
   fence coverage.

3. **AMBIGUOUS is still terminal (1.6), and making it configurable hides
   that.** Turning the tiebreak into `ambiguity: {kind, value}` makes it
   tunable and quietly implies the state machine has a recovery arc. It does
   not. Recommendation: give the base class an `Ambiguous → Searching`
   transition on a bounded retry with a widened window, symmetric with the
   empty-survivors path that already retries (1.6). Do not let "it's data now"
   stand in for "there is still no way out".

4. **`DrainSlot` on the render thread (1.4) is not addressed by any phase
   here.** Worse, the surface vector makes it *easier* to add a fourth 7 MB
   `memcpy` to the Present hook without anyone noticing. Two real options —
   move the map+memcpy to the writer thread (requires the readback buffer to
   stay owned across the handoff, which is a genuine change), or add a
   per-Present byte budget with a log line. Neither is in this plan. It is
   **deliberately deferred, not solved**, and this document should not be read
   as having solved it.

5. **The two profile maps and the global mutex per transition (1.16).** The
   registry fixes this only if the shared store is built in Phase 8. A
   per-surface identifier that each keeps its own map would make the hot path
   worse in proportion to the surface count — and it would look exactly like
   the abstraction working.

6. **`GetDesc()` on a barrier-encountered resource is still an unproven bet.**
   `resource_tracking.h:36-46` documents the recycled-address hole in the seen
   set and `:52-61` documents the budget that bounds the exposure. Nothing here
   changes either. A reader of the refactored code should not be able to infer
   they were fixed; keep those comments verbatim.

7. **The identify budget and reopen still interact fragilely.**
   `ForgetAllResourcesSeen` refills the budget (`resource_tracking.cpp:128`),
   which is right, but nothing else does — so a long session that exhausts 8000
   and never reopens can never notice a replacement. Documented, not fixed,
   and staying that way.

8. **`kCaptureFrames = 2400` is described as "~20s of gameplay"
   (`capture.cpp:26-30`) and the README says 60** (1.17). Deleting the Python
   constant fixes one of three copies. The README is the other.

---

## 7. Risks and open questions

**Could not be determined from the code; needs runtime observation:**

- Whether GBufferA takes its own `ResourceBarrier` call or arrives as one entry
  in a multi-barrier array with the other GBuffer targets (§3.2 #1). Determines
  whether intra-array arbitration needs to exist at all.
- Whether GBufferA is a transient placed resource like velocity (§3.2 #2).
- `r.GBufferFormat` on any real target, and therefore which normals format
  entry fires (§3.2 #3).
- Whether the `R16G16B16A16_UNORM` collision between velocity and normals
  occurs in the wild, or only in principle (§3.2, §3.3).
- Whether Substrate is enabled on any 5.7 target in scope, and if so whether a
  standalone world-normal surface exists at all (§3.2 #5).
- The normals encoding, and whether it differs between 5.2 and 5.7 (§3.2 #6).

**Could not be determined because it has never been measured:**

- **Depth's actual fence coverage.** 1.2 says the grading has never run for
  depth. Until Phase 1 puts `recordedList` in `SurfaceCapture`, nobody knows
  whether depth readback has ever been covered. Measure before designing around
  it.
- **How often a mid-burst layout change actually happens** on a DRS title. That
  decides whether the layout-epoch model (§4.1) is right, or whether a layout
  change should simply end the burst. One instrumented DRS session settles it.

**Design risks:**

- **Phase 7 may fail.** Depth may turn out to need code. The plan's answer is
  to say so rather than widen `SurfaceSpec` until it fits — a spec with a
  depth-shaped hole is worse than a subclass.
- **Refusing unsealed dumps (§4.3) will lose data the first time a session
  crashes.** `allow_unsealed` mitigates it; the trade is deliberate and should
  be re-examined after the first real crash.
- **The frozen required-mask (§2.7) will end bursts that today complete.** That
  is the point — those bursts are currently producing heterogeneous dumps
  (1.14) — but it will look like a regression in frame counts and should be
  announced as such.

---

## 8. Future considerations — out of scope

**Barrier trace recorder and offline replay harness.** Once identification is a
pure function of `(SurfaceSpec, event stream)` — which is precisely what
Phases 6–9 make it — recording the event stream turns **every past session into
a regression test**. It is also the natural way to settle most of §7's open
questions without a running game: a captured barrier trace from a UE5 title
with a GBuffer pass would answer §3.2's first four unknowns offline, and a
replay of an archived session would let Phase 7 and Phase 8 be verified against
real event streams rather than `mv_testhost`'s synthetic four.

Noted as the obvious follow-on. **Not specified here.**

**Cross-engine portability** is explicitly out of scope, as stated at the top.
`SurfaceSpec` is D3D12/UE5-shaped on purpose.
