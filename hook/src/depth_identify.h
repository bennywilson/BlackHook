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
// mis-identified velocity buffer would mis-identify depth the same way. As
// of docs/REFACTOR_PLAN.md Phase 8, that link (spec.extentFrom == "velocity")
// is resolved and wired by SurfaceRegistry::Configure(), not by this file
// reaching into velocity_identify.h directly - depth_identify.cpp no longer
// includes it at all.
//
// As of Phase 7, depth is a pure SurfaceIdentifier (see surface_identify.h)
// configured by data - this header's declarations are unchanged so nothing
// outside depth_identify.cpp needed to change, but depth_identify.cpp itself
// is now a thin wrapper, not a second copy of the identification logic.

namespace mv
{

class SurfaceRegistry;

// Called once, from InstallD3D12Hooks, before hooks go live.
void RegisterDepthIdentifier(SurfaceRegistry& registry);

// The identified resource, or nullptr while the search is running,
// ambiguous, or waiting for velocity. Exported so the harness can read it.
ID3D12Resource* IdentifiedDepthResource();

} // namespace mv
