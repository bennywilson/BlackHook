#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

namespace mv
{

// Function-pointer types for the originals we must call through
using ResourceBarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

void SetOriginalFunctions(ResourceBarrierFn resourceBarrier, ExecuteCommandListsFn executeCommandLists);

// Remembers the queue the game submits on. The back-buffer copy is recorded
// on our own list and must land on this queue so one fence covers both it
// and the velocity copy.
void NoteCommandQueue(ID3D12CommandQueue* queue);

// The game's direct queue, once seen. The overlay submits its composite pass
// here so it lands on the same queue as the frame it is drawing over.
ID3D12CommandQueue* GetGameQueue();

// Starts the hotkey polling thread (F8 = begin a capture burst).
void StartCaptureHotkeyThread();

// Called from the ResourceBarrier hook when velocity transitions
// RENDER_TARGET -> shader-resource: the only point where it's both final and
// still valid, since it's a transient resource whose heap memory gets
// recycled later in the frame.
//
// `stateAfter` is whatever the game's own barrier moved it to (e.g.
// ALL_SHADER_RESOURCE or PIXEL_SHADER_RESOURCE, title-dependent); our copy
// restores that same state afterward.
void OnVelocityReadable(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* velocity, int stateAfter);

// Called from the ResourceBarrier hook when scene depth transitions
// DEPTH_WRITE -> readable. Unlike velocity this can fire multiple times per
// frame (prepass + base-pass edges); each copy overwrites the same readback
// buffer, so the last one in the frame is what lands.
void OnDepthReadable(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* depth, int stateAfter);

// Called from the ResourceBarrier hook on GBufferA's RENDER_TARGET ->
// shader-resource edge, when MV_SURFACES=normals registered the normals
// identifier. One copy per frame (the first edge). Optional: a frame without
// it is still written.
void OnNormalsReadable(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* normals, int stateAfter);

// Called from the Present hooks *before* chaining to the real Present, while
// the back buffer still holds the finished frame. Pairs the back buffer with
// the velocity copy recorded earlier in the same frame, submits, and drains
// any earlier captures whose GPU work has completed.
void OnPresent(IDXGISwapChain* swapChain);

// Called by d3d12_hook.cpp whenever SurfaceRegistry::OnBarrierTransition or
// ::NoteBackBufferSize reports that `surfaceName`'s search reopened. Ends
// the current burst (docs/REFACTOR_PLAN.md finding 1.14) if that surface is
// required for it and a burst is in progress; a no-op otherwise. `surfaceName`
// matches a SurfaceSpec::name - "velocity", "depth" or "normals" (never
// required, so its reopen never ends a burst).
void NotifySurfaceReopened(const char* surfaceName);

// Current present index, used to pair velocity and colour within one frame.
void SetFrameIndex(unsigned long long frameIndex);

// Called from the ExecuteCommandLists hook on every submission. Confirms
// the velocity copy's command list was actually submitted to the fenced
// queue. Cheap no-op outside a capture burst.
void NoteSubmission(ID3D12CommandQueue* queue, UINT numCommandLists, ID3D12CommandList* const* commandLists);

// Stops the F8 hotkey poller, without touching the writer thread. Joins if
// `processExiting` is false; detaches if true (DLL_PROCESS_DETACH, where
// Windows has already killed every other thread) - see the definition.
void StopCaptureHotkeyThread(bool processExiting);

// Stops the writer thread. `processExiting` should be true from
// DLL_PROCESS_DETACH, where the queue cannot be drained - see the definition.
void ShutdownCapture(bool processExiting);

} // namespace mv
