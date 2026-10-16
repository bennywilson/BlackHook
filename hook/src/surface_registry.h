#pragma once

#include "surface_identify.h"

#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

// Owns every `SurfaceIdentifier` (one per enabled surface in the active
// profile) and what only something that sees all of them can do:
//   - one shared `BarrierProfileStore`, so a transition costs one lookup
//     however many surfaces there are;
//   - the reopen cascade, from `spec.extentFrom`: reopening a surface reopens
//     everything sized from it;
//   - the identify seen-set and `GetDesc()` budget, refilled once per cascade;
//   - mutual exclusion (no resource claimed by two surfaces) and known-source
//     exclusion (no search may select a swapchain back buffer).

namespace mv
{

struct SurfaceTrigger
{
    SurfaceIdentifier* identifier = nullptr;
    ID3D12Resource* resource = nullptr;
    int stateAfter = 0;
};

class SurfaceRegistry : public MutualExclusionChecker
{
public:
    SurfaceRegistry() noexcept;

    // Once per surface, before `Configure()`. Takes ownership.
    void Register(std::unique_ptr<SurfaceIdentifier> identifier) noexcept;

    // Called for every identifier a cascade reopens, root or dependent, whatever
    // caused it. capture.cpp uses it to end a burst that required the surface.
    using ReopenObserver = void (*)(const char* surfaceName);
    void SetReopenObserver(ReopenObserver observer) noexcept;

    // Once, after every `Register()`, before hooks go live. Wires the profile
    // store, both exclusions, and the extent dependencies.
    void Configure() noexcept;

    // nullptr if no surface of that name is registered (every surface, if the
    // profile failed to load).
    SurfaceIdentifier* Find(const char* name) noexcept;

    // The named surface's selected resource, or nullptr while it is
    // searching, ambiguous, or not registered at all.
    ID3D12Resource* Selected(const char* name) noexcept;

    // ---- barrier-hook entry points --------------------------------------

    // Every resource the barrier hook meets. The first time a pointer is seen,
    // and while the identify budget lasts, its desc is read once, its profile
    // seeded, and every identifier's structural gate run.
    //
    // The budget bounds how many distinct resources `GetDesc()` is called on.
    // On a resource met in a barrier that is believed safe but not proven (a
    // `GetDesc()` at view creation crashed a game; DEBUGGING.md). UE's
    // transient allocator creates resources all session, so the budget runs
    // out; that is logged, and a reopen refills it. The seen set holds bare
    // pointers, so a recycled address reads as already seen: a resource can be
    // missed, but candidates are ref-held, so the wrong one is never used.
    void ConsiderResource(ID3D12Resource* resource) noexcept;

    void OnUavBarrier(ID3D12Resource* resource) noexcept;

    // Every transition barrier: profiles it, tracks liveness, validates a
    // candidate taking its trigger edge (reopening on a changed descriptor) and
    // offers other trigger edges for rival adoption. Writes the surfaces to
    // capture into `out` and returns how many.
    int OnBarrierTransition(
        ID3D12Resource* resource, uint64_t frame, int stateBefore, int stateAfter, SurfaceTrigger* out,
        int outCapacity) noexcept;

    // Called once per presented frame.
    void OnFrame(uint64_t frame) noexcept;

    // One whole `ResourceBarrier` call, after `OnBarrierTransition` saw each
    // entry. Feeds `BarrierProfile::triggerEdgesSharingCallWith` (the input of
    // `SignalId::SharesCallWithEdge`) when a spec uses it. With `MV_DIAG`, also
    // tallies what changed state in the same call as each optional surface's
    // shortlisted RT -> SRV edges, logged at frames 600 and 2000 (how GBufferA
    // and GBufferB were told apart on Skyrunner). With `MV_SNAPSHOT`, passes
    // those edges to the `CandidateEdgeObserver`. Otherwise one branch.
    bool WantsBarrierBatches() const noexcept
    {
        return sharedCallSignals_ || (!optionals_.empty() && (diag_ || candidateEdgeObserver_ != nullptr));
    }
    void OnBarrierBatch(ID3D12GraphicsCommandList* cmdList, const D3D12_RESOURCE_BARRIER* barriers, UINT count) noexcept;

