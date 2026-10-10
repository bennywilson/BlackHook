#pragma once

#include <d3d12.h>

#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// The generic half of finding a scene texture without already knowing its
// descriptor - factored out of velocity_identify.cpp per
// docs/REFACTOR_PLAN.md sec 2.3/2.4 (Phase 6), then generalised in Phase 7 to
// also cover depth (sec 2.4: "the proof point the whole design rests on"),
// then wired into a shared SurfaceRegistry in Phase 8 (sec 2.5) - see
// surface_registry.h for the part that owns multiple identifiers and fans
// out from one barrier walk.
//
// Phase 7's finding: depth genuinely differs from velocity in more ways than
// the plan's sec 2.4 sketch names (trigger edge and extent linkage), but
// every one of them is a SCALAR/ENUM/BOOL difference, not new mutable
// cross-call state - the test in sec 2.4 ("if it could be expressed as data
// in a profile, it isn't a subclass") is met. What depth needed beyond the
// original sketch, and why each is data not code:
//
//   - forbiddenFlags: depth rejects DENY_SHADER_RESOURCE; velocity has none.
//   - trigger (TriggerEdge): which edge counts as "produced then consumed".
//   - extentFrom / SetExtentSource: depth's structural gate is gated on
//     ANOTHER identifier's live extent, not a static back-buffer bound.
//   - formatScoreGate: velocity's format-tier signal is a gated bonus
//     (score counts only above a threshold); depth's score IS the raw
//     format score, always. Both are "how much does format score
//     contribute", just with a different gate value (4 vs 0).
//   - reopenClearsShortlist: velocity's reopen throws away the shortlist and
//     re-scans everything; depth's reopen deliberately keeps its shortlist
//     and only resets measurement counters (see the long comment on
//     Reopen() about why re-scanning would have made depth losable).
//   - logPrefix: the plan's own Phase 7 verify bullet names the exact line
//     to check - "the `depth: SELECTED` line matches" - so the prefix is
//     preserved as data.
//
// Phase 8 removed one more field that Phase 6/7 still had:
// usesSharedCandidateSet. Every identifier now owns its own candidate set
// (see MarkCandidate/IsCandidate/ValidateCandidate below) - there is no more
// "shared vs local" distinction to encode, because resource_tracking.h's old
// process-wide g_candidateResources map is gone (docs/REFACTOR_PLAN.md
// findings 1.11/1.12).
//
// SCOPE NOTE: this remains the identification-relevant subset of the plan's
// SurfaceSpec (sec 2.1). The capture-side fields - source/known,
// arbitration, required/enabled - stay on capture.cpp's own (TU-local)
// SurfaceSpec until Phase 9 actually needs one struct for both concerns.

