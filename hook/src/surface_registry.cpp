#include "surface_registry.h"

#include "logging.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace mv
{

namespace
{

// When `mv_barrier_profile.log` is written: late enough that every resource has
// had thousands of frames to show its pattern.
constexpr uint64_t kProfileDumpFrame = 2000;

// Distinct resources `GetDesc()` is called on (see `ConsiderResource`).
// Identification needs far fewer: it finishes in the first ~90 frames.
constexpr int64_t kIdentifyBudget = 8000;

} // namespace

SurfaceRegistry::SurfaceRegistry() noexcept : identifyBudget_(kIdentifyBudget)
{
    entries_.reserve(8); // room for every surface: `Configure()` hands out pointers into it
}

void SurfaceRegistry::ConsiderResource(ID3D12Resource* resource) noexcept
{
    if (resource == nullptr)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(seenMutex_);
        if (!seen_.insert(resource).second)
        {
            return;
        }
    }

    if (identifyBudget_.fetch_sub(1, std::memory_order_relaxed) <= 0)
    {
        if (!budgetExhaustionReported_.exchange(true))
        {
            Log("registry: identification budget of " + std::to_string(kIdentifyBudget) +
                " resources is spent; resources created from now on are not examined until a reopen.");
        }

        return;
    }

    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    profileStore_.EnsureProfiled(resource, desc);
    for (Entry& e : entries_)
    {
        e.identifier->ConsiderResource(resource, desc);
    }
}

void SurfaceRegistry::ForgetAllResourcesSeen() noexcept
{
    // Refill first: with an empty budget a reopened search would find nothing.
    identifyBudget_.store(kIdentifyBudget, std::memory_order_relaxed);
    budgetExhaustionReported_.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(seenMutex_);
    seen_.clear();
}

void SurfaceRegistry::Register(std::unique_ptr<SurfaceIdentifier> identifier) noexcept
{
    Entry e;
    e.identifier = identifier.get();
    owned_.push_back(std::move(identifier));
    entries_.push_back(e);
}

SurfaceRegistry::Entry* SurfaceRegistry::FindEntry(const char* name) noexcept
{
    for (Entry& e : entries_)
    {
        if (std::strcmp(e.identifier->Spec().name, name) == 0)
        {
            return &e;
        }
    }

    return nullptr;
}

SurfaceRegistry::Entry* SurfaceRegistry::FindEntry(const SurfaceIdentifier* identifier) noexcept
{
    for (Entry& e : entries_)
    {
        if (e.identifier == identifier)
        {
            return &e;
        }
    }

    return nullptr;
}

void SurfaceRegistry::Configure() noexcept
{
    for (Entry& e : entries_)
    {
        e.identifier->SetProfileStore(&profileStore_);
        e.identifier->SetKnownSourceExclusion(&knownSources_);
        e.identifier->SetMutualExclusion(this);
    }

    diag_ = DiagEnabled();
    velocity_ = Find("velocity");
    depth_ = Find("depth");
    optionals_.clear();
    for (const Entry& e : entries_)
    {
        if (e.identifier != velocity_ && e.identifier != depth_)
        {
            optionals_.push_back(e.identifier);
        }
    }

    for (const Entry& e : entries_)
    {
        const SurfaceSpec& spec = e.identifier->Spec();
        for (uint8_t i = 0; i < spec.signalCount; ++i)
        {
            sharedCallSignals_ = sharedCallSignals_ || spec.signals[i].id == SignalId::SharesCallWithEdge;
        }
    }

    // Second pass, once every identifier has a stable address: resolve each
    // `spec.extentFrom` and record the reverse edge for the reopen cascade.
    for (Entry& e : entries_)
    {
        const char* extentFrom = e.identifier->Spec().extentFrom;
        if (extentFrom == nullptr)
        {
            continue;
        }

        Entry* source = FindEntry(extentFrom);
        if (source == nullptr)
        {
            Log(std::string("registry: ") + e.identifier->Spec().name + "'s extentFrom names \"" + extentFrom +
                "\", which is not a registered surface - extent dependency NOT wired");
            continue;
        }

        e.identifier->SetExtentSource(source->identifier);
        e.extentSource = source->identifier;
        source->dependents.push_back(e.identifier);
    }
}

