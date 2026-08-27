#include "surface_identify.h"

#include "logging.h"
#include "resource_tracking.h"

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

} // namespace

SurfaceIdentifier::SurfaceIdentifier(const SurfaceSpec& spec) noexcept : spec_(spec)
{
}

void SurfaceIdentifier::SetExtentSource(const SurfaceIdentifier* source) noexcept
{
    extentSource_ = source;
}

bool SurfaceIdentifier::IsTriggerEdge(int stateBefore, int stateAfter) const noexcept
{
    switch (spec_.trigger)
    {
        case TriggerEdge::RenderTargetToShaderResource:
            return stateBefore == D3D12_RESOURCE_STATE_RENDER_TARGET && IsShaderResourceState(stateAfter);
        case TriggerEdge::DepthWriteToReadable:
            return stateBefore == D3D12_RESOURCE_STATE_DEPTH_WRITE && IsDepthReadableState(stateAfter);
    }
    return false;
}

bool SurfaceIdentifier::IsReverseTriggerEdge(int stateBefore, int stateAfter) const noexcept
{
    switch (spec_.trigger)
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

bool SurfaceIdentifier::StructurallyPlausible(const D3D12_RESOURCE_DESC& desc, int* formatScore, const char** rejection)
    const noexcept
{
    // Format tested first so *formatScore is set even on rejection - lets the
    // caller report near-misses (e.g. "velocity-shaped format, missing UAV flag").
    *formatScore = 0;
    *rejection = "";
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

bool SurfaceIdentifier::StructurallyPlausibleForRival(const D3D12_RESOURCE_DESC& desc) const noexcept
{
    int formatScore = 0;
    const char* rejection = "";
    return StructurallyPlausible(desc, &formatScore, &rejection);
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

std::string SurfaceIdentifier::ProfileLine(const BarrierProfile& p)
{
    return "framesSeen=" + std::to_string(p.framesSeen) + " exactly2=" + std::to_string(p.framesWithExactlyTwo) +
           " even=" + std::to_string(p.framesWithEvenCount) + " trigger=" + std::to_string(p.triggerEdgeCount) +
           " reverse=" + std::to_string(p.reverseEdgeCount) + " otherPairs=" + std::to_string(p.otherPairs) +
           " violations=" + std::to_string(p.alternationViolations) + " uavBarriers=" + std::to_string(p.uavBarriers) +
           " srvState=" + std::to_string(p.shaderResourceState);
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

void SurfaceIdentifier::OnResourceDesc(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc) noexcept
{
    // Every resource gets a behaviour profile, whatever its descriptor - that
    // is what makes the survey a survey rather than a confirmation.
    constexpr size_t kMaxProfiledResources = 20000;
    {
        std::lock_guard<std::mutex> lock(profileMutex_);
        if (profiles_.size() < kMaxProfiledResources && profiles_.find(resource) == profiles_.end())
        {
            BarrierProfile p;
            p.width = desc.Width;
            p.height = desc.Height;
            p.format = static_cast<int>(desc.Format);
            p.flags = static_cast<unsigned>(desc.Flags);
            profiles_.emplace(resource, p);
        }
    }

    // The resolution histogram is over render-targetable 2D textures only, no buffers or staging textures.
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        (static_cast<UINT>(desc.Flags) & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0)
    {
        std::lock_guard<std::mutex> lock(resolutionMutex_);
        ++resolutionHistogram_[{desc.Width, desc.Height}];
    }

    int formatScore = 0;
    const char* rejection = "";
    if (!StructurallyPlausible(desc, &formatScore, &rejection))
    {
        if (formatScore > 0)
        {
            // A shape near-miss - worth a line each, since these are what to
            // check first if the search finds nothing.
            Log(std::string(spec_.logPrefix) + ": near-miss " + DescLine(resource, desc) + " - rejected: " + rejection);
        }
        return;
    }

    constexpr size_t kMaxShortlisted = 64;
    std::lock_guard<std::mutex> lock(shortlistMutex_);
    if (shortlist_.size() >= kMaxShortlisted)
    {
        return;
    }

    shortlist_.emplace_back(resource); // ComPtr ctor AddRefs
    Log(std::string(spec_.logPrefix) + ": shortlisted (structure) " + DescLine(resource, desc) + " - now watching its barriers");
}

bool SurfaceIdentifier::OnTransition(ID3D12Resource* resource, uint64_t frame, int stateBefore, int stateAfter) noexcept
{
    // Liveness of the selected resource (and any rival-adopted alternate),
    // tracked outside the profile map since the map clears on reopen and
    // this must keep working across that. Only the trigger edge counts.
    const bool isTracked = resource == identified_.load(std::memory_order_acquire) || IsAlsoTracked(resource);
    const bool isTrigger = IsTriggerEdge(stateBefore, stateAfter);
    if (isTracked && isTrigger)
    {
        lastEdgeFrame_.store(frame, std::memory_order_relaxed);
        if (silenceReported_.exchange(false))
        {
            Log(std::string(spec_.logPrefix) + ": the pass has RESUMED on frame " + std::to_string(frame) +
                " - on one of the tracked targets, so nothing was reidentified.");
        }
    }

    std::lock_guard<std::mutex> lock(profileMutex_);
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

    if (isTrigger)
    {
        ++p.triggerEdgeCount;
        p.shaderResourceState = stateAfter;
    }
    else if (IsReverseTriggerEdge(stateBefore, stateAfter))
    {
        ++p.reverseEdgeCount;
        p.shaderResourceState = stateBefore;
    }
    else
    {
        ++p.otherPairs;
    }

    // State continuity: whatever a resource transitioned *to* last time must
    // be what it transitions *from* next time, or someone else moved it.
    if (p.lastAfter >= 0 && stateBefore != p.lastAfter)
    {
        ++p.alternationViolations;
    }
    p.lastAfter = stateAfter;

    return isTracked && isTrigger;
}

void SurfaceIdentifier::OnUavBarrier(ID3D12Resource* resource) noexcept
{
    std::lock_guard<std::mutex> lock(profileMutex_);
    const auto it = profiles_.find(resource);
    if (it != profiles_.end())
    {
        ++it->second.uavBarriers;
    }
}

int SurfaceIdentifier::CollectSignals(
    const D3D12_RESOURCE_DESC& desc,
    const BarrierProfile& p,
    int formatScore,
    const std::pair<UINT64, UINT>& modal,
    std::string* signalsLine) const noexcept
{
    const double perFrame = p.framesSeen > 0 ? static_cast<double>(p.triggerEdgeCount) / p.framesSeen : 0.0;

    struct Evaluated
    {
        const char* name;
        int weight;
        bool present;
    };
    Evaluated evaluated[8]{};
    int n = 0;

    for (uint8_t i = 0; i < spec_.signalCount; ++i)
    {
        const SignalWeight& sw = spec_.signals[i];
        const char* name = "";
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

    std::vector<Survivor> survivors;
    for (const ComPtr<ID3D12Resource>& held : shortlist)
    {
        ID3D12Resource* resource = held.Get();
        BarrierProfile profile;
        {
            std::lock_guard<std::mutex> lock(profileMutex_);
            const auto it = profiles_.find(resource);
            if (it == profiles_.end())
            {
                continue;
            }
            profile = it->second;
        }

        // Behaviour gate: a necessary condition, deliberately weak - "it gets
        // produced and then consumed, most frames". Anything stronger has
        // already been falsified once (see CollectSignals).
        if (profile.framesSeen < spec_.minFramesObserved || profile.triggerEdgeCount * 2 < profile.framesSeen)
        {
            continue;
        }

        D3D12_RESOURCE_DESC desc = resource->GetDesc(); // safe: the shortlist holds a reference
        int formatScore = 0;
        const char* rejection = "";
        if (!StructurallyPlausible(desc, &formatScore, &rejection))
        {
            // desc changed since shortlisting
            continue;
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
        s.score = CollectSignals(s.desc, profile, formatScore, modal, &s.signalsLine);
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
            std::lock_guard<std::mutex> lock(profileMutex_);
            const auto it = profiles_.find(held.Get());
            if (it != profiles_.end())
            {
                Log(std::string(spec_.logPrefix) + ":   shortlisted " + DescLine(held.Get(), held->GetDesc()) + " | " +
                    ProfileLine(it->second));
                Log(std::string(spec_.logPrefix) + ":     observed transitions:" + PairsLine(it->second));
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
        // each attempt is a clean window.
        {
            std::lock_guard<std::mutex> lock(profileMutex_);
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
            if (a.profile.triggerEdgeCount != b.profile.triggerEdgeCount)
            {
                return a.profile.triggerEdgeCount > b.profile.triggerEdgeCount;
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
                        survivors[0].profile.triggerEdgeCount * 100 >
                            survivors[1].profile.triggerEdgeCount * static_cast<uint64_t>(spec_.ambiguityMargin);
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
                std::to_string(survivors[0].profile.triggerEdgeCount) + " trigger edges against " +
                std::to_string(survivors[1].profile.triggerEdgeCount) +
                " for the runner-up over the same window. This is a ranked choice among structurally "
                "indistinguishable candidates, not a unique match.");
        }
    }

    const Survivor& winner = survivors.front();
    if (spec_.usesSharedCandidateSet)
    {
        MarkAsCandidate(winner.resource, winner.desc);
    }
    {
        std::lock_guard<std::mutex> lock(acceptedDescMutex_);
        acceptedDesc_ = winner.desc;
    }
    identifiedWidth_.store(winner.desc.Width, std::memory_order_relaxed);
    identifiedHeight_.store(winner.desc.Height, std::memory_order_relaxed);
    identifiedFormat_.store(static_cast<int>(winner.desc.Format), std::memory_order_relaxed);
    // Start the liveness clock from the decision, not from zero - otherwise a
    // decision reached at frame 90 is already "silent" the moment it is made.
    lastEdgeFrame_.store(frame, std::memory_order_relaxed);
    silenceReported_.store(false, std::memory_order_relaxed);
    identified_.store(winner.resource, std::memory_order_release);
    decisionMade_.store(true, std::memory_order_relaxed);
    OnSelected(winner.resource, winner.desc);
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

bool SurfaceIdentifier::Validate(ID3D12Resource* resource) const noexcept
{
    if (resource == nullptr || resource != identified_.load(std::memory_order_acquire))
    {
        return false;
    }
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    std::lock_guard<std::mutex> lock(acceptedDescMutex_);
    const D3D12_RESOURCE_DESC& accepted = acceptedDesc_;
    return desc.Width == accepted.Width && desc.Height == accepted.Height && desc.Format == accepted.Format &&
           desc.MipLevels == accepted.MipLevels && desc.DepthOrArraySize == accepted.DepthOrArraySize &&
           desc.SampleDesc.Count == accepted.SampleDesc.Count && desc.Layout == accepted.Layout;
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
        {
            std::lock_guard<std::mutex> lock(profileMutex_);
            profiles_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(resolutionMutex_);
            resolutionHistogram_.clear();
        }
        // Without this the whole search is a no-op second time round: the
        // "seen" set exists to make sure GetDesc() is called once per
        // resource, so every resource in the process is already marked and
        // OnResourceDesc would never be reached again.
        ForgetAllResourcesSeen();
    }
    else
    {
        // Keep the shortlist and profile population; only reset the
        // measurement window. Clearing them was tried and reverted for depth
        // (see the pre-extraction ReopenDepthIdentification): the shortlist
        // is refilled only from OnResourceDesc, which the barrier hook
        // reaches only for resources not yet marked "seen" - a set this
        // branch deliberately does NOT clear, since re-feeding every live
        // resource to a DIFFERENT surface's search (one sharing the same
        // "seen" set) could poison that search with duplicate structurally-
        // identical entries. A cleared shortlist here could only ever be
        // refilled by resources the engine happens to allocate AFTER the
        // reopen - intermittent, worse than reproducible.
        std::lock_guard<std::mutex> lock(profileMutex_);
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

    // Clearing identified_ above is NOT what stops capture for a surface
    // using the shared candidate set - the barrier hook gates on
    // IsCandidate(), a separate set this function used to leave alone. See
    // the long comment this preserved, originally on velocity's
    // ReopenIdentification, for why forgetting it here is required. A
    // surface with its own local candidate model (depth) doesn't touch that
    // shared set at all - its own gate (Validate(), above) already re-reads
    // the live descriptor every time, so there is nothing stale to forget.
    if (spec_.usesSharedCandidateSet)
    {
        ForgetAllCandidates();
    }
    OnReopen();
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

void SurfaceIdentifier::DumpBarrierProfiles() noexcept
{
    if (profileDumped_.exchange(true))
    {
        return;
    }
    std::lock_guard<std::mutex> lock(profileMutex_);
    LogTo(
        "barrier_profile",
        "# ptr width height format flags transitions framesSeen framesWithExactlyTwo "
        "alternationViolations triggerEdgeCount reverseEdgeCount otherPairs uavBarriers");
    for (const auto& [resource, p] : profiles_)
    {
        LogTo(
            "barrier_profile",
            std::to_string(reinterpret_cast<uintptr_t>(resource)) + " " + std::to_string(p.width) + " " +
                std::to_string(p.height) + " " + std::to_string(p.format) + " " + std::to_string(p.flags) + " " +
                std::to_string(p.transitions) + " " + std::to_string(p.framesSeen) + " " +
                std::to_string(p.framesWithExactlyTwo) + " " + std::to_string(p.alternationViolations) + " " +
                std::to_string(p.triggerEdgeCount) + " " + std::to_string(p.reverseEdgeCount) + " " +
                std::to_string(p.otherPairs) + " " + std::to_string(p.uavBarriers));
    }
    Log("barrier survey: dumped " + std::to_string(profiles_.size()) + " resource profiles to mv_barrier_profile.log");
    // LogTo batches; the survey is written once and never again, so without
    // an explicit flush the tail of it sits in memory until the process exits.
    FlushLogs();
}

} // namespace mv