namespace mv
{

enum class IdentifyState : uint8_t
{
    Searching, // settling window still open, or no candidate survived yet
    Selected,
    Ambiguous, // decided NOT to pick; see AmbiguityRule
};

enum class ReopenReason : uint8_t
{
    DescriptorChanged,
    BackBufferResized,
    DependencyReopened, // e.g. depth reopening because velocity's own search reopened
};

// Which barrier edge means "this surface was just produced and is about to
// be consumed" - the one moment a transient/placed resource is safe to copy.
enum class TriggerEdge : uint8_t
{
    RenderTargetToShaderResource, // velocity (and, per docs/REFACTOR_PLAN.md sec 3.2, normals later)
    DepthWriteToReadable,         // depth
};
constexpr int kTriggerEdgeCount = 2;

// True for any non-empty subset of the shader-resource state bits. UE5's RDG
// maps SRVMask -> ALL_SHADER_RESOURCE (0x40|0x80) and pixel-shader-only reads
// (SRVGraphics) -> PIXEL_SHADER_RESOURCE (0x80).
inline bool IsShaderResourceState(int state)
{
    return state != 0 && (state & D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) == state;
}

// True for the states a depth buffer is read in: DEPTH_READ or any subset of
// the shader-resource bits (UE5 consumes depth both ways).
inline bool IsDepthReadableState(int state)
{
    if (state == 0)
    {
        return false;
    }
    constexpr int kReadable =
        static_cast<int>(D3D12_RESOURCE_STATE_DEPTH_READ) | static_cast<int>(D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    return (state & ~kReadable) == 0;
}

// The two tests above, generalised over TriggerEdge so BarrierProfileStore
// can compute per-edge counts once per transition without knowing which
// identifiers exist (docs/REFACTOR_PLAN.md sec 2.5 point 1 / finding 1.16).
// SurfaceIdentifier::IsTriggerEdge/IsReverseTriggerEdge are thin wrappers
// over these, scoped to spec_.trigger, kept for the liveness-tracking call
// sites that only ever care about one identifier's own edge.
bool IsTriggerEdgeFor(TriggerEdge trigger, int stateBefore, int stateAfter) noexcept;
bool IsReverseTriggerEdgeFor(TriggerEdge trigger, int stateBefore, int stateAfter) noexcept;

struct FormatCandidate
{
    DXGI_FORMAT format;
    int score; // desktop D3D12 path outranks GLES-only/legacy fallbacks
    const char* name;
};

// One weighted piece of BONUS behavioural/structural evidence, on top of the
// format-score baseline (see SurfaceSpec::formatScoreGate). Mirrors
// CollectSignals from the pre-extraction velocity_identify.cpp verbatim -
// same seven signals, same weights, same meaning. Depth uses none of these
// (signalCount == 0); its score is the format-score baseline alone.
enum class SignalId : uint8_t
{
    AtModalResolution,
    ExactRequiredFlags,
    ProducedOnceExactly, // one produce->consume edge per frame, +-10%
    NeverTakesUavBarrier,
    NeverEntersUnorderedAccess,
    StrictTwoEventsPerFrame,
    NoContinuityViolations,
};

struct SignalWeight
{
    SignalId id;
    int weight;
};

enum class AmbiguityRule : uint8_t
{
    // Top two survivors must differ by at least `ambiguityMargin` points.
    ScoreMargin,
    // Separated if scores differ OR the winner's trigger-edge count exceeds
    // the runner-up's by at least `ambiguityMargin` percent (depth: 150 ==
    // 1.5x - "4 DEPTH_WRITE->readable edges measured" vs "renders at most
    // once", the pre-extraction depth_identify.cpp comment for this rule).
    EdgeCountRatio,
};

// Data-only. Every field here is what velocity_identify.cpp/depth_identify.cpp
// used to spell as file-scope constants or hand-written control flow; the
// values are unchanged, just given one home.
struct SurfaceSpec
{
    const char* name;
    const char* logPrefix; // "identify" for velocity, "depth" for depth

    // --- structural gate ---------------------------------------------
    FormatCandidate formats[8];
    uint8_t formatCount;
    UINT requiredFlags;
    UINT forbiddenFlags;     // 0 == none. depth: DENY_SHADER_RESOURCE.
    UINT64 minWidth;         // 0 == no minimum (depth has none)
    UINT minHeight;          // 0 == no minimum (depth has none)
    uint8_t maxBackBufferMultiple; // velocity: 2. 0 == unbounded (depth).

    // --- extent linkage ----------------------------------------------
    // Documentation AND, as of Phase 8, functional data: SurfaceRegistry::
    // Configure() resolves this name to the identifier it names and calls
    // SetExtentSource() itself (docs/REFACTOR_PLAN.md sec 2.5 point 2,
    // finding 1.9) - callers no longer wire this by hand. nullptr for
    // velocity, "velocity" for depth.
    const char* extentFrom;

    // --- behaviour ------------------------------------------------------
    TriggerEdge trigger;
    uint64_t minFramesObserved;
    uint64_t settleFrames;
    // The format table's own score counts toward the total only once it
    // reaches this gate; below it, this survivor's format contributes 0.
    // velocity: 4 (desktop-tier only). depth: 0 (every table entry counts,
    // unconditionally - depth's score IS the format score).
    int formatScoreGate;
    SignalWeight signals[8];
    uint8_t signalCount;
    AmbiguityRule ambiguityRule;
    int ambiguityMargin; // ScoreMargin: point margin. EdgeCountRatio: percent (150 == 1.5x).

    // --- rival adoption (consumed only by VelocitySurfaceIdentifier) ----
    uint64_t rivalStallFrames;
    uint64_t rivalFramesBeforeAdopt;
    int rivalMaxAdoptions;
    int rivalMaxReports;

    // --- lifecycle -----------------------------------------------------
    // velocity: true (reopen throws away the shortlist/profiles and
    // rescans). depth: false (reopen keeps them, only resets counters - see
    // the long comment this preserves, originally on ReopenDepthIdentification).
    bool reopenClearsShortlist;
};

// Raw, identifier-agnostic per-resource barrier bookkeeping. Owned by
// BarrierProfileStore, not by any one SurfaceIdentifier (docs/REFACTOR_PLAN.md
// sec 2.5 point 1, finding 1.16): recording a transition costs one map
// lookup and one mutex acquisition, once, no matter how many identifiers
// exist. The only quantities that used to differ per identifier - trigger-
// edge count, reverse-edge count, the state last read in - are indexed by
// TriggerEdge instead of duplicated per identifier: two identifiers sharing
// a TriggerEdge (e.g. velocity and a future normals surface, both
// RenderTargetToShaderResource) correctly share one count, which is the
// point, not a coincidence.
struct BarrierProfile
{
    UINT64 width = 0;
    UINT height = 0;
    int format = 0;
    unsigned flags = 0;
    uint64_t transitions = 0;
    uint64_t framesSeen = 0;
    uint64_t framesWithExactlyTwo = 0;
    uint64_t framesWithEvenCount = 0;
    uint64_t lastFrame = ~0ull;
    uint32_t eventsThisFrame = 0;
    uint64_t alternationViolations = 0;
    uint64_t triggerEdgeCountByEdge[kTriggerEdgeCount]{};
    uint64_t reverseEdgeCountByEdge[kTriggerEdgeCount]{};
    int lastReadableStateByEdge[kTriggerEdgeCount] = {-1, -1};
    uint64_t uavBarriers = 0;
    bool sawUnorderedAccess = false;
    int lastAfter = -1;

    static constexpr int kMaxPairs = 12;
    struct PairCount
    {
        int before = 0;
        int after = 0;
        uint64_t count = 0;
    };
    PairCount pairs[kMaxPairs]{};
    int pairsUsed = 0;
};

// One shared map+mutex for every SurfaceIdentifier, replacing the one-map-
// per-identifier design Phase 6/7 shipped with (docs/REFACTOR_PLAN.md sec
// 2.5 point 1). Owned by SurfaceRegistry; every identifier gets a pointer to
// the same instance via SurfaceIdentifier::SetProfileStore, called once by
// SurfaceRegistry::Configure() before hooks go live - the same wiring
// pattern SetExtentSource already used in Phase 7.
class BarrierProfileStore
{
public:
    // Seeds a profile entry for `resource` if one doesn't exist yet. Safe to
    // call more than once for the same resource - idempotent, and every
    // identifier that considers a resource calls this once, so a resource
    // structurally interesting to two identifiers is still only inserted
    // once.
    void EnsureProfiled(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) noexcept;

    // Records one transition against the shared profile - the hot-path
    // operation finding 1.16 is about. Returns false if `resource` has no
    // profile entry (the kMaxProfiledResources cap was hit before this
    // resource's first desc, or EnsureProfiled was never reached for it);
    // callers must not treat the transition as a trigger edge in that case,
    // matching the pre-Phase-8 per-identifier map's exact behaviour.
    bool RecordTransition(ID3D12Resource* resource, uint64_t frame, int stateBefore, int stateAfter) noexcept;

    void RecordUav(ID3D12Resource* resource) noexcept;

    // Copies out the current profile, if one exists.
    bool Get(ID3D12Resource* resource, BarrierProfile* out) const noexcept;

    // Resets every profile's counters (preserving width/height/format/flags)
    // for the empty-survivors retry path. Shared across every identifier:
    // resetting is safe to trigger from any one identifier's retry because a
    // dependent identifier (depth) cannot be mid-decision while its
    // dependency (velocity) is still empty-retrying - Decide() for a
    // dependent never runs until the dependency's own Decide() has already
    // succeeded (see SurfaceIdentifier::OnFrame's extent-dependency path) -
    // and once an identifier HAS decided, decisionMade_ keeps its own
    // OnFrame from re-entering Decide()/the retry path at all, so a reset
    // triggered by a still-searching sibling can never land mid-decision for
    // an already-decided one either.
    void ResetMeasurement() noexcept;

    // Drops every profile. Called only when an identifier's
    // reopenClearsShortlist is true (velocity) - see SurfaceIdentifier::
    // Reopen(). Safe for the same reason ResetMeasurement is: a reopen
    // cascade reopens the root first, and a dependent's own reopen (which
    // preserves its shortlist) simply repopulates from whatever the shared
    // store has next, exactly as if it were starting fresh.
    void Clear() noexcept;

    // For the one-time survey dump - now a single registry-level survey
    // instead of one per identifier, since the map itself is now single
    // (see SurfaceRegistry::DumpBarrierProfiles).
    template <typename Fn>
    void ForEach(Fn&& fn) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [resource, profile] : profiles_)
        {
            fn(resource, profile);
        }
    }
    size_t Size() const noexcept;

private:
    mutable std::mutex mutex_;
    std::unordered_map<ID3D12Resource*, BarrierProfile> profiles_;
};

