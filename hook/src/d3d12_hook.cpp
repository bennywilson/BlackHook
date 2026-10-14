#include "d3d12_hook.h"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <MinHook.h>

#include <atomic>
#include <memory>
#include <string>

#include "capture.h"
#include "logging.h"
#include "overlay.h"
#include "surface_profile.h"
#include "surface_registry.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace mv
{
namespace
{

// Vtable slot indices, verified against the Windows SDK IDL (dxgi.idl,
// dxgi1_2.idl, d3d12.idl - 10.0.26100.0), not guessed from memory: each
// interface's vtable is [inherited IUnknown/base methods][this interface's
// own methods in declaration order]. COM guarantees these indices are
// stable across vendors and OS versions.
//
// IDXGISwapChain:
//      [00-02] IUnknown                (QueryInterface, AddRef, Release)
//      [03-06] IDXGIObject
//      [07]    IDXGIDeviceSubObject    (GetDevice)
//      [08-17] IDXGISwapChain          (Present = 8, GetBuffer = 9, ...)
//
//  IDXGISwapChain1:
//      [00-17] Inherits IDXGISwapChain
//      [18-21] IDXGISwapChain1         (GetDesc1 .. GetCoreWindow)
//      [22]    Present1                <-- Target hook
//
//
//  ID3D12CommandQueue:
//      [00-02] IUnknown
//      [03-06] ID3D12Object
//      [07]    ID3D12DeviceChild/ ID3D12Pageables
//      [08]    UpdateTileMappings
//      [09]    CopyTileMappings
//      [10]    ExecuteCommandLists  <-- Target hook
//
//  ID3D12GraphicsCommandList:
//      [00-07] Base Interfaces      (IUnknown through ID3D12Pageable)
//      [08]    ID3D12CommandList    (GetType)
//      [09]    Close
//      [26]    ResourceBarrier      <-- Target hook

constexpr size_t kPresentIndex = 8;
constexpr size_t kPresent1Index = 22;
constexpr size_t kExecuteCommandListsIndex = 10;
constexpr size_t kResourceBarrierIndex = 26;

// Idiomatic way to hook COM interfaces in C++ using MinHook
void* VTableEntry(void* instance, size_t index)
{
    void** vtable = *reinterpret_cast<void***>(instance);
    return vtable[index];
}

// Hook Function Pointer Signatures.
// Note: First parameter is always the implicit 'this' interface pointer.
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ExecuteCommandLists_t = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using ResourceBarrier_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);

Present_t g_originalPresent = nullptr;
Present1_t g_originalPresent1 = nullptr;
ExecuteCommandLists_t g_originalExecuteCommandLists = nullptr;
ResourceBarrier_t g_originalResourceBarrier = nullptr;

// Global frame key: single source of truth for pairing velocity and colour
// captures across both Present paths. Atomic since multiple threads record
// command lists in parallel; the counters below are diagnostics only.
std::atomic<uint64_t> g_frameCounter{0};

std::atomic<uint64_t> g_presentCount{0};
std::atomic<uint64_t> g_present1Count{0};
std::atomic<uint64_t> g_executeCommandListsCount{0};
std::atomic<uint64_t> g_resourceBarrierCount{0};

// Periodic summary of high-frequency hooks
void LogHeartbeatIfDue()
{
    // Gate on activity from *either* Present path - modern D3D12 titles
    // (UE5 included) almost always present via IDXGISwapChain3::Present1
    // (flip-model/HDR/VRR support), so gating on legacy Present alone would
    // silently never fire even with everything working correctly.
    const uint64_t n = g_presentCount.load(std::memory_order_relaxed) + g_present1Count.load(std::memory_order_relaxed);
    if (n <= 5 || n % 300 == 0)
    {
        // Total alongside individual Present/Present1 counts reveals which
        // swapchain API the engine uses, rather than masking it in a sum.
        Log("heartbeat: frames=" + std::to_string(n) +
            " present=" + std::to_string(g_presentCount.load(std::memory_order_relaxed)) +
            " present1=" + std::to_string(g_present1Count.load(std::memory_order_relaxed)) +
            " executeCommandLists=" + std::to_string(g_executeCommandListsCount.load(std::memory_order_relaxed)) +
            " resourceBarrier=" + std::to_string(g_resourceBarrierCount.load(std::memory_order_relaxed)));
    }
}

// These hooks fully replace the game's Present/ExecuteCommandLists/etc.
// Every path through them MUST call the original.
//
// The capture hand-off happens *before* the counter is incremented and
// before chaining to the real Present: the back buffer still holds the
// finished frame at this point, and the frame index must match the one
// ResourceBarrier used earlier in the same frame so velocity/colour pair up.
//
// Back-buffer size is read every frame until identification has an answer
// (the search uses it), then polled (the resize check uses it).

// Window resizing abandons the identified velocity resource which requires
// reidentifying it. Poll every 30 frames
constexpr uint64_t kResizePollFrames = 30;

void NoteBackBufferSizeIfNeeded(IDXGISwapChain* const swapChain, const uint64_t frame)
{
    if (!swapChain)
    {
        return;
    }

    if (mv::GetSurfaceRegistry().Selected("velocity") && (frame % kResizePollFrames))
    {
        return;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    if (SUCCEEDED(swapChain->GetDesc(&desc)))
    {
        mv::GetSurfaceRegistry().NoteBackBufferSize(desc.BufferDesc.Width, desc.BufferDesc.Height);
    }
}

// Known-source exclusion (docs/REFACTOR_PLAN.md sec 3.3), fed from here on
// EVERY Present - not from capture.cpp, which only reached it mid-burst. Until
// Phase 10 nothing noticed: no surface had a format matching the back buffer.
// Normals does (R8G8B8A8_UNORM), so outside a burst its structural shortlist
// was holding references to the swapchain's buffers - which hung process exit
// (flip-model teardown with outstanding buffer refs) and would make a real
// game's ResizeBuffers fail. All buffers, not just the current one: a buffer
// can take a barrier before it is first presented, and the registry purges a
// newly-noted one from any shortlist that got there first.
void NoteKnownSources(IDXGISwapChain* const swapChain)
{
    if (!swapChain)
    {
        return;
    }
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc)))
    {
        return;
    }
    for (UINT i = 0; i < desc.BufferCount; ++i)
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        if (SUCCEEDED(swapChain->GetBuffer(i, IID_PPV_ARGS(&buffer))) && buffer)
        {
            mv::GetSurfaceRegistry().NoteKnownSourceResource(buffer.Get());
        }
    }
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    const uint64_t frame = g_frameCounter.load(std::memory_order_relaxed);
    mv::SetFrameIndex(frame);
    NoteKnownSources(swapChain);
    NoteBackBufferSizeIfNeeded(swapChain, frame);
    mv::GetSurfaceRegistry().OnFrame(frame);
    mv::OnPresent(swapChain);
    mv::OverlayOnPresent(swapChain, mv::GetGameQueue(), frame);
    g_frameCounter.fetch_add(1, std::memory_order_relaxed);
    g_presentCount.fetch_add(1, std::memory_order_relaxed);
    LogHeartbeatIfDue();
    return g_originalPresent(swapChain, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE
HookPresent1(IDXGISwapChain1* swapChain, UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* presentParams)
{
    const uint64_t frame = g_frameCounter.load(std::memory_order_relaxed);
    mv::SetFrameIndex(frame);
    NoteKnownSources(swapChain);
    NoteBackBufferSizeIfNeeded(swapChain, frame);
    mv::GetSurfaceRegistry().OnFrame(frame);
    mv::OnPresent(swapChain);
    mv::OverlayOnPresent(swapChain, mv::GetGameQueue(), frame);
    g_frameCounter.fetch_add(1, std::memory_order_relaxed);
    g_present1Count.fetch_add(1, std::memory_order_relaxed);
    LogHeartbeatIfDue();
    return g_originalPresent1(swapChain, syncInterval, flags, presentParams);
}

void STDMETHODCALLTYPE
HookExecuteCommandLists(ID3D12CommandQueue* queue, UINT numCommandLists, ID3D12CommandList* const* commandLists)
{
    g_executeCommandListsCount.fetch_add(1, std::memory_order_relaxed);
    // Remember the game's graphics queue so our own back-buffer copy can be
    // submitted to it (and covered by the same fence as the velocity copy).
    mv::NoteCommandQueue(queue);
    // Check that the list carrying the velocity copy is submitted to that queue before we signal the fence.
    mv::NoteSubmission(queue, numCommandLists, commandLists);
    g_originalExecuteCommandLists(queue, numCommandLists, commandLists);
}

// Re-entrancy guard for the barrier hook.
//
// Under the D3D12 debug layer, every ResourceBarrier call reaches this hook
// twice: the validation layer forwards to the core layer through the same
// patched code, so one app call arrives as two nested calls. Left
// unguarded, this doubles every count (the real velocity buffer scores 4
// and gets rejected) and the continuity check sees RT->SRV twice and flags
// a violation that never happened - both behavioural filters invert under
// instrumentation.
thread_local int g_barrierHookDepth = 0;

void STDMETHODCALLTYPE
HookResourceBarrier(ID3D12GraphicsCommandList* cmdList, UINT numBarriers, const D3D12_RESOURCE_BARRIER* barriers)
{
    ++g_barrierHookDepth;
    g_originalResourceBarrier(cmdList, numBarriers, barriers);
    const int depth = --g_barrierHookDepth;
    if (depth > 0)
    {
        // An inner, forwarded call. The outer one describes the same barriers
        // and will do the work; doing it here as well double-counts everything.
        return;
    }
    g_resourceBarrierCount.fetch_add(1, std::memory_order_relaxed);

    if (!barriers)
    {
        return;
    }

    // Recurrence logging for known candidates
    const uint64_t frame = g_frameCounter.load(std::memory_order_relaxed);
    mv::SurfaceRegistry& registry = mv::GetSurfaceRegistry();
    for (UINT i = 0; i < numBarriers; ++i)
    {
        switch (barriers[i].Type)
        {
            case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
            {
                ID3D12Resource* const resource = barriers[i].Transition.pResource;
                const int stateBefore = static_cast<int>(barriers[i].Transition.StateBefore);
                const int stateAfter = static_cast<int>(barriers[i].Transition.StateAfter);
                registry.ConsiderResource(resource);

                if (mv::DiagEnabled() && registry.IsAnyCandidate(resource))
                {
                    LogTo(
                        "candidates",
                        "frame=" + std::to_string(frame) +
                            " ptr=" + std::to_string(reinterpret_cast<uintptr_t>(resource)) + " TRANSITION before=" +
                            std::to_string(stateBefore) + " after=" + std::to_string(stateAfter));
                }

                // Profiles the transition, tracks liveness, and returns the
                // surfaces whose selected (or rival-adopted) resource just took
                // its trigger edge. A reopen it triggers reaches capture.cpp
                // through the registry's ReopenObserver.
                constexpr int kMaxTriggers = 4; // mutual exclusion: at most one in practice
                mv::SurfaceTrigger triggers[kMaxTriggers];
                const int triggerCount = registry.OnBarrierTransition(resource, frame, stateBefore, stateAfter, triggers, kMaxTriggers);
                for (int t = 0; t < triggerCount; ++t)
                {
                    const mv::SurfaceTrigger& trig = triggers[t];
                    const int surface = mv::SurfaceIndex(trig.identifier->Spec().name);
                    mv::SetFrameIndex(frame);
                    mv::OnSurfaceReadable(surface, cmdList, trig.resource, trig.stateAfter);
                    // The live overlay copies at the same edge, on the GPU.
                    mv::OverlayCaptureSurface(surface, cmdList, trig.resource,
                                              static_cast<D3D12_RESOURCE_STATES>(trig.stateAfter), frame);
                }
                break;
            }
            case D3D12_RESOURCE_BARRIER_TYPE_UAV:
            {
                ID3D12Resource* resource = barriers[i].UAV.pResource;
                registry.ConsiderResource(resource);
                registry.OnUavBarrier(resource);
                if (mv::DiagEnabled() && registry.IsAnyCandidate(resource))
                {
                    LogTo(
                        "candidates",
                        "frame=" + std::to_string(frame) +
                            " ptr=" + std::to_string(reinterpret_cast<uintptr_t>(resource)) + " UAV");
                }
                break;
            }
            default:
            {
                break;
            }
        }
    }

    // Which edges share one ResourceBarrier call: the shares_call_with_edge
    // signal's input, plus MV_DIAG / MV_SNAPSHOT. One branch when none is in use.
    if (registry.WantsBarrierBatches())
    {
        registry.OnBarrierBatch(cmdList, barriers, numBarriers);
    }
}

bool CreateHook(void* target, void* detour, void** original, const char* name)
{
    const MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK)
    {
        Log(std::string("MH_CreateHook failed for ") + name + ": " + MH_StatusToString(status));
        return false;
    }
    return true;
}

} // namespace

