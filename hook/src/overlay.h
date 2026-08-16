#pragma once

#include "capture.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>

namespace mv
{

// Draws captured surfaces into the game's back buffer at Present. F7 cycles the
// modes; `MV_OVERLAY=<n>` picks the starting one.
//
//   0  Off             Nothing drawn, no GPU cost.
//   1  SceneVelocity   The channels straight to RGB, as the engine's
//                      `visualizetexture SceneVelocity` shows them.
//   2  Over gameplay   The same, composited over the frame.
//   3  GBuffer grid    2x2: frame   | depth (log-scaled)
//                           normals | specular
//                      Raw channels. A quadrant whose surface was not
//                      refreshed this frame gets a red border. Normals and
//                      specular need `MV_SURFACES`.
//
enum class OverlayMode
{
    Off = 0,
    Velocity = 1,
    VelocityOverGameplay = 2,
    GBufferGrid = 3,
    Count = 4
};

// Called from the Present hooks before the real Present. `frame` is the
// present count, used to check that each copy being drawn is this frame's.
void OverlayOnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, uint64_t frame);

// Copies `surface` (a `capture.h` index) into the overlay's own texture for it,
// at the surface's trigger edge, where the transient source is complete. The
// first edge of a frame wins, except depth, which is copied again on each edge
// until velocity has been (its first edge can be a partial pass). A no-op
// unless the current mode draws that surface. `frame` stamps the copy.
void OverlayCaptureSurface(
    int surface, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source,
    D3D12_RESOURCE_STATES stateBefore, uint64_t frame);

// The overlay's barriers and submissions go through the original functions,
// not the hooks.
void OverlaySetOriginals(ResourceBarrierFn resourceBarrier, ExecuteCommandListsFn executeCommandLists);

void StartOverlayHotkeyThread();

// Stops the F7 thread. Detaches: at `DLL_PROCESS_DETACH` Windows has already
// ended it, and destroying a joinable `std::thread` terminates.
void StopOverlayHotkeyThread();

} // namespace mv