// Resources obtained through a documented API contract (today: only
// IDXGISwapChain::GetBuffer, for colour) that no SurfaceIdentifier may ever
// select - docs/REFACTOR_PLAN.md sec 3.3, "known-source exclusion". A closed,
// tiny set (back buffers: 2-3 today) fed by CaptureSystem/capture.cpp once
// per Present, checked on every structural-gate evaluation. Lock-free: sized
// for a handful of entries, updated far less often than it's read.
class KnownSourceSet
{
public:
    static constexpr int kMaxKnownSources = 8;

    void Note(ID3D12Resource* resource) noexcept;
    bool Contains(ID3D12Resource* resource) const noexcept;

private:
    std::atomic<ID3D12Resource*> resources_[kMaxKnownSources]{};
};

class SurfaceIdentifier;

// Cross-identifier invariant: at most one identifier may hold a given
// resource as a candidate (docs/REFACTOR_PLAN.md sec 3.3, "mutual
// exclusion"). Implemented by SurfaceRegistry, which is the only thing that
// can see every identifier's candidate set; each SurfaceIdentifier is handed
// a pointer via SetMutualExclusion and consults it from MarkCandidate.
class MutualExclusionChecker
{
public:
    virtual ~MutualExclusionChecker() = default;

    // `claimant` wants to add `resource` to its own candidate set. Returns
    // true if no OTHER registered identifier already holds `resource` as a
    // candidate.
    virtual bool TryClaim(ID3D12Resource* resource, const SurfaceIdentifier* claimant) noexcept = 0;
};

