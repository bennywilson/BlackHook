#include "surface_registry.h"

#include "logging.h"
#include "resource_tracking.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace mv
{

namespace
{

// The survey dump (see README, "identifying by behaviour") stays on its
// original schedule: late enough that every resource has had thousands of
// frames to show its pattern. Previously velocity_identify.cpp's
// kProfileDumpFrame; moved here because the survey is now of the one shared
// store, not of one identifier's own map.
constexpr uint64_t kProfileDumpFrame = 2000;

} // namespace

SurfaceRegistry::SurfaceRegistry() noexcept
{
    entries_.reserve(8); // kMaxSurfaces, capture.cpp's own constant - avoid any reallocation after Configure()
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
    velocity_ = Find("velocity");
    depth_ = Find("depth");
    normals_ = Find("normals");
    for (const Entry& e : entries_)
    {
        const SurfaceSpec& spec = e.identifier->Spec();
        for (uint8_t i = 0; i < spec.signalCount; ++i)
        {
            sharedCallSignals_ = sharedCallSignals_ || spec.signals[i].id == SignalId::SharesCallWithEdge;
        }
    }
    // Second pass, after every identifier has a stable address in
    // entries_: resolve each spec.extentFrom name to the identifier it
    // names, wire the functional half (SetExtentSource, same as Phase 7's
    // manual wiring), and record the reverse edge for the reopen cascade
    // (docs/REFACTOR_PLAN.md finding 1.9).
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

void SurfaceRegistry::ConsiderResource(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) noexcept
{
    profileStore_.EnsureProfiled(resource, desc);
    for (Entry& e : entries_)
    {
        e.identifier->ConsiderResource(resource, desc);
    }
}

void SurfaceRegistry::OnUavBarrier(ID3D12Resource* resource) noexcept
{
    profileStore_.RecordUav(resource);
}

int SurfaceRegistry::OnBarrierTransition(
    ID3D12Resource* resource, uint64_t frame, int stateBefore, int stateAfter, SurfaceTrigger* out,
    int outCapacity) noexcept
{
    const bool profiled = profileStore_.RecordTransition(resource, frame, stateBefore, stateAfter);

    int count = 0;
    for (Entry& e : entries_)
    {
        SurfaceIdentifier* id = e.identifier;
        // Liveness tracking (lastEdgeFrame_/silence reporting) - side effect
        // only; the capture decision below mirrors the pre-Phase-8 dispatch,
        // which never used this return value either (NoteTransition/
        // NoteDepthTransition were called in void context at the one call
        // site that existed).
        id->OnTransition(resource, frame, stateBefore, stateAfter, profiled);

        const bool isThisEdge = IsTriggerEdgeFor(id->Spec().trigger, stateBefore, stateAfter);
        if (id->IsCandidate(resource))
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
    // Registration order is velocity-then-depth (InstallD3D12Hooks), so this
    // naturally runs a dependency's OnFrame after its source's, same as the
    // pre-Phase-8 hand-ordered calls in d3d12_hook.cpp - not load-bearing
    // (OnFrame re-reads the live source each call, so a stale-by-one-Present
    // read is harmless), but worth keeping true rather than relying on it.
    for (Entry& e : entries_)
    {
        e.identifier->OnFrame(frame);
    }

    if (frame == kProfileDumpFrame && !profileDumped_.exchange(true))
    {
        DumpBarrierProfiles();
    }
    if (normals_ != nullptr && (frame == 600 || frame == kProfileDumpFrame))
    {
        DumpBatchStats(frame);
    }
}

void SurfaceRegistry::OnBarrierBatch(const D3D12_RESOURCE_BARRIER* barriers, UINT count) noexcept
{
    if (barriers == nullptr)
    {
        return;
    }

    // SignalId::SharesCallWithEdge's input, for every profiled resource: which
    // of its trigger edges shared this call with another resource's trigger
    // edge, by type. The two trigger types are disjoint (their before-states
    // differ), so a transition is at most one of them.
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

    if (normals_ == nullptr || velocity_ == nullptr || depth_ == nullptr)
    {
        return;
    }
    // Only once both anchors are known: before that "not in the same call as
    // velocity's edge" would just mean velocity was not selected yet.
    ID3D12Resource* const velocity = velocity_->Selected();
    ID3D12Resource* const depth = depth_->Selected();
    if (velocity == nullptr || depth == nullptr)
    {
        return;
    }

    bool velocityEdge = false;
    bool depthEdge = false;
    uint64_t rtToSrv = 0;
    for (UINT i = 0; i < count; ++i)
    {
        if (barriers[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
        {
            continue;
        }
        const D3D12_RESOURCE_TRANSITION_BARRIER& t = barriers[i].Transition;
        const int before = static_cast<int>(t.StateBefore);
        const int after = static_cast<int>(t.StateAfter);
        if (IsTriggerEdgeFor(TriggerEdge::RenderTargetToShaderResource, before, after))
        {
            ++rtToSrv;
            velocityEdge = velocityEdge || t.pResource == velocity;
        }
        if (t.pResource == depth && IsTriggerEdgeFor(TriggerEdge::DepthWriteToReadable, before, after))
        {
            depthEdge = true;
        }
    }
    if (rtToSrv == 0)
    {
        return; // the common case: nothing here a normals candidate could be part of
    }

    for (UINT i = 0; i < count; ++i)
    {
        if (barriers[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
        {
            continue;
        }
        const D3D12_RESOURCE_TRANSITION_BARRIER& t = barriers[i].Transition;
        if (!IsTriggerEdgeFor(TriggerEdge::RenderTargetToShaderResource, static_cast<int>(t.StateBefore),
                              static_cast<int>(t.StateAfter)) ||
            !normals_->IsShortlisted(t.pResource))
        {
            continue;
        }
        std::lock_guard<std::mutex> lock(batchMutex_);
        BatchStats& s = batchStats_[t.pResource];
        ++s.edges;
        s.withVelocityEdge += (velocityEdge && t.pResource != velocity) ? 1 : 0;
        s.withDepthEdge += depthEdge ? 1 : 0;
        s.otherRtToSrv += rtToSrv - 1;
        s.maxOtherRtToSrv = std::max<uint64_t>(s.maxOtherRtToSrv, rtToSrv - 1);
    }
}

void SurfaceRegistry::DumpBatchStats(uint64_t frame) noexcept
{
    std::lock_guard<std::mutex> lock(batchMutex_);
    Log("normals-diag: frame " + std::to_string(frame) + " - per normals-shortlisted resource, what shared its "
        "RT->SRV ResourceBarrier call (plan sec 3.2 #1; counted only once velocity and depth were both selected). "
        "Measured on Skyrunner: GBufferA shares one RT->SRV call a frame with scene depth; a same-format "
        "post-process target never does. sameCallAsAnyDepthEdge is what SignalId::SharesCallWithEdge scores.");
    // The signal's own input, over the resource's whole life, for comparison
    // with the selected-depth-only tally beside it.
    const auto anyDepth = [](const BarrierProfile& p, bool known) -> std::string
    {
        const int rt = static_cast<int>(TriggerEdge::RenderTargetToShaderResource);
        const int depth = static_cast<int>(TriggerEdge::DepthWriteToReadable);
        if (!known || p.triggerEdgeCountByEdge[rt] == 0)
        {
            return " sameCallAsAnyDepthEdge=-";
        }
        return " sameCallAsAnyDepthEdge=" +
               std::to_string(p.triggerEdgesSharingCallWith[rt][depth] * 100 / p.triggerEdgeCountByEdge[rt]) + "%";
    };
    for (const auto& [resource, s] : batchStats_)
    {
        BarrierProfile p;
        const bool known = profileStore_.Get(resource, &p);
        const auto pct = [&](uint64_t n) { return s.edges ? std::to_string(n * 100 / s.edges) + "%" : "-"; };
        char avg[32];
        snprintf(avg, sizeof(avg), "%.2f", s.edges ? static_cast<double>(s.otherRtToSrv) / s.edges : 0.0);
        Log("normals-diag:   " + std::to_string(reinterpret_cast<uintptr_t>(resource)) +
            (known ? " " + std::to_string(p.width) + "x" + std::to_string(p.height) + " fmt=" + std::to_string(p.format) +
                         " flags=" + std::to_string(p.flags)
                   : std::string(" (no profile)")) +
            (resource == velocity_->Selected() ? " [velocity's]" : "") + " | rtToSrvEdges=" + std::to_string(s.edges) +
            " sameCallAsVelocityEdge=" + pct(s.withVelocityEdge) + " sameCallAsDepthEdge=" +
            pct(s.withDepthEdge) + anyDepth(p, known) + " avgOtherRtToSrvInCall=" + avg +
            " maxOtherRtToSrvInCall=" + std::to_string(s.maxOtherRtToSrv));
    }
    FlushLogs();
}

void SurfaceRegistry::NoteBackBufferSize(uint64_t width, uint32_t height) noexcept
{
    for (Entry& e : entries_)
    {
        if (e.extentSource != nullptr)
        {
            // A dependent (depth) inherits reopening through the cascade
            // when its source resizes; it does not track the back buffer
            // itself.
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
        return; // already known - the per-Present common case
    }
    // Newly known: purge it from any search that shortlisted it first.
    for (Entry& e : entries_)
    {
        if (e.identifier->ForgetKnownSource(resource))
        {
            // Selected a back buffer: wrong by definition (sec 3.1 - it is
            // colour's, by API contract). Should be unreachable now that
            // sources are noted every Present, but if it happens, say so and
            // search again rather than keep capturing it as something else.
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

    // Once per cascade, not once per identifier in it (docs/REFACTOR_PLAN.md
    // sec 2.5 point 3, finding 1.7) - see the long comment this replaced on
    // SurfaceIdentifier::Reopen for why a depth-only reopen never refilling
    // this was a real, permanent loss of the ability to notice a
    // replacement resource once the identify budget was spent.
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
            e->identifier->Reopen(ReopenReason::DependencyReopened, dependentDetail.c_str());
        }
        // Fires for every identifier in the cascade, not just the root - a
        // dependent's own requirement (e.g. depth's, per capture.cpp's
        // frozen per-burst mask) is a separate question from the root's,
        // and only the observer's caller knows the answer. By construction:
        // this is the ONLY place ReopenCascade calls Reopen(), so nothing
        // that reopens can skip notifying, regardless of what triggered it.
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
    // LogTo batches; the survey is written once and never again, so without
    // an explicit flush the tail of it sits in memory until the process exits.
    FlushLogs();
}

} // namespace mv
