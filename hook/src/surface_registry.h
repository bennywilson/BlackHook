#pragma once

#include "surface_identify.h"

#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

// SurfaceRegistry - docs/REFACTOR_PLAN.md sec 2.5, Phase 8.
//
// Owns the identifiers themselves (built by InstallD3D12Hooks from the active
// profile's specs - a plain SurfaceIdentifier for depth, velocity_identify.h's
// subclass for velocity's rival adoption), and everything that used to be
// hand-wired across d3d12_hook.cpp and resource_tracking.h because no single
// file had visibility into every registered identifier at once:
//
//   - ONE shared BarrierProfileStore, so N identifiers cost one map lookup
//     per transition, not N (finding 1.16).
//   - The reopen cascade, derived from each identifier's spec.extentFrom
//     instead of hand-wired call sites (finding 1.9).
//   - The global identify seen-set/budget refill, called once per cascade
//     instead of once per identifier that happens to clear its own shortlist
//     (finding 1.7).
//   - Per-identifier candidate sets (surface_identify.h), plus the one
//     invariant only a registry can enforce: mutual exclusion, no resource
//     claimed by two identifiers (docs/REFACTOR_PLAN.md sec 3.3).
//   - Known-source exclusion: resources obtained via a documented API
//     contract (colour's swapchain buffers) are never eligible for any
//     identifier's search (sec 3.3).
//
// d3d12_hook.cpp calls this registry's entry points instead of the
// per-surface free functions it used to call directly, and capture.cpp/
// dllmain.cpp reach individual surfaces by name (Find/Selected) rather than
// through per-surface wrapper headers.

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

    // Called once per surface, from InstallD3D12Hooks, before Configure().
    // Takes ownership; the identifier lives for the process. Order matters
    // only in that a surface naming another via spec.extentFrom must be
    // registered after the surface it names - true for depth-after-velocity
    // today, and worth keeping true rather than making Configure()
    // topologically sort a two-node graph.
    void Register(std::unique_ptr<SurfaceIdentifier> identifier) noexcept;

    // Called at most once, any time before the first Reopen (typically from
    // InstallD3D12Hooks, alongside Register/Configure). ReopenCascade calls
    // this for every identifier it reopens, root or dependent - by
    // construction, not by every call site that can trigger a reopen
    // remembering to notify capture.cpp itself (docs/REFACTOR_PLAN.md
    // finding 1.14: the whole point is that this must hold regardless of
    // WHICH path triggered the reopen - a barrier-hook descriptor mismatch,
    // a back-buffer resize, or anything else, present or future). The
    // registry has no notion of "burst"; it just reports what reopened.
    using ReopenObserver = void (*)(const char* surfaceName);
    void SetReopenObserver(ReopenObserver observer) noexcept;

    // Called once, after every Register() call, before hooks go live. Wires
    // the shared profile store, known-source exclusion, mutual exclusion,
    // and the extentFrom dependency graph.
    void Configure() noexcept;

    // nullptr if no surface of that name is registered - which is every
    // surface, if the profile failed to load (surface_profile.h).
    SurfaceIdentifier* Find(const char* name) noexcept;

    // The named surface's selected resource, or nullptr while it is
    // searching, ambiguous, or not registered at all.
    ID3D12Resource* Selected(const char* name) noexcept;

    // ---- barrier-hook entry points --------------------------------------

    // Every resource the barrier hook walks past, once per resource. Seeds
    // the shared profile store once, then every registered identifier's own
    // structural gate - replaces d3d12_hook.cpp's TryIdentifyResource
    // calling NoteResourceDesc/NoteResourceDescForDepth by hand.
    void ConsiderResource(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) noexcept;

    void OnUavBarrier(ID3D12Resource* resource) noexcept;

    // Every transition barrier. Records it into the shared store once, fans
    // out to every identifier for liveness tracking, and reproduces what the
    // old hand-wired dispatch did at the call site: candidate validation
    // (capturing on success, reopening on failure) and rival-edge adoption.
    // Writes 0..n capturable triggers into `out` (capacity `outCapacity`)
    // and returns the count. A reopen triggered here reaches the
    // ReopenObserver the same as any other (see SetReopenObserver) - there
    // is no separate out-param for it.
    int OnBarrierTransition(
        ID3D12Resource* resource, uint64_t frame, int stateBefore, int stateAfter, SurfaceTrigger* out,
        int outCapacity) noexcept;

    // Called once per presented frame.
    void OnFrame(uint64_t frame) noexcept;

    // Forwards to every self-contained identifier (spec.extentFrom ==
    // nullptr) - a dependent inherits reopening through the cascade instead
    // of tracking the back buffer itself. Reopens (reaching the
    // ReopenObserver, see above) if any of them reports the size changed
    // after a decision was made.
    void NoteBackBufferSize(uint64_t width, uint32_t height) noexcept;

    // Resources obtained via a documented API contract (colour's swapchain
    // buffers) - fed here so no identifier's search can ever select one
    // (docs/REFACTOR_PLAN.md sec 3.3).
    void NoteKnownSourceResource(ID3D12Resource* resource) noexcept;

    // Reopens `rootName` and every identifier transitively depending on its
    // extent (spec.extentFrom), refilling the global identify seen-set/
    // budget exactly once for the whole cascade.
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

    // Diagnostic convenience for d3d12_hook.cpp's "candidates" log channel:
    // true if ANY registered identifier holds `resource` as a candidate.
public:
    bool IsAnyCandidate(ID3D12Resource* resource) const noexcept;

private:
    Entry* FindEntry(const char* name) noexcept;
    Entry* FindEntry(const SurfaceIdentifier* identifier) noexcept;
    void CollectCascade(Entry* root, std::vector<Entry*>* out) noexcept;
    void DumpBarrierProfiles() noexcept;

    std::vector<std::unique_ptr<SurfaceIdentifier>> owned_;
    std::vector<Entry> entries_;
    BarrierProfileStore profileStore_;
    KnownSourceSet knownSources_;
    std::atomic<bool> profileDumped_{false};
    ReopenObserver reopenObserver_ = nullptr;
};

// The one process-wide registry. Register()/Configure() are called once from
// InstallD3D12Hooks, before hooks go live. Exposed as a singleton, rather
// than passed around explicitly, because both d3d12_hook.cpp (the barrier
// hook) and capture.cpp (known-source feeding, ending a burst on a required
// surface's reopen) need to reach the same instance, and neither owns the
// other.
SurfaceRegistry& GetSurfaceRegistry() noexcept;

} // namespace mv
