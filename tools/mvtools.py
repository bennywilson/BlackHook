"""Decoding helpers for the raw buffers dumped by the hook.

The hook writes untouched GPU bytes plus a manifest.json describing them
(docs/REFACTOR_PLAN.md sec 4.1), so all format knowledge lives here rather
than in the injected DLL - keeping the in-process code as small and as
low-risk as possible.

load_dump() is the only supported door into a dump (sec 4.3): it gates on
schema_version, refuses an unsealed dump unless told not to, and returns a
Dump whose surfaces are typed by how their identity was established -
IdentifiedSurface (found by search, has a coverage tally) or KnownSurface
(given by an API contract, e.g. colour - no score, no signals, because there
was nothing to search for). Every decode function below still takes a plain
dict (`meta`), not a Surface - Dump.meta reconstructs that dict from the
manifest so this file's numerically-sensitive decode code did not need to
change shape for this migration.
"""
import json
import os
import sys
from dataclasses import dataclass, field
from typing import Optional

import numpy as np

# DXGI formats we actually encounter, verified against dxgiformat.h in the
# installed Windows SDK (10.0.26100.0), not recalled. The neighbours matter:
# R16G16B16A16_FLOAT is 10 and R16G16B16A16_UNORM is 11, and confusing those two
# is the error that cost this project the most (see DEBUGGING.md). R16G16_UNORM
# sits at 35, between R16G16_FLOAT (34) and R16G16_UINT (36).
DXGI_R16G16B16A16_UNORM = 11
DXGI_R16G16B16A16_UINT = 12
DXGI_R10G10B10A2_UNORM = 24
DXGI_R8G8B8A8_UNORM = 28
DXGI_R16G16_UNORM = 35
DXGI_R16G16_UINT = 36
DXGI_B8G8R8A8_UNORM = 87

# The four formats FVelocityRendering::GetFormat can return (VelocityRendering.cpp:758),
# crossed with the D3D12 RHI's PF_* -> DXGI table (D3D12RHI.cpp:139,144):
#
#     bNeedVelocityDepth ? PF_A16B16G16R16 : PF_G16R16              (desktop)
#     bNeedVelocityDepth ? PF_R16G16B16A16_UINT : PF_R16G16_UINT    (OpenGL/GLES)
#
# -> (channels, bytes per pixel). The UINT variants store the same 0..65535
# integers the UNORM ones do - the GLES path just writes them explicitly
# (`return uint4(EncodedV * 65535.0 + 0.5f)`, Common.ush:2081) - so normalising
# by 65535 recovers the identical [0,1] value and the decode below is unchanged.
VELOCITY_FORMATS = {
    DXGI_R16G16B16A16_UNORM: (4, 8),
    DXGI_R16G16B16A16_UINT: (4, 8),
    DXGI_R16G16_UNORM: (2, 4),
    DXGI_R16G16_UINT: (2, 4),
}


# ---------------------------------------------------------------------------
# The manifest-backed loader. See this file's module docstring and
# docs/REFACTOR_PLAN.md sec 4.3 - this is the only supported door into a
# dump; every other function in this file takes bytes or a dict this loader
# produces, never a raw manifest.

SUPPORTED_SCHEMA = 1
# Schema versions this build can still read, each mapped to a function that
# upgrades that version's manifest dict to the current shape. Empty today -
# schema_version 1 is the only version that has ever existed - but the seam
# is here so a real migration is a function added to this dict, not a
# reason to touch load_dump() itself.
_MIGRATIONS = {}


@dataclass(frozen=True)
class Plane:
    """One D3D12 subresource plane's footprint. Depth has two (depth,
    stencil); velocity and colour have one."""
    index: int
    offset: int
    width: int
    height: int
    row_pitch: int
    format: int


def _parse_planes(layout_obj):
    if layout_obj is None:
        return ()
    return tuple(
        Plane(p["index"], p["offset"], p["width"], p["height"], p["row_pitch"], p["format"])
        for p in layout_obj.get("planes", []))


@dataclass(frozen=True)
class IdentifiedSurface:
    """A surface whose identity came from the structural+behavioural search
    (velocity, depth, and - when MV_SURFACES=normals - normals).

    Deliberately has no .score/.margin/.signals/.survivors attributes: the
    hook does not track that state anywhere queryable yet (see the commit
    that added manifest writing, docs/REFACTOR_PLAN.md Phase 4c-2) - it
    exists only as local variables inside SurfaceIdentifier::Decide()
    (hook/src/surface_identify.cpp) that get logged and discarded. Adding
    placeholder fields for it here would invite exactly the kind of
    silent-plausible-wrong reading this project's own README warns about
    (the format-enum bug, the missing sqrt in the velocity decode - both
    looked fine until checked against a primary source). It arrives once
    Phase 6-7's SurfaceIdentifier owns that state.
    """
    name: str
    present: bool
    required: bool
    trigger: str
    arbitration: str
    file_pattern: str
    format: Optional[int] = None
    bytes: Optional[int] = None
    subresources: Optional[int] = None
    planes: tuple = ()
    coverage: Optional[dict] = None       # velocity: {"strong","weak","none"}
    coverage_note: Optional[str] = None   # depth: "not evaluated - ..."
    reason: Optional[str] = None          # set only when not present
    detail: Optional[str] = None          # normals: why not present, in words
    encoding: Optional[dict] = None       # normals: {"kind": "unknown"} until verified

    # Subresource 0 / plane 0 is the top mip / first plane, which is what
    # every offline tool actually reads (matches WriteMetadata's own comment
    # in capture.cpp) - exposed directly rather than making every caller
    # write `.planes[0].width`.
    @property
    def width(self):
        return self.planes[0].width

    @property
    def height(self):
        return self.planes[0].height

    @property
    def row_pitch(self):
        return self.planes[0].row_pitch


