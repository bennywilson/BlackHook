#include "velocity_identify.h"

#include "logging.h"
#include "surface_identify.h"
#include "surface_registry.h"

#include <windows.h>

#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <string>

using Microsoft::WRL::ComPtr;

namespace mv
{

namespace
{

// FVelocityRendering::GetFormat() returns PF_A16B16G16R16 and PF_R16G16_UINT
// on Windows which map to D3D12's DXGI_FORMAT_R16G16_UNORM, and
// DXGI_FORMAT_R16G16B16A16_UNORM. Android GLES-only in the engine, so the
// UINT variants should never appear on a D3D12 title - kept as low-score
// fallbacks rather than excluded, so a near-miss still shows up in the log
// with its actual score instead of a flat rejection.
constexpr FormatCandidate kVelocityFormats[] = {
    {DXGI_FORMAT_R16G16B16A16_UNORM, 4, "R16G16B16A16_UNORM (PF_A16B16G16R16, NeedVelocityDepth)"},
    {DXGI_FORMAT_R16G16_UNORM, 4, "R16G16_UNORM (PF_G16R16, no velocity depth)"},
    {DXGI_FORMAT_R16G16B16A16_UINT, 1, "R16G16B16A16_UINT (PF_R16G16B16A16_UINT, Android GLES path)"},
    {DXGI_FORMAT_R16G16_UINT, 1, "R16G16_UINT (PF_R16G16_UINT, Android GLES path)"},
};

const SurfaceSpec kVelocitySpec = []
{
    SurfaceSpec spec{};
    spec.name = "velocity";
    spec.logPrefix = "identify";

    spec.formatCount = 0;
    for (const FormatCandidate& f : kVelocityFormats)
    {
        spec.formats[spec.formatCount++] = f;
    }
    // FVelocityRendering::GetCreateFlags: TexCreate_RenderTargetable | UAV |
    // ShaderResource maps to ALLOW_RENDER_TARGET (0x01) | ALLOW_UNORDERED_ACCESS
    // (0x04). The filter only requires those two bits set; an exact Flags == 5
    // match scores higher than a superset with other flags (see the
    // ExactRequiredFlags signal below).
    spec.requiredFlags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    spec.forbiddenFlags = 0;
    // Anything smaller than this is a downsampled/tile target. Deliberately
    // generous - excludes 1/16-res things like MotionBlur.VelocityFlatten,
    // doesn't try to guess the render resolution.
    spec.minWidth = 320;
    spec.minHeight = 180;
    spec.maxBackBufferMultiple = 2;
    spec.extentFrom = nullptr; // velocity is the extent source, not a dependent

    spec.trigger = TriggerEdge::RenderTargetToShaderResource;
    // Behaviour: how long to watch before deciding. The barrier signature is
    // a per-frame invariant, not an average - 90 frames to be sure it's
    // genuine.
    spec.minFramesObserved = 30;
    spec.settleFrames = 90;
    // Format-tier bonus counts only at the desktop-tier score (4) - see
    // SurfaceIdentifier::CollectSignals. GLES-fallback formats (score 1)
    // pass the structural gate but earn nothing from this baseline.
    spec.formatScoreGate = 4;

    spec.signalCount = 0;
    spec.signals[spec.signalCount++] = {SignalId::AtModalResolution, 3};
    spec.signals[spec.signalCount++] = {SignalId::ExactRequiredFlags, 2};
    spec.signals[spec.signalCount++] = {SignalId::ProducedOnceExactly, 2};
    spec.signals[spec.signalCount++] = {SignalId::NeverTakesUavBarrier, 2};
    spec.signals[spec.signalCount++] = {SignalId::NeverEntersUnorderedAccess, 2};
    spec.signals[spec.signalCount++] = {SignalId::StrictTwoEventsPerFrame, 3};
    spec.signals[spec.signalCount++] = {SignalId::NoContinuityViolations, 1};

    spec.ambiguityRule = AmbiguityRule::ScoreMargin;
    spec.ambiguityMargin = 2;

    // Behaviour, tracked outside the profile map: how long to watch a stall
    // before checking for a rival, and how many consecutive frames a rival
    // must carry the edge before being adopted as a second target.
    spec.rivalStallFrames = 3;
    spec.rivalFramesBeforeAdopt = 8;
    // Sized for a single one-off reallocation, the only pattern seen before a
    // slow-motion sequence reallocated SceneVelocity 6 times in ~8 seconds and
    // exhausted this budget seconds before the pass was reported STOPPED - at
    // which point a further reallocation would have looked like silence
    // rather than being followed. Raised to give sustained churn real
    // headroom instead of a limit sized for a rare one-time event.
    spec.rivalMaxAdoptions = 64;
    // Output cap for the rival-edge diagnostic - stalls recur every few
    // seconds and would otherwise log forever; a few dozen lines is enough to
    // tell whether a rival exists.
    spec.rivalMaxReports = 40;

    spec.reopenClearsShortlist = true; // reopen re-scans everything
    return spec;
}();

// Rival adoption: a run-length state machine over consecutive frames, kept
// as a subclass per docs/REFACTOR_PLAN.md sec 2.4 because no arrangement of
// scalars in a profile expresses "this same resource has now carried the
// edge on N consecutive frames while the selected one carried none".
class VelocitySurfaceIdentifier : public SurfaceIdentifier
{
public:
    explicit VelocitySurfaceIdentifier(const SurfaceSpec& spec) : SurfaceIdentifier(spec)
    {
    }