class SurfaceIdentifier
{
public:
    explicit SurfaceIdentifier(const SurfaceSpec& spec) noexcept;
    virtual ~SurfaceIdentifier() = default;

    SurfaceIdentifier(const SurfaceIdentifier&) = delete;
    SurfaceIdentifier& operator=(const SurfaceIdentifier&) = delete;

    const SurfaceSpec& Spec() const noexcept
    {
        return spec_;
    }

    // Wiring calls, each made exactly once by SurfaceRegistry::Configure()
    // before hooks go live - the same contract SetExtentSource already had
    // in Phase 7.
    void SetProfileStore(BarrierProfileStore* store) noexcept;
    void SetExtentSource(const SurfaceIdentifier* source) noexcept;
    void SetKnownSourceExclusion(const KnownSourceSet* excluded) noexcept;
    void SetMutualExclusion(MutualExclusionChecker* checker) noexcept;

    // Every resource the barrier hook walks past, once per resource, AFTER
    // the caller (SurfaceRegistry) has already seeded the shared profile
    // store via BarrierProfileStore::EnsureProfiled. Records the structural
    // shortlist; does not touch the store itself.
    void ConsiderResource(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) noexcept;

    // Every transition barrier, AFTER the caller has already recorded it
    // into the shared store exactly once via BarrierProfileStore::
    // RecordTransition. `profiled` is that call's return value - whether the
    // store actually has an entry for `resource`. Returns true when
    // `resource` is the selected (or rival-adopted) one and this transition
    // is its trigger edge - the caller should capture.
    bool OnTransition(ID3D12Resource* resource, uint64_t frame, int stateBefore, int stateAfter, bool profiled) noexcept;