@dataclass(frozen=True)
class KnownSurface:
    """A surface whose identity is given by a documented API contract, not
    found by search - colour, today, via IDXGISwapChain::GetBuffer.

    Deliberately has NO .score, .margin, .signals or .coverage_note
    attribute: docs/REFACTOR_PLAN.md sec 3.1/4.3 wants a consumer that tries
    to read identification evidence off a known-source surface to fail
    loudly (AttributeError) rather than silently get None back - the whole
    point of two dataclasses instead of one optional-field class.
    """
    name: str
    present: bool
    required: bool
    mechanism: str
    trigger: str
    arbitration: str
    file_pattern: str
    format: Optional[int] = None
    bytes: Optional[int] = None
    subresources: Optional[int] = None
    planes: tuple = ()
    coverage: Optional[dict] = None       # {"by_construction": N}

    @property
    def width(self):
        return self.planes[0].width

    @property
    def height(self):
        return self.planes[0].height

    @property
    def row_pitch(self):
        return self.planes[0].row_pitch


def _parse_surface(name, obj, manifest):
    present = bool(obj.get("present", False))
    layout = None
    if present:
        layout = manifest.get("layouts", [{}])[0].get("surfaces", {}).get(name)
        if layout is None:
            raise KeyError(f"'{name}' is present but layouts[0].surfaces has no entry for it")

    common = dict(
        name=name,
        present=present,
        required=bool(obj.get("required", False)),
        trigger=obj["trigger"],
        arbitration=obj["arbitration"],
        file_pattern=obj["file_pattern"],
        format=layout["format"] if layout else None,
        bytes=layout["bytes"] if layout else None,
        subresources=layout["subresources"] if layout else None,
        planes=_parse_planes(layout),
    )
    source = obj.get("source", {})
    if source.get("kind") == "known_by_construction":
        return KnownSurface(
            mechanism=source.get("mechanism", ""),
            coverage=obj.get("coverage"),
            **common)
    return IdentifiedSurface(
        coverage=obj.get("coverage"),
        coverage_note=obj.get("coverage_note"),
        reason=obj.get("reason"),
        detail=obj.get("detail"),
        encoding=obj.get("encoding"),
        **common)


class Dump:
    """A loaded, version-gated dump directory. Construct via load_dump()."""

    def __init__(self, dump_dir, manifest, surfaces, unusable):
        self.dir = dump_dir
        self.manifest = manifest
        self._surfaces = surfaces
        self.unusable = unusable
        self.migrated_from = None
        self.notes = []

    def has(self, name):
        s = self._surfaces.get(name)
        return s is not None and s.present

    def surface(self, name):
        """The named surface, typed IdentifiedSurface or KnownSurface.

        Raises rather than returning None: KeyError for a name the manifest
        never mentions, ValueError naming the manifest's own reason for a
        surface that exists but is not present (or was refused as unusable).
        """
        s = self._surfaces.get(name)
        if s is None:
            if name in self.unusable:
                raise ValueError(f"surface '{name}' is unusable: {self.unusable[name]}")
            raise KeyError(f"no such surface in this manifest: '{name}'")
        if not s.present:
            reason = getattr(s, "reason", None) or "not present in this session"
            raise ValueError(f"surface '{name}' is not present in this dump: {reason}")
        return s

    @property
    def meta(self):
        """Flat dict, field-for-field compatible with the pre-manifest
        load_meta() output, so every existing decode function in this file
        (all of which take this dict shape) works unchanged against a
        manifest-backed Dump. Built fresh each access rather than cached -
        this is a read of already-parsed data, not a file read.
        """
        m = {}
        if self.has("velocity"):
            v = self.surface("velocity")
            p = v.planes[0]
            m.update(velocity_width=p.width, velocity_height=p.height, velocity_row_pitch=p.row_pitch,
                      velocity_format=v.format, velocity_bytes=v.bytes, velocity_subresources=v.subresources)
        if self.has("color"):
            c = self.surface("color")
            p = c.planes[0]
            m.update(color_width=p.width, color_height=p.height, color_row_pitch=p.row_pitch,
                      color_format=c.format, color_bytes=c.bytes, color_subresources=c.subresources)
        engine = self.manifest.get("engine")
        if engine:
            m["engine_version_major"] = engine["major"]
            m["engine_version_minor"] = engine["minor"]
        if self.has("depth"):
            d = self.surface("depth")
            m.update(depth_format=d.format, depth_bytes=d.bytes, depth_subresources=d.subresources)
            for p in d.planes:
                prefix = f"depth_plane{p.index}_"
                m[prefix + "offset"] = p.offset
                m[prefix + "width"] = p.width
                m[prefix + "height"] = p.height
                m[prefix + "row_pitch"] = p.row_pitch
                m[prefix + "format"] = p.format
        return m

    def frame_indices(self):
        return frame_indices(self.dir)

    def pairs(self):
        return consecutive_pairs(self.dir, self.frame_indices())


