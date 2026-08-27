#pragma once

#include "surface_identify.h" // IsDepthReadableState - canonical definition, docs/REFACTOR_PLAN.md sec 2.3

#include <d3d12.h>

#include <cstdint>

// Finding SceneDepth, by the same rules as SceneVelocity.
//
// UE5 creates scene depth in SceneTextures.cpp as PF_DepthStencil with
// TexCreate_DepthStencilTargetable | TexCreate_ShaderResource, at the same
// extent as velocity. On D3D12 that's a TYPELESS depth format with
// ALLOW_DEPTH_STENCIL - unique on both titles measured, and cheap to check
// since the extent is already known once velocity identification has run.
//
// Not an independent identification: it's linked to velocity's extent, so a
// mis-identified velocity buffer would mis-identify depth the same way.
//
// As of docs/REFACTOR_PLAN.md Phase 7, depth is a pure SurfaceIdentifier
// (see surface_identify.h) configured by data - this header's declarations
// are unchanged so nothing outside depth_identify.cpp needed to change, but
// depth_identify.cpp itself is now a thin wrapper, not a second copy of the
// identification logic.

namespace mv
{

// Every resource the barrier hook walks past, once per resource.
void NoteResourceDescForDepth(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc);

// Every transition barrier, for the behaviour profile.
void NoteDepthTransition(ID3D12Resource* resource, uint64_t frame, int stateBefore, int stateAfter);

// Called once per presented frame, after velocity identification has run.
void OnFrameForDepthIdentification(uint64_t frame);

// Throws away the answer and starts again.
void ReopenDepthIdentification(const char* reason);

// The identified resource, or nullptr while the search is running,
// ambiguous, or waiting for velocity. Exported so the harness can read it.
ID3D12Resource* IdentifiedDepthResource();

// Fast path for the barrier hook: is this the resource we selected?
bool IsDepthCandidate(ID3D12Resource* resource);

// Confirms the selected resource still has the descriptor it was accepted
// with, before recording a barrier/copy sized from its footprints.
bool ValidateDepthCandidate(ID3D12Resource* resource);

} // namespace mv
