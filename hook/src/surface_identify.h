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

// Finds a scene texture without knowing its descriptor: a structural gate
// (format table, flags, extent), then behaviour over a settling window
// (barrier patterns, scored as evidence; every hard rule measured on one title
// was false on another). What differs between surfaces is data in a
// `SurfaceSpec`, loaded from `profiles/ue5.json`. `SurfaceRegistry` owns the
// identifiers and feeds them from one barrier walk.

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
    // F6 restarts every search from an empty shortlist (see `Reopen`).
    Requested,
};

// The barrier edge that finishes a surface: the one moment the transient
// resource is complete and safe to copy.
enum class TriggerEdge : uint8_t
{
    RenderTargetToShaderResource, // velocity, GBuffers
    DepthWriteToReadable,         // depth
};
constexpr int kTriggerEdgeCount = 2;

// True for any non-empty subset of the shader-resource state bits. UE5's RDG
// maps `SRVMask` to `ALL_SHADER_RESOURCE` (0x40|0x80) and pixel-shader reads
// (`SRVGraphics`) to `PIXEL_SHADER_RESOURCE` (0x80).
inline bool IsShaderResourceState(int state)
{
    return state != 0 && (state & D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE) == state;
}

// True for the states depth is read in: `DEPTH_READ`, or any subset of the
// shader-resource bits (UE5 reads depth both ways).
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

// The two tests above, per `TriggerEdge`.
bool IsTriggerEdgeFor(TriggerEdge trigger, int stateBefore, int stateAfter) noexcept;
bool IsReverseTriggerEdgeFor(TriggerEdge trigger, int stateBefore, int stateAfter) noexcept;

struct FormatCandidate
{
    DXGI_FORMAT format;
    int score; // desktop D3D12 path outranks GLES-only/legacy fallbacks
    const char* name;
};

// One weighted piece of evidence on top of the format-score baseline (see
// `SurfaceSpec::formatScoreGate`).
enum class SignalId : uint8_t
{
    AtModalResolution,
    ExactRequiredFlags,
    ProducedOnceExactly, // one produce->consume edge per frame, +-10%
    NeverTakesUavBarrier,
    NeverEntersUnorderedAccess,
    StrictTwoEventsPerFrame,
    NoContinuityViolations,
    // At least `minPercent` of this resource's trigger edges arrive in the same
    // `ResourceBarrier` call as an `edge`-type transition on another resource.
    // On Skyrunner, GBufferA and GBufferB become readable together with scene
    // depth (the passes that read them read depth too); same-format
    // post-process targets never do.
    SharesCallWithEdge,
};

struct SignalWeight
{
    SignalId id;
    int weight;
    // SharesCallWithEdge only.
    TriggerEdge edge = TriggerEdge::RenderTargetToShaderResource;
    int minPercent = 0;
};

enum class AmbiguityRule : uint8_t
{
    // Top two survivors must differ by at least `ambiguityMargin` points.
    ScoreMargin,
    // Separated if the scores differ, or if the winner's trigger-edge count is
    // at least `ambiguityMargin` percent of the runner-up's (depth: 150; scene
    // depth takes several edges a frame, CustomDepth at most one).
    EdgeCountRatio,
};

// Data only. Each field maps to one field of a profile in `profiles/ue5.json`,
// which documents them.
struct SurfaceSpec
{
    const char* name;
    const char* logPrefix; // "identify" for velocity, else the surface name

    // --- structural gate ---------------------------------------------
    FormatCandidate formats[8];
    uint8_t formatCount;
    UINT requiredFlags;
    UINT forbiddenFlags;           // 0 == none
    UINT64 minWidth;               // 0 == no minimum
    UINT minHeight;                // 0 == no minimum
    uint8_t maxBackBufferMultiple; // 0 == unbounded

    // The surface whose live extent this one must match; `SurfaceRegistry`
    // wires it and reopens this one with it. nullptr for velocity.
    const char* extentFrom;

    // --- behaviour ------------------------------------------------------
    TriggerEdge trigger;
    uint64_t minFramesObserved;
    uint64_t settleFrames;
    // The format table's score counts toward the total only at or above this.
    // velocity: 4 (desktop formats only); others: 0 (always counts).
    int formatScoreGate;
    SignalWeight signals[8];
    uint8_t signalCount;
    AmbiguityRule ambiguityRule;
    int ambiguityMargin; // ScoreMargin: point margin. EdgeCountRatio: percent (150 == 1.5x).

    // --- rival adoption (OnUnmatchedEdge; all zero == never adopts) ------
    uint64_t rivalStallFrames;
    uint64_t rivalFramesBeforeAdopt;
    int rivalMaxAdoptions;
    int rivalMaxReports;
    bool rivalSameDescriptor; // a rival must have the selected one's exact format and flags

    // --- lifecycle -----------------------------------------------------
    // true: a reopen discards the shortlist and profiles and rescans.
    // false: a reopen keeps them and resets the counters (see `Reopen()`).
    bool reopenClearsShortlist;