def load_dump(dump_dir, *, allow_unsealed=False):
    """The only supported way to open a dump. See docs/REFACTOR_PLAN.md
    sec 4.3 for the full version-gate table this implements.
    """
    manifest_path = os.path.join(dump_dir, "manifest.json")
    if not os.path.exists(manifest_path):
        legacy = os.path.join(dump_dir, "meta.txt")
        if os.path.exists(legacy):
            raise ValueError(
                f"{dump_dir} is a pre-manifest dump (meta.txt but no manifest.json). Re-capture with a "
                f"hook built after docs/REFACTOR_PLAN.md Phase 4 - load_dump() only reads manifest.json "
                f"and does not fall back to the old format.")
        raise ValueError(f"{dump_dir} has no manifest.json - not a dump this loader recognises")

    with open(manifest_path) as f:
        manifest = json.load(f)

    version = manifest.get("schema_version")
    if version is None:
        raise ValueError(f"{manifest_path} has no schema_version - a manifest without one is not a manifest")
    if version > SUPPORTED_SCHEMA:
        raise ValueError(
            f"{manifest_path} is schema_version {version}; this mvtools.py only understands up to "
            f"{SUPPORTED_SCHEMA}. Never read forward-compatibly by guessing - update mvtools.py first.")
    migrated_from = None
    if version < SUPPORTED_SCHEMA:
        if version not in _MIGRATIONS:
            raise ValueError(
                f"{manifest_path} is schema_version {version}, older than {SUPPORTED_SCHEMA}, and this "
                f"mvtools.py has no migration registered for it (see _MIGRATIONS).")
        manifest = _MIGRATIONS[version](manifest)
        migrated_from = version

    sealed = manifest.get("integrity", {}).get("sealed", False)
    if not sealed and not allow_unsealed:
        raise ValueError(
            f"{manifest_path} is not sealed (integrity.sealed is false or absent) - this dump was not "
            f"fully drained when its manifest was written. Pass allow_unsealed=True to read it anyway; "
            f"frames may be missing or the layout may have changed mid-capture.")
    if not sealed:
        print(
            f"WARNING: {dump_dir} is UNSEALED - reading anyway because allow_unsealed=True.\n"
            f"         This dump may be missing frames, or describe a layout that changed mid-capture.\n"
            f"         Treat every number from it as provisional.",
            file=sys.stderr)

    surfaces = {}
    unusable = {}
    for name, obj in manifest.get("surfaces", {}).items():
        try:
            surfaces[name] = _parse_surface(name, obj, manifest)
        except (KeyError, TypeError) as e:
            unusable[name] = str(e)

    if "velocity" in unusable or surfaces.get("velocity") is None or not surfaces["velocity"].present:
        reason = unusable.get("velocity") or "not present in this session"
        raise ValueError(
            f"{dump_dir}: velocity is unusable ({reason}) - every downstream tool keys off velocity, "
            f"so this dump cannot be read at all")

    dump = Dump(dump_dir, manifest, surfaces, unusable)
    dump.migrated_from = migrated_from
    return dump


def decode_normals(dump, capture_index):
    """World-space normals for one captured frame - REFUSES, by design.

    The hook captures GBufferA's bytes (MV_SURFACES=normals), but how UE packs
    the normal into them (octahedral vs direct, and whether that changed
    between 5.2 and 5.7) has not been verified against engine source or a
    live capture - docs/REFACTOR_PLAN.md sec 3.2 #6. The manifest says so
    ("encoding": {"kind": "unknown"}), and decoding anyway would produce a
    plausible normal field from a guess: the failure mode this project has
    paid for twice (README: the format-enum bug, the missing sqrt).

    When the encoding is established, the hook should write it into the
    manifest and this function should decode exactly that kind and refuse
    every other.
    """
    surface = dump.surface("normals")  # raises if not captured, with the manifest's reason
    kind = (surface.encoding or {}).get("kind", "unknown")
    raise ValueError(
        f"refusing to decode normals (frame {capture_index}): manifest encoding is '{kind}', and this "
        f"mvtools.py knows no verified normals encoding. The raw bytes are in "
        f"{surface.file_pattern % capture_index} if you need them for investigation.")