    bool OnUnmatchedEdge(ID3D12Resource* resource, uint64_t frame) noexcept override
    {
        // Cheapest checks first: this runs from the barrier hook, on every
        // RENDER_TARGET -> shader-resource edge in the frame.
        ID3D12Resource* selected = SelectedRaw();
        if (selected == nullptr || resource == selected || resource == nullptr ||
            resource == identifiedAlt_.load(std::memory_order_acquire))
        {
            return false;
        }
        if (FramesSinceEdge(frame) < Spec().rivalStallFrames)
        {
            return false;
        }

        const D3D12_RESOURCE_DESC desc = resource->GetDesc();

        // Judged by the SAME structural filter identification uses.
        if (!StructurallyPlausibleForRival(resource, desc))
        {
            return false;
        }

        // Same render extent as the established answer - both widths of
        // SceneVelocity share SceneTexturesConfig::Extent, so a
        // velocity-shaped texture at a different extent is some other
        // buffer.
        if (desc.Width != IdentifiedWidth() || desc.Height != IdentifiedHeight())
        {
            return false;
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

        if (run >= Spec().rivalFramesBeforeAdopt)
        {
            rivalRunLength_.store(0, std::memory_order_relaxed);
            if (adoptions_.fetch_add(1, std::memory_order_relaxed) >= Spec().rivalMaxAdoptions)
            {
                if (!adoptLimitReported_.exchange(true))
                {
                    Log("identify: a further velocity target would have been adopted, but the limit of " +
                        std::to_string(Spec().rivalMaxAdoptions) +
                        " is spent - NOT adopting. Adopting without bound cannot be told apart from adopting "
                        "the wrong resources, so this stops and says so instead. Restart to reset it.");
                }
                return false;
            }

            identifiedAlt_.store(resource, std::memory_order_release);
            Log("identify: ADOPTED a second velocity target - " + DescLine(resource, desc) +
                " has taken the "
                "RENDER_TARGET -> shader-resource edge on " +
                std::to_string(run) + " consecutive frames while the selected one (fmt=" +
                std::to_string(IdentifiedFormat()) + ") took none. Same extent, same structural filter" +
                (static_cast<int>(desc.Format) == IdentifiedFormat()
                     ? std::string(", same format: a reallocation to a new address with an identical "
                                    "descriptor")
                     : std::string(", different format: possibly FVelocityRendering::GetFormat's other "
                                    "answer, unconfirmed - every prior adoption has been same-format")) +
                ". BOTH are now captured from, whichever takes the edge - the pass moved, it did not stop.");
            return true;
        }

        if (rivalReports_.load(std::memory_order_relaxed) >= Spec().rivalMaxReports)
        {
            return false;
        }
        const int n = rivalReports_.fetch_add(1, std::memory_order_relaxed);
        if (n >= Spec().rivalMaxReports)
        {
            return false;
        }
        Log("identify: RIVAL velocity-format edge while the identified resource is stalled - ptr=" +
            std::to_string(reinterpret_cast<uintptr_t>(resource)) + " " + std::to_string(desc.Width) + "x" +
            std::to_string(desc.Height) + " fmt=" + std::to_string(static_cast<int>(desc.Format)) +
            " took RENDER_TARGET -> shader-resource on frame " + std::to_string(frame) + ", " +
            std::to_string(FramesSinceEdge(frame)) + " frames into the stall. The identified resource is " +
            std::to_string(IdentifiedWidth()) + "x" + std::to_string(IdentifiedHeight()) +
            ". If this repeats, the engine moved the velocity pass to another target rather than "
            "stopping it, and the search must reopen rather than wait.");
        if (n + 1 == Spec().rivalMaxReports)
        {
            Log("identify: further rival-edge reports suppressed (cap " + std::to_string(Spec().rivalMaxReports) +
                " per stall episode, re-armed when the pass resumes). Absence of more lines is the cap, not the "
                "absence of rivals.");
        }
        return false;
    }

protected:
    bool IsAlsoTracked(ID3D12Resource* resource) const noexcept override
    {
        return resource == identifiedAlt_.load(std::memory_order_acquire);
    }

    void OnSelected(ID3D12Resource* /*resource*/, const D3D12_RESOURCE_DESC& /*desc*/) noexcept override
    {
        rivalReports_.store(0, std::memory_order_relaxed);
    }

    void OnReopen() noexcept override
    {
        identifiedAlt_.store(nullptr, std::memory_order_release);
        rivalRunLength_.store(0, std::memory_order_relaxed);
        lastRivalResource_ = nullptr;
    }

private:
    // The SECOND velocity target, when the engine is using two. This was
    // originally written up as FVelocityRendering::GetFormat returning a
    // different width because NeedVelocityDepth() changed at runtime - that
    // diagnosis is WRONG (see DEBUGGING.md, "Correction: it works, and the
    // width story was wrong") and kept only as a note not to re-propose it.
    // Every adoption observed so far has been same width AND same format as
    // the selected resource: a plain reallocation to a new address with an
    // identical descriptor, not a format flip.
    std::atomic<ID3D12Resource*> identifiedAlt_{nullptr};

    std::atomic<int> rivalReports_{0};
    std::atomic<uint64_t> rivalRunLength_{0};
    std::atomic<uint64_t> lastRivalFrame_{0};
    ID3D12Resource* lastRivalResource_ = nullptr;
    std::atomic<int> adoptions_{0};
    std::atomic<bool> adoptLimitReported_{false};
};

VelocitySurfaceIdentifier g_velocity(kVelocitySpec);

} // namespace

void RegisterVelocityIdentifier(SurfaceRegistry& registry)
{
    registry.Register(&g_velocity);
}

bool VelocityPassIsSilent(uint64_t frame, uint64_t* framesSilent)
{
    return g_velocity.IsSilent(frame, framesSilent);
}

ID3D12Resource* IdentifiedVelocityResource()
{
    return g_velocity.Selected();
}

} // namespace mv
