#pragma once

#include <d3d12.h>

#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// The generic half of finding a scene texture without already knowing its
// descriptor - factored out of velocity_identify.cpp per
// docs/REFACTOR_PLAN.md sec 2.3/2.4 (Phase 6), then generalised in Phase 7 to
// also cover depth (sec 2.4: "the proof point the whole design rests on").
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
//   - usesSharedCandidateSet: velocity participates in resource_tracking.h's
//     process-wide candidate set (what the barrier hook actually gates
//     capture on); depth never has - it keeps its own local accepted-desc
//     and validates against that directly. Both are pre-existing, not
//     something this phase changes.
//   - reopenClearsShortlist: velocity's reopen throws away the shortlist and
//     re-scans everything; depth's reopen deliberately keeps its shortlist
//     and only resets measurement counters (see the long comment in the
//     pre-extraction ReopenDepthIdentification about why re-scanning would
//     have made depth losable).
//   - logPrefix: the plan's own Phase 7 verify bullet names the exact line
//     to check - "the `depth: SELECTED` line matches" - so the prefix is
//     preserved as data. Bullet points beyond the SELECTED/AMBIGUOUS verdict
//     lines (the explanatory sentences) are allowed to become shared,
//     slightly more uniform prose; the plan's own Phase 7 bar is visibly
//     lower than Phase 1's "line-identical modulo pointer values", which is
//     read here as permission for that.
//
// SCOPE NOTE: this remains the identification-relevant subset of the plan's
// SurfaceSpec (sec 2.1). The capture-side fields - source/known,
// arbitration, required/enabled - stay on capture.cpp's own (TU-local)
// SurfaceSpec until Phase 8/9 actually needs one struct for both concerns.

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
    RenderTargetToShaderResource, // velocity
    DepthWriteToReadable,         // depth
};

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
    // Documentation/profile data only; the functional wiring is
    // SetExtentSource(), called once by whichever .cpp owns both
    // identifiers. nullptr for velocity, "velocity" for depth.
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
    // velocity: true (participates in resource_tracking.h's process-wide
    // candidate set, which is what the barrier hook gates capture on).
    // depth: false (keeps its own local accepted-desc; see Validate()).
    bool usesSharedCandidateSet;
    // velocity: true (reopen throws away the shortlist/profiles and
    // rescans). depth: false (reopen keeps them, only resets counters - see
    // the long comment this preserves, originally on ReopenDepthIdentification).
    bool reopenClearsShortlist;
};

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

class SurfaceIdentifier
{
public:
    explicit SurfaceIdentifier(const SurfaceSpec& spec) noexcept;
    virtual ~SurfaceIdentifier() = default;

    SurfaceIdentifier(const SurfaceIdentifier&) = delete;
    SurfaceIdentifier& operator=(const SurfaceIdentifier&) = delete;

    // Every resource the barrier hook walks past, once per resource.
    // Records the structural shortlist and seeds the behaviour profile.
    void OnResourceDesc(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) noexcept;

    // Every transition barrier, for the behaviour profile AND liveness
    // tracking of the selected resource. Returns true when `resource` is the
    // selected (or rival-adopted) one and this transition is its trigger
    // edge - the caller should capture.
    bool OnTransition(ID3D12Resource* resource, uint64_t frame, int stateBefore, int stateAfter) noexcept;

    // Every UAV barrier - a scored signal (NeverEntersUnorderedAccess /
    // NeverTakesUavBarrier), not a hard rule. No-op for a surface whose
    // spec has neither signal (depth).
    void OnUavBarrier(ID3D12Resource* resource) noexcept;

    // Called once per presented frame: runs the decision when the settling
    // window has elapsed (or, for a surface with an extent dependency, as
    // soon as that dependency resolves - see SetExtentSource).
    void OnFrame(uint64_t frame) noexcept;

    ID3D12Resource* Selected() const noexcept;
    IdentifyState State() const noexcept;
    bool Extent(UINT64* width, UINT* height) const noexcept;

    // Ties this identifier's structural gate to another identifier's live
    // extent (spec.extentFrom's functional half - see the class comment).
    // Must be called once, before hooks go live, if spec.extentFrom is set.
    // `source` must outlive this identifier.
    void SetExtentSource(const SurfaceIdentifier* source) noexcept;

    // For a surface with usesSharedCandidateSet == false (depth): re-checks
    // that `resource` is still the selected one AND still has the descriptor
    // it was accepted with. Mirrors resource_tracking.h's ValidateCandidate,
    // which surfaces with usesSharedCandidateSet == true (velocity) use
    // instead - this exists because depth was never wired into that shared
    // set (see the class comment) and Phase 7 preserves that, not because
    // the two mechanisms differ in what they check.
    bool Validate(ID3D12Resource* resource) const noexcept;

    // Starts the search again from scratch. `detail` is folded into the log
    // line the same way ReopenIdentification's `reason` string was.
    void Reopen(ReopenReason reason, const char* detail) noexcept;

    // The swapchain's back-buffer size, when known - used to bound candidate
    // size (spec.maxBackBufferMultiple). Returns true when the size CHANGED
    // after a decision was already made or was in progress; the caller
    // should then call Reopen(BackBufferResized, ...).
    bool NoteBackBufferSize(uint64_t width, uint32_t height) noexcept;