def _rows(path, width, height, row_pitch, bytes_per_pixel):
    """Un-pad a GPU row-pitched buffer into a tight (height, width*bpp) array.

    D3D12 pads each row up to a 256-byte multiple, and GetCopyableFootprints
    reports a total size where the *last* row is not padded - so the file is
    row_pitch*(height-1) + width*bpp bytes, not row_pitch*height.
    """
    raw = np.fromfile(path, dtype=np.uint8)
    useful = width * bytes_per_pixel
    out = np.empty((height, useful), dtype=np.uint8)
    for y in range(height):
        start = y * row_pitch
        out[y] = raw[start:start + useful]
    return out


def velocity_channels(meta):
    """(channels, bytes per pixel) for the captured velocity format.

    Read from meta.txt rather than asserted. The previous version hardcoded
    4 channels and 8 bytes per pixel and asserted the format was
    R16G16B16A16_UNORM, which is what the one title this was developed against
    happens to use - a title without Lumen, distance fields or ray tracing gets
    PF_G16R16 (2 channels, 4 bytes) from the same engine code, and would have
    failed here with an AssertionError naming a number, at the very end of the
    pipeline, long after the expensive part.
    """
    fmt = meta["velocity_format"]
    if fmt not in VELOCITY_FORMATS:
        raise ValueError(
            f"velocity format {fmt} is not one FVelocityRendering::GetFormat can return "
            f"({sorted(VELOCITY_FORMATS)}). Either the identification filter selected the wrong "
            f"resource, or this engine version uses a format this tool has not been told about - "
            f"check FVelocityRendering::GetFormat for the version in question before adding it.")
    return VELOCITY_FORMATS[fmt]


def load_velocity_raw(path, meta):
    """Returns raw velocity as float (h, w, channels) in [0,1].

    Channel count depends on the format: 4 for PF_A16B16G16R16 (the
    NeedVelocityDepth case), 2 for PF_G16R16. Callers that need channels 2/3
    must check - decode_velocity_depth() does.
    """
    channels, bpp = velocity_channels(meta)
    w, h = meta["velocity_width"], meta["velocity_height"]
    rows = _rows(path, w, h, meta["velocity_row_pitch"], bpp)
    u16 = rows.view(np.uint16).reshape(h, w, channels)
    return u16.astype(np.float32) / 65535.0


# ---------------------------------------------------------------------------
# The SceneVelocity encoding.
#
# These are no longer fitted. They are transcribed from the encode/decode pair
# in Engine/Shaders/Private/Common.ush of the engine this title is built on
# (UE 5.7.1, on disk), and every constant below can be pointed at a line there.
# The previous fitted constants (-0.1758 / +0.1991 UV per UNORM unit) are gone;
# what they actually were is explained at the bottom of this comment, because
# the reason they looked plausible is the interesting part.
#
# EncodeVelocityToTexture(), Common.ush:2060 --
#
#     #if VELOCITY_ENCODE_GAMMA                          // 1 whenever SM5+
#         V.xy = sign(V.xy) * sqrt(abs(V.xy)) * (2.0 / sqrt(2.0));
#     #endif
#     EncodedV.xy = V.xy * (0.499f * 0.5f) + 32767.0f / 65535.0f;
#
# So the stored value is a SQUARE-ROOT encoding of the clip-space delta, not a
# linear one. VELOCITY_ENCODE_GAMMA is `#define`d to 1 for every feature level
# at or above SM5 (Common.ush:238), i.e. unconditionally for a D3D12 PC title -
# there is no cvar and no per-project setting to check.
#
# The inverse, DecodeVelocityFromTexture() at Common.ush:2107:
#
#     V.xy = EncodedV.xy * InvDiv - 32767.0f/65535.0f * InvDiv;   // InvDiv = 1/(0.499*0.5)
#     #if VELOCITY_ENCODE_GAMMA
#         V.xy = (V.xy * abs(V.xy)) * 0.5;
#     #endif
#
# V is then a CLIP-space delta, current minus previous, with x right and y up.
# Two more engine facts fix the rest of the convention:
#
#   * `PrevScreenPos = ScreenPos - DecodeVelocityFromTexture(...).xy`
#     (PostProcessAmbientOcclusion.usf:1203, VisualizeMotionVectors.usf:78, and
#     every other consumer) - so V points forward, and stepping *back* to the
#     previous frame subtracts it.
#   * `ScreenPosToViewportUV(S) = float2(0.5 + 0.5*S.x, 0.5 - 0.5*S.y)`
#     (Common.ush:1658) - clip to UV flips Y.
#
# Composing those two gives the backward (current -> previous) UV displacement
# this module returns:
#
#     du = -0.5 * V.x        dv = +0.5 * V.y
#
# which is where the X-negative / Y-positive sign pattern comes from. It was
# guessed correctly before; it is now derived.
#
# Jitter: the encoded vector is already de-jittered. Calculate3DVelocityBase()
# (VelocityCommon.ush:9) subtracts View.TemporalAAJitter.xy from the current
# screen position and .zw from the previous one before differencing, so the
# TAA/TSR sub-pixel offset is not in the buffer and must not be removed again.
#
# Why the old fitted constants looked right: linearising the sqrt encoding
# around the dominant motion in the station capture (a train at ~11 px/frame
# across a 1212-wide velocity buffer) gives an apparent slope of
#     (11/1212) / (sqrt(2*11/1212) * sqrt(2) * 0.2495) = 0.191 UV per unit,
# against the -0.1758 / +0.1991 that were fitted. The fit was not wrong about
# its own data - it measured the local tangent of a curve and reported it as
# the curve. That is also the whole of the "~10x disagreement with Common.ush":
# 2.004 is the coefficient of the sqrt term, 0.19 is a chord across it, and the
# ratio between them is a function of how fast the scene happened to be moving.
ENCODE_ZERO = 32767.0 / 65535.0     # Common.ush:2069
ENCODE_SCALE = 0.499 * 0.5          # Common.ush:2069
ENCODE_GAMMA = True                 # Common.ush:238, SM5+ => always on here

