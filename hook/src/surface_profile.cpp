#include "surface_profile.h"

#include "embedded_profiles.h"
#include "logging.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace mv
{
namespace
{

using nlohmann::json;

struct ProfileError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

[[noreturn]] void Fail(const std::string& message)
{
    throw ProfileError(message);
}

// SurfaceSpec carries const char* (it is plain data, copied around freely).
// The strings behind them live here for the rest of the process.
const char* Intern(const std::string& s)
{
    static std::deque<std::string> pool;
    pool.push_back(s);
    return pool.back().c_str();
}

template <typename T>
struct Named
{
    const char* name;
    T value;
};

// Names, not numbers: confusing DXGI 10 (R16G16B16A16_FLOAT) with 11
// (_UNORM) is the error that cost this project the most (DEBUGGING.md). Only
// formats a UE5 velocity/depth/GBuffer target can plausibly use; anything else
// is a profile error until someone adds it here on purpose.
constexpr Named<DXGI_FORMAT> kFormats[] = {
    {"R16G16B16A16_FLOAT", DXGI_FORMAT_R16G16B16A16_FLOAT},
    {"R16G16B16A16_UNORM", DXGI_FORMAT_R16G16B16A16_UNORM},
    {"R16G16B16A16_UINT", DXGI_FORMAT_R16G16B16A16_UINT},
    {"R10G10B10A2_UNORM", DXGI_FORMAT_R10G10B10A2_UNORM},
    {"R8G8B8A8_UNORM", DXGI_FORMAT_R8G8B8A8_UNORM},
    {"B8G8R8A8_UNORM", DXGI_FORMAT_B8G8R8A8_UNORM},
    {"R16G16_FLOAT", DXGI_FORMAT_R16G16_FLOAT},
    {"R16G16_UNORM", DXGI_FORMAT_R16G16_UNORM},
    {"R16G16_UINT", DXGI_FORMAT_R16G16_UINT},
    {"R32G8X24_TYPELESS", DXGI_FORMAT_R32G8X24_TYPELESS},
    {"D32_FLOAT_S8X24_UINT", DXGI_FORMAT_D32_FLOAT_S8X24_UINT},
    {"R24G8_TYPELESS", DXGI_FORMAT_R24G8_TYPELESS},
    {"D24_UNORM_S8_UINT", DXGI_FORMAT_D24_UNORM_S8_UINT},
    {"R32_TYPELESS", DXGI_FORMAT_R32_TYPELESS},
    {"D32_FLOAT", DXGI_FORMAT_D32_FLOAT},
};

constexpr Named<UINT> kFlags[] = {
    {"ALLOW_RENDER_TARGET", D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET},
    {"ALLOW_DEPTH_STENCIL", D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL},
    {"ALLOW_UNORDERED_ACCESS", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS},
    {"DENY_SHADER_RESOURCE", D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE},
    {"ALLOW_CROSS_ADAPTER", D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER},
    {"ALLOW_SIMULTANEOUS_ACCESS", D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS},
};

constexpr Named<TriggerEdge> kTriggers[] = {
    {"render_target_to_shader_resource", TriggerEdge::RenderTargetToShaderResource},
    {"depth_write_to_readable", TriggerEdge::DepthWriteToReadable},
};

constexpr Named<AmbiguityRule> kAmbiguityRules[] = {
    {"score_margin", AmbiguityRule::ScoreMargin},
    {"edge_count_ratio", AmbiguityRule::EdgeCountRatio},
};

constexpr Named<SignalId> kSignals[] = {
    {"at_modal_resolution", SignalId::AtModalResolution},
    {"exact_required_flags", SignalId::ExactRequiredFlags},
    {"produced_once_exactly", SignalId::ProducedOnceExactly},
    {"never_takes_uav_barrier", SignalId::NeverTakesUavBarrier},
    {"never_enters_unordered_access", SignalId::NeverEntersUnorderedAccess},
    {"strict_two_events_per_frame", SignalId::StrictTwoEventsPerFrame},
    {"no_continuity_violations", SignalId::NoContinuityViolations},
    {"shares_call_with_edge", SignalId::SharesCallWithEdge},
};

template <typename T, size_t N>
T Lookup(const Named<T> (&table)[N], const std::string& name, const char* what, const std::string& path)
{
    std::string known;
    for (const Named<T>& entry : table)
    {
        if (name == entry.name)
        {
            return entry.value;
        }
        known += (known.empty() ? "" : ", ") + std::string(entry.name);
    }
    Fail(path + ": unknown " + what + " '" + name + "' (known: " + known + ")");
}

const char* FormatName(DXGI_FORMAT format)
{
    for (const Named<DXGI_FORMAT>& entry : kFormats)
    {
        if (entry.value == format)
        {
            return entry.name;
        }
    }
    return "?";
}

// One JSON object's fields, read strictly: every read marks its key, and
// RejectUnknown() then fails on anything left over - a misspelt key must not
// quietly leave a value at zero.
class Fields
{
public:
    Fields(const json& obj, std::string path) : obj_(obj), path_(std::move(path))
    {
        if (!obj_.is_object())
        {
            Fail(path_ + ": expected an object");
        }
    }

    std::string Path(const char* key) const
    {
        return path_ + "." + key;
    }

    const json* Optional(const char* key)
    {
        used_.insert(key);
        const auto it = obj_.find(key);
        return it == obj_.end() ? nullptr : &*it;
    }

    const json& Required(const char* key)
    {
        const json* value = Optional(key);
        if (value == nullptr)
        {
            Fail(path_ + ": missing required field '" + key + "'");
        }
        return *value;
    }

    int64_t Int(const char* key, int64_t lo, int64_t hi)
    {
        const json& value = Required(key);
        if (!value.is_number_integer())
        {
            Fail(Path(key) + ": expected an integer");
        }
        const int64_t v = value.get<int64_t>();
        if (v < lo || v > hi)
        {
            Fail(Path(key) + ": " + std::to_string(v) + " is outside [" + std::to_string(lo) + ", " +
                 std::to_string(hi) + "]");
        }
        return v;
    }

    std::string String(const char* key)
    {
        const json& value = Required(key);
        if (!value.is_string())
        {
            Fail(Path(key) + ": expected a string");
        }
        return value.get<std::string>();
    }

    bool Bool(const char* key)
    {
        const json& value = Required(key);
        if (!value.is_boolean())
        {
            Fail(Path(key) + ": expected true or false");
        }
        return value.get<bool>();
    }

    void RejectUnknown() const
    {
        for (auto it = obj_.begin(); it != obj_.end(); ++it)
        {
            if (used_.count(it.key()) == 0)
            {
                Fail(path_ + ": unknown field '" + it.key() + "'");
            }
        }
    }

private:
    const json& obj_;
    std::string path_;
    std::set<std::string> used_;
};

UINT ParseFlags(const json& value, const std::string& path)
{
    if (!value.is_array())
    {
        Fail(path + ": expected an array of flag names");
    }
    UINT flags = 0;
    for (size_t i = 0; i < value.size(); ++i)
    {
        const std::string at = path + "[" + std::to_string(i) + "]";
        if (!value[i].is_string())
        {
            Fail(at + ": expected a flag name");
        }
        flags |= Lookup(kFlags, value[i].get<std::string>(), "resource flag", at);
    }
    return flags;
}

SurfaceSpec ParseSurface(const json& obj, const std::string& name, const std::string& path)
{
    Fields f(obj, path);
    SurfaceSpec spec{};
    spec.name = Intern(name);
    spec.logPrefix = Intern(f.String("log_prefix"));

    // --- structural gate -------------------------------------------------
    const json& formats = f.Required("formats");
    if (!formats.is_array() || formats.empty() || formats.size() > std::size(spec.formats))
    {
        Fail(f.Path("formats") + ": expected 1.." + std::to_string(std::size(spec.formats)) + " format entries");
    }
    for (size_t i = 0; i < formats.size(); ++i)
    {
        Fields ff(formats[i], f.Path("formats") + "[" + std::to_string(i) + "]");
        FormatCandidate candidate{};
        candidate.format = Lookup(kFormats, ff.String("format"), "DXGI format", ff.Path("format"));
        candidate.score = static_cast<int>(ff.Int("score", 0, 100));
        candidate.name = Intern(ff.String("description"));
        ff.RejectUnknown();
        for (uint8_t j = 0; j < spec.formatCount; ++j)
        {
            if (spec.formats[j].format == candidate.format)
            {
                Fail(ff.Path("format") + ": " + FormatName(candidate.format) + " is listed twice");
            }
        }
        spec.formats[spec.formatCount++] = candidate;
    }
    spec.requiredFlags = ParseFlags(f.Required("required_flags"), f.Path("required_flags"));
    spec.forbiddenFlags = ParseFlags(f.Required("forbidden_flags"), f.Path("forbidden_flags"));
    if ((spec.requiredFlags & spec.forbiddenFlags) != 0)
    {
        Fail(path + ": a flag is both required and forbidden, so nothing could ever pass");
    }
    spec.minWidth = static_cast<UINT64>(f.Int("min_width", 0, 1 << 16));
    spec.minHeight = static_cast<UINT>(f.Int("min_height", 0, 1 << 16));
    spec.maxBackBufferMultiple = static_cast<uint8_t>(f.Int("max_back_buffer_multiple", 0, 255));

    // --- extent linkage --------------------------------------------------
    if (const json* extentFrom = f.Optional("extent_from"))
    {
        if (!extentFrom->is_string() || extentFrom->get<std::string>() == name)
        {
            Fail(f.Path("extent_from") + ": expected the name of another surface");
        }
        spec.extentFrom = Intern(extentFrom->get<std::string>());
    }

    // --- behaviour ---------------------------------------------------------
    spec.trigger = Lookup(kTriggers, f.String("trigger"), "trigger", f.Path("trigger"));
    spec.minFramesObserved = static_cast<uint64_t>(f.Int("min_frames_observed", 1, 1000000));
    spec.settleFrames = static_cast<uint64_t>(f.Int("settle_frames", 1, 1000000));
    spec.formatScoreGate = static_cast<int>(f.Int("format_score_gate", 0, 100));

    const json& signals = f.Required("signals");
    if (!signals.is_array() || signals.size() > std::size(spec.signals))
    {
        Fail(f.Path("signals") + ": expected 0.." + std::to_string(std::size(spec.signals)) + " signal entries");
    }
    for (size_t i = 0; i < signals.size(); ++i)
    {
        Fields sf(signals[i], f.Path("signals") + "[" + std::to_string(i) + "]");
        SignalWeight signal{};
        const std::string signalName = sf.String("signal");
        signal.id = Lookup(kSignals, signalName, "signal", sf.Path("signal"));
        signal.weight = static_cast<int>(sf.Int("weight", -100, 100));
        // Parameters exist only on shares_call_with_edge; on any other signal
        // RejectUnknown() below refuses them, so they cannot be set and ignored.
        if (signal.id == SignalId::SharesCallWithEdge)
        {
            signal.edge = Lookup(kTriggers, sf.String("edge"), "edge", sf.Path("edge"));
            signal.minPercent = static_cast<int>(sf.Int("min_percent", 1, 100));
        }
        sf.RejectUnknown();
        for (uint8_t j = 0; j < spec.signalCount; ++j)
        {
            // Two shares_call_with_edge entries on different edges are
            // distinct evidence; any other repeat is a mistake.
            if (spec.signals[j].id == signal.id &&
                (signal.id != SignalId::SharesCallWithEdge || spec.signals[j].edge == signal.edge))
            {
                Fail(sf.Path("signal") + ": '" + signalName + "' is listed twice");
            }
        }
        spec.signals[spec.signalCount++] = signal;
    }

    Fields ambiguity(f.Required("ambiguity"), f.Path("ambiguity"));
    spec.ambiguityRule = Lookup(kAmbiguityRules, ambiguity.String("rule"), "ambiguity rule", ambiguity.Path("rule"));
    spec.ambiguityMargin = static_cast<int>(ambiguity.Int("margin", 0, 100000));
    ambiguity.RejectUnknown();

    // --- rival adoption (optional; absent == never adopt) -------------------
    if (const json* rivalJson = f.Optional("rival"))
    {
        Fields rival(*rivalJson, f.Path("rival"));
        spec.rivalStallFrames = static_cast<uint64_t>(rival.Int("stall_frames", 1, 100000));
        spec.rivalFramesBeforeAdopt = static_cast<uint64_t>(rival.Int("frames_before_adopt", 1, 100000));
        spec.rivalMaxAdoptions = static_cast<int>(rival.Int("max_adoptions", 0, 100000));
        spec.rivalMaxReports = static_cast<int>(rival.Int("max_reports", 0, 100000));
        rival.RejectUnknown();
    }

    // --- lifecycle ---------------------------------------------------------
    spec.reopenClearsShortlist = f.Bool("reopen_clears_shortlist");

    // Optional; absent == enabled.
    spec.enabled = true;
    if (const json* enabled = f.Optional("enabled"))
    {
        if (!enabled->is_boolean())
        {
            Fail(f.Path("enabled") + ": expected true or false");
        }
        spec.enabled = enabled->get<bool>();
    }

    f.RejectUnknown();
    return spec;
}

struct ProfileSet
{
    json profiles = json::object();
    std::map<std::string, std::string> sourceOf; // profile name -> "embedded" or file path
    std::string defaultProfile;
};

void AddProfiles(ProfileSet& set, const std::string& text, const std::string& source)
{
    json doc;
    try
    {
        doc = json::parse(text, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
    }
    catch (const json::parse_error& e)
    {
        Fail(source + ": not valid JSON - " + e.what());
    }

    Fields top(doc, source);
    top.Int("schema_version", 1, 1);
    if (const json* defaultProfile = top.Optional("default_profile"))
    {
        if (!defaultProfile->is_string())
        {
            Fail(top.Path("default_profile") + ": expected a profile name");
        }
        set.defaultProfile = defaultProfile->get<std::string>();
    }
    const json& profiles = top.Required("profiles");
    if (!profiles.is_object())
    {
        Fail(top.Path("profiles") + ": expected an object of named profiles");
    }
    for (auto it = profiles.begin(); it != profiles.end(); ++it)
    {
        if (set.profiles.contains(it.key()))
        {
            Log("profile: '" + it.key() + "' from " + source + " replaces the one from " + set.sourceOf[it.key()]);
        }
        set.profiles[it.key()] = it.value();
        set.sourceOf[it.key()] = source;
    }
    top.RejectUnknown();
}

// The named profile with its "extends" chain applied, as an RFC 7386 merge
// patch over the base: objects merge field by field, arrays replace whole -
// so narrowing a format list is just restating it.
json Resolve(const ProfileSet& set, const std::string& name, int depth)
{
    if (depth > 8)
    {
        Fail("profile '" + name + "': 'extends' chain is deeper than 8 - is there a cycle?");
    }
    const auto it = set.profiles.find(name);
    if (it == set.profiles.end())
    {
        std::string known;
        for (auto p = set.profiles.begin(); p != set.profiles.end(); ++p)
        {
            known += (known.empty() ? "" : ", ") + p.key();
        }
        Fail("no profile named '" + name + "' (have: " + known + ")");
    }
    json profile = *it;
    if (!profile.is_object())
    {
        Fail("profile '" + name + "': expected an object");
    }
    const auto extends = profile.find("extends");
    if (extends == profile.end())
    {
        return profile;
    }
    if (!extends->is_string())
    {
        Fail("profile '" + name + "'.extends: expected a profile name");
    }
    json resolved = Resolve(set, extends->get<std::string>(), depth + 1);
    profile.erase("extends");
    resolved.merge_patch(profile);
    return resolved;
}

std::string EnvVar(const char* name)
{
    char buffer[1024]{};
    const DWORD length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    if (length == 0 || length >= sizeof(buffer))
    {
        return {};
    }
    return std::string(buffer, length);
}

std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        Fail("MV_PROFILE_FILE: cannot open '" + path + "'");
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string Summary(const SurfaceSpec& spec)
{
    std::string line = std::string(spec.name) + ": formats [";
    for (uint8_t i = 0; i < spec.formatCount; ++i)
    {
        line += (i ? ", " : "") + std::string(FormatName(spec.formats[i].format)) + "=" +
                std::to_string(spec.formats[i].score);
    }
    char flags[64];
    snprintf(flags, sizeof(flags), "], required_flags=0x%X, forbidden_flags=0x%X", spec.requiredFlags,
             spec.forbiddenFlags);
    line += flags;
    line += ", settle_frames=" + std::to_string(spec.settleFrames) + ", signals=" +
            std::to_string(spec.signalCount) + ", ambiguity_margin=" + std::to_string(spec.ambiguityMargin);
    if (spec.extentFrom != nullptr)
    {
        line += ", extent_from=" + std::string(spec.extentFrom);
    }
    return line;
}

SurfaceProfile g_profile;
bool g_profileLoaded = false;

void Load()
{
    ProfileSet set;
    AddProfiles(
        set,
        std::string(reinterpret_cast<const char*>(kEmbeddedProfiles), sizeof(kEmbeddedProfiles)),
        "embedded");
    const std::string overrideFile = EnvVar("MV_PROFILE_FILE");
    if (!overrideFile.empty())
    {
        AddProfiles(set, ReadFile(overrideFile), overrideFile);
    }

    std::string name = EnvVar("MV_PROFILE");
    std::string selectedBy = "MV_PROFILE";
    if (name.empty())
    {
        const std::string version = EnvVar("MV_ENGINE_VERSION");
        if (!version.empty())
        {
            int major = 0;
            int minor = 0;
            if (sscanf_s(version.c_str(), "%d.%d", &major, &minor) != 2)
            {
                Fail("MV_ENGINE_VERSION='" + version +
                     "' is not major.minor, so no profile can be derived from it - fix it, or set MV_PROFILE");
            }
            name = "ue" + std::to_string(major) + "." + std::to_string(minor) + "-generic";
            selectedBy = "MV_ENGINE_VERSION=" + version;
        }
        else
        {
            if (set.defaultProfile.empty())
            {
                Fail("neither MV_PROFILE nor MV_ENGINE_VERSION is set, and no default_profile is defined");
            }
            name = set.defaultProfile;
            selectedBy = "default_profile";
        }
    }

    const json resolved = Resolve(set, name, 0);
    const std::string path = "profile '" + name + "'";
    Fields profile(resolved, path);
    if (const json* description = profile.Optional("description"))
    {
        if (!description->is_string())
        {
            Fail(profile.Path("description") + ": expected a string");
        }
    }
    const json& surfacesJson = profile.Required("surfaces");
    Fields surfaces(surfacesJson, profile.Path("surfaces"));
    // velocity and depth are required and always on: every dump is keyed off
    // velocity, and depth's extent link and burst atomicity assume it.
    // normals (Phase 10) is optional in a profile and off unless opted into.
    // Any other surface name is a mistake worth hearing about.
    g_profile.velocity = ParseSurface(surfaces.Required("velocity"), "velocity", surfaces.Path("velocity"));
    g_profile.depth = ParseSurface(surfaces.Required("depth"), "depth", surfaces.Path("depth"));
    for (const SurfaceSpec* spec : {&g_profile.velocity, &g_profile.depth})
    {
        if (!spec->enabled)
        {
            Fail(surfaces.Path(spec->name) + ".enabled: " + spec->name + " cannot be disabled");
        }
    }
    g_profile.hasNormals = false;
    if (const json* normals = surfaces.Optional("normals"))
    {
        g_profile.normals = ParseSurface(*normals, "normals", surfaces.Path("normals"));
        g_profile.hasNormals = true;
    }
    surfaces.RejectUnknown();
    profile.RejectUnknown();

    // MV_IDENTIFY_FRAMES: the one per-run override, kept from before profiles
    // existed (it is how mv_testhost fits the settling window into a short
    // run). Applied here, on top of whichever profile ran, so the spec stays
    // the single source the identifier reads - and logged, so a capture made
    // with it says so. Phase 6 dropped this by accident; restored here.
    g_profile.overrides.clear();
    const std::string identifyFrames = EnvVar("MV_IDENTIFY_FRAMES");
    if (!identifyFrames.empty())
    {
        char* end = nullptr;
        const unsigned long long frames = strtoull(identifyFrames.c_str(), &end, 10);
        if (end == identifyFrames.c_str() || *end != '\0' || frames == 0 || frames > 1000000)
        {
            Fail("MV_IDENTIFY_FRAMES='" + identifyFrames + "' is not a frame count in [1, 1000000]");
        }
        g_profile.velocity.settleFrames = frames;
        g_profile.overrides = "velocity.settle_frames=" + std::to_string(frames) + " (MV_IDENTIFY_FRAMES)";
    }

    // MV_SURFACES: comma-separated optional surfaces to switch on for this
    // run (docs/REFACTOR_PLAN.md Phase 10: "opt in with MV_SURFACES=normals").
    // Only optional surfaces may be named - velocity and depth are always on -
    // and a name this profile has no spec for is an error, not a no-op.
    std::istringstream optIn(EnvVar("MV_SURFACES"));
    std::string surface;
    while (std::getline(optIn, surface, ','))
    {
        if (surface.empty())
        {
            continue;
        }
        if (surface != "normals")
        {
            Fail("MV_SURFACES names '" + surface + "' - only optional surfaces can be opted into (normals)");
        }
        if (!g_profile.hasNormals)
        {
            Fail("MV_SURFACES=normals, but " + path + " has no normals spec");
        }
        if (!g_profile.normals.enabled)
        {
            g_profile.normals.enabled = true;
            g_profile.overrides += (g_profile.overrides.empty() ? "" : "; ") + std::string("normals.enabled=true (MV_SURFACES)");
        }
    }

    std::vector<const SurfaceSpec*> all = {&g_profile.velocity, &g_profile.depth};
    if (g_profile.hasNormals)
    {
        all.push_back(&g_profile.normals);
    }
    for (const SurfaceSpec* spec : all)
    {
        if (spec->extentFrom != nullptr && std::string(spec->extentFrom) != "velocity" &&
            std::string(spec->extentFrom) != "depth")
        {
            Fail(path + ": " + spec->name + ".extent_from names '" + spec->extentFrom + "', which is not a surface");
        }
    }

    g_profile.name = name;
    g_profile.source = set.sourceOf[name];
    g_profile.selectedBy = selectedBy;
}

} // namespace

const SurfaceProfile* LoadSurfaceProfile() noexcept
{
    try
    {
        Load();
        g_profileLoaded = true;
        Log("profile: using '" + g_profile.name + "' (selected by " + g_profile.selectedBy + ", from " +
            (g_profile.source == "embedded" ? std::string("embedded ") + kEmbeddedProfilesPath : g_profile.source) +
            ")");
        Log("profile:   " + Summary(g_profile.velocity));
        Log("profile:   " + Summary(g_profile.depth));
        if (g_profile.hasNormals)
        {
            Log("profile:   " + Summary(g_profile.normals) +
                (g_profile.normals.enabled ? "" : " [DISABLED - opt in with MV_SURFACES=normals]"));
        }
        if (!g_profile.overrides.empty())
        {
            Log("profile:   override: " + g_profile.overrides);
        }
        return &g_profile;
    }
    catch (const std::exception& e)
    {
        Log(std::string("profile: FAILED - ") + e.what() +
            ". Surface identification is DISABLED for this session: nothing will be identified and nothing "
            "captured. There is deliberately no built-in fallback spec (docs/REFACTOR_PLAN.md Phase 9).");
    }
    catch (...)
    {
        Log("profile: FAILED with an unknown exception. Surface identification is DISABLED for this session.");
    }
    return nullptr;
}

const SurfaceProfile* ActiveSurfaceProfile() noexcept
{
    return g_profileLoaded ? &g_profile : nullptr;
}

} // namespace mv
