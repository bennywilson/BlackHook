#include "surface_identify.h"

#include "logging.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace mv
{

namespace
{

// D3D12_RESOURCE_STATES is a bit field; these are the ones a render target
// realistically passes through. Names transcribed from d3d12.h.
std::string StateName(int state)
{
    if (state == 0)
    {
        return "COMMON/PRESENT";
    }
    struct Bit
    {
        int value;
        const char* name;
    };
    static constexpr Bit kBits[] = {
        {D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, "VB_CB"},
        {D3D12_RESOURCE_STATE_INDEX_BUFFER, "IB"},
        {D3D12_RESOURCE_STATE_RENDER_TARGET, "RT"},
        {D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "UAV"},
        {D3D12_RESOURCE_STATE_DEPTH_WRITE, "DEPTH_WRITE"},
        {D3D12_RESOURCE_STATE_DEPTH_READ, "DEPTH_READ"},
        {D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "NON_PS_SRV"},
        {D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, "PS_SRV"},
        {D3D12_RESOURCE_STATE_STREAM_OUT, "STREAM_OUT"},
        {D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, "INDIRECT"},
        {D3D12_RESOURCE_STATE_COPY_DEST, "COPY_DEST"},
        {D3D12_RESOURCE_STATE_COPY_SOURCE, "COPY_SRC"},
        {D3D12_RESOURCE_STATE_RESOLVE_DEST, "RESOLVE_DEST"},
        {D3D12_RESOURCE_STATE_RESOLVE_SOURCE, "RESOLVE_SRC"},
    };
    std::string out;
    int remaining = state;
    for (const Bit& bit : kBits)
    {
        if ((state & bit.value) == bit.value && bit.value != 0)
        {
            if (!out.empty())
            {
                out += "|";
            }
            out += bit.name;
            remaining &= ~bit.value;
        }
    }
    if (remaining != 0 || out.empty())
    {
        out += (out.empty() ? "" : "|") + std::string("0x") + std::to_string(remaining);
    }
    return out;
}

// Liveness: how long to watch before reporting the selected resource silent.
// A menu/pause can silence it for hundreds of thousands of frames without it
// being lost - the same pointer can resume much later - so this only reports
// the gap, it never reidentifies on its own.
constexpr uint64_t kQuietFramesBeforeSilent = 300;

constexpr size_t kMaxProfiledResources = 20000;
constexpr size_t kMaxShortlisted = 64;

} // namespace

bool IsTriggerEdgeFor(TriggerEdge trigger, int stateBefore, int stateAfter) noexcept
{
    switch (trigger)
    {
        case TriggerEdge::RenderTargetToShaderResource:
            return stateBefore == D3D12_RESOURCE_STATE_RENDER_TARGET && IsShaderResourceState(stateAfter);
        case TriggerEdge::DepthWriteToReadable:
            return stateBefore == D3D12_RESOURCE_STATE_DEPTH_WRITE && IsDepthReadableState(stateAfter);
    }
    return false;
}

bool IsReverseTriggerEdgeFor(TriggerEdge trigger, int stateBefore, int stateAfter) noexcept
{
    switch (trigger)
    {
        case TriggerEdge::RenderTargetToShaderResource:
            return IsShaderResourceState(stateBefore) && stateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET;
        case TriggerEdge::DepthWriteToReadable:
            // Depth has no reverse-edge concept in the pre-extraction code -
            // readable -> DEPTH_WRITE was never tracked.
            return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// BarrierProfileStore
// ---------------------------------------------------------------------------

void BarrierProfileStore::EnsureProfiled(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (profiles_.size() >= kMaxProfiledResources || profiles_.find(resource) != profiles_.end())
    {
        return;
    }
    BarrierProfile p;
    p.width = desc.Width;
    p.height = desc.Height;
    p.format = static_cast<int>(desc.Format);
    p.flags = static_cast<unsigned>(desc.Flags);
    profiles_.emplace(resource, p);
}

bool BarrierProfileStore::RecordTransition(ID3D12Resource* resource, uint64_t frame, int stateBefore, int stateAfter) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = profiles_.find(resource);
    if (it == profiles_.end())
    {
        return false;
    }

    BarrierProfile& p = it->second;
    if (p.lastFrame != frame)
    {
        if (p.lastFrame != ~0ull)
        {
            ++p.framesSeen;
            if (p.eventsThisFrame == 2)
            {
                ++p.framesWithExactlyTwo;
            }
            if (p.eventsThisFrame % 2 == 0)
            {
                ++p.framesWithEvenCount;
            }
        }
        p.eventsThisFrame = 0;
        p.lastFrame = frame;
    }
    ++p.eventsThisFrame;
    ++p.transitions;

    bool recorded = false;
    for (int i = 0; i < p.pairsUsed; ++i)
    {
        if (p.pairs[i].before == stateBefore && p.pairs[i].after == stateAfter)
        {
            ++p.pairs[i].count;
            recorded = true;
            break;
        }
    }
    if (!recorded && p.pairsUsed < BarrierProfile::kMaxPairs)
    {
        p.pairs[p.pairsUsed].before = stateBefore;
        p.pairs[p.pairsUsed].after = stateAfter;
        p.pairs[p.pairsUsed].count = 1;
        ++p.pairsUsed;
    }

    if (stateBefore == D3D12_RESOURCE_STATE_UNORDERED_ACCESS || stateAfter == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        p.sawUnorderedAccess = true;
    }

    // Computed once, for every TriggerEdge that exists - not once per
    // identifier (docs/REFACTOR_PLAN.md sec 2.5 point 1, finding 1.16).
    for (int edge = 0; edge < kTriggerEdgeCount; ++edge)
    {
        const auto trigger = static_cast<TriggerEdge>(edge);
        if (IsTriggerEdgeFor(trigger, stateBefore, stateAfter))
        {
            ++p.triggerEdgeCountByEdge[edge];
            p.lastReadableStateByEdge[edge] = stateAfter;
        }
        else if (IsReverseTriggerEdgeFor(trigger, stateBefore, stateAfter))
        {
            ++p.reverseEdgeCountByEdge[edge];
            p.lastReadableStateByEdge[edge] = stateBefore;
        }
    }

    // State continuity: whatever a resource transitioned *to* last time must
    // be what it transitions *from* next time, or someone else moved it.
    if (p.lastAfter >= 0 && stateBefore != p.lastAfter)
    {
        ++p.alternationViolations;
    }
    p.lastAfter = stateAfter;

    return true;
}

void BarrierProfileStore::RecordUav(ID3D12Resource* resource) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = profiles_.find(resource);
    if (it != profiles_.end())
    {
        ++it->second.uavBarriers;
    }
}

bool BarrierProfileStore::Get(ID3D12Resource* resource, BarrierProfile* out) const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = profiles_.find(resource);
    if (it == profiles_.end())
    {
        return false;
    }
    *out = it->second;
    return true;
}

void BarrierProfileStore::RecordSharedCall(ID3D12Resource* resource, TriggerEdge own, TriggerEdge other) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = profiles_.find(resource);
    if (it != profiles_.end())
    {
        ++it->second.triggerEdgesSharingCallWith[static_cast<int>(own)][static_cast<int>(other)];
    }
}

