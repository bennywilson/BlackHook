#pragma once

#include "surface_identify.h"

#include <memory>

// Velocity's one piece of surface-specific code: rival adoption
// (docs/REFACTOR_PLAN.md sec 2.4). Everything else about finding SceneVelocity
// - formats, flags, signals, thresholds - is data in the active profile
// (profiles/ue5.json, "velocity"), run by the generic SurfaceIdentifier.
//
// Rival adoption is a subclass rather than data because no arrangement of
// scalars expresses "this same resource has now carried the edge on N
// consecutive frames while the selected one carried none" - see
// velocity_identify.cpp. Its thresholds are still data (the profile's
// "rival" block).
//
// The registry owns the result; callers reach velocity by name through
// SurfaceRegistry::Find/Selected("velocity").

namespace mv
{

// `spec` must outlive the identifier - the active profile's, which lives for
// the process (surface_profile.h).
std::unique_ptr<SurfaceIdentifier> MakeVelocityIdentifier(const SurfaceSpec& spec);

} // namespace mv
