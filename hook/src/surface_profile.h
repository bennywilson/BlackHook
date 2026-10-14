#pragma once

#include "capture.h"
#include "surface_identify.h"

#include <string>

// Surface profiles: the SurfaceSpecs identification runs on, from
// profiles/ue5.json (embedded into the DLL at configure time) plus any
// MV_PROFILE_FILE (same name replaces). Which profile runs: MV_PROFILE, else
// "ue<major>.<minor>-generic" from MV_ENGINE_VERSION, else the file's
// default_profile. MV_IDENTIFY_FRAMES and MV_SURFACES then apply, logged as
// overrides.
//
// Any failure - malformed JSON, an unknown or missing field, an out-of-range
// value, a missing profile - is logged and DISABLES identification for the
// session. There is deliberately no built-in fallback: a capture against a
// spec nobody chose is worse than none.

namespace mv
{

struct SurfaceProfile
{
    std::string name;
    std::string source;     // "embedded" or the MV_PROFILE_FILE path the profile came from
    std::string selectedBy; // "MV_PROFILE", "MV_ENGINE_VERSION=5.2" or "default_profile"
    std::string overrides;  // per-run env overrides applied on top, e.g. MV_IDENTIFY_FRAMES; "" if none
    SurfaceSpec velocity{};
    SurfaceSpec depth{};

    // Optional surfaces: slot i is kSurfaceNames[kFirstOptionalSurface + i].
    // `present` when the profile has a spec for it; registered only when also
    // spec.enabled - off by default, MV_SURFACES=<name>[,<name>] opts in. A
    // fixed array: identifiers keep a reference to their spec.
    static constexpr int kOptionalSurfaceCount = kSurfaceCount - kFirstOptionalSurface;
    struct OptionalSurface
    {
        bool present = false;
        SurfaceSpec spec{};
    };
    OptionalSurface optional[kOptionalSurfaceCount]{};

    // The named optional surface's spec, or nullptr if this profile has none.
    const SurfaceSpec* Optional(const char* surfaceName) const noexcept;
};

// Called once, from InstallD3D12Hooks, before any identifier is registered.
// Returns nullptr on failure, having logged why.
const SurfaceProfile* LoadSurfaceProfile() noexcept;

// The profile LoadSurfaceProfile() loaded, or nullptr before that / on
// failure. For the manifest and the test harness.
const SurfaceProfile* ActiveSurfaceProfile() noexcept;

} // namespace mv
