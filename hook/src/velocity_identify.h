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
//
// As of docs/REFACTOR_PLAN.md Phase 8, the hot-path entry points
// (ConsiderResource/OnTransition/etc.) are reached only through
// SurfaceRegistry, not by d3d12_hook.cpp calling into this file directly -
// see RegisterVelocityIdentifier below. This header now exposes only what
// still has external callers: capture.cpp's silence check and dllmain.cpp's
// DLL export.

namespace mv
{

class SurfaceRegistry;

// Called once, from InstallD3D12Hooks, before hooks go live. Constructs
// nothing (g_velocity is a file-scope static, per the usual static-init-order
// caution) - just hands the registry a pointer to it.
void RegisterVelocityIdentifier(SurfaceRegistry& registry);

// True if the identified velocity buffer hasn't taken a RENDER_TARGET ->
// shader-resource edge in over 300 frames while the game keeps presenting.
bool VelocityPassIsSilent(uint64_t frame, uint64_t* framesSilent);

// The identified resource, or nullptr while the search is running or
// ambiguous. Exported so the test harness can check which resource won.
ID3D12Resource* IdentifiedVelocityResource();

} // namespace mv