bool InstallD3D12Hooks()
{
    // Register a message-only window to create a swapchain against
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"MvHookDummyWindowClass";
    RegisterClassExW(&windowClass);

    HWND hwnd = CreateWindowExW(
        0,
        windowClass.lpszClassName,
        L"",
        WS_POPUP,
        0,
        0,
        64,
        64,
        HWND_MESSAGE,
        nullptr,
        windowClass.hInstance,
        nullptr);
    if (!hwnd)
    {
        Log("InstallD3D12Hooks: CreateWindowExW failed: " + std::to_string(GetLastError()));
        return false;
    }

    // Select the high-performance adapter, in case the user is on a
    // hybrid-graphics laptop.
    ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        Log("InstallD3D12Hooks: CreateDXGIFactory1 failed: 0x" + std::to_string(static_cast<unsigned long>(hr)));
        DestroyWindow(hwnd);
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    if (FAILED(hr))
    {
        Log("InstallD3D12Hooks: EnumAdapterByGpuPreference failed: 0x" +
            std::to_string(static_cast<unsigned long>(hr)) + ", falling back to default adapter");

        // nullptr falls back to system default
        adapter.Reset();
    }
    else
    {
        DXGI_ADAPTER_DESC1 adapterDesc{};
        adapter->GetDesc1(&adapterDesc);
        char nameBuf[128]{};
        WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, nameBuf, sizeof(nameBuf), nullptr, nullptr);
        Log(std::string("InstallD3D12Hooks: selected high-performance adapter: ") + nameBuf);
    }

    ComPtr<ID3D12Device> device;
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr))
    {
        Log("InstallD3D12Hooks: D3D12CreateDevice failed: 0x" + std::to_string(static_cast<unsigned long>(hr)));
        DestroyWindow(hwnd);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> commandQueue;
    hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue));
    if (FAILED(hr))
    {
        Log("InstallD3D12Hooks: CreateCommandQueue failed: 0x" + std::to_string(static_cast<unsigned long>(hr)));
        DestroyWindow(hwnd);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.BufferCount = 2;
    swapChainDesc.Width = 64;
    swapChainDesc.Height = 64;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain;
    hr = factory->CreateSwapChainForHwnd(commandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &swapChain);
    if (FAILED(hr))
    {
        Log("InstallD3D12Hooks: CreateSwapChainForHwnd failed: 0x" + std::to_string(static_cast<unsigned long>(hr)));
        DestroyWindow(hwnd);
        return false;
    }

    // 6. Dummy command list (to get ID3D12GraphicsCommandList's vtable).
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator));
    if (FAILED(hr))
    {
        Log("InstallD3D12Hooks: CreateCommandAllocator failed: 0x" + std::to_string(static_cast<unsigned long>(hr)));
        DestroyWindow(hwnd);
        return false;
    }

    ComPtr<ID3D12GraphicsCommandList> commandList;
    hr = device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(), nullptr, IID_PPV_ARGS(&commandList));
    if (FAILED(hr))
    {
        Log("InstallD3D12Hooks: CreateCommandList failed: 0x" + std::to_string(static_cast<unsigned long>(hr)));
        DestroyWindow(hwnd);
        return false;
    }
    commandList->Close();

    // Extract vtable pointers and install hooks. MinHook patches the
    // underlying function code (shared by every instance of a given
    // interface), not anything specific to our dummy instance - once
    // patched, every caller, including the game, goes through our hook.
    Log("InstallD3D12Hooks: dummy device/swapchain/command-list created, installing hooks");

    if (MH_Initialize() != MH_OK)
    {
        Log("InstallD3D12Hooks: MH_Initialize failed");
        DestroyWindow(hwnd);
        return false;
    }

    bool ok = true;
    ok &= CreateHook(
        VTableEntry(swapChain.Get(), kPresentIndex),
        reinterpret_cast<void*>(&HookPresent),
        reinterpret_cast<void**>(&g_originalPresent),
        "Present");
    ok &= CreateHook(
        VTableEntry(swapChain.Get(), kPresent1Index),
        reinterpret_cast<void*>(&HookPresent1),
        reinterpret_cast<void**>(&g_originalPresent1),
        "Present1");
    ok &= CreateHook(
        VTableEntry(commandQueue.Get(), kExecuteCommandListsIndex),
        reinterpret_cast<void*>(&HookExecuteCommandLists),
        reinterpret_cast<void**>(&g_originalExecuteCommandLists),
        "ExecuteCommandLists");
    ok &= CreateHook(
        VTableEntry(commandList.Get(), kResourceBarrierIndex),
        reinterpret_cast<void*>(&HookResourceBarrier),
        reinterpret_cast<void**>(&g_originalResourceBarrier),
        "ResourceBarrier");

    // Wire the surface registry before any hook can fire (docs/
    // REFACTOR_PLAN.md sec 2.5, Phase 8). Order matters: velocity must
    // register before depth, since depth's spec.extentFrom names
    // "velocity" and Configure() resolves that name by looking it up in
    // what's already registered.
    //
    // The specs come from the active profile (Phase 9). If it failed to load
    // nothing is registered: hooks still install, so the game runs and the
    // log explains, but no surface is ever identified or captured - there is
    // no built-in spec to fall back to (surface_profile.h).
    if (const mv::SurfaceProfile* profile = mv::LoadSurfaceProfile())
    {
        // Every surface is a plain SurfaceIdentifier, configured entirely by
        // its spec - including rival adoption, which velocity's subclass used
        // to own until Phase 10 found normals needed it too.
        mv::GetSurfaceRegistry().Register(std::make_unique<mv::SurfaceIdentifier>(profile->velocity));
        mv::GetSurfaceRegistry().Register(std::make_unique<mv::SurfaceIdentifier>(profile->depth));
        // Optional surfaces, after velocity (their extent source), each only
        // if its spec exists and MV_SURFACES enabled it.
        for (const mv::SurfaceProfile::OptionalSurface& optional : profile->optional)
        {
            if (optional.present && optional.spec.enabled)
            {
                mv::GetSurfaceRegistry().Register(std::make_unique<mv::SurfaceIdentifier>(optional.spec));
            }
        }
    }
    mv::GetSurfaceRegistry().SetReopenObserver(&mv::NotifySurfaceReopened);
    if (GetEnvironmentVariableA("MV_SNAPSHOT", nullptr, 0) != 0)
    {
        mv::GetSurfaceRegistry().SetCandidateEdgeObserver(&mv::SnapshotCandidate);
    }
    mv::GetSurfaceRegistry().Configure();

    if (ok)
    {
        const MH_STATUS enableStatus = MH_EnableHook(MH_ALL_HOOKS);
        if (enableStatus != MH_OK)
        {
            Log(std::string("InstallD3D12Hooks: MH_EnableHook failed: ") + MH_StatusToString(enableStatus));
            ok = false;
        }
    }

    // Hand the capture module the *original* function pointers
    if (ok)
    {
        mv::SetOriginalFunctions(g_originalResourceBarrier, g_originalExecuteCommandLists);
        mv::StartCaptureHotkeyThread();
        mv::OverlaySetOriginals(g_originalResourceBarrier, g_originalExecuteCommandLists);
        mv::StartOverlayHotkeyThread();
    }

    // Tear down temp objects
    DestroyWindow(hwnd);
    UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);

    Log(std::string("InstallD3D12Hooks: ") + (ok ? "all hooks installed" : "one or more hooks failed"));
    return ok;
}

} // namespace mv
