#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

namespace mv
{

// The per-frame surfaces a dump carries, in manifest order: the one list of
// their names. Velocity and colour are always captured, depth once identified,
// and the optional ones (from `kFirstOptionalSurface` on) only when
// `MV_SURFACES` names them; a frame without them is still complete. A new
// surface needs an entry here and in capture.cpp's `kCaptureFacts`, and a spec
// in `profiles/ue5.json`.
constexpr int kSurfaceVelocity = 0;
constexpr int kSurfaceDepth = 1;
constexpr int kSurfaceColor = 2;    // the back buffer, known by construction
constexpr int kSurfaceNormals = 3;  // GBufferA
constexpr int kSurfaceSpecular = 4; // GBufferB: metallic, specular, roughness, shading model
constexpr int kSurfaceCount = 5;
constexpr int kFirstOptionalSurface = kSurfaceNormals;
inline constexpr const char* kSurfaceNames[kSurfaceCount] = {"velocity", "depth", "color", "normals", "specular"};

// `kSurfaceNames` index of `name`, or -1.
int SurfaceIndex(const char* name);

// The original functions, which our own barriers and submissions call.
using ResourceBarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

void SetOriginalFunctions(ResourceBarrierFn resourceBarrier, ExecuteCommandListsFn executeCommandLists);

// Remembers the game's direct queue. The back-buffer copy is submitted there,
// so one fence covers it and the velocity copy.
void NoteCommandQueue(ID3D12CommandQueue* queue);

// The game's direct queue, once seen. The overlay submits there, after the
// frame it draws over.
ID3D12CommandQueue* GetGameQueue();

// Starts the F8 hotkey thread.
void StartCaptureHotkeyThread();

// Called from the barrier hook on an identified surface's trigger edge
// (`RENDER_TARGET` to shader-readable; depth: `DEPTH_WRITE` to readable), the
// one point a transient resource is complete and not yet reused. Copies it into
// this frame's slot and restores `stateAfter`, the state the game moved it to.
// Which of a frame's edges wins is per surface (`kCaptureFacts`). Ignored
// outside a burst and for `kSurfaceColor`.
void OnSurfaceReadable(int surface, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource, int stateAfter);

// `MV_SNAPSHOT=<surface>`: copies one frame of each of that surface's
// scene-extent candidates to `snapshot_<surface>_<ptr>_*.bin` in the dump
// directory. A no-op otherwise. The registry's `CandidateEdgeObserver`.
void SnapshotCandidate(
    const char* surfaceName, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource, int stateAfter);

// Called from the Present hooks before the real Present, while the back buffer
// holds the finished frame. Pairs it with the copies recorded earlier in the
// frame, submits, and drains earlier captures whose GPU work has completed.
void OnPresent(IDXGISwapChain* swapChain);

// The registry's `ReopenObserver`: `surfaceName`'s search reopened. Ends the
// burst in progress if it requires that surface (velocity always, depth if it
// was identified when the burst began).
void NotifySurfaceReopened(const char* surfaceName);

// The present index that pairs a frame's copies with its Present.
void SetFrameIndex(unsigned long long frameIndex);

// Called from the `ExecuteCommandLists` hook on every submission: checks that
// the velocity copy's command list goes to the fenced queue. A no-op outside a
// burst.
void NoteSubmission(ID3D12CommandQueue* queue, UINT numCommandLists, ID3D12CommandList* const* commandLists);

// Stops the F8 thread, leaving the writer alone. Joins, or detaches when
// `processExiting` (at `DLL_PROCESS_DETACH` Windows has already ended it).
void StopCaptureHotkeyThread(bool processExiting);

// Stops the writer thread. `processExiting` (from `DLL_PROCESS_DETACH`): the
// queue cannot be drained.
void ShutdownCapture(bool processExiting);

} // namespace mv
