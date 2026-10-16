"""Decoding helpers for the raw buffers dumped by the hook.

The hook writes untouched GPU bytes plus a manifest.json describing them, so
all format knowledge lives here, out of the injected DLL.

`load_dump()` opens a dump: it checks `schema_version`, refuses an unsealed dump
unless told otherwise, and returns a `Dump` whose surfaces are typed by how they
were found: `IdentifiedSurface` (found by search) or `KnownSurface` (named by an
API: colour). The decode functions take the flat dict `Dump.meta`.
"""
import json
import os
import sys
from dataclasses import dataclass, field
from typing import Optional

import numpy as np

# DXGI formats seen in captures, from dxgiformat.h (Windows SDK 10.0.26100.0).
# R16G16B16A16_FLOAT is 10 and R16G16B16A16_UNORM is 11; mistaking one for the
# other cost this project more than any other error (DEBUGGING.md).
DXGI_R16G16B16A16_UNORM = 11
DXGI_R16G16B16A16_UINT = 12
DXGI_R10G10B10A2_UNORM = 24
DXGI_R8G8B8A8_UNORM = 28
DXGI_R16G16_UNORM = 35
DXGI_R16G16_UINT = 36
DXGI_B8G8R8A8_UNORM = 87

# The four formats `FVelocityRendering::GetFormat` can return
# (VelocityRendering.cpp:758), through the D3D12 RHI's PF_* -> DXGI table
# (D3D12RHI.cpp:139,144):
#
#     bNeedVelocityDepth ? PF_A16B16G16R16 : PF_G16R16              (desktop)
#     bNeedVelocityDepth ? PF_R16G16B16A16_UINT : PF_R16G16_UINT    (OpenGL/GLES)
#
# -> (channels, bytes per pixel). The UINT variants hold the same 0..65535
# values (`return uint4(EncodedV * 65535.0 + 0.5f)`, Common.ush:2081), so dividing
# by 65535 gives the same [0,1] value and the same decode.
VELOCITY_FORMATS = {
    DXGI_R16G16B16A16_UNORM: (4, 8),
    DXGI_R16G16B16A16_UINT: (4, 8),
    DXGI_R16G16_UNORM: (2, 4),
    DXGI_R16G16_UINT: (2, 4),
}


# ---------------------------------------------------------------------------
# The manifest loader. Every other function here takes what it produces.

SUPPORTED_SCHEMA = 1
# Older schema versions this build can read, each mapped to a function that
# upgrades its manifest dict. None yet: version 1 is the only one.
_MIGRATIONS = {}


@dataclass(frozen=True)
class Plane:
    """One D3D12 subresource plane's footprint. Depth has two (depth and
    stencil); the other surfaces have one."""
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
    """A surface found by the structural and behavioural search: velocity,
    depth, and the optional normals and specular.

    No score or signal fields: the hook logs its identification evidence in
    mv_hook.log but does not write it to the manifest.
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

    # Plane 0 (the top mip, first plane) is what every tool reads.
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
    """A surface named by an API rather than found: colour, from
    `IDXGISwapChain::GetBuffer`.

    It has no identification fields at all, so code that reads them off colour
    raises AttributeError instead of getting None.
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
        """The flat dict every decode function in this file takes, built from
        the manifest on each access."""
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
    """Opens a dump directory through its manifest.json. Refuses a dump
    without one, an unknown schema version, or an unsealed manifest (unless
    `allow_unsealed`)."""
    manifest_path = os.path.join(dump_dir, "manifest.json")
    if not os.path.exists(manifest_path):
        legacy = os.path.join(dump_dir, "meta.txt")
        if os.path.exists(legacy):
            raise ValueError(
                f"{dump_dir} is a pre-manifest dump (meta.txt but no manifest.json). Backfill one with "
                f"legacy_meta.py, or re-capture - load_dump() only reads manifest.json.")
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
    """World-space normals for one captured frame. Always refuses.

    How UE packs the normal into GBufferA (octahedral or direct, and whether
    that differs between 5.2 and 5.7) is not verified, and the manifest says so
    (`"encoding": {"kind": "unknown"}`). Decoding on a guess would give
    believable but wrong normals. Once the encoding is known, the hook should
    write it into the manifest and this should decode that kind only.
    """
    surface = dump.surface("normals")  # raises if not captured, with the manifest's reason
    kind = (surface.encoding or {}).get("kind", "unknown")
    raise ValueError(
        f"refusing to decode normals (frame {capture_index}): manifest encoding is '{kind}', and this "
        f"mvtools.py knows no verified normals encoding. The raw bytes are in "
        f"{surface.file_pattern % capture_index} if you need them for investigation.")