    // false: not registered. Nothing is identified or captured, and the
    // manifest says "not_enabled". `MV_SURFACES` turns optional surfaces on.
    bool enabled;
};

// Per-resource barrier counts, shared by every identifier: a transition costs
// one lookup and one lock however many surfaces exist. The counts that differ
// between surfaces are indexed by `TriggerEdge`.
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
    // [own][other]: this resource's `own`-type trigger edges that arrived in
    // the same `ResourceBarrier` call as an `other`-type edge on a different
    // resource. Fed by `SurfaceRegistry::OnBarrierBatch` only while some spec
    // uses `SignalId::SharesCallWithEdge`.
    uint64_t triggerEdgesSharingCallWith[kTriggerEdgeCount][kTriggerEdgeCount]{};
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

// The one map of `BarrierProfile`s, owned by `SurfaceRegistry` and shared with
// every identifier.
class BarrierProfileStore
{
public:
    // Seeds a profile for `resource` if it has none. Idempotent.
    void EnsureProfiled(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) noexcept;

    // Records one transition. False if `resource` has no profile (never
    // seeded, or `kMaxProfiledResources` was reached).
    bool RecordTransition(ID3D12Resource* resource, uint64_t frame, int stateBefore, int stateAfter) noexcept;

    void RecordUav(ID3D12Resource* resource) noexcept;

    // One `own`-type trigger edge of `resource` shared its `ResourceBarrier`
    // call with an `other`-type edge on a different resource.
    void RecordSharedCall(ID3D12Resource* resource, TriggerEdge own, TriggerEdge other) noexcept;

    // Copies out the profile, if there is one.
    bool Get(ID3D12Resource* resource, BarrierProfile* out) const noexcept;

    // Resets every profile's counters, keeping the descs, for the
    // empty-survivors retry. Safe although shared: a dependent decides only
    // after its source has, and a decided identifier never re-enters
    // `Decide()`, so no reset lands mid-decision.
    void ResetMeasurement() noexcept;

    // Drops every profile (a reopen with `reopenClearsShortlist`). A
    // dependent's own reopen keeps its shortlist and re-measures.
    void Clear() noexcept;

    // For the one-time survey dump (`SurfaceRegistry::DumpBarrierProfiles`).
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

// Resources a documented API names, so no search may select them: the
// swapchain's back buffers, which are the colour surface. Fed every Present,
// checked on every structural-gate evaluation. Lock-free: a handful of entries,
// written far less often than read.
class KnownSourceSet
{
public:
    static constexpr int kMaxKnownSources = 8;

    // True if `resource` was new; the caller then purges it from any shortlist
    // that took it first. Evicts round-robin when full (see the definition).
    bool Note(ID3D12Resource* resource) noexcept;
    bool Contains(ID3D12Resource* resource) const noexcept;

private:
    std::atomic<ID3D12Resource*> resources_[kMaxKnownSources]{};
    std::atomic<unsigned> nextEviction_{0};
};

class SurfaceIdentifier;

// At most one identifier may hold a given resource as a candidate.
// Implemented by `SurfaceRegistry`, the only thing that sees every identifier.
class MutualExclusionChecker
{
public:
    virtual ~MutualExclusionChecker() = default;

    // True if no other identifier holds `resource` as a candidate.
    virtual bool TryClaim(ID3D12Resource* resource, const SurfaceIdentifier* claimant) noexcept = 0;

    // The name of another identifier holding `resource` as a candidate, or
    // nullptr. `Decide()` drops such a resource before ranking; otherwise it
    // could win and have its claim refused forever, or tie and make a
    // separable choice AMBIGUOUS. The case that needs it: velocity's own buffer
    // passes the normals gates (both trigger on RT -> shader-resource).
    virtual const char* HeldByOther(ID3D12Resource* resource, const SurfaceIdentifier* claimant) noexcept = 0;
};

class SurfaceIdentifier
{
public:
    explicit SurfaceIdentifier(const SurfaceSpec& spec) noexcept;

    SurfaceIdentifier(const SurfaceIdentifier&) = delete;
    SurfaceIdentifier& operator=(const SurfaceIdentifier&) = delete;

    const SurfaceSpec& Spec() const noexcept
    {
        return spec_;
    }

    // Wiring, done once by `SurfaceRegistry::Configure()` before hooks go live.
    void SetProfileStore(BarrierProfileStore* store) noexcept;
    void SetExtentSource(const SurfaceIdentifier* source) noexcept;
    void SetKnownSourceExclusion(const KnownSourceSet* excluded) noexcept;
    void SetMutualExclusion(MutualExclusionChecker* checker) noexcept;

    // Each resource the barrier hook meets, once, after the registry has
    // seeded its profile. Records the structural shortlist.
    void ConsiderResource(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) noexcept;

    // Every transition barrier: liveness of the selected and rival-adopted
    // resources. `isTrigger`: this surface's trigger edge. `isCandidate`: the
    // caller's `IsCandidate(resource)`.
    void OnTransition(ID3D12Resource* resource, uint64_t frame, bool isTrigger, bool isCandidate) noexcept;

