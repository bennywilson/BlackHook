#include "velocity_identify.h"

#include "logging.h"

#include <windows.h>

#include <atomic>
#include <string>

namespace mv
{

namespace
{

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

} // namespace

std::unique_ptr<SurfaceIdentifier> MakeVelocityIdentifier(const SurfaceSpec& spec)
{
    return std::make_unique<VelocitySurfaceIdentifier>(spec);
}

} // namespace mv