# ---------------------------------------------------------------------------
# Engine version.
#
# The x/y encoding above is stable across the versions checked: UE 5.2 and
# UE 5.7.1 both guard VELOCITY_ENCODE_GAMMA on `FEATURE_LEVEL >= FEATURE_LEVEL_SM5`
# and both apply the identical `V.xy = (V.xy * abs(V.xy)) * 0.5` on decode. So
# the displacement decode ports between them unchanged.
#
# The DEPTH channels do NOT. 5.7.1 steals the bottom 1-2 bits of channel 3 to
# carry bHasPixelAnimation / temporal responsiveness:
#
#   5.7.1:  V.z = asfloat((z << 16) | (w & VELOCITY_Z_LOW_MASK))   // 0xFFFE or 0xFFFC
#   5.2:    V.z = asfloat((z << 16) |  w)                          // no mask at all
#
# On 5.2 those bits are ordinary float32 mantissa. Masking them off loses (a
# negligible amount of) precision, but *reporting bit 0 as a flag* would be
# presenting the low mantissa bit of a float as a semantic boolean - which is
# the exact mistake this project already made once, when channel 2's bimodality
# was written up as "a flag, not a velocity component" (it is the sign bit of
# this same float). Same error, one bit lower down. So the flag is only decoded
# where it is known to exist.
#
# Verified endpoints only. 5.2: no mask, no flag. 5.7.1: masked, flag present.
# The introduction point in between has NOT been checked - anything else warns
# rather than guessing, because a silently wrong boolean is worse than a noisy
# one.
#
# One caveat on the 5.7 entry, found while porting and worth stating because it
# is the same class of thing this table exists to prevent. VELOCITY_Z_LOW_MASK
# is not a constant even within 5.7.1 (Common.ush:2049-2055):
#
#     #if VELOCITY_ENCODE_TEMPORAL_RESPONSIVENESS
#         #define TEMPORAL_RESPONSIVENESS_MASK  0x3
#         #define VELOCITY_Z_LOW_MASK           0xFFFC     <- two bits stolen
#     #else
#         #define TEMPORAL_RESPONSIVENESS_MASK  0x1
#         #define VELOCITY_Z_LOW_MASK           0xFFFE     <- one bit stolen
#     #endif
#
# and VELOCITY_ENCODE_TEMPORAL_RESPONSIVENESS depends on
# VELOCITY_SUPPORT_TEMPORAL_RESPONSIVENESS, a per-shader-platform property we
# cannot read from a dump. 0xFFFE below is therefore the *narrower* of the two
# possibilities, chosen deliberately: if the title actually uses 0xFFFC, this
# leaves bit 1 in the mantissa, which perturbs V.z by ~1e-7 relative - nothing.
# Guessing the other way round would mask off a real mantissa bit AND report
# bit 1 as part of a flag it is not. Both bits are low mantissa either way, so
# V.z is safe; only bHasPixelAnimation (bit 0) is claimed, and bit 0 is stolen
# under both branches.
ENGINE_VERSION = (5, 7)

_VERIFIED_ENCODINGS = {
    # version: (velocity_z_low_mask, has_pixel_animation_bit)
    (5, 2): (0xFFFF, False),
    (5, 7): (0xFFFE, True),
}


