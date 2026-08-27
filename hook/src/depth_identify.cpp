#include "depth_identify.h"

#include "velocity_identify.h"

#include <mutex>

namespace mv
{

namespace
{

// GPixelFormats[PF_DepthStencil].PlatformFormat (D3D12RHI.cpp:117/127) is one
// of exactly two things, chosen by the r.D3D12.UseD24 cvar:
//
//     UseD24 != 0 : DXGI_FORMAT_R24G8_TYPELESS      (44)
//     UseD24 == 0 : DXGI_FORMAT_R32G8X24_TYPELESS   (19)
//
// Both are TYPELESS (UE puts a typed view on top) and MULTI-PLANE (depth in
// plane 0, stencil in plane 1) - capture.cpp already derives plane/subresource
// counts from the desc rather than assuming one.
//
// Fully-typed and single-plane variants are listed below but unused in
// practice - UE5 never creates scene depth that way - kept so a near-miss
// still scores instead of being flatly rejected.
constexpr FormatCandidate kDepthFormats[] = {
    {DXGI_FORMAT_R24G8_TYPELESS, 4, "R24G8_TYPELESS (PF_DepthStencil, r.D3D12.UseD24=1)"},
    {DXGI_FORMAT_R32G8X24_TYPELESS, 4, "R32G8X24_TYPELESS (PF_DepthStencil, r.D3D12.UseD24=0)"},
    {DXGI_FORMAT_D24_UNORM_S8_UINT, 2, "D24_UNORM_S8_UINT (typed - not how UE creates it)"},
    {DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 2, "D32_FLOAT_S8X24_UINT (typed - not how UE creates it)"},
    {DXGI_FORMAT_R32_TYPELESS, 1, "R32_TYPELESS (depth without stencil)"},
    {DXGI_FORMAT_D32_FLOAT, 1, "D32_FLOAT (depth without stencil, typed)"},
};

const SurfaceSpec kDepthSpec = []
{
    SurfaceSpec spec{};
    spec.name = "depth";
    spec.logPrefix = "depth";

    spec.formatCount = 0;
    for (const FormatCandidate& f : kDepthFormats)
    {
        spec.formats[spec.formatCount++] = f;
    }
    // TexCreate_DepthStencilTargetable | TexCreate_ShaderResource maps to
    // ALLOW_DEPTH_STENCIL with no DENY_SHADER_RESOURCE; requiring the deny
    // bit absent separates scene depth from shadow-map atlases
    // (TargetableOnly).
    spec.requiredFlags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    spec.forbiddenFlags = D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    spec.minWidth = 0;  // no size floor of its own - the extent test below is exact
    spec.minHeight = 0;
    spec.maxBackBufferMultiple = 0; // unbounded - bounded by the exact extent test instead
    spec.extentFrom = "velocity";   // functional half: SetExtentSource, wired in EnsureExtentSourceWired below

    spec.trigger = TriggerEdge::DepthWriteToReadable;
    // How long to watch before deciding, measured from when velocity
    // produces an extent (depth identification can't start before that) -
    // see SurfaceIdentifier::OnFrame's extent-dependency path. This value is
    // now unused (that path decides the frame the dependency resolves, not
    // settleFrames after), kept only as documentation of the pre-extraction
    // constant it replaces.
    spec.minFramesObserved = 30;
    spec.settleFrames = 60;
    // depth's score IS the raw format score, always - gate of 0 means every
    // structurally-accepted format counts in full, with no bonus/checkbox
    // breakdown (see CollectSignals - depth's pre-extraction log never had
    // a signals section).
    spec.formatScoreGate = 0;
    spec.signalCount = 0; // no behavioural signals beyond the format baseline

    // A clear margin: 1.5x as many produce-then-consume edges per frame is
    // the difference between a depth buffer the frame is built around and
    // one that's occasionally rendered (e.g. CustomDepth).
    spec.ambiguityRule = AmbiguityRule::EdgeCountRatio;
    spec.ambiguityMargin = 150;

    spec.rivalStallFrames = 0;
    spec.rivalFramesBeforeAdopt = 0;
    spec.rivalMaxAdoptions = 0;
    spec.rivalMaxReports = 0;

    spec.usesSharedCandidateSet = false; // depth keeps its own local accepted-desc; see Validate()
    spec.reopenClearsShortlist = false;  // reopen keeps the shortlist, only resets counters
    return spec;
}();

SurfaceIdentifier g_depth(kDepthSpec);

std::once_flag g_extentSourceWired;

void EnsureExtentSourceWired()
{
    // Deferred rather than done at static-init time: global initialisation
    // order across translation units (this file's g_depth vs velocity_
    // identify.cpp's g_velocity) is unspecified, so this cannot safely run
    // as part of either global's construction.
    std::call_once(g_extentSourceWired, [] { g_depth.SetExtentSource(&VelocityIdentifier()); });
}

} // namespace

void NoteResourceDescForDepth(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc)
{
    g_depth.OnResourceDesc(resource, desc);
}

void NoteDepthTransition(ID3D12Resource* resource, uint64_t frame, int stateBefore, int stateAfter)
{
    g_depth.OnTransition(resource, frame, stateBefore, stateAfter);
}

void OnFrameForDepthIdentification(uint64_t frame)
{
    EnsureExtentSourceWired();
    g_depth.OnFrame(frame);
}

void ReopenDepthIdentification(const char* reason)
{
    g_depth.Reopen(ReopenReason::DependencyReopened, reason);
}

ID3D12Resource* IdentifiedDepthResource()
{
    return g_depth.Selected();
}

bool IsDepthCandidate(ID3D12Resource* resource)
{
    return resource != nullptr && resource == g_depth.Selected();
}

bool ValidateDepthCandidate(ID3D12Resource* resource)
{
    return g_depth.Validate(resource);
}

} // namespace mv
