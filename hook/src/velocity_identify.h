#pragma once

#include "surface_identify.h" // IsShaderResourceState - canonical definition, docs/REFACTOR_PLAN.md sec 2.3

#include <d3d12.h>

#include <cstdint>

// Finding SceneVelocity on a title whose descriptor you do not already know.
//
// Two independent filters:
//
//   1. STRUCTURE - properties derived from the engine's own rules, not one
//      observed instance. UE5 picks the velocity format in
//      FVelocityRendering::GetFormat and its create flags in
//      GetCreateFlags; both are small closed sets (see the tables in
//      velocity_identify.cpp, each with its engine citation).
//
//   2. BEHAVIOUR - a lenient binary gate (produced as a render target and
//      read as a shader resource on at least half the observed frames; see
//      BehaviourGate). Anything stronger - the strict two-events-per-frame
//      RT<->SRV cycle, no UAV barriers, no continuity violations - is scored
//      evidence, not a requirement: it held on Oxi and failed on a second
//      title. See CollectSignals.
//
// Neither filter is applied until #2 has had a settling window to
// accumulate, so identification costs a couple of seconds at startup.
//
// When more than one resource survives, the top two scores decide: a margin
// of 2+ picks the top scorer. Ambiguous picks are treated as failure.

namespace mv
{

// Every resource the barrier hook walks past, once per resource. Records
// the structural shortlist and seeds the behaviour profile.
void NoteResourceDesc(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc);

// Every transition barrier, for the behaviour profile.
void NoteTransition(ID3D12Resource* resource, uint64_t frame, int stateBefore, int stateAfter);

// Every UAV barrier - a scored signal, not a hard rule. A resource taking
// UAV barriers is compute-written, which SceneVelocity mostly isn't, but
// this was falsified on a second title (see README, "the second title").
void NoteUavBarrier(ID3D12Resource* resource);

// Called once per presented frame. Runs the decision when the settling window
// has elapsed, and dumps the full behaviour survey when due.
void OnFrameForIdentification(uint64_t frame);

// The swapchain's back-buffer size, when known - velocity renders at or
// below it, one of the few resolution facts available without source.
//
// Returns true when the size CHANGED after a decision was already made. UE
// rebuilds scene textures at the new extent rather than resizing them, so
// the caller must reopen both searches - see the definition.
bool NoteBackBufferSize(uint64_t width, uint32_t height);

// Starts the search again from scratch, e.g. when a resolution change
// recreates the velocity texture and the confirmed candidate's descriptor
// changes.
void ReopenIdentification(const char* reason);

// True if the identified velocity buffer hasn't taken a RENDER_TARGET ->
// shader-resource edge in over 300 frames while the game keeps presenting.
bool VelocityPassIsSilent(uint64_t frame, uint64_t* framesSilent);

// The identified resource, or nullptr while the search is running or
// ambiguous. Exported so the test harness can check which resource won.
ID3D12Resource* IdentifiedVelocityResource();

// The extent of the identified velocity texture, or false if there isn't one.
// Depth identification keys off it: UE5 creates SceneDepth and
// SceneVelocity at the same SceneTexturesConfig::Extent.
bool IdentifiedVelocityExtent(uint64_t* width, uint32_t* height);

// Frames since the identified resource last took a RENDER_TARGET ->
// shader-resource edge. Unlike VelocityPassIsSilent this reports the raw
// number rather than gating on a 300-frame threshold - real stalls turned
// out to be shorter than that.
uint64_t FramesSinceVelocityEdge(uint64_t frame);

// Diagnostic, not a decision: while the identified resource is stalled,
// checks whether another texture of the same format has taken over its
// edge - i.e. the engine moved the velocity pass to a different target
// rather than stopped rendering it. Needed because resource tracking stops
// examining new resources once its identification budget is spent, so a
// later-allocated replacement is otherwise invisible; this looks at the
// barrier directly and only costs a GetDesc during a stall.
//
// Returns true once the resource has held the edge long enough to be
// adopted as a second velocity target (the target game switches between
// R16G16B16A16F and R16G16F) - caller should start treating it as
// capturable. Adoption is bounded internally; past the limit this returns
// false rather than adopt indefinitely.
bool NoteRivalVelocityEdge(ID3D12Resource* resource, uint64_t frame);

// The velocity identifier itself, for depth_identify.cpp to bind its extent
// dependency to (SurfaceIdentifier::SetExtentSource) - docs/REFACTOR_PLAN.md
// Phase 7. Not for general use; every other need is served by the functions
// above.
const SurfaceIdentifier& VelocityIdentifier();

} // namespace mv
