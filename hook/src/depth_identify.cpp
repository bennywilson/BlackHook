#include "depth_identify.h"

#include "surface_registry.h"

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
    spec.extentFrom = "velocity";   // resolved and wired by SurfaceRegistry::Configure()

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

    spec.reopenClearsShortlist = false; // reopen keeps the shortlist, only resets counters
    return spec;
}();

SurfaceIdentifier g_depth(kDepthSpec);

} // namespace

void RegisterDepthIdentifier(SurfaceRegistry& registry)
{
    registry.Register(&g_depth);
}

ID3D12Resource* IdentifiedDepthResource()
{
    return g_depth.Selected();
}

} // namespace mv
