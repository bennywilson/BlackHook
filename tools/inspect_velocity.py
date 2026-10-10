"""Sanity-check a captured velocity buffer before trusting it.

Reports how much of the buffer was actually written, and the distribution of
decoded displacements - the first thing to look at when deciding whether a
capture contains real motion or whether the game was sitting still.
"""
import os
import sys
import numpy as np
import mvtools
import paths

dump_dir = sys.argv[1] if len(sys.argv) > 1 else paths.DEFAULT_DUMP
dump = mvtools.load_dump(dump_dir)
meta = dump.meta
print("meta:", meta)

# State the two things the decode depends on that are NOT in the pixel data, so
# every run of this tool says out loud which assumptions produced its numbers.
channels, bpp = mvtools.velocity_channels(meta)
version = mvtools.engine_version(meta)
if "engine_version_major" in meta:
    recorded = "recorded in the capture"
elif os.environ.get("MV_ENGINE_VERSION"):
    recorded = "from MV_ENGINE_VERSION; not recorded in the capture"
else:
    recorded = ("NOT recorded and not supplied - falling back to mvtools.ENGINE_VERSION, so "
                "channels 2/3 below are decoded under an assumption that may be wrong")
print(f"velocity format {meta['velocity_format']}: {channels} channels, {bpp} bytes/pixel")
print(f"decoding as UE {version[0]}.{version[1]} ({recorded})\n")

indices = mvtools.frame_indices(dump_dir)
print(f"{len(indices)} captured frames: {indices[0]}..{indices[-1]}\n")

for i in indices[:: max(1, len(indices) // 6)]:
    path = os.path.join(dump_dir, f"vel_{i:05d}.bin")
    raw = mvtools.load_velocity_raw(path, meta)
    written = mvtools.velocity_written_mask(path, meta)
    flow = mvtools.decode_velocity(path, meta)

    h, w = flow.shape[:2]
    px = np.stack([flow[:, :, 0] * w, flow[:, :, 1] * h], axis=2)
    mag = np.sqrt((px ** 2).sum(axis=2))

    frac = written.mean()
    print(f"frame {i:5d}: written={frac*100:6.2f}%  "
          f"raw_ch0=[{raw[:,:,0].min():.4f},{raw[:,:,0].max():.4f}] "
          f"raw_ch1=[{raw[:,:,1].min():.4f},{raw[:,:,1].max():.4f}]")
    if frac > 0:
        m = mag[written]
        print(f"              motion(px): mean={m.mean():8.3f} p50={np.percentile(m,50):8.3f} "
              f"p95={np.percentile(m,95):8.3f} max={m.max():8.3f}")
        # Channels 2 and 3 are the two halves of one float32 (the DeviceZ
        # delta), so their individual ranges say nothing. Report the value they
        # encode instead - printing raw_ch2/raw_ch3 ranges is what made them
        # look like a bimodal channel and a noise channel for so long.
        #
        # A PF_G16R16 title has no channels 2/3 at all, because the same engine
        # predicate (NeedVelocityDepth) picks both the format and whether V.z is
        # encoded. That is a fact about the capture, not a failure.
        if channels < 4:
            print("              V.z: not encoded - this is a 2-channel PF_G16R16 capture, so "
                  "NeedVelocityDepth() was false on this title")
        else:
            vz, anim = mvtools.decode_velocity_depth(path, meta)
            sel = vz[written]
            flag = (f"bHasPixelAnimation on {anim[written].mean()*100:.1f}%" if anim is not None
                    else "bHasPixelAnimation not encoded in this engine version")
            print(f"              V.z (DeviceZ delta): mean={sel.mean():+.3e} "
                  f"p05={np.percentile(sel,5):+.3e} p95={np.percentile(sel,95):+.3e}  {flag}")