def _rows(path, width, height, row_pitch, bytes_per_pixel):
    """Un-pads a GPU row-pitched buffer into a tight (height, width*bpp) array.

    D3D12 pads each row to a multiple of 256 bytes, and the last row is not
    padded: the file is row_pitch*(height-1) + width*bpp bytes.
    """
    raw = np.fromfile(path, dtype=np.uint8)
    useful = width * bytes_per_pixel
    out = np.empty((height, useful), dtype=np.uint8)
    for y in range(height):
        start = y * row_pitch
        out[y] = raw[start:start + useful]
    return out


def velocity_channels(meta):
    """(channels, bytes per pixel) of the captured velocity format.

    Read from the capture: a title without Lumen, distance fields or ray
    tracing gets PF_G16R16 (2 channels, 4 bytes) where others get
    R16G16B16A16_UNORM.
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
    """Raw velocity as float (h, w, channels) in [0,1].

    4 channels for PF_A16B16G16R16 (when velocity depth is needed), 2 for
    PF_G16R16. Callers that need channels 2 and 3 must check, as
    `decode_velocity_depth()` does.
    """
    channels, bpp = velocity_channels(meta)
    w, h = meta["velocity_width"], meta["velocity_height"]
    rows = _rows(path, w, h, meta["velocity_row_pitch"], bpp)
    u16 = rows.view(np.uint16).reshape(h, w, channels)
    return u16.astype(np.float32) / 65535.0


# ---------------------------------------------------------------------------
# The SceneVelocity encoding, from the encode/decode pair in
# Engine/Shaders/Private/Common.ush (UE 5.7.1).
#
# EncodeVelocityToTexture(), Common.ush:2060:
#
#     #if VELOCITY_ENCODE_GAMMA                          // 1 whenever SM5+
#         V.xy = sign(V.xy) * sqrt(abs(V.xy)) * (2.0 / sqrt(2.0));
#     #endif
#     EncodedV.xy = V.xy * (0.499f * 0.5f) + 32767.0f / 65535.0f;
#
# So the stored value is a square-root encoding of the clip-space delta.
# VELOCITY_ENCODE_GAMMA is 1 at every feature level from SM5 up (Common.ush:238),
# so always on for a D3D12 PC title; there is no cvar for it.
#
# The inverse, DecodeVelocityFromTexture(), Common.ush:2107:
#
#     V.xy = EncodedV.xy * InvDiv - 32767.0f/65535.0f * InvDiv;   // InvDiv = 1/(0.499*0.5)
#     #if VELOCITY_ENCODE_GAMMA
#         V.xy = (V.xy * abs(V.xy)) * 0.5;
#     #endif
#
# V is a clip-space delta, current minus previous, x right and y up. Two more
# engine facts fix the convention:
#
#   * `PrevScreenPos = ScreenPos - DecodeVelocityFromTexture(...).xy`
#     (PostProcessAmbientOcclusion.usf:1203, VisualizeMotionVectors.usf:78): V
#     points forward, and stepping back to the previous frame subtracts it.
#   * `ScreenPosToViewportUV(S) = float2(0.5 + 0.5*S.x, 0.5 - 0.5*S.y)`
#     (Common.ush:1658): clip to UV flips Y.
#
# Together they give the backward (current -> previous) UV displacement this
# module returns:
#
#     du = -0.5 * V.x        dv = +0.5 * V.y
#
# The vector is already de-jittered: Calculate3DVelocityBase()
# (VelocityCommon.ush:9) subtracts View.TemporalAAJitter from both positions
# before differencing, so the TAA/TSR offset must not be removed again.
#
# A linear fit to captured data looks right on one clip and is wrong elsewhere:
# it measures the local slope of the sqrt curve at that clip's typical speed.
ENCODE_ZERO = 32767.0 / 65535.0     # Common.ush:2069
ENCODE_SCALE = 0.499 * 0.5          # Common.ush:2069
ENCODE_GAMMA = True                 # Common.ush:238, SM5+ => always on here

# ---------------------------------------------------------------------------
# Engine version.
#
# The x/y encoding is the same in UE 5.2 and 5.7.1 (both guard
# VELOCITY_ENCODE_GAMMA on `FEATURE_LEVEL >= FEATURE_LEVEL_SM5` and decode with
# `V.xy = (V.xy * abs(V.xy)) * 0.5`). The depth channels differ: 5.7.1 uses the
# bottom 1-2 bits of channel 3 for bHasPixelAnimation and temporal
# responsiveness.
#
#   5.7.1:  V.z = asfloat((z << 16) | (w & VELOCITY_Z_LOW_MASK))   // 0xFFFE or 0xFFFC
#   5.2:    V.z = asfloat((z << 16) |  w)                          // no mask
#
# On 5.2 those bits are float32 mantissa, so the flag is decoded only where it
# is known to exist. Only 5.2 and 5.7.1 are verified; for other versions
# `depth_encoding()` warns and uses the nearer one.
#
# VELOCITY_Z_LOW_MASK also varies within 5.7.1 (Common.ush:2049-2055): 0xFFFC
# with VELOCITY_ENCODE_TEMPORAL_RESPONSIVENESS, else 0xFFFE. That depends on a
# per-shader-platform property a dump does not record, so the table uses
# 0xFFFE. If the title uses 0xFFFC, bit 1 stays in the mantissa (a ~1e-7
# relative error in V.z); bit 0, the only flag reported, is taken in both.
ENGINE_VERSION = (5, 7)

_VERIFIED_ENCODINGS = {
    # version: (velocity_z_low_mask, has_pixel_animation_bit)
    (5, 2): (0xFFFF, False),
    (5, 7): (0xFFFE, True),
}


def engine_version(meta=None):
    """The engine version to decode with: the capture's own if the manifest
    recorded one (it does when MV_ENGINE_VERSION was set for the capture),
    else MV_ENGINE_VERSION now, else `ENGINE_VERSION`.
    """
    if meta and "engine_version_major" in meta and "engine_version_minor" in meta:
        return (meta["engine_version_major"], meta["engine_version_minor"])
    # Not recorded: take MV_ENGINE_VERSION from the environment, which beats a
    # default that may be wrong for the title (and would then report
    # bHasPixelAnimation for an engine that has no such flag).
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

# UE5 clamps the encoded channel to [0,1], so this is the largest clip-space
# delta a capture can hold.
MAX_CLIP_DELTA = (ENCODE_ZERO / ENCODE_SCALE) ** 2 * 0.5 * 2.0


def decode_velocity_clip(path, meta):
    """SceneVelocity as the engine's clip-space delta.

    (h, w, 2) of `V.xy` as `DecodeVelocityFromTexture()` returns it: current
    minus previous, clip space, x right, y up. Use it to compare against engine
    code; use `decode_velocity()` to compare against images.
    """
    raw = load_velocity_raw(path, meta)
    v = (raw[:, :, :2] - ENCODE_ZERO) / ENCODE_SCALE
    if ENCODE_GAMMA:
        v = (v * np.abs(v)) * 0.5
    return v


def decode_velocity(path, meta):
    """SceneVelocity as screen-space UV displacement.

    The displacement from this frame's pixel back to where that surface was in
    the previous frame, in UV units. Meaningful only where
    `velocity_written_mask()` is true.
    """
    v = decode_velocity_clip(path, meta)
    out = np.empty(v.shape, dtype=np.float32)
    out[:, :, 0] = -0.5 * v[:, :, 0]
    out[:, :, 1] = +0.5 * v[:, :, 1]
    return out


def decode_velocity_depth(path, meta, version=None):
    """Channels 2 and 3: the DeviceZ delta, not a colour or a displacement.

    Under VELOCITY_ENCODE_DEPTH (Common.ush:2071) the float32 bit pattern of
    `V.z`, the DeviceZ delta (current minus previous), is split across the two
    spare 16-bit channels:

        EncodedV.z = (asuint(V.z) >> 16) & 0xFFFF
        EncodedV.w = (asuint(V.z)        & VELOCITY_Z_LOW_MASK) | ResponsivenessMask

    with the bottom 1-2 bits of w used for flags. Viewed separately, channel 2
    looks bimodal (it holds the sign bit) and channel 3 like noise.

    Returns (device_z_delta, has_pixel_animation). has_pixel_animation is None
    on engine versions that do not encode it (see `depth_encoding()`).
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
    """True where the velocity buffer holds written data.

    UE5 writes SceneVelocity only for pixels with motion beyond the camera's;
    elsewhere it holds its cleared value 0, which decodes to a large false
    displacement. The test is the engine's own, `EncodedV.x > 0`
    (`bIsRenderedVelocity`, VisualizeMotionVectors.usf:73): zero motion encodes
    to 32767, so 0 means never written.
    """
    raw = load_velocity_raw(path, meta)
    return raw[:, :, 0] > 0.0