def engine_version(meta=None):
    """The engine version to decode with: from the capture if it recorded one.

    ENGINE_VERSION is a module global, which is fine for one title at a time and
    silently wrong the moment two dumps from different engine versions are
    analysed in the same session - the second one gets whatever the first one
    set. The hook now writes engine_version_major/minor into meta.txt when
    MV_ENGINE_VERSION was set for the capture, so the dump carries its own
    answer and the global is only a fallback.
    """
    if meta and "engine_version_major" in meta and "engine_version_minor" in meta:
        return (meta["engine_version_major"], meta["engine_version_minor"])
    # Not in the capture. MV_ENGINE_VERSION lets the operator supply it after
    # the fact, which is not as good as recording it but is far better than the
    # alternative: falling back to a default that is wrong for the title, and
    # then reporting bHasPixelAnimation for an engine version that has no such
    # flag. That is exactly the error this whole mechanism exists to prevent -
    # reading semantics into bits of a value whose type has not been
    # established - and it happened here, silently, on the first third-party
    # capture, because the game was launched without the variable set.
    env = os.environ.get("MV_ENGINE_VERSION", "")
    if "." in env:
        try:
            major, minor = env.split(".", 1)
            return (int(major), int(minor.split(".")[0]))
        except ValueError:
            pass
    return ENGINE_VERSION


def depth_encoding(version=None):
    """(velocity_z_low_mask, has_pixel_animation_bit) for an engine version."""
    version = version or ENGINE_VERSION
    if version in _VERIFIED_ENCODINGS:
        return _VERIFIED_ENCODINGS[version]
    import warnings
    nearest = min(_VERIFIED_ENCODINGS, key=lambda v: abs(v[0] * 100 + v[1] - (version[0] * 100 + version[1])))
    warnings.warn(
        f"UE {version[0]}.{version[1]} velocity depth encoding has not been verified against "
        f"engine source; falling back to the UE {nearest[0]}.{nearest[1]} rules. Check "
        f"VELOCITY_Z_LOW_MASK in that version's Common.ush before trusting V.z or "
        f"bHasPixelAnimation.")
    return _VERIFIED_ENCODINGS[nearest]

# UE5 clamps the *encoded* channel to [0,1], so the representable clip-space
# delta is bounded. Recorded here because it is the ceiling on anything this
# module can report, not because anything divides by it.
MAX_CLIP_DELTA = (ENCODE_ZERO / ENCODE_SCALE) ** 2 * 0.5 * 2.0


def decode_velocity_clip(path, meta):
    """Decode SceneVelocity into the engine's own clip-space delta.

    Returns (h, w, 2) of `V.xy` exactly as DecodeVelocityFromTexture() would
    hand it to a shader: current-minus-previous, clip space, x right, y up.
    Use this to compare against engine code; use decode_velocity() to compare
    against images.
    """
    raw = load_velocity_raw(path, meta)
    v = (raw[:, :, :2] - ENCODE_ZERO) / ENCODE_SCALE
    if ENCODE_GAMMA:
        v = (v * np.abs(v)) * 0.5
    return v


def decode_velocity(path, meta):
    """Decode the velocity buffer into screen-space UV displacement.

    Returned value is the displacement from this frame's pixel back to where
    that surface was in the previous frame, in UV units (fraction of screen).
    Only meaningful where velocity_written_mask() is true.
    """
    v = decode_velocity_clip(path, meta)
    out = np.empty(v.shape, dtype=np.float32)
    out[:, :, 0] = -0.5 * v[:, :, 0]
    out[:, :, 1] = +0.5 * v[:, :, 1]
    return out


def decode_velocity_depth(path, meta, version=None):
    """Decode channels 2 and 3, which are not a colour and not a displacement.

    Under VELOCITY_ENCODE_DEPTH (Common.ush:2071) the encoder splits the
    float32 bit pattern of `V.z` - the DeviceZ delta, current minus previous -
    across the two spare 16-bit channels:

        EncodedV.z = (asuint(V.z) >> 16) & 0xFFFF
        EncodedV.w = (asuint(V.z)        & VELOCITY_Z_LOW_MASK) | ResponsivenessMask

    with the bottom 1-2 bits of w stolen for the temporal-responsiveness /
    bHasPixelAnimation flags. Reassembling the halves is the only way to read
    them: on their own, channel 2 looks bimodal and channel 3 looks like noise,
    which is exactly what the top and bottom halves of a clustered float32 do
    look like when you plot them as if they were colours.

    Returns (device_z_delta, has_pixel_animation). has_pixel_animation is None
    on engine versions that do not encode it - see depth_encoding().
    """
    channels, _ = velocity_channels(meta)
    if channels != 4:
        raise ValueError(
            f"velocity format {meta['velocity_format']} has no depth channels. UE only encodes "
            f"V.z when NeedVelocityDepth() is true (VelocityRendering.cpp), which is also what "
            f"selects the 4-channel PF_A16B16G16R16 format; a 2-channel PF_G16R16 capture has "
            f"no channels 2/3 to decode.")
    mask, has_anim_bit = depth_encoding(version or engine_version(meta))
    raw = load_velocity_raw(path, meta)
    hi = np.round(raw[:, :, 2] * 65535.0).astype(np.uint32)
    lo = np.round(raw[:, :, 3] * 65535.0).astype(np.uint32)
    bits = ((hi << 16) | (lo & mask)).astype(np.uint32)
    anim = (lo & 0x1).astype(bool) if has_anim_bit else None
    return bits.view(np.float32), anim