void BarrierProfileStore::ResetMeasurement() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [resource, p] : profiles_)
    {
        const UINT64 width = p.width;
        const UINT height = p.height;
        const int format = p.format;
        const unsigned flags = p.flags;
        p = BarrierProfile{};
        p.width = width;
        p.height = height;
        p.format = format;
        p.flags = flags;
    }
}

void BarrierProfileStore::Clear() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    profiles_.clear();
}

size_t BarrierProfileStore::Size() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return profiles_.size();
}

// ---------------------------------------------------------------------------
// KnownSourceSet
// ---------------------------------------------------------------------------

bool KnownSourceSet::Note(ID3D12Resource* resource) noexcept
{
    if (resource == nullptr || Contains(resource))
    {
        return false;
    }
    for (auto& slot : resources_)
    {
        ID3D12Resource* expected = nullptr;
        if (slot.compare_exchange_strong(expected, resource, std::memory_order_acq_rel))
        {
            return true;
        }
        if (expected == resource)
        {
            return false; // raced with another Note() of the same pointer
        }
    }
    // Full. Every ResizeBuffers hands back a fresh set of buffer pointers, so
    // this fills after a few resizes - and refusing new entries (as this used
    // to) would leave the CURRENT back buffers searchable, with any shortlist
    // holding them breaking the game's next ResizeBuffers. Evict round-robin
    // instead: the oldest entries belong to buffers a resize already freed.
    const unsigned victim = nextEviction_.fetch_add(1, std::memory_order_relaxed) % kMaxKnownSources;
    resources_[victim].store(resource, std::memory_order_release);
    return true;
}