    // `MV_SNAPSHOT`: each RT -> SRV edge of a resource an optional surface has
    // shortlisted at the scene extent, once velocity and depth are selected, so
    // one frame of each candidate can be copied out and compared by its pixels.
    // Decides nothing.
    using CandidateEdgeObserver = void (*)(
        const char* surfaceName, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource, int stateAfter);
    void SetCandidateEdgeObserver(CandidateEdgeObserver observer) noexcept
    {
        candidateEdgeObserver_ = observer;
    }

    // Forwards to every surface not sized from another; reopens (cascading)
    // any that report the size changed after a decision.
    void NoteBackBufferSize(uint64_t width, uint32_t height) noexcept;

    // A swapchain back buffer: never selectable, purged from any shortlist.
    void NoteKnownSourceResource(ID3D12Resource* resource) noexcept;

    // Reopens `rootName` and every identifier sized from it, directly or not,
    // refilling the identify seen-set and budget once for the whole cascade.
    void ReopenCascade(const char* rootName, ReopenReason reason, const char* detail) noexcept;

    // MutualExclusionChecker
    bool TryClaim(ID3D12Resource* resource, const SurfaceIdentifier* claimant) noexcept override;
    const char* HeldByOther(ID3D12Resource* resource, const SurfaceIdentifier* claimant) noexcept override;

private:
    struct Entry
    {
        SurfaceIdentifier* identifier = nullptr;
        SurfaceIdentifier* extentSource = nullptr;  // resolved from spec.extentFrom at Configure()
        std::vector<SurfaceIdentifier*> dependents; // identifiers whose extentSource == this one
    };

    // For `mv_candidates.log` (`MV_DIAG`): does any surface hold `resource`?
public:
    bool IsAnyCandidate(ID3D12Resource* resource) const noexcept;

private:
    Entry* FindEntry(const char* name) noexcept;
    Entry* FindEntry(const SurfaceIdentifier* identifier) noexcept;
    void CollectCascade(Entry* root, std::vector<Entry*>* out) noexcept;
    void DumpBarrierProfiles() noexcept;

    void ForgetAllResourcesSeen() noexcept; // refills the budget too

    std::mutex seenMutex_;
    std::unordered_set<ID3D12Resource*> seen_;
    std::atomic<int64_t> identifyBudget_;
    std::atomic<bool> budgetExhaustionReported_{false};

    std::vector<std::unique_ptr<SurfaceIdentifier>> owned_;
    std::vector<Entry> entries_;
    BarrierProfileStore profileStore_;
    KnownSourceSet knownSources_;
    std::atomic<bool> profileDumped_{false};
    ReopenObserver reopenObserver_ = nullptr;

    // `OnBarrierBatch`'s `MV_DIAG` tallies.
    static constexpr size_t kMaxBatchSurfaces = 8; // entries_ capacity
    struct BatchStats
    {
        uint64_t edges = 0;                            // RT -> SRV edges counted (velocity and depth both selected)
        uint64_t withSurface[kMaxBatchSurfaces] = {}; // ...in a call where entries_[i] had a tracked trigger edge
        uint64_t otherRtToSrv = 0;                     // sum, over those edges, of OTHER RT -> SRV entries in the call
        uint64_t maxOtherRtToSrv = 0;
    };
    void DumpBatchStats(uint64_t frame) noexcept;
    SurfaceIdentifier* velocity_ = nullptr; // resolved at Configure(), for OnBarrierBatch
    SurfaceIdentifier* depth_ = nullptr;
    std::vector<SurfaceIdentifier*> optionals_; // every registered surface but velocity and depth
    bool sharedCallSignals_ = false; // any registered spec uses SignalId::SharesCallWithEdge
    bool diag_ = false;              // MV_DIAG: keep and log the batch tallies
    CandidateEdgeObserver candidateEdgeObserver_ = nullptr;
    std::mutex batchMutex_;
    std::map<std::pair<SurfaceIdentifier*, ID3D12Resource*>, BatchStats> batchStats_;
};

// The one registry, set up by `InstallD3D12Hooks` before the hooks go live.
SurfaceRegistry& GetSurfaceRegistry() noexcept;

} // namespace mv