def velocity_written_mask(path, meta):
    """True where the velocity buffer actually holds written data.

    UE5 only writes SceneVelocity for pixels it considers to have non-trivial
    motion; everywhere else the buffer holds its cleared value (0), which
    decodes to a large bogus displacement rather than zero. Those pixels must
    be excluded, not trusted.

    The test is `EncodedV.x > 0`, which is the engine's own - see
    `bIsRenderedVelocity` in VisualizeMotionVectors.usf:73 and
    PostProcessAmbientOcclusion.usf:1200. Zero motion encodes to 32767, so 0 in
    channel 0 is unambiguously "never written"; the engine falls back to
    reconstructing camera motion from depth for those pixels, which is why they
    are absent rather than zero.
    """
    raw = load_velocity_raw(path, meta)
    return raw[:, :, 0] > 0.0


# ---------------------------------------------------------------------------
# Scene depth.
#
# UE5 creates scene depth as PF_DepthStencil, which the D3D12 RHI maps to a
# TYPELESS format (D3D12RHI.cpp:117/127) - R24G8_TYPELESS or R32G8X24_TYPELESS
# depending on the r.D3D12.UseD24 cvar. Both are MULTI-PLANE: depth is plane 0,
# stencil is plane 1, with different formats and different row pitches. The hook
# writes every plane's footprint into meta_depth.txt rather than describing only
# the first, so this does not have to assume which case it is looking at.
#
# The values are DeviceZ exactly as the shader would read them: UE5 uses a
# reversed-Z buffer, so 1.0 is the near plane and 0.0 is the far plane / sky.
# Nothing is linearised here.
DEPTH_PLANE0_DECODERS = {
    39: "float32",   # R32_TYPELESS       - plane 0 of R32G8X24_TYPELESS
    40: "float32",   # D32_FLOAT
    41: "float32",   # R32_FLOAT
    44: "unorm24",   # R24G8_TYPELESS
    45: "unorm24",   # D24_UNORM_S8_UINT
    46: "unorm24",   # R24_UNORM_X8_TYPELESS - plane 0 of R24G8_TYPELESS
    55: "unorm16",   # D16_UNORM
    56: "unorm16",   # R16_UNORM
}


def has_depth(meta):
    return "depth_plane0_format" in meta


def load_depth(path, meta):
    """Decode the depth capture's plane 0 into DeviceZ as (h, w) float32."""
    if not has_depth(meta):
        raise ValueError(
            "this dump has no meta_depth.txt, so it was taken before depth capture existed or the "
            "hook never identified a depth buffer in that session - check the 'depth:' lines in "
            "mv_hook.log rather than assuming the copy failed")
    fmt = meta["depth_plane0_format"]
    if fmt not in DEPTH_PLANE0_DECODERS:
        raise ValueError(
            f"depth plane 0 format {fmt} is not one this tool knows how to read. Add it only after "
            f"checking what GetCopyableFootprints reports for the resource format in question - the "
            f"plane formats of a typeless depth resource are not the resource's own format.")
    kind = DEPTH_PLANE0_DECODERS[fmt]
    w = meta["depth_plane0_width"]
    h = meta["depth_plane0_height"]
    pitch = meta["depth_plane0_row_pitch"]
    offset = meta["depth_plane0_offset"]
    bpp = 2 if kind == "unorm16" else 4
    raw = np.fromfile(path, dtype=np.uint8)
    useful = w * bpp
    out = np.empty((h, useful), dtype=np.uint8)
    for y in range(h):
        start = offset + y * pitch
        out[y] = raw[start:start + useful]
    if kind == "float32":
        return out.view(np.float32).reshape(h, w)
    if kind == "unorm24":
        # Depth is in the low 24 bits; the top 8 are the X8 padding, NOT the
        # stencil (stencil lives in plane 1, in its own footprint).
        packed = out.view(np.uint32).reshape(h, w)
        return (packed & 0x00FFFFFF).astype(np.float32) / 16777215.0
    return out.view(np.uint16).reshape(h, w).astype(np.float32) / 65535.0