void SurfaceRegistry::SetReopenObserver(ReopenObserver observer) noexcept
{
    reopenObserver_ = observer;
}

SurfaceIdentifier* SurfaceRegistry::Find(const char* name) noexcept
{
    Entry* e = FindEntry(name);
    return e != nullptr ? e->identifier : nullptr;
}

ID3D12Resource* SurfaceRegistry::Selected(const char* name) noexcept
{
    SurfaceIdentifier* identifier = Find(name);
    return identifier != nullptr ? identifier->Selected() : nullptr;
}

void SurfaceRegistry::OnUavBarrier(ID3D12Resource* resource) noexcept
{
    profileStore_.RecordUav(resource);
}

int SurfaceRegistry::OnBarrierTransition(
    ID3D12Resource* resource, uint64_t frame, int stateBefore, int stateAfter, SurfaceTrigger* out,
    int outCapacity) noexcept
{
    profileStore_.RecordTransition(resource, frame, stateBefore, stateAfter);

    int count = 0;
    for (Entry& e : entries_)
    {
        SurfaceIdentifier* id = e.identifier;
        const bool isThisEdge = IsTriggerEdgeFor(id->Spec().trigger, stateBefore, stateAfter);
        const bool isCandidate = id->IsCandidate(resource);
        id->OnTransition(resource, frame, isThisEdge, isCandidate);
        if (isCandidate)
        {
            if (!isThisEdge)
            {
                continue;
            }

            if (id->ValidateCandidate(resource))
            {
                if (count < outCapacity)
                {
                    out[count++] = SurfaceTrigger{id, resource, stateAfter};
                }
            }
            else
            {
                ReopenCascade(
                    id->Spec().name, ReopenReason::DescriptorChanged,
                    "the selected resource's descriptor changed underneath us (resolution change, dynamic "
                    "resolution scaling, or a swapchain resize)");
            }
        }
        else if (isThisEdge && id->OnUnmatchedEdge(resource, frame))
        {
            id->MarkCandidate(resource, resource->GetDesc());
        }
    }

    return count;
}

void SurfaceRegistry::OnFrame(uint64_t frame) noexcept
{
    // F6's restart runs here, on the Present thread, as a resize's reopen
    // does. Every surface in the embedded profile is sized from velocity, so
    // velocity's cascade restarts them all.
    if (restartRequested_.exchange(false, std::memory_order_relaxed))
    {
        ReopenCascade("velocity", ReopenReason::Requested, "F6 pressed");
    }

    // Velocity registers first, so its `OnFrame` runs before its dependents'.
    // A dependent then sees an extent velocity found this frame without
    // waiting a frame. Nothing else depends on the order.
    for (Entry& e : entries_)
    {
        e.identifier->OnFrame(frame);
    }

    if (frame == kProfileDumpFrame && !profileDumped_.exchange(true))
    {
        DumpBarrierProfiles();
    }

    if (diag_ && !optionals_.empty() && (frame == 600 || frame == kProfileDumpFrame))
    {
        DumpBatchStats(frame);
    }
}