# ---------------------------------------------------------------------------
# Scene depth.
#
# UE5 creates scene depth as PF_DepthStencil, which the D3D12 RHI maps to
# R24G8_TYPELESS or R32G8X24_TYPELESS depending on r.D3D12.UseD24
# (D3D12RHI.cpp:117/127). Both have two planes, depth then stencil, with
# different formats and row pitches; the manifest gives both.
#
# Values are DeviceZ as a shader reads them, reversed-Z: 1.0 is the near plane,
# 0.0 the far plane or sky. Nothing is linearised here.
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
        # Depth is the low 24 bits; the top 8 are padding (stencil is plane 1).
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
    """Back buffer to displayable RGB. The back buffer is already
    display-encoded, so no gamma is applied."""
    return np.clip(color, 0, 1).astype(np.float32)


def flow_to_rgb(flow_uv, meta, max_pixels=None, gamma=0.5, floor=0.18):
    """HSV colour wheel: hue is direction, brightness is magnitude.

    One frame can hold ~11 px/frame motion next to ~1 px/frame, so brightness
    follows magnitude**gamma (sqrt by default) with a floor for written pixels;
    hue is left linear, so colours compare across frames.
    """
    import cv2
    h, w = flow_uv.shape[:2]
    # UV to pixels, so the scale is in pixels.
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
    """capture index -> the game frame it came from, from frames.csv."""
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
    """(prev, cur) capture pairs that are adjacent game frames.

    Capture indices count what was drained, so a dropped frame, a busy slot or
    a second burst makes numerically adjacent captures that are not adjacent
    frames. Pairs are checked against frames.csv.

    Returns (pairs, note), the note saying how many were kept and rejected.
    """
    if indices is None:
        indices = frame_indices(dump_dir)
    index_set = set(indices)
    frames = load_frame_map(dump_dir)
    if frames is None:
        # Unreachable through `load_dump()`: a dump without frames.csv has no
        # manifest either.
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