    // Once per presented frame: decides when the settling window has elapsed,
    // or, for a dependent surface, as soon as its source has an extent.
    void OnFrame(uint64_t frame) noexcept;

    ID3D12Resource* Selected() const noexcept;
    IdentifyState State() const noexcept;
    bool Extent(UINT64* width, UINT* height) const noexcept;

    // The resources this surface captures from: the selected one and any
    // rival-adopted ones. Held by reference; cleared by `Reopen()`.
    bool IsCandidate(ID3D12Resource* resource) const noexcept;
    // True if `resource` passed the structural gate and is being watched. Used
    // by the `MV_DIAG` and `MV_SNAPSHOT` paths only.
    bool IsShortlisted(ID3D12Resource* resource) noexcept;
    // Re-checks that a candidate still has the desc it was accepted with, and
    // drops it (logged) if not. Call before relying on its layout or state.
    bool ValidateCandidate(ID3D12Resource* resource) noexcept;
    // Adds `resource` as a candidate unless another identifier holds it
    // (refused and logged).
    void MarkCandidate(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) noexcept;

    // `resource` is a swapchain back buffer. Drops it, and the reference to
    // it, from the shortlist and candidates. True if it was the selected
    // resource, which the caller must treat as a wrong identification and
    // reopen.
    bool ForgetKnownSource(ID3D12Resource* resource) noexcept;

    // Starts the search again; `detail` goes into the log line. The registry
    // refills the shared seen-set and budget (`ReopenCascade`).
    void Reopen(ReopenReason reason, const char* detail) noexcept;

    // The back-buffer size, which bounds candidate size
    // (`spec.maxBackBufferMultiple`). True when it changed after a decision was
    // made or started; the caller should then reopen.
    bool NoteBackBufferSize(uint64_t width, uint32_t height) noexcept;

    // True if the selected resource has not taken its trigger edge for
    // `kQuietFramesBeforeSilent` frames while the game keeps presenting.
    bool IsSilent(uint64_t frame, uint64_t* framesSilent) noexcept;

    // Frames since the selected resource last took its trigger edge.
    uint64_t FramesSinceEdge(uint64_t frame) const noexcept;

    // Another resource took this surface's trigger edge. True to have the
    // caller adopt it as an extra candidate: UE keeps several copies of each
    // scene texture and switches between them, so a surface tied to one would
    // capture only while that copy is in use. Off unless the spec has a
    // "rival" block. A rival must carry the `shares_call_with_edge` evidence
    // the selection was made on (`RivalHasSelectedEvidence`), or normals would
    // adopt a same-format post-process target that takes its edge every
    // frame.
    bool OnUnmatchedEdge(ID3D12Resource* resource, uint64_t frame) noexcept;

private:
    ID3D12Resource* SelectedRaw() const noexcept
    {
        return identified_.load(std::memory_order_acquire);
    }

    bool StructurallyPlausibleForRival(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) const noexcept;
    std::string DescLine(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) const noexcept;

    bool SharesCallSignalPresent(const BarrierProfile& p, const SignalWeight& sw) const noexcept;
    // -1: too few edges to judge yet. 0: fails. 1: carries every
    // `shares_call_with_edge` signal the selected resource carried.
    int RivalHasSelectedEvidence(ID3D12Resource* resource) const noexcept;
    struct Survivor
    {
        ID3D12Resource* resource = nullptr;
        D3D12_RESOURCE_DESC desc{};
        BarrierProfile profile;
        uint64_t triggerEdgeCount = 0; // profile.triggerEdgeCountByEdge[spec_.trigger]
        int score = 0;
        std::string signalsLine; // for the summary log
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
    std::atomic<unsigned> identifiedFlags_{0};

    mutable std::shared_mutex candidateMutex_;
    std::unordered_map<ID3D12Resource*, Candidate> candidates_;

    std::atomic<uint64_t> lastEdgeFrame_{0};
    std::atomic<bool> silenceReported_{false};

    std::atomic<bool> decisionMade_{false};
    std::atomic<uint64_t> decideAtFrame_{0};
    std::atomic<int> attempt_{0};

    // --- rival adoption (see OnUnmatchedEdge) ------------------------------
    // The latest adopted rival. Every adoption seen live had the selected
    // resource's exact width and format: a reallocation to a new address.
    std::atomic<ID3D12Resource*> identifiedAlt_{nullptr};
    std::atomic<int> rivalReports_{0};
    std::atomic<uint64_t> rivalRunLength_{0};
    std::atomic<uint64_t> lastRivalFrame_{0};
    ID3D12Resource* lastRivalResource_ = nullptr;
    std::atomic<int> adoptions_{0};
    std::atomic<bool> adoptLimitReported_{false};
    std::atomic<bool> evidenceRejectReported_{false};
    // Bit i: `spec_.signals[i]` (a `shares_call_with_edge` signal) held for the
    // selected resource. Written at selection, read from the barrier hook.
    std::atomic<uint32_t> selectedEvidenceMask_{0};
};

} // namespace mv
