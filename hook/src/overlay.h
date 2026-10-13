#pragma once

#include "capture.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>

namespace mv
{

// Draws extracted surfaces into the game's back buffer at Present. F7 cycles
// through (MV_OVERLAY=<n> picks the starting mode):
//
//   0  Normal            - chained straight through, zero GPU cost
//   1  SceneVelocity     - the stored channels mapped straight to RGB, the
//                          same picture the engine's own
//                          "visualizetexture SceneVelocity" draws
//   2  Over gameplay     - the same, composited over the live frame
//   3  GBuffer grid      - 2x2: frame   | depth (log-scaled, last edge before velocity)
//                               normals | specular (metallic/spec/rough)
//                          Raw channels, no decode. A quadrant whose surface
//                          was not refreshed this frame gets a red border;
//                          normals/specular need MV_SURFACES to be identified.
//
enum class OverlayMode
{
    Off = 0,
    Velocity = 1,
    VelocityOverGameplay = 2,
    GBufferGrid = 3,
    Count = 4
};

// The surfaces the overlay keeps a GPU-side copy of. Depth, normals and
// specular are the grid's other three quadrants; velocity feeds modes 1-2 and
// paces depth's copy (see OverlayCaptureSurface).
enum class OverlaySurface
{
    Velocity = 0,
    Depth = 1,
    Normals = 2,
    Specular = 3,
    Count = 4
};

// Called from the Present hooks before chaining through, while the back buffer
// `frame` is the presented-frame counter. This function checsk that the velocity texture
// being drawn actually belongs to this frame.
void OverlayOnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, uint64_t frame);

// Records a copy of a surface into the overlay's own SRV-able texture for it.
// Called at the surface's trigger edge, where the source is known to hold
// valid contents (they are transient resources). The first edge of a frame
// wins - except depth, which keeps re-copying until velocity has been copied
// that frame (its first edge can be a partial pass). A no-op unless the
// current mode needs that surface. `frame` stamps the copy so
// OverlayOnPresent knows if it is this frame's.
void OverlayCaptureSurface(
    OverlaySurface surface, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source,
    D3D12_RESOURCE_STATES stateBefore, uint64_t frame);

// The overlay records its own barriers and submissions; they must go through
// the original function pointers, not our hooks, so they do not re-enter
// identification/logging.
void OverlaySetOriginals(ResourceBarrierFn resourceBarrier, ExecuteCommandListsFn executeCommandLists);

void StartOverlayHotkeyThread();

// Stops the F7 hotkey poller. Always detaches, never joins: by the time
// DLL_PROCESS_DETACH runs at process exit, Windows has already killed the
// thread, and destroying a still-joinable std::thread calls std::terminate().
void StopOverlayHotkeyThread();

} // namespace mv