    // True if the selected resource hasn't taken its trigger edge in over
    // kQuietFramesBeforeSilent frames while the game keeps presenting.
    bool IsSilent(uint64_t frame, uint64_t* framesSilent) noexcept;

    // Frames since the selected resource last took its trigger edge. Unlike
    // IsSilent this reports the raw number rather than gating on a
    // threshold.
    uint64_t FramesSinceEdge(uint64_t frame) const noexcept;

    // One-time survey dump of every profiled resource's barrier behaviour,
    // to mv_barrier_profile.log. No-op after the first call.
    void DumpBarrierProfiles() noexcept;

    // The only two extension points (docs/REFACTOR_PLAN.md sec 2.3). Public,
    // not protected: today's caller is the free-function wrapper in
    // velocity_identify.cpp (there is no SurfaceRegistry yet); Phase 8's
    // SurfaceRegistry will call it the same way, through a base pointer -
    // external in both cases, so protected access would gain nothing.

    // Called when some OTHER resource takes this surface's trigger edge
    // while the selected one is stalled. Return true to have the caller
    // adopt it as an additional candidate (via MarkAsCandidate) - the base
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
    // OnTransition's liveness/resume tracking must not distinguish "the
    // selected resource took its edge" from "the adopted rival did" - the
    // pre-extraction NoteTransition tested `resource == g_identified ||
    // resource == g_identifiedAlt` in one condition, and this is that same
    // test, generalised just enough to stay one override short of moving all
    // of OnTransition into the subclass.
    virtual bool IsAlsoTracked(ID3D12Resource* /*resource*/) const noexcept
    {
        return false;
    }

    // Called at the end of Reopen(), for state Reopen() itself doesn't know
    // about - VelocitySurfaceIdentifier clears its adopted alternate here.
    // NOT called for the rival run-length/report/adoption COUNTERS: those
    // are deliberately session-wide budgets that survive a reopen in the
    // pre-extraction code (see the adoption-limit comment in
    // velocity_identify.cpp), so this hook only ever clears the "who is
    // currently adopted" state, not the "how much adoption has happened"
    // counters.
    virtual void OnReopen() noexcept
    {
    }

    const SurfaceSpec& Spec() const noexcept
    {
        return spec_;
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
    bool StructurallyPlausibleForRival(const D3D12_RESOURCE_DESC& desc) const noexcept;

    // Needed by VelocitySurfaceIdentifier's own log lines (ADOPTED, RIVAL).
    std::string DescLine(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) const noexcept;

private:
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
        uint64_t triggerEdgeCount = 0;  // spec.trigger's edge: RT->SRV for velocity, DEPTH_WRITE->readable for depth
        uint64_t reverseEdgeCount = 0;  // velocity only (SRV->RT); always 0 for a trigger with no reverse concept
        uint64_t otherPairs = 0;
        uint64_t uavBarriers = 0;
        int lastAfter = -1;
        int shaderResourceState = 0;
        bool sawUnorderedAccess = false;

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

    struct Survivor
    {
        ID3D12Resource* resource = nullptr;
        D3D12_RESOURCE_DESC desc{};
        BarrierProfile profile;
        int score = 0;
        std::string signalsLine; // computed once, at scoring time, for the summary log
    };

    bool StructurallyPlausible(const D3D12_RESOURCE_DESC& desc, int* formatScore, const char** rejection) const noexcept;
    std::pair<UINT64, UINT> ModalResolution() const noexcept;
    void LogResolutionHistogram() const noexcept;
    int CollectSignals(
        const D3D12_RESOURCE_DESC& desc, const BarrierProfile& profile, int formatScore, const std::pair<UINT64, UINT>& modal,
        std::string* signalsLine) const noexcept;
    void Decide(uint64_t frame) noexcept;
    bool IsTriggerEdge(int stateBefore, int stateAfter) const noexcept;
    bool IsReverseTriggerEdge(int stateBefore, int stateAfter) const noexcept;

    static std::string ProfileLine(const BarrierProfile& profile);
    static std::string PairsLine(const BarrierProfile& profile);

    const SurfaceSpec& spec_;
    const SurfaceIdentifier* extentSource_ = nullptr;

    mutable std::mutex profileMutex_;
    std::unordered_map<ID3D12Resource*, BarrierProfile> profiles_;
    std::atomic<bool> profileDumped_{false};

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

    // Only meaningful (and only written) for a surface with
    // usesSharedCandidateSet == false; Validate() reads it. Guarded
    // separately from profileMutex_ since it's touched from a different call
    // pattern (once at selection, read on every barrier for that surface).
    mutable std::mutex acceptedDescMutex_;
    D3D12_RESOURCE_DESC acceptedDesc_{};

    std::atomic<uint64_t> lastEdgeFrame_{0};
    std::atomic<bool> silenceReported_{false};

    std::atomic<bool> decisionMade_{false};
    std::atomic<uint64_t> decideAtFrame_{0};
    std::atomic<int> attempt_{0};
};

} // namespace mv