bool KnownSourceSet::Contains(ID3D12Resource* resource) const noexcept
{
    if (resource == nullptr)
    {
        return false;
    }
    for (const auto& slot : resources_)
    {
        if (slot.load(std::memory_order_acquire) == resource)
        {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// SurfaceIdentifier
// ---------------------------------------------------------------------------

SurfaceIdentifier::SurfaceIdentifier(const SurfaceSpec& spec) noexcept : spec_(spec)
{
}

void SurfaceIdentifier::SetProfileStore(BarrierProfileStore* store) noexcept
{
    profileStore_ = store;
}

void SurfaceIdentifier::SetExtentSource(const SurfaceIdentifier* source) noexcept
{
    extentSource_ = source;
}

void SurfaceIdentifier::SetKnownSourceExclusion(const KnownSourceSet* excluded) noexcept
{
    knownSourceExclusion_ = excluded;
}

void SurfaceIdentifier::SetMutualExclusion(MutualExclusionChecker* checker) noexcept
{
    mutualExclusion_ = checker;
}

bool SurfaceIdentifier::IsTriggerEdge(int stateBefore, int stateAfter) const noexcept
{
    return IsTriggerEdgeFor(spec_.trigger, stateBefore, stateAfter);
}

bool SurfaceIdentifier::IsReverseTriggerEdge(int stateBefore, int stateAfter) const noexcept
{
    return IsReverseTriggerEdgeFor(spec_.trigger, stateBefore, stateAfter);
}

bool SurfaceIdentifier::StructurallyPlausible(
    ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc, int* formatScore, const char** rejection) const noexcept
{
    // Known-source exclusion first (docs/REFACTOR_PLAN.md sec 3.3): a
    // resource obtained through a documented API contract (the swapchain's
    // own back buffers) is never a candidate for anything this class
    // searches for, no matter what its descriptor looks like.
    *formatScore = 0;
    *rejection = "";
    if (knownSourceExclusion_ != nullptr && knownSourceExclusion_->Contains(resource))
    {
        *rejection = "this resource is known by construction (e.g. a swapchain back buffer) - excluded from every search";
        return false;
    }

    // Format tested first so *formatScore is set even on rejection - lets the
    // caller report near-misses (e.g. "velocity-shaped format, missing UAV flag").
    for (uint8_t i = 0; i < spec_.formatCount; ++i)
    {
        if (desc.Format == spec_.formats[i].format)
        {
            *formatScore = spec_.formats[i].score;
            break;
        }
    }
    if (*formatScore == 0)
    {
        *rejection = "format is not one this surface's table can return";
        return false;
    }

    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        *rejection = "not a 2D texture";
        return false;
    }

    if (desc.MipLevels != 1)
    {
        *rejection = "mips != 1";
        return false;
    }

    if (desc.DepthOrArraySize != 1)
    {
        *rejection = "arraySize != 1";
        return false;
    }

    if (desc.SampleDesc.Count != 1)
    {
        *rejection = "sampleCount != 1";
        return false;
    }

    // D3D12 requires UNKNOWN for ordinary (non-cross-adapter, non-row-major)
    // textures, so this excludes occasional CPU-visible staging textures.
    if (desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN)
    {
        *rejection = "layout != UNKNOWN";
        return false;
    }

    if ((static_cast<UINT>(desc.Flags) & spec_.requiredFlags) != spec_.requiredFlags)
    {
        *rejection = "flags lack the required set";
        return false;
    }

    if (spec_.forbiddenFlags != 0 && (static_cast<UINT>(desc.Flags) & spec_.forbiddenFlags) != 0)
    {
        *rejection = "flags include a forbidden bit (e.g. a shadow/occluder map, not the scene surface)";
        return false;
    }

    if (desc.Width < spec_.minWidth || desc.Height < spec_.minHeight)
    {
        *rejection = "below plausible render resolution (tile/downsampled target)";
        return false;
    }

    // Bound size against the back buffer, but generously: "render target no
    // larger than back buffer" is false (measured 1707x1067 back buffer vs
    // 1708x1068 SceneVelocity - UE rounds extents up, and screen percentage
    // above 100 is also legitimate). 2x linear still excludes what this is
    // for - 4096x4096 shadow/lightmap atlases - while admitting rounding and
    // supersampling.
    if (spec_.maxBackBufferMultiple != 0)
    {
        const UINT64 bbWidth = backBufferWidth_.load(std::memory_order_relaxed);
        const UINT bbHeight = backBufferHeight_.load(std::memory_order_relaxed);
        if (bbWidth != 0 &&
            (desc.Width > bbWidth * spec_.maxBackBufferMultiple || desc.Height > bbHeight * spec_.maxBackBufferMultiple))
        {
            *rejection = "more than the plausible multiple of the back buffer in a dimension";
            return false;
        }
    }
    return true;
}

bool SurfaceIdentifier::StructurallyPlausibleForRival(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) const noexcept
{
    int formatScore = 0;
    const char* rejection = "";
    return StructurallyPlausible(resource, desc, &formatScore, &rejection);
}

std::string SurfaceIdentifier::DescLine(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) const noexcept
{
    return std::to_string(reinterpret_cast<uintptr_t>(resource)) + " " + std::to_string(desc.Width) + "x" +
           std::to_string(desc.Height) + " fmt=" + std::to_string(static_cast<int>(desc.Format)) +
           " mips=" + std::to_string(desc.MipLevels) + " arraySize=" + std::to_string(desc.DepthOrArraySize) +
           " samples=" + std::to_string(desc.SampleDesc.Count) +
           " flags=" + std::to_string(static_cast<unsigned>(desc.Flags)) +
           " layout=" + std::to_string(static_cast<int>(desc.Layout));
}

std::string SurfaceIdentifier::ProfileLine(const BarrierProfile& p) const
{
    const int edge = static_cast<int>(spec_.trigger);
    const uint64_t triggerEdgeCount = p.triggerEdgeCountByEdge[edge];
    const uint64_t reverseEdgeCount = p.reverseEdgeCountByEdge[edge];
    const uint64_t otherPairs = p.transitions > triggerEdgeCount + reverseEdgeCount
                                     ? p.transitions - triggerEdgeCount - reverseEdgeCount
                                     : 0;
    return "framesSeen=" + std::to_string(p.framesSeen) + " exactly2=" + std::to_string(p.framesWithExactlyTwo) +
           " even=" + std::to_string(p.framesWithEvenCount) + " trigger=" + std::to_string(triggerEdgeCount) +
           " reverse=" + std::to_string(reverseEdgeCount) + " otherPairs=" + std::to_string(otherPairs) +
           " violations=" + std::to_string(p.alternationViolations) + " uavBarriers=" + std::to_string(p.uavBarriers) +
           " srvState=" + std::to_string(p.lastReadableStateByEdge[edge]);
}

std::string SurfaceIdentifier::PairsLine(const BarrierProfile& p)
{
    std::string out;
    for (int i = 0; i < p.pairsUsed; ++i)
    {
        out += " " + StateName(p.pairs[i].before) + "->" + StateName(p.pairs[i].after) + "=" +
               std::to_string(p.pairs[i].count);
    }
    return out.empty() ? " (none)" : out;
}

std::pair<UINT64, UINT> SurfaceIdentifier::ModalResolution() const noexcept
{
    std::lock_guard<std::mutex> lock(resolutionMutex_);
    std::pair<UINT64, UINT> best{0, 0};
    int bestCount = 0;
    for (const auto& [size, count] : resolutionHistogram_)
    {
        if (count > bestCount)
        {
            bestCount = count;
            best = size;
        }
    }
    return best;
}

void SurfaceIdentifier::LogResolutionHistogram() const noexcept
{
    std::vector<std::pair<int, std::pair<UINT64, UINT>>> byCount;
    {
        std::lock_guard<std::mutex> lock(resolutionMutex_);
        for (const auto& [size, count] : resolutionHistogram_)
        {
            byCount.push_back({count, size});
        }
    }
    std::sort(byCount.rbegin(), byCount.rend());
    std::string line = std::string(spec_.logPrefix) + ": render-target resolution histogram (top 6, derived not assumed):";
    for (size_t i = 0; i < byCount.size() && i < 6; ++i)
    {
        line += " " + std::to_string(byCount[i].second.first) + "x" + std::to_string(byCount[i].second.second) + "=" +
                std::to_string(byCount[i].first);
    }
    Log(line);
}

void SurfaceIdentifier::ConsiderResource(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) noexcept
{
    // The resolution histogram is over render-targetable 2D textures only, no buffers or staging textures.
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        (static_cast<UINT>(desc.Flags) & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0)
    {
        std::lock_guard<std::mutex> lock(resolutionMutex_);
        ++resolutionHistogram_[{desc.Width, desc.Height}];
    }

    int formatScore = 0;
    const char* rejection = "";
    if (!StructurallyPlausible(resource, desc, &formatScore, &rejection))
    {
        if (formatScore > 0)
        {
            // A shape near-miss - worth a line each, since these are what to
            // check first if the search finds nothing.
            Log(std::string(spec_.logPrefix) + ": near-miss " + DescLine(resource, desc) + " - rejected: " + rejection);
        }
        return;
    }

    std::lock_guard<std::mutex> lock(shortlistMutex_);
    if (shortlist_.size() >= kMaxShortlisted)
    {
        return;
    }
    // A reopen that clears the global "seen" set (SurfaceRegistry::
    // ReopenCascade) but preserves THIS identifier's own shortlist
    // (spec.reopenClearsShortlist == false, depth) can re-present an
    // already-shortlisted resource once it becomes unseen again elsewhere in
    // the cascade - without this check that would add a second reference to
    // the same physical resource, and Decide() would then compare it against
    // itself as if it were a second, tied candidate.
    for (const ComPtr<ID3D12Resource>& held : shortlist_)
    {
        if (held.Get() == resource)
        {
            return;
        }
    }

    shortlist_.emplace_back(resource); // ComPtr ctor AddRefs
    Log(std::string(spec_.logPrefix) + ": shortlisted (structure) " + DescLine(resource, desc) + " - now watching its barriers");
}

bool SurfaceIdentifier::OnTransition(
    ID3D12Resource* resource, uint64_t frame, int stateBefore, int stateAfter, bool profiled) noexcept
{
    // Liveness of the selected resource and EVERY rival-adopted one, tracked
    // outside the profile store since the store can be cleared on reopen and
    // this must keep working across that. Only the trigger edge counts.
    //
    // "Every", not just the latest (identifiedAlt_): on Skyrunner UE rotates
    // through 5+ allocations of SceneVelocity and 10+ of GBufferA. An
    // allocation adopted earlier stays a candidate - so it is still CAPTURED
    // from - but its edges used to leave this clock alone, and it is never
    // offered as a rival again (it is already a candidate). Result: velocity
    // read as "silent" while rendering, and bursts ended with
    // velocity_pass_silent after 0-1061 frames. The candidate lookup runs only
    // on trigger edges, a few per frame.
    const bool isTrigger = IsTriggerEdge(stateBefore, stateAfter);
    const bool isTracked = resource == identified_.load(std::memory_order_acquire) ||
                           resource == identifiedAlt_.load(std::memory_order_acquire) ||
                           (isTrigger && identified_.load(std::memory_order_acquire) != nullptr && IsCandidate(resource));
    if (isTracked && isTrigger)
    {
        lastEdgeFrame_.store(frame, std::memory_order_relaxed);
        if (silenceReported_.exchange(false))
        {
            Log(std::string(spec_.logPrefix) + ": the pass has RESUMED on frame " + std::to_string(frame) +
                " - on one of the tracked targets, so nothing was reidentified.");
        }
    }

    return isTracked && isTrigger && profiled;
}

int SurfaceIdentifier::CollectSignals(
    const D3D12_RESOURCE_DESC& desc,
    const BarrierProfile& p,
    uint64_t triggerEdgeCount,
    int formatScore,
    const std::pair<UINT64, UINT>& modal,
    std::string* signalsLine) const noexcept
{
    const double perFrame = p.framesSeen > 0 ? static_cast<double>(triggerEdgeCount) / p.framesSeen : 0.0;

    struct Evaluated
    {
        std::string name;
        int weight;
        bool present;
    };
    Evaluated evaluated[8]{};
    int n = 0;

    for (uint8_t i = 0; i < spec_.signalCount; ++i)
    {
        const SignalWeight& sw = spec_.signals[i];
        std::string name;
        bool present = false;
        switch (sw.id)
        {
            case SignalId::AtModalResolution:
                name = "at the modal render-target resolution";
                present = desc.Width == modal.first && desc.Height == modal.second;
                break;
            case SignalId::ExactRequiredFlags:
                name = "flags are exactly the required set";
                present = static_cast<UINT>(desc.Flags) == spec_.requiredFlags;
                break;
            case SignalId::ProducedOnceExactly:
                name = "produced once per frame (one producer)";
                present = perFrame >= 0.9 && perFrame <= 1.1;
                break;
            case SignalId::NeverTakesUavBarrier:
                name = "never takes a UAV barrier";
                present = p.uavBarriers == 0;
                break;
            case SignalId::NeverEntersUnorderedAccess:
                name = "never enters UNORDERED_ACCESS";
                present = !p.sawUnorderedAccess;
                break;
            case SignalId::StrictTwoEventsPerFrame:
                name = "strict 2 events every frame (Oxi shape)";
                present = p.framesSeen > 0 && p.framesWithExactlyTwo == p.framesSeen;
                break;
            case SignalId::NoContinuityViolations:
                name = "no state-continuity violations";
                present = p.alternationViolations == 0;
                break;
            case SignalId::SharesCallWithEdge:
            {
                const uint64_t shared =
                    p.triggerEdgesSharingCallWith[static_cast<int>(spec_.trigger)][static_cast<int>(sw.edge)];
                const uint64_t percent = triggerEdgeCount > 0 ? shared * 100 / triggerEdgeCount : 0;
                name = std::string("shares a ResourceBarrier call with a ") +
                       (sw.edge == TriggerEdge::DepthWriteToReadable ? "DEPTH_WRITE->readable"
                                                                      : "RENDER_TARGET->shader-resource") +
                       " edge on >= " + std::to_string(sw.minPercent) + "% of its edges (measured " +
                       std::to_string(percent) + "%)";
                present = triggerEdgeCount > 0 && percent >= static_cast<uint64_t>(sw.minPercent);
                break;
            }
        }
        evaluated[n++] = {name, sw.weight, present};
    }

    // The format table's own score is a baseline, not a signal: it counts in
    // full once it clears spec.formatScoreGate, and is not scaled by a
    // per-signal weight. velocity gates this at 4 (desktop-tier only,
    // reported as its own checkbox below); depth's gate is 0, so every
    // structurally-accepted format counts in full and this line is skipped -
    // depth's pre-extraction log never had a signals section at all.
    int total = (formatScore >= spec_.formatScoreGate) ? formatScore : 0;
    if (signalsLine != nullptr)
    {
        signalsLine->clear();
        if (spec_.formatScoreGate > 0)
        {
            const bool present = formatScore >= spec_.formatScoreGate;
            *signalsLine += std::string("\n ") + spec_.logPrefix + ":       [" + (present ? "x" : " ") + "] +" +
                            std::to_string(formatScore) + "  format is a desktop-tier format";
        }
    }
    for (int i = 0; i < n; ++i)
    {
        if (evaluated[i].present)
        {
            total += evaluated[i].weight;
        }
        if (signalsLine != nullptr)
        {
            *signalsLine += std::string("\n ") + spec_.logPrefix + ":       [" + (evaluated[i].present ? "x" : " ") +
                            "] +" + std::to_string(evaluated[i].weight) + "  " + evaluated[i].name;
        }
    }
    return total;
}

void SurfaceIdentifier::Decide(uint64_t frame) noexcept
{
    std::vector<ComPtr<ID3D12Resource>> shortlist;
    {
        std::lock_guard<std::mutex> lock(shortlistMutex_);
        shortlist = shortlist_;
    }

    LogResolutionHistogram();
    const auto modal = ModalResolution();

    // A dependent surface (depth) is bound to another identifier's live
    // extent rather than a modal-resolution prior - see SetExtentSource.
    UINT64 extentWidth = 0;
    UINT extentHeight = 0;
    const bool hasExtent = extentSource_ != nullptr && extentSource_->Extent(&extentWidth, &extentHeight);

    Log(std::string(spec_.logPrefix) + ": modal render-target resolution " + std::to_string(modal.first) + "x" +
        std::to_string(modal.second) + "; back buffer " + std::to_string(backBufferWidth_.load()) + "x" +
        std::to_string(backBufferHeight_.load()) + "; " + std::to_string(shortlist.size()) +
        " structurally plausible resource(s) after " + std::to_string(frame) + " frames");

    const int edge = static_cast<int>(spec_.trigger);
    std::vector<Survivor> survivors;
    for (const ComPtr<ID3D12Resource>& held : shortlist)
    {
        ID3D12Resource* resource = held.Get();
        BarrierProfile profile;
        if (profileStore_ == nullptr || !profileStore_->Get(resource, &profile))
        {
            continue;
        }
        const uint64_t triggerEdgeCount = profile.triggerEdgeCountByEdge[edge];

        // Behaviour gate: a necessary condition, deliberately weak - "it gets
        // produced and then consumed, most frames". Anything stronger has
        // already been falsified once (see CollectSignals).
        if (profile.framesSeen < spec_.minFramesObserved || triggerEdgeCount * 2 < profile.framesSeen)
        {
            continue;
        }

        D3D12_RESOURCE_DESC desc = resource->GetDesc(); // safe: the shortlist holds a reference
        int formatScore = 0;
        const char* rejection = "";
        if (!StructurallyPlausible(resource, desc, &formatScore, &rejection))
        {
            // desc changed since shortlisting
            continue;
        }

        // Mutual exclusion, applied before ranking - see
        // MutualExclusionChecker::HeldByOther for why not only at claim time.
        if (mutualExclusion_ != nullptr)
        {
            if (const char* holder = mutualExclusion_->HeldByOther(resource, this))
            {
                Log(std::string(spec_.logPrefix) + ": excluded " + DescLine(resource, desc) + " - already held by " +
                    holder + " (mutual exclusion: one resource, one surface)");
                continue;
            }
        }

        if (extentSource_ != nullptr)
        {
            if (!hasExtent)
            {
                continue; // dependency hasn't resolved; can't test extent yet
            }
            if (desc.Width != extentWidth || desc.Height != extentHeight)
            {
                Log(std::string(spec_.logPrefix) + ": rejected " + DescLine(resource, desc) +
                    " - not at the dependency's extent (" + std::to_string(extentWidth) + "x" +
                    std::to_string(extentHeight) + ")");
                continue;
            }
        }

        Survivor s;
        s.resource = resource;
        s.desc = desc;
        s.profile = profile;
        s.triggerEdgeCount = triggerEdgeCount;
        s.score = CollectSignals(s.desc, profile, triggerEdgeCount, formatScore, modal, &s.signalsLine);
        survivors.push_back(std::move(s));
    }

    if (survivors.empty())
    {
        // Not a failure yet: the settling window may simply have been too
        // short, or the title may not have rendered anything interesting
        // during it. Widen and try again rather than giving up for the
        // session.
        const int attempt = attempt_.fetch_add(1) + 1;
        Log(std::string(spec_.logPrefix) + ": NO candidate after " + std::to_string(frame) + " frames (attempt " +
            std::to_string(attempt) + "). Structural shortlist had " + std::to_string(shortlist.size()) +
            " entries; none of them was produced and then consumed on at least half the frames. Retrying with a "
            "longer window. If this repeats, either the title was not rendering a scene during the window (a "
            "menu will do this), or this surface's pass does not look like anything measured so far - read the "
            "observed transitions below and mv_barrier_profile.log.");
        for (const ComPtr<ID3D12Resource>& held : shortlist)
        {
            BarrierProfile profile;
            if (profileStore_ != nullptr && profileStore_->Get(held.Get(), &profile))
            {
                Log(std::string(spec_.logPrefix) + ":   shortlisted " + DescLine(held.Get(), held->GetDesc()) + " | " +
                    ProfileLine(profile));
                Log(std::string(spec_.logPrefix) + ":     observed transitions:" + PairsLine(profile));
            }
        }
        // Measure the NEXT window fresh rather than continuing to accumulate.
        // The strict tests (e.g. "framesWithExactlyTwo == framesSeen") are
        // all-or-nothing and counters are cumulative, so a stretch where the
        // pass behaves differently (e.g. injecting at a main menu, which
        // renders no scene) poisons the test permanently - identification
        // would fail forever with no more explanation than "none showed a
        // clean cycle".
        //
        // Resetting keeps the population/descs and restarts measurement, so
        // each attempt is a clean window. Shared across every identifier -
        // see BarrierProfileStore::ResetMeasurement for why that's safe.
        if (profileStore_ != nullptr)
        {
            profileStore_->ResetMeasurement();
        }
        decideAtFrame_.store(frame + spec_.settleFrames * 2, std::memory_order_relaxed);
        return;
    }

    // Primary: score. Secondary/tertiary: trigger-edge count, then total
    // transitions - both always computed (harmless when unused: a surface
    // whose survivors never tie on score never reaches them). velocity's
    // ambiguity rule (ScoreMargin) always treats an exact score tie as
    // AMBIGUOUS regardless of order, so this is a no-op for velocity's
    // observable behaviour; depth's ambiguity rule (EdgeCountRatio) needs
    // exactly this secondary ordering to break a same-format tie correctly -
    // this is the ordering the pre-extraction depth Decide() used.
    std::sort(
        survivors.begin(),
        survivors.end(),
        [](const Survivor& a, const Survivor& b)
        {
            if (a.score != b.score)
            {
                return a.score > b.score;
            }
            if (a.triggerEdgeCount != b.triggerEdgeCount)
            {
                return a.triggerEdgeCount > b.triggerEdgeCount;
            }
            return a.profile.transitions > b.profile.transitions;
        });

    // Always dump every survivor, whether or not the pick is unambiguous.
    Log(std::string(spec_.logPrefix) + ": " + std::to_string(survivors.size()) +
        " resource(s) passed the structural gate and the produce-then-consume test. Evidence for each:");
    for (const Survivor& s : survivors)
    {
        Log(std::string(spec_.logPrefix) + ":   score=" + std::to_string(s.score) + " " + DescLine(s.resource, s.desc) +
            " | " + ProfileLine(s.profile) + s.signalsLine);
        Log(std::string(spec_.logPrefix) + ":     observed transitions:" + PairsLine(s.profile));
    }

    if (survivors.size() > 1)
    {
        bool separated;
        if (spec_.ambiguityRule == AmbiguityRule::ScoreMargin)
        {
            separated = (survivors[0].score - survivors[1].score) >= spec_.ambiguityMargin;
        }
        else // EdgeCountRatio
        {
            separated = survivors[0].score > survivors[1].score ||
                        survivors[0].triggerEdgeCount * 100 >
                            survivors[1].triggerEdgeCount * static_cast<uint64_t>(spec_.ambiguityMargin);
        }

        if (!separated)
        {
            Log(std::string(spec_.logPrefix) + ": AMBIGUOUS - the top two survivors score " +
                std::to_string(survivors[0].score) + " and " + std::to_string(survivors[1].score) +
                " with comparable usage, which is not enough to pick between them. Refusing to capture rather "
                "than guessing: a wrong pick produces a dump that decodes to plausible garbage, which is the "
                "most expensive failure mode this project has.");
            decisionMade_.store(true, std::memory_order_relaxed);
            return;
        }

        if (spec_.ambiguityRule == AmbiguityRule::ScoreMargin)
        {
            Log(std::string(spec_.logPrefix) + ": picking the top-scoring survivor by a margin of " +
                std::to_string(survivors[0].score - survivors[1].score) +
                " points. This is a ranked choice among several matches, not a unique one.");
        }
        else
        {
            Log(std::string(spec_.logPrefix) + ": picking the top candidate on usage - " +
                std::to_string(survivors[0].triggerEdgeCount) + " trigger edges against " +
                std::to_string(survivors[1].triggerEdgeCount) +
                " for the runner-up over the same window. This is a ranked choice among structurally "
                "indistinguishable candidates, not a unique match.");
        }
    }

    const Survivor& winner = survivors.front();
    MarkCandidate(winner.resource, winner.desc);
    if (!IsCandidate(winner.resource))
    {
        // Mutual exclusion refused the claim (docs/REFACTOR_PLAN.md sec 3.3) -
        // another identifier already holds this resource. Stay in Searching
        // rather than reporting a selection nothing will ever capture from;
        // retry like the empty-survivors path so a later window (after
        // whichever identifier collided has moved on) gets another chance.
        Log(std::string(spec_.logPrefix) + ": top candidate " + DescLine(winner.resource, winner.desc) +
            " could not be claimed (see the REFUSING line above) - not selecting it. Retrying.");
        decideAtFrame_.store(frame + spec_.settleFrames * 2, std::memory_order_relaxed);
        return;
    }
    identifiedWidth_.store(winner.desc.Width, std::memory_order_relaxed);
    identifiedHeight_.store(winner.desc.Height, std::memory_order_relaxed);
    identifiedFormat_.store(static_cast<int>(winner.desc.Format), std::memory_order_relaxed);
    identifiedFlags_.store(static_cast<unsigned>(winner.desc.Flags), std::memory_order_relaxed);
    // Start the liveness clock from the decision, not from zero - otherwise a
    // decision reached at frame 90 is already "silent" the moment it is made.
    lastEdgeFrame_.store(frame, std::memory_order_relaxed);
    silenceReported_.store(false, std::memory_order_relaxed);
    identified_.store(winner.resource, std::memory_order_release);
    decisionMade_.store(true, std::memory_order_relaxed);
    rivalReports_.store(0, std::memory_order_relaxed);
    // The barrier-batch evidence this selection rests on - a rival will have
    // to show the same (see OnUnmatchedEdge).
    uint32_t evidenceMask = 0;
    for (uint8_t i = 0; i < spec_.signalCount; ++i)
    {
        if (spec_.signals[i].id == SignalId::SharesCallWithEdge && SharesCallSignalPresent(winner.profile, spec_.signals[i]))
        {
            evidenceMask |= 1u << i;
        }
    }
    selectedEvidenceMask_.store(evidenceMask, std::memory_order_relaxed);
    Log(std::string(spec_.logPrefix) + ": SELECTED " + DescLine(winner.resource, winner.desc) +
        " | score=" + std::to_string(winner.score) + " | survivors=" + std::to_string(survivors.size()));
    if (survivors.size() == 1)
    {
        Log(std::string(spec_.logPrefix) +
            ": this was the ONLY resource in the process that passed this surface's structural gate (format, "
            "flags, dimensions) AND is produced then consumed each frame. The behavioural signals above are "
            "corroboration; they are not what made it unique.");
    }
}

void SurfaceIdentifier::OnFrame(uint64_t frame) noexcept
{
    if (extentSource_ != nullptr)
    {
        UINT64 w = 0;
        UINT h = 0;
        if (!extentSource_->Extent(&w, &h))
        {
            // No extent to test against until the dependency resolves.
            // Profiles accumulate from the first barrier regardless, so
            // there is no separate settling window needed once it does.
            return;
        }
        if (decideAtFrame_.load(std::memory_order_relaxed) == 0)
        {
            decideAtFrame_.store(frame, std::memory_order_relaxed);
        }
    }
    else if (decideAtFrame_.load(std::memory_order_relaxed) == 0)
    {
        // Relative to now, not to frame 0 - after Reopen the session is
        // already thousands of frames in, and an absolute deadline of
        // spec_.settleFrames would be long past, so the retry would decide
        // immediately on an empty profile set.
        decideAtFrame_.store(frame + spec_.settleFrames, std::memory_order_relaxed);
    }
    if (!decisionMade_.load(std::memory_order_relaxed) && frame >= decideAtFrame_.load(std::memory_order_relaxed))
    {
        Decide(frame);
    }
}

ID3D12Resource* SurfaceIdentifier::Selected() const noexcept
{
    return identified_.load(std::memory_order_acquire);
}

IdentifyState SurfaceIdentifier::State() const noexcept
{
    if (identified_.load(std::memory_order_acquire) != nullptr)
    {
        return IdentifyState::Selected;
    }
    return decisionMade_.load(std::memory_order_relaxed) ? IdentifyState::Ambiguous : IdentifyState::Searching;
}

bool SurfaceIdentifier::Extent(UINT64* width, UINT* height) const noexcept
{
    if (identified_.load(std::memory_order_acquire) == nullptr)
    {
        return false;
    }
    *width = identifiedWidth_.load(std::memory_order_relaxed);
    *height = identifiedHeight_.load(std::memory_order_relaxed);
    return *width != 0 && *height != 0;
}

bool SurfaceIdentifier::IsCandidate(ID3D12Resource* resource) const noexcept
{
    if (resource == nullptr)
    {
        return false;
    }
    std::shared_lock lock(candidateMutex_);
    return candidates_.count(resource) != 0;
}

bool SurfaceIdentifier::ValidateCandidate(ID3D12Resource* resource) noexcept
{
    if (resource == nullptr)
    {
        return false;
    }
    D3D12_RESOURCE_DESC recorded{};
    {
        std::shared_lock lock(candidateMutex_);
        const auto it = candidates_.find(resource);
        if (it == candidates_.end())
        {
            return false;
        }
        recorded = it->second.desc;
    }
    // Safe to call GetDesc() here specifically because a candidate is held BY
    // REFERENCE (see MarkCandidate) - this is one of the few places in the
    // project where touching a resource outside the call that handed it to
    // us is defensible, and only for that reason.
    const D3D12_RESOURCE_DESC current = resource->GetDesc();
    const bool same = recorded.Dimension == current.Dimension && recorded.Width == current.Width &&
                       recorded.Height == current.Height && recorded.DepthOrArraySize == current.DepthOrArraySize &&
                       recorded.MipLevels == current.MipLevels && recorded.Format == current.Format &&
                       recorded.SampleDesc.Count == current.SampleDesc.Count && recorded.Flags == current.Flags;
    if (same)
    {
        return true;
    }
    Log(std::string(spec_.logPrefix) + ": candidate " + std::to_string(reinterpret_cast<uintptr_t>(resource)) +
        " no longer matches the desc it was accepted with (" + std::to_string(recorded.Width) + "x" +
        std::to_string(recorded.Height) + " fmt=" + std::to_string(static_cast<int>(recorded.Format)) + " -> " +
        std::to_string(current.Width) + "x" + std::to_string(current.Height) +
        " fmt=" + std::to_string(static_cast<int>(current.Format)) + "); dropping it");
    std::unique_lock lock(candidateMutex_);
    candidates_.erase(resource);
    return false;
}

void SurfaceIdentifier::MarkCandidate(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) noexcept
{
    if (mutualExclusion_ != nullptr && !mutualExclusion_->TryClaim(resource, this))
    {
        Log(std::string(spec_.logPrefix) + ": REFUSING to claim " + DescLine(resource, desc) +
            " - another surface already holds it as a candidate. Two surfaces capturing the same resource "
            "would double-count it in the dump; whichever claimed it first keeps it.");
        return;
    }
    std::unique_lock lock(candidateMutex_);
    auto& entry = candidates_[resource];
    entry.resource = resource; // ComPtr::operator= AddRefs
    entry.desc = desc;
}

bool SurfaceIdentifier::SharesCallSignalPresent(const BarrierProfile& p, const SignalWeight& sw) const noexcept
{
    const int own = static_cast<int>(spec_.trigger);
    const uint64_t edges = p.triggerEdgeCountByEdge[own];
    const uint64_t shared = p.triggerEdgesSharingCallWith[own][static_cast<int>(sw.edge)];
    return edges > 0 && shared * 100 / edges >= static_cast<uint64_t>(sw.minPercent);
}

int SurfaceIdentifier::RivalHasSelectedEvidence(ID3D12Resource* resource) const noexcept
{
    BarrierProfile p;
    if (profileStore_ == nullptr || !profileStore_->Get(resource, &p) ||
        p.triggerEdgeCountByEdge[static_cast<int>(spec_.trigger)] < spec_.rivalFramesBeforeAdopt)
    {
        return -1; // too little history to judge either way
    }
    const uint32_t mask = selectedEvidenceMask_.load(std::memory_order_relaxed);
    for (uint8_t i = 0; i < spec_.signalCount; ++i)
    {
        if ((mask & (1u << i)) != 0 && !SharesCallSignalPresent(p, spec_.signals[i]))
        {
            return 0;
        }
    }
    return 1;
}

bool SurfaceIdentifier::OnUnmatchedEdge(ID3D12Resource* resource, uint64_t frame) noexcept
{
    // No "rival" block in the spec (depth): never adopts.
    if (spec_.rivalFramesBeforeAdopt == 0)
    {
        return false;
    }
    // Cheapest checks first: this runs from the barrier hook, on every
    // trigger edge in the frame.
    ID3D12Resource* selected = SelectedRaw();
    if (selected == nullptr || resource == selected || resource == nullptr ||
        resource == identifiedAlt_.load(std::memory_order_acquire))
    {
        return false;
    }
    if (FramesSinceEdge(frame) < spec_.rivalStallFrames)
    {
        return false;
    }

    const D3D12_RESOURCE_DESC desc = resource->GetDesc();

    // Judged by the SAME structural filter identification uses.
    if (!StructurallyPlausibleForRival(resource, desc))
    {
        return false;
    }

    // Same render extent as the established answer - every copy of a scene
    // texture shares SceneTexturesConfig::Extent, so a same-shaped texture at
    // a different extent is some other buffer.
    if (desc.Width != identifiedWidth_.load(std::memory_order_relaxed) ||
        desc.Height != identifiedHeight_.load(std::memory_order_relaxed))
    {
        return false;
    }

    // rival.same_descriptor: a reallocation has the selected resource's exact
    // format and flags. Checked before the run below, like the evidence
    // check, so a different texture taking its edge every frame cannot keep
    // resetting the real copy's run.
    if (spec_.rivalSameDescriptor &&
        (static_cast<int>(desc.Format) != identifiedFormat_.load(std::memory_order_relaxed) ||
         static_cast<unsigned>(desc.Flags) != identifiedFlags_.load(std::memory_order_relaxed)))
    {
        return false;
    }

    // The evidence that WON the selection must hold for a rival too. Only
    // shares_call_with_edge is checked: it is the one signal that separated
    // normals from a same-descriptor decoy (an identical desc passes every
    // structural test above). A rival with enough history that lacks it is
    // ignored outright - without touching the run below, so a decoy taking
    // its edge every frame cannot keep resetting the real copy's run. One
    // without enough history yet is tracked, and must pass before adoption.
    // The rival is profiled here if it was not already: it may have
    // appeared after the identify budget was spent.
    int evidence = 1;
    if (selectedEvidenceMask_.load(std::memory_order_relaxed) != 0)
    {
        if (profileStore_ != nullptr)
        {
            profileStore_->EnsureProfiled(resource, desc);
        }
        evidence = RivalHasSelectedEvidence(resource);
        if (evidence == 0)
        {
            if (!evidenceRejectReported_.exchange(true))
            {
                Log(std::string(spec_.logPrefix) + ": NOT a rival - " + DescLine(resource, desc) +
                    " takes the edge while the selected one is stalled, but lacks the barrier-batch evidence the "
                    "selection was made on (shares_call_with_edge). Same descriptor, different pass. (Logged once.)");
            }
            return false;
        }
    }

    // Run of CONSECUTIVE frames on which THIS resource carried the edge,
    // counted once per frame however many barriers that frame contains.
    // Keyed on the resource as well as the frame: two different rivals
    // alternating must not add up to a run.
    const uint64_t previous = lastRivalFrame_.exchange(frame, std::memory_order_relaxed);
    const bool sameResource = (resource == lastRivalResource_);
    lastRivalResource_ = resource;

    uint64_t run = rivalRunLength_.load(std::memory_order_relaxed);
    if (sameResource && frame == previous)
    {
        // Same frame, another barrier - already counted.
    }
    else if (sameResource && frame == previous + 1)
    {
        run += 1;
        rivalRunLength_.store(run, std::memory_order_relaxed);
    }
    else
    {
        run = 1;
        rivalRunLength_.store(run, std::memory_order_relaxed);
    }

    // Long enough - but a rival still short of evidence keeps accumulating
    // rather than being adopted on the run alone.
    if (run >= spec_.rivalFramesBeforeAdopt && evidence == 1)
    {
        rivalRunLength_.store(0, std::memory_order_relaxed);
        if (adoptions_.fetch_add(1, std::memory_order_relaxed) >= spec_.rivalMaxAdoptions)
        {
            if (!adoptLimitReported_.exchange(true))
            {
                Log(std::string(spec_.logPrefix) + ": a further " + spec_.name +
                    " target would have been adopted, but the limit of " + std::to_string(spec_.rivalMaxAdoptions) +
                    " is spent - NOT adopting. Adopting without bound cannot be told apart from adopting the wrong "
                    "resources, so this stops and says so instead. Restart to reset it.");
            }
            return false;
        }

        const int identifiedFormat = identifiedFormat_.load(std::memory_order_relaxed);
        identifiedAlt_.store(resource, std::memory_order_release);
        Log(std::string(spec_.logPrefix) + ": ADOPTED a second " + spec_.name + " target - " +
            DescLine(resource, desc) + " has taken its trigger edge on " + std::to_string(run) +
            " consecutive frames while the selected one (fmt=" + std::to_string(identifiedFormat) +
            ") took none. Same extent, same structural filter" +
            (static_cast<int>(desc.Format) == identifiedFormat
                 ? std::string(", same format: a reallocation to a new address with an identical descriptor")
                 : std::string(", different format: unconfirmed - every prior adoption has been same-format")) +
            (selectedEvidenceMask_.load(std::memory_order_relaxed) != 0
                 ? std::string(", and the same barrier-batch evidence")
                 : std::string()) +
            ". BOTH are now captured from, whichever takes the edge - the pass moved, it did not stop.");
        return true;
    }

    if (rivalReports_.load(std::memory_order_relaxed) >= spec_.rivalMaxReports)
    {
        return false;
    }
    const int n = rivalReports_.fetch_add(1, std::memory_order_relaxed);
    if (n >= spec_.rivalMaxReports)
    {
        return false;
    }
    Log(std::string(spec_.logPrefix) + ": RIVAL " + spec_.name +
        "-format edge while the identified resource is stalled - ptr=" +
        std::to_string(reinterpret_cast<uintptr_t>(resource)) + " " + std::to_string(desc.Width) + "x" +
        std::to_string(desc.Height) + " fmt=" + std::to_string(static_cast<int>(desc.Format)) +
        " took its trigger edge on frame " + std::to_string(frame) + ", " + std::to_string(FramesSinceEdge(frame)) +
        " frames into the stall. The identified resource is " +
        std::to_string(identifiedWidth_.load(std::memory_order_relaxed)) + "x" +
        std::to_string(identifiedHeight_.load(std::memory_order_relaxed)) + ". If this repeats, the engine moved the " +
        spec_.name + " pass to another target rather than stopping it.");
    if (n + 1 == spec_.rivalMaxReports)
    {
        Log(std::string(spec_.logPrefix) + ": further rival-edge reports suppressed (cap " +
            std::to_string(spec_.rivalMaxReports) +
            " per stall episode, re-armed when the pass resumes). Absence of more lines is the cap, not the absence "
            "of rivals.");
    }
    return false;
}

bool SurfaceIdentifier::IsShortlisted(ID3D12Resource* resource) noexcept
{
    std::lock_guard<std::mutex> lock(shortlistMutex_);
    for (const ComPtr<ID3D12Resource>& held : shortlist_)
    {
        if (held.Get() == resource)
        {
            return true;
        }
    }
    return false;
}

bool SurfaceIdentifier::ForgetKnownSource(ID3D12Resource* resource) noexcept
{
    bool dropped = false;
    {
        std::lock_guard<std::mutex> lock(shortlistMutex_);
        const auto it = std::remove_if(
            shortlist_.begin(), shortlist_.end(), [resource](const ComPtr<ID3D12Resource>& held) { return held.Get() == resource; });
        dropped = it != shortlist_.end();
        shortlist_.erase(it, shortlist_.end());
    }
    {
        std::unique_lock lock(candidateMutex_);
        dropped = candidates_.erase(resource) != 0 || dropped;
    }
    if (dropped)
    {
        Log(std::string(spec_.logPrefix) + ": dropped " + std::to_string(reinterpret_cast<uintptr_t>(resource)) +
            " from the shortlist - it is a swapchain back buffer (known by construction), noted after it was "
            "shortlisted. No search may hold one.");
    }
    return SelectedRaw() == resource;
}

void SurfaceIdentifier::ForgetAllCandidates() noexcept
{
    size_t dropped = 0;
    {
        std::unique_lock lock(candidateMutex_);
        dropped = candidates_.size();
        candidates_.clear();
    }
    if (dropped != 0)
    {
        Log(std::string(spec_.logPrefix) + ": dropped " + std::to_string(dropped) +
            " candidate(s) because the search reopened - capture stops here and does not resume until a new "
            "decision is reached");
    }
}

void SurfaceIdentifier::Reopen(ReopenReason /*reason*/, const char* detail) noexcept
{
    Log(std::string(spec_.logPrefix) + ": REOPENING the search - " + detail +
        ". The previously selected resource is no longer the thing it was identified as, so anything captured from "
        "it now would be sized from a stale footprint. Nothing is captured until a new decision is reached.");
    identified_.store(nullptr, std::memory_order_release);
    identifiedWidth_.store(0, std::memory_order_relaxed);
    identifiedHeight_.store(0, std::memory_order_relaxed);
    decisionMade_.store(false, std::memory_order_relaxed);
    decideAtFrame_.store(0, std::memory_order_relaxed);

    if (spec_.reopenClearsShortlist)
    {
        {
            std::lock_guard<std::mutex> lock(shortlistMutex_);
            shortlist_.clear();
        }
        if (profileStore_ != nullptr)
        {
            profileStore_->Clear();
        }
        {
            std::lock_guard<std::mutex> lock(resolutionMutex_);
            resolutionHistogram_.clear();
        }
        // Note: refilling the global identify budget/seen-set used to happen
        // here (ForgetAllResourcesSeen). It is now SurfaceRegistry::
        // ReopenCascade's job, called exactly once per cascade rather than
        // once per identifier in it (docs/REFACTOR_PLAN.md sec 2.5 point 3,
        // finding 1.7) - without that move, a cascade reopening two
        // identifiers that both clear the shortlist would refill it twice
        // for no benefit, and a depth-only reopen (reopenClearsShortlist ==
        // false, so this branch never ran for it) would never refill it at
        // all, permanently losing a replacement resource once the budget was
        // spent.
    }
    else
    {
        // Keep the shortlist and profile population; only reset the
        // measurement window. Clearing them was tried and reverted for depth
        // (see the pre-extraction ReopenDepthIdentification): the shortlist
        // is refilled only from ConsiderResource, which the barrier hook
        // reaches only for resources not yet marked "seen" - a set this
        // branch deliberately does NOT clear itself, since re-feeding every
        // live resource to a DIFFERENT surface's search (one sharing the
        // same "seen" set) could poison that search with duplicate
        // structurally-identical entries. A cleared shortlist here could
        // only ever be refilled by resources the engine happens to allocate
        // AFTER the reopen - intermittent, worse than reproducible.
        if (profileStore_ != nullptr)
        {
            profileStore_->ResetMeasurement();
        }
    }

    // Clearing identified_ above is NOT what stops capture - the barrier
    // hook gates on IsCandidate(), a set this function used to leave alone
    // (see the long comment that used to live here, when that set was
    // process-wide in resource_tracking.h rather than per-identifier).
    ForgetAllCandidates();
    // Rival-adoption state goes with the answer it was relative to.
    identifiedAlt_.store(nullptr, std::memory_order_release);
    rivalRunLength_.store(0, std::memory_order_relaxed);
    lastRivalResource_ = nullptr;
    selectedEvidenceMask_.store(0, std::memory_order_relaxed);
}

bool SurfaceIdentifier::NoteBackBufferSize(uint64_t width, uint32_t height) noexcept
{
    const uint64_t previousWidth = backBufferWidth_.exchange(width, std::memory_order_relaxed);
    const uint32_t previousHeight = backBufferHeight_.exchange(static_cast<uint32_t>(height), std::memory_order_relaxed);

    // A swapchain resize makes UE rebuild scene textures at a new extent. The
    // selected resource is abandoned and should be reopened.
    const bool changed =
        previousWidth != 0 && previousHeight != 0 && (previousWidth != width || previousHeight != static_cast<uint32_t>(height));
    if (!changed)
    {
        return false;
    }
    if (identified_.load(std::memory_order_acquire) == nullptr && !decisionMade_.load(std::memory_order_relaxed))
    {
        // Nothing decided yet; the search will pick this up on its own.
        return false;
    }
    Log(std::string(spec_.logPrefix) + ": back buffer resized " + std::to_string(previousWidth) + "x" +
        std::to_string(previousHeight) + " -> " + std::to_string(width) + "x" + std::to_string(height) +
        ". UE rebuilds its scene textures at the new extent, so the selected resource is abandoned rather than "
        "changed - its own descriptor stays valid, which is why nothing else here notices.");
    return true;
}

bool SurfaceIdentifier::IsSilent(uint64_t frame, uint64_t* framesSilent) noexcept
{
    if (identified_.load(std::memory_order_acquire) == nullptr)
    {
        return false;
    }

    const uint64_t last = lastEdgeFrame_.load(std::memory_order_relaxed);
    if (frame <= last + kQuietFramesBeforeSilent)
    {
        return false;
    }
    if (framesSilent != nullptr)
    {
        *framesSilent = frame - last;
    }
    // Log once per silence episode, not once per frame while it persists - a
    // menu can hold this silent for hundreds of thousands of frames.
    if (!silenceReported_.exchange(true))
    {
        Log(std::string(spec_.logPrefix) + ": the pass has STOPPED. The identified resource has not taken its "
            "trigger edge for " +
            std::to_string(frame - last) + " presented frames (last on frame " + std::to_string(last) + ", now at " +
            std::to_string(frame) +
            "), while the game carries on presenting. That is what a menu, a "
            "pause, or the end of a run looks like from in here - the scene is no longer being drawn. "
            "It is NOT a reason to reidentify: on the one title where this was measured the same "
            "resource resumed 271,123 frames later.");
    }
    return true;
}

uint64_t SurfaceIdentifier::FramesSinceEdge(uint64_t frame) const noexcept
{
    if (identified_.load(std::memory_order_acquire) == nullptr)
    {
        return 0;
    }
    const uint64_t last = lastEdgeFrame_.load(std::memory_order_relaxed);
    return frame > last ? frame - last : 0;
}

} // namespace mv