    // Note: there is no per-identifier OnUavBarrier anymore. A UAV barrier
    // carries no identifier-specific state (only the shared uavBarriers
    // count, read by CollectSignals via the profile) - the caller
    // (SurfaceRegistry) records it once, directly on the shared store, via
    // BarrierProfileStore::RecordUav.

    // Called once per presented frame: runs the decision when the settling
    // window has elapsed (or, for a surface with an extent dependency, as
    // soon as that dependency resolves - see SetExtentSource).
    void OnFrame(uint64_t frame) noexcept;

    ID3D12Resource* Selected() const noexcept;
    IdentifyState State() const noexcept;
    bool Extent(UINT64* width, UINT* height) const noexcept;

    // Per-identifier candidate set (docs/REFACTOR_PLAN.md sec 2.5, fixes
    // findings 1.11/1.12) - replaces resource_tracking.h's old process-wide
    // g_candidateResources map. Every identifier owns its own now, cleared
    // unconditionally by Reopen(), so "reopening throws away the previous
    // answer" is true by construction rather than by remembering to call
    // ForgetAllCandidates from the right place.
    bool IsCandidate(ID3D12Resource* resource) const noexcept;
    // Re-checks that a candidate still has the desc it was accepted with,
    // dropping (and logging) it if not. Call before doing anything that
    // depends on its layout or its assumed resource state.
    bool ValidateCandidate(ID3D12Resource* resource) noexcept;
    // Adds `resource` as a candidate, subject to mutual exclusion (see
    // SetMutualExclusion) - refused, and logged, if another identifier
    // already holds it.
    void MarkCandidate(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) noexcept;

    // Starts the search again from scratch. `detail` is folded into the log
    // line the same way ReopenIdentification's `reason` string was. Does
    // NOT refill the global identify budget/seen-set - that is now
    // SurfaceRegistry::ReopenCascade's job, called once per cascade rather
    // than once per identifier (docs/REFACTOR_PLAN.md sec 2.5 point 3,
    // finding 1.7).
    void Reopen(ReopenReason reason, const char* detail) noexcept;

    // The swapchain's back-buffer size, when known - used to bound candidate
    // size (spec.maxBackBufferMultiple). Returns true when the size CHANGED
    // after a decision was already made or was in progress; the caller
    // should then reopen (SurfaceRegistry::ReopenCascade).
    bool NoteBackBufferSize(uint64_t width, uint32_t height) noexcept;

    // True if the selected resource hasn't taken its trigger edge in over
    // kQuietFramesBeforeSilent frames while the game keeps presenting.
    bool IsSilent(uint64_t frame, uint64_t* framesSilent) noexcept;

    // Frames since the selected resource last took its trigger edge. Unlike
    // IsSilent this reports the raw number rather than gating on a
    // threshold.
    uint64_t FramesSinceEdge(uint64_t frame) const noexcept;

    // The only two extension points (docs/REFACTOR_PLAN.md sec 2.3).

    // Called when some OTHER resource takes this surface's trigger edge
    // while the selected one is stalled. Return true to have the caller
    // adopt it as an additional candidate (via MarkCandidate) - the base
    // class does not adopt on the subclass's behalf.
    virtual bool OnUnmatchedEdge(ID3D12Resource* /*resource*/, uint64_t /*frame*/) noexcept
    {
        return false;
    }

protected:
    // Called once, when a resource is selected.
    virtual void OnSelected(ID3D12Resource* /*resource*/, const D3D12_RESOURCE_DESC& /*desc*/) noexcept
    {
    }