def load_color(path, meta):
    """Returns the back buffer as float RGB (h, w, 3) in [0,1]."""
    w, h = meta["color_width"], meta["color_height"]
    fmt = meta["color_format"]
    if fmt == DXGI_R10G10B10A2_UNORM:
        rows = _rows(path, w, h, meta["color_row_pitch"], 4)
        packed = rows.view(np.uint32).reshape(h, w)
        r = (packed & 0x3FF).astype(np.float32) / 1023.0
        g = ((packed >> 10) & 0x3FF).astype(np.float32) / 1023.0
        b = ((packed >> 20) & 0x3FF).astype(np.float32) / 1023.0
        return np.stack([r, g, b], axis=2)
    if fmt in (DXGI_R8G8B8A8_UNORM, DXGI_B8G8R8A8_UNORM):
        rows = _rows(path, w, h, meta["color_row_pitch"], 4)
        px = rows.reshape(h, w, 4).astype(np.float32) / 255.0
        if fmt == DXGI_B8G8R8A8_UNORM:
            return px[:, :, [2, 1, 0]]
        return px[:, :, :3]
    raise ValueError(f"unhandled colour format {fmt}")


def to_display(color):
    """Back buffer -> displayable RGB.

    The presented back buffer is already display-encoded (the swapchain is what
    goes to the monitor), so no gamma is applied here. An earlier version
    applied **(1/2.2) on the assumption the data was linear, which double
    gamma-corrected it and visibly washed out an otherwise dark scene - the
    give-away was captures looking much brighter than the game does.
    """
    return np.clip(color, 0, 1).astype(np.float32)


def flow_to_rgb(flow_uv, meta, max_pixels=None, gamma=0.5, floor=0.18):
    """HSV colour-wheel visualisation: hue = direction, value = magnitude.

    A real scene spans a wide range of speeds at once - here a train crossing
    at ~11px/frame alongside a weapon viewmodel drifting at ~1px/frame. Mapping
    magnitude to brightness linearly crushes everything slow to black, so the
    ramp is gamma-corrected (sqrt by default) and written pixels get a small
    brightness floor. Direction (hue) stays untouched; only the brightness ramp
    is perceptual, so colours remain comparable between frames.
    """
    import cv2
    h, w = flow_uv.shape[:2]
    # Convert UV displacement to pixels so the scale is physically meaningful.
    fx = flow_uv[:, :, 0] * w
    fy = flow_uv[:, :, 1] * h
    mag = np.sqrt(fx * fx + fy * fy)
    ang = np.arctan2(fy, fx)
    if max_pixels is None:
        max_pixels = max(np.percentile(mag, 99), 1e-3)
    norm = np.clip(mag / max_pixels, 0, 1) ** gamma
    hsv = np.zeros((h, w, 3), dtype=np.float32)
    hsv[:, :, 0] = (ang + np.pi) / (2 * np.pi) * 179.0
    hsv[:, :, 1] = 255.0
    hsv[:, :, 2] = (floor + (1.0 - floor) * norm) * 255.0
    return cv2.cvtColor(hsv.astype(np.uint8), cv2.COLOR_HSV2BGR)


def frame_indices(dump_dir):
    idx = []
    for name in os.listdir(dump_dir):
        if name.startswith("vel_") and name.endswith(".bin"):
            idx.append(int(name[4:-4]))
    return sorted(idx)


def load_frame_map(dump_dir):
    """capture index -> the game frame the capture came from, or None.

    The hook appends one `captureIndex,frameIndex` line per drained frame to
    frames.csv. Dumps taken before that existed have no such file.
    """
    path = os.path.join(dump_dir, "frames.csv")
    if not os.path.exists(path):
        return None
    out = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            capture, frame = line.split(",")[:2]
            out[int(capture)] = int(frame)
    return out or None


def consecutive_pairs(dump_dir, indices=None):
    """(prev, cur) capture pairs that really are adjacent game frames.

    The whole warp test assumes frame N-1 and frame N are one frame apart. That
    was previously assumed rather than checked: capture indices are a counter
    of what we managed to drain, so a dropped frame, a slot that was busy, or a
    second F8 burst all produce numerically adjacent captures that are not
    temporally adjacent. With frames.csv present this is now verified against
    the game's own frame counter and non-adjacent pairs are dropped.

    Returns (pairs, note) where note describes which rule was applied, so
    callers can print it rather than silently implying the strong one.
    """
    if indices is None:
        indices = frame_indices(dump_dir)
    index_set = set(indices)
    frames = load_frame_map(dump_dir)
    if frames is None:
        # A dump old enough to lack frames.csv also lacks manifest.json, so
        # load_dump() already refuses it before any caller reaches here -
        # this function is only ever called (via Dump.pairs()) on a dump
        # that passed the loader gate. No fallback rule is needed any more
        # (finding 1.17: the BURST constant this used to fall back to had
        # drifted from kCaptureFrames for most of the project's life).
        raise ValueError(
            f"{dump_dir} has no frames.csv, so pairing cannot be verified. This should be unreachable "
            f"through load_dump(), which refuses any dump this old before it gets here.")
    pairs = []
    dropped = 0
    for i in indices:
        if (i - 1) not in index_set:
            continue
        if i in frames and (i - 1) in frames and frames[i] - frames[i - 1] == 1:
            pairs.append((i - 1, i))
        else:
            dropped += 1
    return pairs, f"adjacency verified against frames.csv ({len(pairs)} adjacent, {dropped} rejected)"