void SurfaceRegistry::OnBarrierBatch(
    ID3D12GraphicsCommandList* cmdList, const D3D12_RESOURCE_BARRIER* barriers, UINT count) noexcept
{
    if (barriers == nullptr)
    {
        return;
    }

    // Input of `SignalId::SharesCallWithEdge`: which trigger edges shared this
    // call with another resource's trigger edge, by type. The two edge types
    // have different before-states, so a transition is at most one of them.
    if (sharedCallSignals_)
    {
        constexpr TriggerEdge kEdges[kTriggerEdgeCount] = {TriggerEdge::RenderTargetToShaderResource,
                                                            TriggerEdge::DepthWriteToReadable};
        uint64_t perEdge[kTriggerEdgeCount] = {};
        for (UINT i = 0; i < count; ++i)
        {
            if (barriers[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
            {
                continue;
            }

            for (int e = 0; e < kTriggerEdgeCount; ++e)
            {
                perEdge[e] += IsTriggerEdgeFor(kEdges[e], static_cast<int>(barriers[i].Transition.StateBefore),
                                               static_cast<int>(barriers[i].Transition.StateAfter))
                                  ? 1
                                  : 0;
            }
        }

        if (perEdge[0] + perEdge[1] >= 2) // a lone edge shares its call with nothing
        {
            for (UINT i = 0; i < count; ++i)
            {
                if (barriers[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
                {
                    continue;
                }

                const D3D12_RESOURCE_TRANSITION_BARRIER& t = barriers[i].Transition;
                for (int own = 0; own < kTriggerEdgeCount; ++own)
                {
                    if (!IsTriggerEdgeFor(kEdges[own], static_cast<int>(t.StateBefore), static_cast<int>(t.StateAfter)))
                    {
                        continue;
                    }

                    for (int other = 0; other < kTriggerEdgeCount; ++other)
                    {
                        if (perEdge[other] - (own == other ? 1 : 0) > 0)
                        {
                            profileStore_.RecordSharedCall(t.pResource, kEdges[own], kEdges[other]);
                        }
                    }
                }
            }
        }
    }

    if (optionals_.empty() || velocity_ == nullptr || depth_ == nullptr ||
        (!diag_ && candidateEdgeObserver_ == nullptr))
    {
        return;
    }

    // Only once velocity and depth are both selected; before that, "not in a
    // call with velocity's edge" means nothing.
    if (velocity_->Selected() == nullptr || depth_->Selected() == nullptr)
    {
        return;
    }

    // For each surface, the tracked resource (selected or adopted) that took
    // its trigger edge in this call, so a candidate never counts as sharing a
    // call with itself.
    uint64_t rtToSrv = 0;
    ID3D12Resource* trackedIn[kMaxBatchSurfaces] = {};
    for (UINT i = 0; i < count; ++i)
    {
        if (barriers[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
        {
            continue;
        }

        const D3D12_RESOURCE_TRANSITION_BARRIER& t = barriers[i].Transition;
        const int before = static_cast<int>(t.StateBefore);
        const int after = static_cast<int>(t.StateAfter);
        rtToSrv += IsTriggerEdgeFor(TriggerEdge::RenderTargetToShaderResource, before, after) ? 1 : 0;
        for (size_t e = 0; diag_ && e < entries_.size() && e < kMaxBatchSurfaces; ++e)
        {
            SurfaceIdentifier* id = entries_[e].identifier;
            if (trackedIn[e] == nullptr && IsTriggerEdgeFor(id->Spec().trigger, before, after) &&
                (t.pResource == id->Selected() || id->IsCandidate(t.pResource)))
            {
                trackedIn[e] = t.pResource;
            }
        }
    }

    if (rtToSrv == 0)
    {
        return; // the common case: nothing here an optional surface's candidate could be part of
    }

    for (UINT i = 0; i < count; ++i)
    {
        if (barriers[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
        {
            continue;
        }

        const D3D12_RESOURCE_TRANSITION_BARRIER& t = barriers[i].Transition;
        if (!IsTriggerEdgeFor(TriggerEdge::RenderTargetToShaderResource, static_cast<int>(t.StateBefore),
                              static_cast<int>(t.StateAfter)))
        {
            continue;
        }

        for (SurfaceIdentifier* optional : optionals_)
        {
            if (!optional->IsShortlisted(t.pResource))
            {
                continue;
            }

            if (candidateEdgeObserver_ != nullptr)
            {
                UINT64 width = 0;
                UINT height = 0;
                const D3D12_RESOURCE_DESC desc = t.pResource->GetDesc();
                if (velocity_->Extent(&width, &height) && desc.Width == width && desc.Height == height)
                {
                    candidateEdgeObserver_(optional->Spec().name, cmdList, t.pResource, static_cast<int>(t.StateAfter));
                }
            }

            if (!diag_)
            {
                continue;
            }

            std::lock_guard<std::mutex> lock(batchMutex_);
            BatchStats& s = batchStats_[{optional, t.pResource}];
            ++s.edges;
            for (size_t e = 0; e < entries_.size() && e < kMaxBatchSurfaces; ++e)
            {
                s.withSurface[e] += (trackedIn[e] != nullptr && trackedIn[e] != t.pResource) ? 1 : 0;
            }

            s.otherRtToSrv += rtToSrv - 1;
            s.maxOtherRtToSrv = std::max<uint64_t>(s.maxOtherRtToSrv, rtToSrv - 1);
        }
    }
}

void SurfaceRegistry::DumpBatchStats(uint64_t frame) noexcept
{
    std::lock_guard<std::mutex> lock(batchMutex_);
    // The signal's input, over the resource's whole life.
    const auto anyDepth = [](const BarrierProfile& p, bool known) -> std::string
    {
        const int rt = static_cast<int>(TriggerEdge::RenderTargetToShaderResource);
        const int depth = static_cast<int>(TriggerEdge::DepthWriteToReadable);
        if (!known || p.triggerEdgeCountByEdge[rt] == 0)
        {
            return "anyDepth=-";
        }

        return "anyDepth=" +
               std::to_string(p.triggerEdgesSharingCallWith[rt][depth] * 100 / p.triggerEdgeCountByEdge[rt]) + "%";
    };
    for (SurfaceIdentifier* optional : optionals_)
    {
        const std::string prefix = std::string(optional->Spec().name) + "-diag:";
        Log(prefix + " frame " + std::to_string(frame) + ": for each " + optional->Spec().name +
            "-shortlisted resource, the share of its RT->SRV edges in a ResourceBarrier call with each other "
            "surface's tracked resources and with any depth edge (counted once velocity and depth are selected).");
        for (const auto& [key, s] : batchStats_)
        {
            if (key.first != optional)
            {
                continue;
            }

            ID3D12Resource* resource = key.second;
            BarrierProfile p;
            const bool known = profileStore_.Get(resource, &p);
            const auto pct = [&](uint64_t n) { return s.edges ? std::to_string(n * 100 / s.edges) + "%" : "-"; };
            std::string shares;
            std::string heldBy;
            for (size_t e = 0; e < entries_.size() && e < kMaxBatchSurfaces; ++e)
            {
                SurfaceIdentifier* id = entries_[e].identifier;
                if (id == optional)
                {
                    continue;
                }

                shares += std::string(id->Spec().name) + "=" + pct(s.withSurface[e]) + " ";
                if (resource == id->Selected() || id->IsCandidate(resource))
                {
                    heldBy += std::string(" [") + id->Spec().name + "'s]";
                }
            }

            char avg[32];
            snprintf(avg, sizeof(avg), "%.2f", s.edges ? static_cast<double>(s.otherRtToSrv) / s.edges : 0.0);
            Log(prefix + "   " + std::to_string(reinterpret_cast<uintptr_t>(resource)) +
                (known ? " " + std::to_string(p.width) + "x" + std::to_string(p.height) + " fmt=" +
                             std::to_string(p.format) + " flags=" + std::to_string(p.flags)
                       : std::string(" (no profile)")) +
                heldBy + " | rtToSrvEdges=" + std::to_string(s.edges) + " sameCallAs: " + shares + anyDepth(p, known) +
                " | avgOtherRtToSrvInCall=" + avg + " max=" + std::to_string(s.maxOtherRtToSrv));
        }
    }

    FlushLogs();
}

void SurfaceRegistry::NoteBackBufferSize(uint64_t width, uint32_t height) noexcept
{
    for (Entry& e : entries_)
    {
        if (e.extentSource != nullptr)
        {
            // A dependent reopens through its source's cascade and does not
            // track the back buffer itself.
            continue;
        }

        if (e.identifier->NoteBackBufferSize(width, height))
        {
            ReopenCascade(
                e.identifier->Spec().name, ReopenReason::BackBufferResized,
                "the back buffer changed size which abandons identified Scene Textures");
        }
    }
}

void SurfaceRegistry::NoteKnownSourceResource(ID3D12Resource* resource) noexcept
{
    if (!knownSources_.Note(resource))
    {
        return; // already known: the usual case, every Present
    }

    // Newly known: purge it from any shortlist that got it first.
    for (Entry& e : entries_)
    {
        if (e.identifier->ForgetKnownSource(resource))
        {
            // A selected back buffer is wrong by definition. Should not happen
            // while sources are noted every Present; if it does, search again.
            ReopenCascade(e.identifier->Spec().name, ReopenReason::DescriptorChanged,
                          "the selected resource is a swapchain back buffer (known by construction)");
        }
    }
}

void SurfaceRegistry::CollectCascade(Entry* root, std::vector<Entry*>* out) noexcept
{
    out->push_back(root);
    for (SurfaceIdentifier* dependent : root->dependents)
    {
        Entry* entry = FindEntry(dependent);
        if (entry != nullptr)
        {
            CollectCascade(entry, out);
        }
    }
}

void SurfaceRegistry::ReopenCascade(const char* rootName, ReopenReason reason, const char* detail) noexcept
{
    Entry* root = FindEntry(rootName);
    if (root == nullptr)
    {
        return;
    }

    std::vector<Entry*> cascade;
    CollectCascade(root, &cascade);

    // Once per cascade. Without the refill, a reopen after the budget ran out
    // could never find the replacement resource.
    ForgetAllResourcesSeen();

    for (Entry* e : cascade)
    {
        if (e == root)
        {
            e->identifier->Reopen(reason, detail);
        }
        else
        {
            const std::string dependentDetail =
                std::string("the ") + root->identifier->Spec().name + " search was reopened";
            e->identifier->Reopen(
                reason == ReopenReason::Requested ? ReopenReason::Requested : ReopenReason::DependencyReopened,
                dependentDetail.c_str());
        }

        // For every identifier in the cascade, root or dependent; the observer
        // decides whether a dependent's reopen matters (depth's, per burst).
        if (reopenObserver_ != nullptr)
        {
            reopenObserver_(e->identifier->Spec().name);
        }
    }
}

bool SurfaceRegistry::TryClaim(ID3D12Resource* resource, const SurfaceIdentifier* claimant) noexcept
{
    return HeldByOther(resource, claimant) == nullptr;
}

const char* SurfaceRegistry::HeldByOther(ID3D12Resource* resource, const SurfaceIdentifier* claimant) noexcept
{
    for (Entry& e : entries_)
    {
        if (e.identifier == claimant)
        {
            continue;
        }

        if (e.identifier->IsCandidate(resource))
        {
            return e.identifier->Spec().name;
        }
    }

    return nullptr;
}

bool SurfaceRegistry::IsAnyCandidate(ID3D12Resource* resource) const noexcept
{
    for (const Entry& e : entries_)
    {
        if (e.identifier->IsCandidate(resource))
        {
            return true;
        }
    }

    return false;
}

SurfaceRegistry& GetSurfaceRegistry() noexcept
{
    static SurfaceRegistry registry;
    return registry;
}

void SurfaceRegistry::DumpBarrierProfiles() noexcept
{
    LogTo(
        "barrier_profile",
        "# ptr width height format flags transitions framesSeen framesWithExactlyTwo alternationViolations "
        "rtToSrvEdges depthWriteToReadableEdges reverseRtToSrvEdges uavBarriers");
    profileStore_.ForEach(
        [](ID3D12Resource* resource, const BarrierProfile& p)
        {
            const int rt = static_cast<int>(TriggerEdge::RenderTargetToShaderResource);
            const int depth = static_cast<int>(TriggerEdge::DepthWriteToReadable);
            LogTo(
                "barrier_profile",
                std::to_string(reinterpret_cast<uintptr_t>(resource)) + " " + std::to_string(p.width) + " " +
                    std::to_string(p.height) + " " + std::to_string(p.format) + " " + std::to_string(p.flags) + " " +
                    std::to_string(p.transitions) + " " + std::to_string(p.framesSeen) + " " +
                    std::to_string(p.framesWithExactlyTwo) + " " + std::to_string(p.alternationViolations) + " " +
                    std::to_string(p.triggerEdgeCountByEdge[rt]) + " " + std::to_string(p.triggerEdgeCountByEdge[depth]) +
                    " " + std::to_string(p.reverseEdgeCountByEdge[rt]) + " " + std::to_string(p.uavBarriers));
        });
    Log("barrier survey: dumped " + std::to_string(profileStore_.Size()) + " resource profiles to mv_barrier_profile.log");
    // `LogTo` batches lines, and the survey is written once, so flush it now
    // rather than at exit.
    FlushLogs();
}

} // namespace mv
