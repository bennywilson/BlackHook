#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

namespace mv
{

// The per-frame surfaces a dump carries, in manifest order: the one list of
// their names. velocity and color are always captured, depth once identified;
// the optional ones (from kFirstOptionalSurface on) only when MV_SURFACES names
// them, and a frame without them is still complete. A new surface is an entry
// here and in capture.cpp's kCaptureFacts, plus a spec in profiles/ue5.json.
constexpr int kSurfaceVelocity = 0;
constexpr int kSurfaceDepth = 1;
constexpr int kSurfaceColor = 2;    // the back buffer, known by construction
constexpr int kSurfaceNormals = 3;  // GBufferA
constexpr int kSurfaceSpecular = 4; // GBufferB: metallic, specular, roughness, shading model
constexpr int kSurfaceCount = 5;
constexpr int kFirstOptionalSurface = kSurfaceNormals;
inline constexpr const char* kSurfaceNames[kSurfaceCount] = {"velocity", "depth", "color", "normals", "specular"};

// kSurfaceNames index of `name`, or -1.
int SurfaceIndex(const char* name);

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

// Called from the ResourceBarrier hook on an identified surface's trigger edge
// (RENDER_TARGET -> shader-resource; depth: DEPTH_WRITE -> readable) - the one
// point a transient resource is both finished and not yet recycled. Copies it
// into this frame's slot; `stateAfter` is the state the game moved it to, which
// the copy restores. Which edge of a frame wins is per surface (see
// kCaptureFacts). Ignored outside a burst or for kSurfaceColor.
void OnSurfaceReadable(int surface, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource, int stateAfter);

// DIAGNOSTIC: with MV_SNAPSHOT=<surface>, copies one frame of each of that
// surface's scene-extent candidates to snapshot_<surface>_<ptr>_*.bin in the
// dump directory (see capture.cpp). A no-op otherwise. Wired as the
// registry's CandidateEdgeObserver.
void SnapshotCandidate(
    const char* surfaceName, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource, int stateAfter);

// Called from the Present hooks *before* chaining to the real Present, while
// the back buffer still holds the finished frame. Pairs the back buffer with
// the velocity copy recorded earlier in the same frame, submits, and drains
// any earlier captures whose GPU work has completed.
void OnPresent(IDXGISwapChain* swapChain);

// The registry's ReopenObserver: `surfaceName`'s search reopened. Ends the
// burst in progress if that surface is required for it (velocity always,
// depth if it was identified when the burst began); a no-op otherwise.
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