    // Extra resources this identifier is also actively capturing from,
    // beyond Selected() - currently only VelocitySurfaceIdentifier's
    // rival-adopted alternate (g_identifiedAlt in the pre-extraction code).
    virtual bool IsAlsoTracked(ID3D12Resource* /*resource*/) const noexcept
    {
        return false;
    }

    // Called at the end of Reopen(), for state Reopen() itself doesn't know
    // about - VelocitySurfaceIdentifier clears its adopted alternate here.
    virtual void OnReopen() noexcept
    {
    }

    // Rival-adoption state lives entirely in VelocitySurfaceIdentifier, but
    // the trigger-edge test and liveness bookkeeping it needs to piggyback
    // on (matching resource, matching edge, extent) are the base class's, so
    // subclasses read them through these rather than duplicating the test.
    ID3D12Resource* SelectedRaw() const noexcept
    {
        return identified_.load(std::memory_order_acquire);
    }
    uint64_t IdentifiedWidth() const noexcept
    {
        return identifiedWidth_.load(std::memory_order_relaxed);
    }
    uint32_t IdentifiedHeight() const noexcept
    {
        return identifiedHeight_.load(std::memory_order_relaxed);
    }
    int IdentifiedFormat() const noexcept
    {
        return identifiedFormat_.load(std::memory_order_relaxed);
    }
    bool StructurallyPlausibleForRival(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) const noexcept;

    // Needed by VelocitySurfaceIdentifier's own log lines (ADOPTED, RIVAL).
    std::string DescLine(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) const noexcept;

private:
    struct Survivor
    {
        ID3D12Resource* resource = nullptr;
        D3D12_RESOURCE_DESC desc{};
        BarrierProfile profile;
        uint64_t triggerEdgeCount = 0; // profile.triggerEdgeCountByEdge[spec_.trigger], read a lot; kept alongside
        int score = 0;
        std::string signalsLine; // computed once, at scoring time, for the summary log
    };

    struct Candidate
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        D3D12_RESOURCE_DESC desc{};
    };

    bool StructurallyPlausible(
        ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc, int* formatScore, const char** rejection) const noexcept;
    std::pair<UINT64, UINT> ModalResolution() const noexcept;
    void LogResolutionHistogram() const noexcept;
    int CollectSignals(
        const D3D12_RESOURCE_DESC& desc, const BarrierProfile& profile, uint64_t triggerEdgeCount, int formatScore,
        const std::pair<UINT64, UINT>& modal, std::string* signalsLine) const noexcept;
    void Decide(uint64_t frame) noexcept;
    bool IsTriggerEdge(int stateBefore, int stateAfter) const noexcept;
    bool IsReverseTriggerEdge(int stateBefore, int stateAfter) const noexcept;
    void ForgetAllCandidates() noexcept;

    std::string ProfileLine(const BarrierProfile& profile) const;
    static std::string PairsLine(const BarrierProfile& profile);

    const SurfaceSpec& spec_;
    const SurfaceIdentifier* extentSource_ = nullptr;
    BarrierProfileStore* profileStore_ = nullptr;
    const KnownSourceSet* knownSourceExclusion_ = nullptr;
    MutualExclusionChecker* mutualExclusion_ = nullptr;

    std::mutex shortlistMutex_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> shortlist_;

    mutable std::mutex resolutionMutex_;
    std::map<std::pair<UINT64, UINT>, int> resolutionHistogram_;

    std::atomic<uint64_t> backBufferWidth_{0};
    std::atomic<uint32_t> backBufferHeight_{0};

    std::atomic<ID3D12Resource*> identified_{nullptr};
    std::atomic<uint64_t> identifiedWidth_{0};
    std::atomic<uint32_t> identifiedHeight_{0};
    std::atomic<int> identifiedFormat_{0};

    mutable std::shared_mutex candidateMutex_;
    std::unordered_map<ID3D12Resource*, Candidate> candidates_;

    std::atomic<uint64_t> lastEdgeFrame_{0};
    std::atomic<bool> silenceReported_{false};

    std::atomic<bool> decisionMade_{false};
    std::atomic<uint64_t> decideAtFrame_{0};
    std::atomic<int> attempt_{0};
};

} // namespace mv
