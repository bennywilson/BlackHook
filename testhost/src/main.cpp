// D3D12 validation harness for the capture path.
//
// WHY THIS EXISTS
//
// Every barrier-correctness claim this project makes used to rest on "the game
// didn't crash". That is weak evidence and it is weak in a specific way: the
// D3D12 debug layer is exactly the thing that catches a transition barrier
// asserting the wrong StateBefore, a copy sized from the wrong footprint, or a
// descriptor overwritten while still referenced - and none of it can be turned
// on inside a shipping game, because a Shipping build does not create its
// device with the debug layer and we cannot make it.
//
// The previous version of this file did nothing D3D12-related at all. It
// smoke-tested the MinHook bootstrap ("do six vtable patches land without
// crashing this process") and nothing else, so the whole capture path - the
// part with the actual risk in it - was never validated by anything but the
// absence of a crash in a program that would not necessarily crash.
//
// WHAT IT DOES
//
// Stands up a real D3D12 device with the debug layer AND GPU-based validation
// enabled, then builds a line-up of PLACED resources (not committed, because the
// real SceneVelocity buffer comes from UE5's TransientResourceAllocator and
// placed resources have their own aliasing rules the debug layer checks
// separately). One of them is a faithful look-alike of SceneVelocity; the rest
// are the plausible mistakes.
//
// Then it loads mv_hook.dll into itself and renders frames. The hook cannot
// tell the difference, so the real capture path runs against resources the
// debug layer is watching. Anything the layer objects to is retrieved from the
// info queue and printed, so the result is an artefact rather than something
// you had to have a debugger attached to see.
//
// WHAT IT ASSERTS, AND WHY THE LINE-UP EXISTS
//
// This used to place a single decoy with SceneVelocity's exact descriptor,
// hardcoded to the same 1212x760 the hook's filter was hardcoded to. That could
// only ever confirm that a constant matched itself, and the assertion it
// supported - "a capture happened" - stayed true throughout the multi-day
// episode in which the filter was selecting the wrong resource entirely (see
// DEBUGGING.md, the R16G16B16A16_FLOAT/_UNORM digit).
//
// So there are now two independent verdicts:
//
//   * debug-layer errors across the run must be zero (the original claim), and
//   * identification must pick the RIGHT resource out of the line-up, checked
//     by pointer through the DLL's MvIdentifiedVelocityResource export - not by
//     the presence of a dump.
//
// Each decoy is rejected by exactly one of the two filters the hook applies, so
// neither filter can be quietly load-bearing on its own. `--ambiguous` adds a
// perfect twin of the real buffer, where the only correct answer is to refuse
// to pick; that path is asserted too.
//
// `--pipeline skyrunner` swaps the real look-alike's barrier pattern for the
// one measured on a third-party UE 5.2 title - five transitions a frame, via
// UNORDERED_ACCESS - which every behavioural rule this filter once enforced
// would have rejected. Both pipelines must select the right resource, which is
// what turns "works across titles" from an anecdote into a test.
//
// `--phase9-negative` runs under testhost/profiles/phase9_negative.json, a
// profile narrowing velocity's format table to the wrong entry
// (docs/REFACTOR_PLAN.md Phase 9). The only correct outcome is a loud
// "NO candidate" and nothing selected - asserted against the hook's own log,
// and against MvActiveProfile so a profile that failed to load cannot pass
// for one that loaded and found nothing.
//
// USAGE
//   mv_testhost.exe [--hook <dll>] [--frames N] [--no-gbv] [--ambiguous]
//                   [--pipeline oxi|skyrunner] [--phase9-negative] [--normals]
//
// `--normals` (Phase 10) adds a GBufferA look-alike, sets MV_SURFACES=normals,
// and asserts normals selects it - with SceneVelocity, which passes the
// normals gates too, kept out by mutual exclusion - and that every captured
// frame carries a normals blob. Without it, no normals blob may appear.
//   mv_testhost.exe --wpo-encode-test <output_dir>
//
// Set MV_AUTOCAPTURE=1 in the environment to have the hook start a capture
// burst by itself, since there is no one here to press F8.
//
// --wpo-encode-test is unrelated to the rest of this file: it does not load
// the hook at all. It renders a known synthetic velocity grid through the
// real EncodeVelocityToTexture()/VELOCITY_ENCODE_DEPTH path (transcribed
// exactly, see wpo_encode_test.cpp) and dumps it in the same shape a real
// capture uses, so tools/wpo_synthetic_test.py can check that the shipping
// decode correctly recovers values representative of WPO-animated pixels -
// which the analytical reprojection cannot exercise, by construction.

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace {

// Deliberately NOT 1212x760. That was the resolution of the one title this
// project was developed against, and the harness used to hardcode it "to match
// the identification filter in d3d12_hook.cpp" - which meant the harness could
// only ever confirm that a filter matching a constant matched the same
// constant. Identification is now property-based, so the harness picks a
// resolution the hook has never been told about and the hook has to derive it.
constexpr UINT64 kVelocityWidth = 1120;
constexpr UINT kVelocityHeight = 630;
constexpr DXGI_FORMAT kVelocityFormat = DXGI_FORMAT_R16G16B16A16_UNORM;

constexpr UINT kBackBufferCount = 3;
constexpr UINT kWidth = 1280;
constexpr UINT kHeight = 720;

// What every look-alike is cleared to, chosen so all four channels decode to
// something checkable rather than only the two that carry displacement.
//
//   x, y = 0.5      -> exactly where UE5's encoding puts zero motion
//                      (32767/65535), so the decode must return 0.00000 px.
//   z, w            -> the two halves of a float32 bit pattern, because that is
//                      what VELOCITY_ENCODE_DEPTH packs there. 13696 = 0x3580
//                      in the high half and 0 in the low half reassemble to
//                      0x35800000 = +9.5367e-07, which is also the ~1e-6
//                      magnitude a real capture shows for the DeviceZ delta.
//
// The previous value cleared z to 0.5 as well, which reassembles to 0x7FFF0000
// - a NaN. That was not wrong, exactly: the harness only claimed to check the
// displacement channels, and it checked them correctly. But it meant the
// depth-channel decode was the one part of the pipeline the harness silently
// did not cover, and "NaN" is indistinguishable from a decode bug when you are
// reading tool output rather than reasoning about the clear value.
constexpr float kVelocityClear[4] = {0.5f, 0.5f, 13696.0f / 65535.0f, 0.0f};

// How each look-alike behaves at the barrier level. The decoys are chosen so
// that each one is rejected by exactly one of the two independent filters - a
// filter that is right for the wrong reason fails here rather than in a game.
enum class Behaviour {
    Velocity,       // RT -> shader-resource -> RT. Exactly 2 events per frame.
    // Velocity's shape, but the RT -> SRV edge goes out in ONE ResourceBarrier
    // call together with scene depth's DEPTH_WRITE -> DEPTH_READ - how GBufferA
    // was measured to become readable on Skyrunner (Phase 10).
    ReadWithDepth,
    ComputeWritten, // goes through UNORDERED_ACCESS and takes a UAV barrier
    ExtraPass,      // the same RT <-> SRV cycle, but twice per frame
    // Measured on a third-party UE 5.2 title, and the reason --pipeline exists.
    // Its SceneVelocity does five transitions a frame and passes through
    // UNORDERED_ACCESS on the way:
    //     RT->UAV   UAV->NON_PS_SRV   NON_PS_SRV->RT   RT->NON_PS_SRV   ...->RT
    // Nothing about that is unusual for a renderer; it is just not what the
    // first game did. Every behavioural rule the identification filter once had
    // rejects this, which is why they are scored signals now - and why the
    // harness has to be able to reproduce a pipeline other than the one the
    // project grew up on, or "works across titles" stays an anecdote.
    VelocityComputeAssisted,
};

struct LookAlike {
    const char* name;
    const char* why;             // which filter is supposed to reject it, and why
    DXGI_FORMAT format;
    UINT64 width;
    UINT height;
    Behaviour behaviour;
    bool shouldBeSelected;          // as velocity
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    bool shouldBeNormals = false;   // --normals: the one normals must select
    // --normals: 0 = renders every frame. 1/2 = one of two allocations of
    // GBufferA that take turns in stretches, as measured on Skyrunner (see
    // gbufferAllocationActive below): 1 first, 2 must be ADOPTED as a rival.
    int allocation = 0;
    // --velocity-realloc: 0 = n/a. 1/2/3 = three allocations of SceneVelocity
    // that take turns (see velocityAllocationActive) - 1 is selected, 2 and 3
    // adopted as rivals, then 2 again for longer than the silence threshold.
    int velocityAllocation = 0;

    ComPtr<ID3D12Heap> heap;
    ComPtr<ID3D12Resource> resource;
    UINT rtvIndex = 0;
};

// The depth line-up. Same idea as the velocity one and the same argument for
// having it: "a depth dump appeared" is satisfied by copying back the wrong
// depth target, and a half-resolution or shadow-map depth buffer decodes to a
// perfectly plausible depth field. Each decoy here is excluded by exactly one
// of the three things the depth filter tests - format, the ALLOW_DEPTH_STENCIL
// / DENY_SHADER_RESOURCE flag pair, and the extent - so none of them can be
// quietly load-bearing.
//
// UE5 creates scene depth TYPELESS (GPixelFormats[PF_DepthStencil], D3D12RHI.cpp:117/127)
// so the look-alikes are typeless too. That is not cosmetic: a typeless
// depth-stencil resource is multi-plane, and copying it back exercises the
// plane/subresource path in capture.cpp that the single-plane velocity buffer
// never reaches.
struct DepthLookAlike {
    const char* name;
    const char* why;
    DXGI_FORMAT format;
    UINT64 width;
    UINT height;
    bool denyShaderResource;
    bool shouldBeSelected;
    // How many DEPTH_WRITE -> readable edges this target takes per frame.
    //
    // The only thing that separates scene depth from CustomDepth, which is why
    // it is a field rather than a constant. UE5 creates both at the same
    // extent, in the same format, with the same
    // TexCreate_DepthStencilTargetable | TexCreate_ShaderResource pair, so
    // their D3D12 descriptors are identical and no structural test can tell
    // them apart. What differs is that the whole frame is built around scene
    // depth and CustomDepth is rendered at most once.
    int edgesPerFrame;

    ComPtr<ID3D12Heap> heap;
    ComPtr<ID3D12Resource> resource;
    UINT dsvIndex = 0;
};

bool Check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        printf("[testhost] FAILED %s: 0x%08lX\n", what, static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

// Pulls everything the debug layer has said since the last call and prints it.
// Returns the number of messages at ERROR severity or worse - the number that
// decides whether this harness passed.
int DrainInfoQueue(ID3D12InfoQueue* infoQueue, const char* phase) {
    if (infoQueue == nullptr) {
        return 0;
    }
    int errors = 0;
    const UINT64 count = infoQueue->GetNumStoredMessages();
    std::vector<uint8_t> buffer;
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T length = 0;
        if (FAILED(infoQueue->GetMessage(i, nullptr, &length))) {
            continue;
        }
        buffer.resize(length);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        if (FAILED(infoQueue->GetMessage(i, message, &length))) {
            continue;
        }
        const char* severity = "INFO";
        switch (message->Severity) {
            case D3D12_MESSAGE_SEVERITY_CORRUPTION: severity = "CORRUPTION"; ++errors; break;
            case D3D12_MESSAGE_SEVERITY_ERROR:      severity = "ERROR";      ++errors; break;
            case D3D12_MESSAGE_SEVERITY_WARNING:    severity = "WARNING";    break;
            default: break;
        }
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
            printf("[debug-layer/%s] %s (id %d): %.*s\n", phase, severity, static_cast<int>(message->ID),
                   static_cast<int>(message->DescriptionByteLength), message->pDescription);
        }
    }
    infoQueue->ClearStoredMessages();
    return errors;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

// Defined in wpo_encode_test.cpp. Self-contained (own device, no window or
// swapchain) - kept out of the harness below entirely rather than threaded
// through its device/window setup, since it validates a different thing (the
// encode format on synthetic WPO-representative values) than the rest of
// this file (identification and barrier correctness against a real, if
// synthetic, capture pipeline).
int RunWpoEncodeTest(const std::string& outputDir);

int main(int argc, char** argv) {
    std::wstring hookPath;
    int frames = 240;
    bool gpuValidation = true;
    bool ambiguous = false;
    // docs/REFACTOR_PLAN.md Phase 8's three required positive tests: resize
    // must pause capture until reidentification, a depth reallocation must
    // recover to SELECTED (not stuck AMBIGUOUS/nothing), and a mid-burst
    // reopen must end the burst rather than silently drop frames. See
    // RunPhase8BehaviorTests below.
    bool phase8Tests = false;
    // docs/REFACTOR_PLAN.md Phase 9's negative test - see the file header.
    bool phase9Negative = false;
    // Phase 10: add a GBufferA look-alike and opt into the normals surface.
    bool normals = false;
    // Phase 10 follow-up: SceneVelocity rotating through three allocations,
    // the way Skyrunner's does. See velocityAllocationActive.
    bool velocityRealloc = false;
    // Which shape of velocity pass to model. "oxi" is the strict 2-cycle this
    // project was developed against; "skyrunner" is the 5-transition,
    // UNORDERED_ACCESS-touching pass measured on a third-party UE 5.2 title.
    // Identification has to find the right resource under both, or "works
    // across titles" is an anecdote rather than a test.
    std::string pipeline = "oxi";
    std::string wpoEncodeTestDir;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--hook" && i + 1 < argc) {
            const std::string next = argv[++i];
            hookPath.assign(next.begin(), next.end());
        } else if (arg == "--frames" && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (arg == "--no-gbv") {
            gpuValidation = false;
        } else if (arg == "--ambiguous") {
            ambiguous = true;
        } else if (arg == "--pipeline" && i + 1 < argc) {
            pipeline = argv[++i];
        } else if (arg == "--wpo-encode-test" && i + 1 < argc) {
            wpoEncodeTestDir = argv[++i];
        } else if (arg == "--phase8-tests") {
            phase8Tests = true;
        } else if (arg == "--phase9-negative") {
            phase9Negative = true;
        } else if (arg == "--normals") {
            normals = true;
        } else if (arg == "--velocity-realloc") {
            velocityRealloc = true;
        }
    }

    if (!wpoEncodeTestDir.empty()) {
        return RunWpoEncodeTest(wpoEncodeTestDir);
    }

    printf("[testhost] pid=%lu, %d frames, pipeline=%s%s\n", GetCurrentProcessId(), frames,
           pipeline.c_str(),
           ambiguous ? ", ambiguous line-up (identification must refuse to pick)" : "");

    // Identification now watches barriers for a settling window before
    // deciding, so the window has to fit inside the run. A third of the frames
    // leaves room for the 60-frame autocapture burst afterwards. Set before
    // LoadLibrary, because the hook reads its environment on first use.
    const int identifyFrames = frames / 3 > 40 ? frames / 3 : 40;
    char identifyFramesText[32];
    snprintf(identifyFramesText, sizeof(identifyFramesText), "%d", identifyFrames);
    SetEnvironmentVariableA("MV_IDENTIFY_FRAMES", identifyFramesText);
    printf("[testhost] MV_IDENTIFY_FRAMES=%d (decision expected around that frame)\n", identifyFrames);

    // The hook reads these once, in InstallD3D12Hooks, so before LoadLibrary.
    const char* const kNegativeProfile = "test-velocity-wrong-format";
    if (phase9Negative) {
        SetEnvironmentVariableA("MV_PROFILE_FILE", MV_TESTHOST_PROFILE_DIR "/phase9_negative.json");
        SetEnvironmentVariableA("MV_PROFILE", kNegativeProfile);
        printf("[testhost] Phase 9 negative test: MV_PROFILE=%s (velocity narrowed to the wrong format) - "
               "identification must find NO candidate and must not fall back\n", kNegativeProfile);
    }
    if (normals) {
        SetEnvironmentVariableA("MV_SURFACES", "normals");
        printf("[testhost] MV_SURFACES=normals: a GBufferA look-alike joins the line-up and must be the one "
               "normals selects\n");
    }
    // Every path below that expects identification to select nothing.
    const bool expectNothingSelected = ambiguous || phase9Negative;

    // 1. Debug layer BEFORE device creation - it has no effect afterwards.
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        printf("[testhost] debug layer enabled\n");
        ComPtr<ID3D12Debug1> debug1;
        if (gpuValidation && SUCCEEDED(debug.As(&debug1))) {
            // GPU-based validation is what catches the errors that matter
            // here: a resource used in a state it is not actually in, and
            // descriptor lifetime problems. It is very slow, hence --no-gbv.
            debug1->SetEnableGPUBasedValidation(TRUE);
            printf("[testhost] GPU-based validation enabled (slow)\n");
        }
    } else {
        printf("[testhost] WARNING: debug layer unavailable - install the Graphics Tools "
               "optional feature. This run validates nothing.\n");
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MvTestHostWindow";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"mv_testhost", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, kWidth, kHeight,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd == nullptr) {
        printf("[testhost] CreateWindowExW failed\n");
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    ComPtr<IDXGIFactory6> factory;
    if (!Check(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2")) {
        return 1;
    }
    ComPtr<IDXGIAdapter1> adapter;
    factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));

    ComPtr<ID3D12Device> device;
    if (!Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice")) {
        return 1;
    }

    // Break-on-error is deliberately NOT set: the point is to collect every
    // complaint and report them all, not to stop at the first.
    ComPtr<ID3D12InfoQueue> infoQueue;
    device.As(&infoQueue);

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (!Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue")) {
        return 1;
    }

    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.BufferCount = kBackBufferCount;
    scDesc.Width = kWidth;
    scDesc.Height = kHeight;
    scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.SampleDesc.Count = 1;
    ComPtr<IDXGISwapChain1> swapChain1;
    if (!Check(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &scDesc, nullptr, nullptr, &swapChain1),
               "CreateSwapChainForHwnd")) {
        return 1;
    }
    ComPtr<IDXGISwapChain3> swapChain;
    swapChain1.As(&swapChain);

    // 2. The line-up. One of these is SceneVelocity; the others are the
    // plausible mistakes. The hook is told none of it.
    //
    // Every entry is a PLACED resource (not committed), because the real one
    // comes from UE5's TransientResourceAllocator and placed resources have
    // aliasing rules the debug layer checks separately.
    std::vector<LookAlike> lineUp = {
        {"SceneVelocity", "the real thing: velocity format, RT|UAV", kVelocityFormat,
         kVelocityWidth, kVelocityHeight,
         pipeline == "skyrunner" ? Behaviour::VelocityComputeAssisted : Behaviour::Velocity, true},

        // The exact shape of this project's most expensive bug: structurally
        // identical, behaviourally identical, one digit different in the format
        // enum. R16G16B16A16_FLOAT is 10; _UNORM is 11. UE5 never asks for
        // FLOAT here - FVelocityRendering::GetFormat cannot return it - so the
        // structural filter must reject this even though everything it DOES
        // trigger on is present.
        {"HDR pool target", "rejected on structure: R16G16B16A16_FLOAT is not a format "
                            "FVelocityRendering::GetFormat can return",
         DXGI_FORMAT_R16G16B16A16_FLOAT, kVelocityWidth, kVelocityHeight, Behaviour::Velocity, false},

        // Right format, right flags, right resolution - and written by compute.
        // Only the behaviour filter can tell this apart, and only via the UAV
        // barrier. This is the discriminator the README claims separates
        // velocity from the compute-written targets; here it is under test
        // rather than asserted.
        {"compute-written RG16", "rejected on behaviour: goes through UNORDERED_ACCESS and takes a UAV barrier",
         DXGI_FORMAT_R16G16_UNORM, kVelocityWidth, kVelocityHeight, Behaviour::ComputeWritten, false},

        // Byte-identical descriptor to the real one. Nothing structural can
        // separate these two; only "exactly 2 barrier events per frame" does.
        {"twin, read twice", "rejected on behaviour: 4 RT<->SRV events per frame, not 2",
         kVelocityFormat, kVelocityWidth, kVelocityHeight, Behaviour::ExtraPass, false},
    };

    if (velocityRealloc) {
        // Without the "twin, read twice" decoy: it takes the velocity edge
        // every frame at the same extent, so its edges interleave with the
        // new allocation's and keep resetting rival adoption's
        // consecutive-frame run - nothing is ever adopted. A real limitation
        // of the run-length rule (it follows one rival at a time), but not one
        // any measured title has shown; Skyrunner, whose reallocations this
        // mode models, has no such twin.
        lineUp.erase(std::remove_if(lineUp.begin(), lineUp.end(),
                                    [](const LookAlike& t) { return t.behaviour == Behaviour::ExtraPass; }),
                     lineUp.end());
        // Two more allocations of SceneVelocity: identical descriptor and
        // behaviour, different address. They only render after identification
        // (velocityAllocationActive), so they never compete with it - they are
        // what rival adoption must follow, and what the silence check must
        // keep counting once the rival slot has moved on to a newer one.
        for (int extra = 2; extra <= 3; ++extra) {
            LookAlike copy = lineUp.front(); // SceneVelocity
            copy.name = extra == 2 ? "SceneVelocity (2nd alloc)" : "SceneVelocity (3rd alloc)";
            copy.why = "the same texture reallocated; adopted as a rival";
            copy.shouldBeSelected = false;
            copy.velocityAllocation = extra;
            lineUp.insert(lineUp.begin() + 1, copy);
        }
        lineUp.front().velocityAllocation = 1;
    }

    if (normals) {
        // UE5's GBufferA: the default r.GBufferFormat's R10G10B10A2, a plain
        // render target (no UAV), finished by the base pass - so its RT -> SRV
        // edge comes BEFORE velocity's, hence the front of the line-up.
        //
        // What makes this a real test rather than a format lookup: SceneVelocity
        // itself (R16G16B16A16_UNORM, RT, RT->SRV every frame) passes the
        // normals structural and behaviour gates too - the collision the plan
        // names. Only mutual exclusion keeps it out, and the HDR pool target
        // (R16G16B16A16_FLOAT, also a normals format) and the twin must lose
        // on evidence. Velocity's own gate rejects this one: no UAV flag.
        //
        // Plus the decoy that made normals AMBIGUOUS on Skyrunner, twice: a
        // second R10G10B10A2, RT-only target at scene extent (a post-process
        // output), identical to GBufferA on every other signal. The one thing
        // separating them - measured live - is that GBufferA becomes readable
        // in the same ResourceBarrier call as scene depth and the decoy never
        // does. So this pair passes only through SignalId::SharesCallWithEdge.
        LookAlike gbufferA{"GBufferA", "normals: R10G10B10A2, RT only, readable in the same call as depth",
                           DXGI_FORMAT_R10G10B10A2_UNORM, kVelocityWidth, kVelocityHeight, Behaviour::ReadWithDepth,
                           false};
        gbufferA.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        gbufferA.shouldBeNormals = true;
        LookAlike postOutput{"post-process output", "normals decoy: same format/flags/extent, never batched with depth",
                             DXGI_FORMAT_R10G10B10A2_UNORM, kVelocityWidth, kVelocityHeight, Behaviour::Velocity,
                             false};
        postOutput.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        // And a second allocation of GBufferA itself: identical descriptor,
        // different address, the two taking turns in stretches. On Skyrunner
        // UE did exactly this (to SceneVelocity too), and normals - pinned to
        // one pointer - captured only a fraction of the burst. Normals must
        // adopt this one as a rival; it must NOT adopt the decoy above, which
        // takes its edge every frame and would be the first thing a naive
        // rival rule grabbed when GBufferA moved.
        LookAlike gbufferA2 = gbufferA;
        gbufferA2.name = "GBufferA (2nd alloc)";
        gbufferA2.why = "normals: the same texture reallocated; must be adopted as a rival, not selected";
        gbufferA2.shouldBeNormals = false;
        gbufferA.allocation = 1;
        gbufferA2.allocation = 2;
        // And scene colour as it measured live: also readable in the same
        // call as depth (so it passes the depth-call evidence), but a
        // different texture - R16G16B16A16_FLOAT, RT|UAV. On Skyrunner it was
        // adopted as a GBufferA "rival" until rival.same_descriptor existed,
        // and the format mismatch then ended the burst.
        LookAlike sceneColor{"scene colour", "normals rival decoy: shares calls with depth, different descriptor",
                             DXGI_FORMAT_R16G16B16A16_FLOAT, kVelocityWidth, kVelocityHeight,
                             Behaviour::ReadWithDepth, false};
        lineUp.insert(lineUp.begin(), sceneColor);
        lineUp.insert(lineUp.begin(), postOutput);
        lineUp.insert(lineUp.begin(), gbufferA2);
        lineUp.insert(lineUp.begin(), gbufferA);
    }

    if (pipeline == "skyrunner") {
        // Drop the "twin, read twice" decoy for this pipeline, and note WHY,
        // because the reason is a finding rather than a convenience.
        //
        // That decoy exists to prove the strict two-events-per-frame signal
        // discriminates. When the real velocity pass is itself not a 2-cycle,
        // the signal fires for neither of them - and the two then score
        // identically (11 and 11, measured), so identification correctly
        // refuses to pick. That is the honest answer: on a title whose velocity
        // pass looks like this one's, a second velocity-format RT|UAV texture
        // read twice a frame really would be indistinguishable from outside the
        // process, and saying so beats guessing.
        //
        // Keeping it would test the refusal path, which --ambiguous already
        // covers. Removing it tests what this pipeline exists to test: that a
        // 5-transition, UNORDERED_ACCESS-touching velocity pass is still found.
        lineUp.erase(std::remove_if(lineUp.begin(), lineUp.end(),
                                    [](const LookAlike& t) {
                                        return t.behaviour == Behaviour::ExtraPass;
                                    }),
                     lineUp.end());
    }

    if (ambiguous) {
        // A perfect twin: same descriptor, same behaviour, indistinguishable by
        // anything this hook can observe. The correct answer is not "pick one",
        // it is "refuse and say so" - which is the behaviour under test.
        //
        // "Same behaviour" has to mean the SAME behaviour as whichever
        // SceneVelocity variant this pipeline actually uses, not a hardcoded
        // one. Hardcoding Behaviour::Velocity here made the twin a clean
        // 2-cycle resource while skyrunner's own SceneVelocity (above) is
        // Behaviour::VelocityComputeAssisted (5 transitions, through UAV) -
        // two genuinely different signatures scoring 16 vs 11, a comfortable
        // margin. Identification then correctly - by its own scoring - picked
        // the twin over the real resource instead of refusing, because the
        // fixture was no longer constructing a real tie for this pipeline.
        lineUp.push_back({"perfect twin", "indistinguishable - identification must REFUSE, not guess",
                          kVelocityFormat, kVelocityWidth, kVelocityHeight, lineUp.front().behaviour, false});
    }

    // RTVs: one per back buffer plus one per look-alike.
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = kBackBufferCount + static_cast<UINT>(lineUp.size());
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    if (!Check(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)), "CreateDescriptorHeap")) {
        return 1;
    }
    const UINT rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto rtvAt = [&](UINT i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * rtvSize;
        return h;
    };

    for (size_t i = 0; i < lineUp.size(); ++i) {
        LookAlike& target = lineUp[i];
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = target.width;
        desc.Height = target.height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = target.format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = target.flags;

        const D3D12_RESOURCE_ALLOCATION_INFO alloc = device->GetResourceAllocationInfo(0, 1, &desc);
        D3D12_HEAP_DESC heapDesc{};
        heapDesc.SizeInBytes = alloc.SizeInBytes;
        heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapDesc.Alignment = alloc.Alignment;
        // Match the real thing rather than the convenient thing.
        //
        // RenderDoc on the third-party target shows SceneVelocity placed at
        // offset 0 of a 128 MiB DEFAULT heap created with CREATE_NOT_ZEROED and
        // NO restriction flags - i.e. a resource-heap-tier-2 pool that can hold
        // buffers and textures alike, with 114 MiB left over for whatever the
        // transient allocator puts there next. This harness was creating an
        // ALLOW_ONLY_RT_DS_TEXTURES heap sized to exactly one texture, which is
        // a different object with different aliasing rules - and aliasing rules
        // are precisely what the debug layer checks about placed resources, so
        // modelling the wrong kind of heap weakens the one thing this harness
        // exists to verify.
        //
        // CREATE_NOT_ZEROED is copied over too. It is not cosmetic: asking the
        // driver not to zero memory is what you do when you expect the previous
        // tenant's garbage, which is the clearest signal in the capture that
        // this memory is recycled.
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        const bool tier2 =
            SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) &&
            options.ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2;
        heapDesc.Flags = (tier2 ? D3D12_HEAP_FLAG_NONE : D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES) |
                         D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
        if (!Check(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&target.heap)), "CreateHeap")) {
            return 1;
        }

        // Must match the value actually cleared to below, or every frame
        // produces a debug-layer warning about a slow clear and drowns the
        // output we care about.
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = target.format;
        memcpy(clearValue.Color, kVelocityClear, sizeof(kVelocityClear));
        if (!Check(device->CreatePlacedResource(
                       target.heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue,
                       IID_PPV_ARGS(&target.resource)),
                   "CreatePlacedResource")) {
            return 1;
        }
        target.rtvIndex = kBackBufferCount + static_cast<UINT>(i);
        device->CreateRenderTargetView(target.resource.Get(), nullptr, rtvAt(target.rtvIndex));
        printf("[testhost] look-alike %zu: %-22s %llux%u fmt=%2d - %s\n", i, target.name, target.width,
               target.height, static_cast<int>(target.format), target.why);
    }

    // 2b. The depth line-up, and the descriptor heap for its DSVs.
    std::vector<DepthLookAlike> depthLineUp = {
        {"SceneDepth", "the real thing: PF_DepthStencil typeless, DS|SRV, at the velocity extent",
         DXGI_FORMAT_R32G8X24_TYPELESS, kVelocityWidth, kVelocityHeight, false, true, 4},
        {"shadow atlas", "rejected on flags AND extent: DENY_SHADER_RESOURCE, 2048x2048",
         DXGI_FORMAT_R32G8X24_TYPELESS, 2048, 2048, true, false, 1},
        {"half-res depth", "rejected on extent alone: same format and flags, half the size",
         DXGI_FORMAT_R32G8X24_TYPELESS, kVelocityWidth / 2, kVelocityHeight / 2, false, false, 1},
        {"occluder depth", "rejected on flags alone: right extent, but DENY_SHADER_RESOURCE",
         DXGI_FORMAT_R32G8X24_TYPELESS, kVelocityWidth, kVelocityHeight, true, false, 1},
        // The one no structural test can reject, and the reason the depth
        // filter needed a behavioural tie-break at all. This is not a
        // hypothetical decoy: identification hit exactly this on the
        // third-party title, found two survivors with identical scores, and
        // refused to pick - which was the right answer and still left the
        // reprojection with no depth. Only "how hard is it worked" separates
        // them.
        {"CustomDepth", "structurally IDENTICAL to scene depth - separable only by per-frame usage",
         DXGI_FORMAT_R32G8X24_TYPELESS, kVelocityWidth, kVelocityHeight, false, false, 1},
    };

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.NumDescriptors = static_cast<UINT>(depthLineUp.size());
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    if (!Check(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap)), "CreateDescriptorHeap(DSV)")) {
        return 1;
    }
    const UINT dsvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    auto dsvAt = [&](UINT i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * dsvSize;
        return h;
    };

    for (size_t i = 0; i < depthLineUp.size(); ++i) {
        DepthLookAlike& target = depthLineUp[i];
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = target.width;
        desc.Height = target.height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = target.format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if (target.denyShaderResource) {
            desc.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        }

        const D3D12_RESOURCE_ALLOCATION_INFO alloc = device->GetResourceAllocationInfo(0, 1, &desc);
        D3D12_HEAP_DESC heapDesc{};
        heapDesc.SizeInBytes = alloc.SizeInBytes;
        heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapDesc.Alignment = alloc.Alignment;
        D3D12_FEATURE_DATA_D3D12_OPTIONS depthOptions{};
        const bool depthTier2 =
            SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &depthOptions, sizeof(depthOptions))) &&
            depthOptions.ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2;
        heapDesc.Flags = (depthTier2 ? D3D12_HEAP_FLAG_NONE : D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES) |
                         D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
        if (!Check(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&target.heap)), "CreateHeap(depth)")) {
            return 1;
        }

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        // UE5 uses a reversed-Z buffer, so the far plane is 0.0 - but clearing
        // to 0.0 here would make "the depth decode works" and "the depth buffer
        // is all far plane" produce identical output, and the offline tools
        // treat DeviceZ == 0 as "no geometry". 0.5 is a value that can only
        // have come through the copy and the plane-0 unpacking intact.
        clearValue.DepthStencil.Depth = 0.5f;
        clearValue.DepthStencil.Stencil = 0;
        if (!Check(device->CreatePlacedResource(
                       target.heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
                       IID_PPV_ARGS(&target.resource)),
                   "CreatePlacedResource(depth)")) {
            return 1;
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        target.dsvIndex = static_cast<UINT>(i);
        device->CreateDepthStencilView(target.resource.Get(), &dsvDesc, dsvAt(target.dsvIndex));
        printf("[testhost] depth look-alike %zu: %-16s %llux%u fmt=%2d - %s\n", i, target.name, target.width,
               target.height, static_cast<int>(target.format), target.why);
    }

    ComPtr<ID3D12Resource> backBuffers[kBackBufferCount];
    for (UINT i = 0; i < kBackBufferCount; ++i) {
        if (!Check(swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffers[i])), "GetBuffer")) {
            return 1;
        }
        device->CreateRenderTargetView(backBuffers[i].Get(), nullptr, rtvAt(i));
    }

    ComPtr<ID3D12CommandAllocator> allocators[kBackBufferCount];
    for (UINT i = 0; i < kBackBufferCount; ++i) {
        if (!Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])),
                   "CreateCommandAllocator")) {
            return 1;
        }
    }
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    if (!Check(device->CreateCommandList(
                   0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr, IID_PPV_ARGS(&cmdList)),
               "CreateCommandList")) {
        return 1;
    }
    cmdList->Close();

    ComPtr<ID3D12Fence> fence;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    int errorsBeforeHook = DrainInfoQueue(infoQueue.Get(), "setup");

    // 3. Load the hook into ourselves. LoadLibrary rather than remote
    // injection: it reaches the same DllMain and the same InstallD3D12Hooks,
    // and what is under test here is the capture path, not the injector.
    HMODULE hookModule = nullptr;
    if (!hookPath.empty()) {
        hookModule = LoadLibraryW(hookPath.c_str());
        if (hookModule == nullptr) {
            printf("[testhost] LoadLibraryW failed: %lu\n", GetLastError());
            return 1;
        }
        printf("[testhost] loaded hook, waiting for it to install...\n");
        Sleep(1500); // InstallD3D12Hooks runs on its own thread
    } else {
        printf("[testhost] no --hook given; running unhooked (baseline)\n");
    }

    auto barrier = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        cmdList->ResourceBarrier(1, &b);
    };

    // Where the depth edges sit relative to the velocity edge.
    //
    // A UE5 frame settles scene depth in the prepass and base pass and only
    // then finishes velocity, so depth becomes readable FIRST - that is the
    // ordering the capture path optimises for, copying depth until the velocity
    // edge arrives and then stopping. But nothing guarantees it on a title
    // whose frame graph is arranged differently, and when the assumption fails
    // the failure is silent: velocity and colour still arrive and the dump
    // looks complete with no depth in it. So both orderings are modelled, and
    // whichever this run uses, a depth file has to reach disk.
    const bool depthBeforeVelocity = pipeline != "skyrunner";
    printf("[testhost] depth edges are emitted %s the velocity edge\n",
           depthBeforeVelocity ? "BEFORE (the UE5 prepass ordering)"
                               : "AFTER (the ordering that exercises the fallback)");

    // The depth line-up. Cleared through a DSV first so the placed memory holds
    // real bytes - GPU-based validation objects to reading uninitialised
    // placed-resource memory, and so it should.
    //
    // The real one gets TWO DEPTH_WRITE -> readable edges per frame, which is
    // the shape UE5 produces on a title with a partial prepass (depth settles
    // once after the prepass and again after the base pass). The capture path
    // is supposed to record a copy on each and let the last one before velocity
    // land; running it that way here is what makes that claim tested rather
    // than asserted.
    auto emitDepthEdges = [&]() {
        for (DepthLookAlike& target : depthLineUp) {
            const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvAt(target.dsvIndex);
            cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
            cmdList->ClearDepthStencilView(
                dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.5f, 0, 0, nullptr);
            ID3D12Resource* resource = target.resource.Get();
            // Alternating readable states, because the real title uses both:
            // DEPTH_READ for the passes that still depth-test against it, and
            // the shader-resource states for the passes that sample it.
            for (int edge = 0; edge < target.edgesPerFrame; ++edge) {
                const D3D12_RESOURCE_STATES readable =
                    (edge % 2) == 0 ? D3D12_RESOURCE_STATE_DEPTH_READ
                                    : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                barrier(resource, D3D12_RESOURCE_STATE_DEPTH_WRITE, readable);
                barrier(resource, readable, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            }
        }
    };

    int totalErrors = 0;

    // One frame's worth of work, extracted into a lambda so the Phase 8
    // behaviour tests (below, gated on --phase8-tests) can keep driving the
    // exact same render loop past the initial identify-and-capture window -
    // a resize or a reopen only means anything if frames keep coming.
    // --normals: which allocation of GBufferA renders this frame. Allocation 1
    // alone until identification has settled (so it is the one selected),
    // then the two alternate in 30-frame stretches - several switches inside
    // a 240-frame run, each of which used to cost every frame of the stretch.
    const int gbufferSwitchStart = identifyFrames + 20;
    auto gbufferAllocationActive = [&](int allocation, int frame) {
        const bool second = frame >= gbufferSwitchStart && ((frame - gbufferSwitchStart) / 30) % 2 == 0;
        return allocation == (second ? 2 : 1);
    };

    // --velocity-realloc: allocation 1 until identification has settled, then
    // 2 (adopted), 3 (adopted - the rival slot moves to it), then 2 again for
    // 500 frames: longer than the 300-frame silence threshold, on an
    // allocation that is a candidate but no longer the rival slot. Liveness
    // used to ignore it there and end the burst as velocity_pass_silent.
    const int velocitySwitchStart = identifyFrames + 20;
    auto velocityAllocationActive = [&](int allocation, int frame) {
        int active = 1;
        if (frame >= velocitySwitchStart + 80) {
            active = 2;
        } else if (frame >= velocitySwitchStart + 40) {
            active = 3;
        } else if (frame >= velocitySwitchStart) {
            active = 2;
        }
        return allocation == active;
    };

    auto renderFrame = [&](int frame) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        const UINT index = swapChain->GetCurrentBackBufferIndex();
        allocators[index]->Reset();
        cmdList->Reset(allocators[index].Get(), nullptr);

        if (depthBeforeVelocity) {
            emitDepthEdges();
        }

        for (LookAlike& target : lineUp) {
            if (target.allocation != 0 && !gbufferAllocationActive(target.allocation, frame)) {
                continue; // the other allocation of GBufferA is in use this frame
            }
            if (target.velocityAllocation != 0 && !velocityAllocationActive(target.velocityAllocation, frame)) {
                continue; // another allocation of SceneVelocity is in use this frame
            }
            // Write each look-alike as a render target, so it holds real bytes
            // rather than undefined memory - GPU-based validation objects to
            // reading uninitialised placed-resource memory, and so it should.
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvAt(target.rtvIndex);
            cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            cmdList->ClearRenderTargetView(rtv, kVelocityClear, 0, nullptr);

            ID3D12Resource* resource = target.resource.Get();
            switch (target.behaviour) {
                case Behaviour::Velocity:
                    // The transition the hook triggers on. Everything the
                    // capture path does - its own barriers, its
                    // CopyTextureRegion, its footprints - is recorded onto THIS
                    // command list, inside this pair of barriers, and is
                    // therefore fully covered by the debug layer.
                    barrier(resource, D3D12_RESOURCE_STATE_RENDER_TARGET,
                            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                    barrier(resource, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_RENDER_TARGET);
                    break;
                case Behaviour::ReadWithDepth: {
                    ID3D12Resource* sceneDepth = nullptr;
                    for (const DepthLookAlike& d : depthLineUp) {
                        if (d.shouldBeSelected) {
                            sceneDepth = d.resource.Get();
                        }
                    }
                    D3D12_RESOURCE_BARRIER pair[2]{};
                    pair[0].Type = pair[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    pair[0].Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE};
                    pair[1].Transition = {sceneDepth, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                          D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ};
                    cmdList->ResourceBarrier(2, pair);
                    barrier(resource, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_RENDER_TARGET);
                    barrier(sceneDepth, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                    break;
                }
                case Behaviour::ComputeWritten: {
                    barrier(resource, D3D12_RESOURCE_STATE_RENDER_TARGET,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    D3D12_RESOURCE_BARRIER uav{};
                    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    uav.UAV.pResource = resource;
                    cmdList->ResourceBarrier(1, &uav);
                    barrier(resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                    barrier(resource, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_RENDER_TARGET);
                    break;
                }
                case Behaviour::VelocityComputeAssisted:
                    // Transition counts and states taken from the real title.
                    // Note it passes through UNORDERED_ACCESS without ever
                    // issuing a UAV barrier, which is why "takes a UAV barrier"
                    // and "enters UNORDERED_ACCESS" are two separate signals
                    // rather than one - collapsing them would make this
                    // indistinguishable from the compute-written decoy.
                    barrier(resource, D3D12_RESOURCE_STATE_RENDER_TARGET,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    barrier(resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    barrier(resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_RENDER_TARGET);
                    barrier(resource, D3D12_RESOURCE_STATE_RENDER_TARGET,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    barrier(resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_RENDER_TARGET);
                    break;
                case Behaviour::ExtraPass:
                    for (int pass = 0; pass < 2; ++pass) {
                        barrier(resource, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                        barrier(resource, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                                D3D12_RESOURCE_STATE_RENDER_TARGET);
                    }
                    break;
            }
        }

        if (!depthBeforeVelocity) {
            emitDepthEdges();
        }

        // Something trivial on the back buffer, so Present has real content
        // and the overlay (if enabled) has something to composite over.
        barrier(backBuffers[index].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float t = static_cast<float>(frame) / static_cast<float>(frames);
        const float clear[4] = {0.1f + 0.4f * t, 0.15f, 0.5f - 0.3f * t, 1.0f};
        cmdList->ClearRenderTargetView(rtvAt(index), clear, 0, nullptr);
        barrier(backBuffers[index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

        cmdList->Close();
        ID3D12CommandList* lists[] = {cmdList.Get()};
        queue->ExecuteCommandLists(1, lists);

        swapChain->Present(1, 0);

        // Serialise every frame. Slower than the real thing, but it means a
        // debug-layer message can be attributed to the frame that caused it.
        queue->Signal(fence.Get(), ++fenceValue);
        if (fence->GetCompletedValue() < fenceValue) {
            fence->SetEventOnCompletion(fenceValue, fenceEvent);
            WaitForSingleObject(fenceEvent, 5000);
        }

        char phase[32];
        snprintf(phase, sizeof(phase), "frame%d", frame);
        totalErrors += DrainInfoQueue(infoQueue.Get(), phase);
    };

    for (int frame = 0; frame < frames; ++frame) {
        renderFrame(frame);
    }

    // ---- Did it identify the right one? --------------------------------
    //
    // This is a separate question from "did the capture path upset the debug
    // layer", and it is the one the previous harness could not ask: with a
    // single decoy matching a hardcoded descriptor, "something was captured"
    // was the only available assertion, and that assertion was equally true
    // when the filter was selecting the wrong resource - which it did, for
    // days, during the R16G16B16A16_FLOAT/_UNORM episode.
    bool identificationPassed = true;
    if (hookModule != nullptr) {
        using IdentifiedFn = void*(*)();
        auto identified = reinterpret_cast<IdentifiedFn>(
            reinterpret_cast<void*>(GetProcAddress(hookModule, "MvIdentifiedVelocityResource")));
        if (identified == nullptr) {
            printf("\n[testhost] FAIL - mv_hook.dll does not export MvIdentifiedVelocityResource; "
                   "cannot check which resource was picked\n");
            identificationPassed = false;
        } else {
            void* picked = identified();
            const LookAlike* expected = nullptr;
            for (const LookAlike& target : lineUp) {
                if (target.shouldBeSelected) {
                    expected = &target;
                }
            }
            printf("\n[testhost] ==== identification ====\n");
            for (const LookAlike& target : lineUp) {
                const bool isPicked = picked == static_cast<void*>(target.resource.Get());
                printf("[testhost]   %-22s %p %s\n", target.name, static_cast<void*>(target.resource.Get()),
                       isPicked ? "<== SELECTED" : "");
            }
            if (phase9Negative) {
                // Three things, each guarding a different way to pass for the
                // wrong reason: the narrowed profile is the one that ran (not a
                // load failure, which also selects nothing), nothing was
                // selected (not a fallback to the full format table, which
                // would find SceneVelocity), and the hook SAID so (the plan's
                // "loudly").
                using StringFn = const char* (*)();
                auto activeProfile = reinterpret_cast<StringFn>(
                    reinterpret_cast<void*>(GetProcAddress(hookModule, "MvActiveProfile")));
                auto logDirectory = reinterpret_cast<StringFn>(
                    reinterpret_cast<void*>(GetProcAddress(hookModule, "MvLogDirectory")));
                const std::string profile = activeProfile != nullptr ? activeProfile() : "<no MvActiveProfile export>";
                const bool profileOk = profile == kNegativeProfile;

                // Only this run's part of the log: it is appended across runs.
                bool saidNoCandidate = false;
                if (logDirectory != nullptr) {
                    std::ifstream in(std::string(logDirectory()) + "mv_hook.log", std::ios::binary);
                    const std::string log((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    const size_t start = log.rfind(std::string("profile: using '") + kNegativeProfile + "'");
                    saidNoCandidate = start != std::string::npos &&
                                      log.find("identify: NO candidate", start) != std::string::npos;
                }

                identificationPassed = profileOk && picked == nullptr && saidNoCandidate;
                printf("[testhost]   active profile: %s %s\n", profile.empty() ? "<none - failed to load>" : profile.c_str(),
                       profileOk ? "(as requested)" : "(NOT the negative-test profile)");
                printf("[testhost]   selected:       %s\n", picked == nullptr ? "nothing" : "SOMETHING");
                printf("[testhost]   log says 'identify: NO candidate' this run: %s\n", saidNoCandidate ? "yes" : "NO");
                printf("[testhost] %s\n",
                       identificationPassed
                           ? "PASS - a profile narrowed to the wrong format found NO candidate, said so, and did not "
                             "fall back"
                           : "FAIL - Phase 9 negative test (see the three lines above)");
            } else if (ambiguous) {
                // Two indistinguishable candidates. The only correct answer is
                // to refuse: a ranked guess between them would be a coin toss
                // presented as an identification.
                identificationPassed = picked == nullptr;
                printf("[testhost] %s\n",
                       identificationPassed
                           ? "PASS - identification refused to pick between two indistinguishable candidates"
                           : "FAIL - identification picked one of two indistinguishable candidates instead of "
                             "reporting the ambiguity");
            } else if (expected != nullptr && picked == static_cast<void*>(expected->resource.Get())) {
                printf("[testhost] PASS - selected the velocity buffer out of %zu look-alikes, with no "
                       "hardcoded resolution or descriptor\n", lineUp.size());
                if (velocityRealloc) {
                    // Both extra allocations adopted, and the pass never
                    // declared STOPPED while allocation 2 - adopted, but no
                    // longer the rival slot - carried it for 500 frames.
                    using StringFn = const char* (*)();
                    auto logDirectory = reinterpret_cast<StringFn>(
                        reinterpret_cast<void*>(GetProcAddress(hookModule, "MvLogDirectory")));
                    int adoptions = 0;
                    bool stopped = false;
                    if (logDirectory != nullptr) {
                        std::ifstream in(std::string(logDirectory()) + "mv_hook.log", std::ios::binary);
                        std::string line;
                        while (std::getline(in, line)) {
                            if (line.find("profile: using '") != std::string::npos) {
                                adoptions = 0; // appended across runs: count this run only
                                stopped = false;
                            }
                            adoptions += line.find("identify: ADOPTED") != std::string::npos ? 1 : 0;
                            stopped = stopped || line.find("identify: the pass has STOPPED") != std::string::npos;
                        }
                    }
                    const bool reallocOk = adoptions >= 2 && !stopped;
                    printf("[testhost]   velocity reallocations adopted: %d (need 2); pass declared STOPPED while "
                           "an earlier-adopted allocation carried it: %s\n", adoptions, stopped ? "YES" : "no");
                    printf("[testhost] %s\n", reallocOk ? "PASS - velocity followed three allocations without a "
                                                          "false silence"
                                                        : "FAIL - velocity reallocation (see the two lines above)");
                    identificationPassed = reallocOk;
                }
            } else {
                printf("[testhost] FAIL - expected %p (%s), got %p\n",
                       expected != nullptr ? static_cast<void*>(expected->resource.Get()) : nullptr,
                       expected != nullptr ? expected->name : "?", picked);
                printf("[testhost]        (nullptr means identification refused or never concluded - "
                       "check temp\\logs\\mv_hook.log for the 'identify:' lines)\n");
                identificationPassed = false;
            }
        }
    }

    // ---- Did it identify the right depth buffer? ------------------------
    bool depthIdentificationPassed = true;
    if (hookModule != nullptr) {
        using IdentifiedFn = void*(*)();
        auto identifiedDepth = reinterpret_cast<IdentifiedFn>(
            reinterpret_cast<void*>(GetProcAddress(hookModule, "MvIdentifiedDepthResource")));
        if (identifiedDepth == nullptr) {
            printf("\n[testhost] FAIL - mv_hook.dll does not export MvIdentifiedDepthResource\n");
            depthIdentificationPassed = false;
        } else {
            void* picked = identifiedDepth();
            const DepthLookAlike* expected = nullptr;
            for (const DepthLookAlike& target : depthLineUp) {
                if (target.shouldBeSelected) {
                    expected = &target;
                }
            }
            printf("\n[testhost] ==== depth identification ====\n");
            for (const DepthLookAlike& target : depthLineUp) {
                const bool isPicked = picked == static_cast<void*>(target.resource.Get());
                printf("[testhost]   %-16s %p %s\n", target.name, static_cast<void*>(target.resource.Get()),
                       isPicked ? "<== SELECTED" : "");
            }
            if (expectNothingSelected) {
                // Velocity identification refuses (or finds nothing) in this
                // mode, and depth identification is gated on the velocity
                // extent, so the only correct answer here is also to have
                // picked nothing. Worth
                // asserting rather than skipping: a depth search that quietly
                // ran anyway would be keying off an extent nobody established.
                depthIdentificationPassed = picked == nullptr;
                printf("[testhost] %s\n",
                       depthIdentificationPassed
                           ? "PASS - no depth selected, because velocity was never identified to key off"
                           : "FAIL - depth was selected even though velocity identification refused");
            } else if (expected != nullptr && picked == static_cast<void*>(expected->resource.Get())) {
                printf("[testhost] PASS - selected scene depth out of %zu depth look-alikes\n",
                       depthLineUp.size());
            } else {
                printf("[testhost] FAIL - expected %p (%s), got %p\n",
                       expected != nullptr ? static_cast<void*>(expected->resource.Get()) : nullptr,
                       expected != nullptr ? expected->name : "?", picked);
                depthIdentificationPassed = false;
            }
        }
    }

    // ---- Phase 10: did normals pick GBufferA, and was velocity kept out? ---
    bool normalsIdentificationPassed = true;
    if (normals && hookModule != nullptr) {
        using IdentifiedFn = void* (*)();
        using StringFn = const char* (*)();
        auto identifiedNormals = reinterpret_cast<IdentifiedFn>(
            reinterpret_cast<void*>(GetProcAddress(hookModule, "MvIdentifiedNormalsResource")));
        auto logDirectory = reinterpret_cast<StringFn>(
            reinterpret_cast<void*>(GetProcAddress(hookModule, "MvLogDirectory")));
        void* picked = identifiedNormals != nullptr ? identifiedNormals() : nullptr;
        const LookAlike* expected = nullptr;
        printf("\n[testhost] ==== normals identification ====\n");
        for (const LookAlike& target : lineUp) {
            if (target.shouldBeNormals) {
                expected = &target;
            }
            const bool isPicked = picked == static_cast<void*>(target.resource.Get());
            printf("[testhost]   %-22s %p %s\n", target.name, static_cast<void*>(target.resource.Get()),
                   isPicked ? "<== SELECTED as normals" : "");
        }
        if (expectNothingSelected) {
            // Normals takes its extent from velocity, which selected nothing.
            normalsIdentificationPassed = picked == nullptr;
            printf("[testhost] %s\n", normalsIdentificationPassed
                                          ? "PASS - no normals selected, because velocity was never identified"
                                          : "FAIL - normals selected with no velocity extent to key off");
        } else {
            // The collision case must actually have been exercised: velocity's
            // own buffer reached normals' ranking and was excluded as held.
            bool excludedVelocity = false;
            if (logDirectory != nullptr) {
                std::ifstream in(std::string(logDirectory()) + "mv_hook.log", std::ios::binary);
                const std::string log((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                const size_t start = log.rfind("profile: using '");
                excludedVelocity = start != std::string::npos &&
                                   log.find("already held by velocity", start) != std::string::npos;
            }
            const bool pickedRight = expected != nullptr && picked == static_cast<void*>(expected->resource.Get());

            // Rival adoption: the second allocation adopted, the decoy refused.
            std::string secondAlloc, decoy;
            for (const LookAlike& target : lineUp) {
                const std::string ptr = std::to_string(reinterpret_cast<uintptr_t>(target.resource.Get()));
                if (target.allocation == 2) {
                    secondAlloc = ptr;
                } else if (std::string(target.name) == "post-process output") {
                    decoy = ptr;
                }
            }
            int adoptions = 0;
            bool adoptedWrong = false;
            bool refusedDecoy = false;
            if (logDirectory != nullptr) {
                std::ifstream in(std::string(logDirectory()) + "mv_hook.log", std::ios::binary);
                std::string line;
                bool thisRun = false;
                while (std::getline(in, line)) {
                    if (line.find("profile: using '") != std::string::npos) {
                        thisRun = true; // the log is appended across runs; restart at each run's start
                        adoptions = 0;
                        adoptedWrong = refusedDecoy = false;
                    }
                    if (!thisRun) {
                        continue;
                    }
                    if (line.find("normals: ADOPTED") != std::string::npos) {
                        ++adoptions;
                        adoptedWrong = adoptedWrong || line.find(secondAlloc) == std::string::npos;
                    }
                    if (line.find("normals: NOT a rival") != std::string::npos && line.find(decoy) != std::string::npos) {
                        refusedDecoy = true;
                    }
                }
            }
            const bool rivalOk = adoptions >= 1 && !adoptedWrong && refusedDecoy;
            normalsIdentificationPassed = pickedRight && excludedVelocity && rivalOk;
            printf("[testhost]   velocity's buffer excluded from the normals ranking as held: %s\n",
                   excludedVelocity ? "yes" : "NO");
            printf("[testhost]   GBufferA's 2nd allocation adopted as a rival: %d adoption(s)%s\n", adoptions,
                   adoptedWrong ? " - AT LEAST ONE OF SOMETHING ELSE" : "");
            printf("[testhost]   same-format decoy refused as a rival (lacks the depth-call evidence): %s\n",
                   refusedDecoy ? "yes" : "NO");
            printf("[testhost] %s\n",
                   normalsIdentificationPassed
                       ? "PASS - normals selected GBufferA, with SceneVelocity kept out by mutual exclusion"
                       : "FAIL - normals identification (see the lines above, and the 'normals:' lines in the log)");
        }
    }

    // ---- docs/REFACTOR_PLAN.md Phase 8's three required behaviour tests -
    // written to exercise SurfaceRegistry's postconditions, not just
    // identification correctness.
    //
    // Order matters. MV_AUTOCAPTURE arms exactly one burst for this whole
    // process (there is no F8 press available to arm a second), and both
    // test 3 and - usually - a real resize end it (capture.cpp's PRE-
    // existing, non-Phase-8 DumpLayoutAccepts(color, ...) check notices a
    // resized back buffer on the very next Present, no poll interval, and
    // ends the burst via BurstEndReason::LayoutChanged well before
    // velocity's own resize poll - every 30 frames - would). So test 3 runs
    // FIRST, via MvForceReopenVelocity (see its comment in dllmain.cpp for
    // why a direct trigger is used rather than a real resize), while the
    // burst is still fresh; test 1's later resize can then only prove its
    // search-level half (finding 1.11) - the capture-stops half is already
    // proven by test 3's burst-level assertion plus ForgetAllCandidates()
    // being unconditional now (surface_identify.cpp), not gated on which
    // path triggered the reopen.
    //
    // Must run BEFORE MvFlushCapture below: it permanently stops the writer
    // thread, and test 3's manifest check (after it) needs the burst to
    // have actually sealed.
    bool midBurstReopenEndedBurst = true;
    bool resizeTestPassed = true;
    bool depthReopenRecoveryPassed = true;
    if (phase8Tests && hookModule != nullptr && !expectNothingSelected && identificationPassed &&
        depthIdentificationPassed) {
        printf("\n[testhost] ==== Phase 8 behaviour tests ====\n");
        char dumpDirBuf[MAX_PATH]{};
        std::string dir;
        if (GetEnvironmentVariableA("MV_DUMP_DIR", dumpDirBuf, MAX_PATH) != 0) {
            dir = dumpDirBuf;
            if (!dir.empty() && dir.back() != '\\' && dir.back() != '/') {
                dir += '\\';
            }
        }
        if (GetEnvironmentVariableA("MV_AUTOCAPTURE", nullptr, 0) == 0 || dir.empty()) {
            printf("[testhost]   FAIL - --phase8-tests requires MV_AUTOCAPTURE=1 and MV_DUMP_DIR set (same as the "
                   "dump-contents check) - nothing to observe without an active burst\n");
            midBurstReopenEndedBurst = false;
            resizeTestPassed = false;
            depthReopenRecoveryPassed = false;
        } else {
            using IdentifiedFn = void* (*)();
            using VoidFn = void (*)();
            auto identifiedVelocityFn = reinterpret_cast<IdentifiedFn>(
                reinterpret_cast<void*>(GetProcAddress(hookModule, "MvIdentifiedVelocityResource")));
            auto identifiedDepthFn = reinterpret_cast<IdentifiedFn>(
                reinterpret_cast<void*>(GetProcAddress(hookModule, "MvIdentifiedDepthResource")));
            auto forceReopenDepth = reinterpret_cast<VoidFn>(
                reinterpret_cast<void*>(GetProcAddress(hookModule, "MvForceReopenDepth")));
            auto forceReopenVelocity = reinterpret_cast<VoidFn>(
                reinterpret_cast<void*>(GetProcAddress(hookModule, "MvForceReopenVelocity")));

            int nextFrame = frames;
            auto runFrames = [&](int n) {
                for (int i = 0; i < n; ++i) {
                    renderFrame(nextFrame++);
                }
            };
            // Renders one frame at a time until `pred` is true, up to
            // `maxFrames`. Returns the frame it became true on, or -1.
            auto waitFor = [&](int maxFrames, auto pred) {
                for (int i = 0; i < maxFrames; ++i) {
                    renderFrame(nextFrame++);
                    if (pred()) {
                        return nextFrame - 1;
                    }
                }
                return -1;
            };
            auto capturingHasStarted = [&] {
                WIN32_FIND_DATAA found{};
                HANDLE h = FindFirstFileA((dir + "vel_*.bin").c_str(), &found);
                if (h == INVALID_HANDLE_VALUE) {
                    return false;
                }
                FindClose(h);
                return true;
            };

            // MV_AUTOCAPTURE arms its burst 1 real-world second after the
            // hotkey thread starts (capture.cpp), which may not have
            // elapsed yet even after the identify-frames loop above (this
            // harness can render well under 1ms/frame with --no-gbv) - wait
            // for actual evidence of capturing rather than assuming it by
            // frame count.
            if (!capturingHasStarted() && waitFor(120, capturingHasStarted) < 0) {
                printf("[testhost]   FAIL - MV_AUTOCAPTURE never produced a captured frame within 120 extra frames - "
                       "is the burst actually arming?\n");
                midBurstReopenEndedBurst = false;
                resizeTestPassed = false;
                depthReopenRecoveryPassed = false;
            } else {
                // ---- Test 3: a mid-burst reopen must END the burst rather
                // than silently drop frames from later ones (finding 1.14).
                printf("[testhost] test 3: forcing velocity to reopen mid-burst (no swapchain change)...\n");
                if (forceReopenVelocity == nullptr) {
                    printf("[testhost]   FAIL - mv_hook.dll does not export MvForceReopenVelocity\n");
                    midBurstReopenEndedBurst = false;
                } else {
                    forceReopenVelocity();
                    // Give any in-flight slots (kSlots = 4 in capture.cpp)
                    // time to drain and the burst to seal - checked for real
                    // via the manifest after MvFlushCapture below, so this
                    // is generous rather than exact.
                    runFrames(20);
                    printf("[testhost]   reopened - burst.ended_because is checked against the sealed manifest "
                           "after MvFlushCapture below\n");
                }

                // ---- Test 1: a swapchain resize must trigger
                // reidentification, not silently keep the abandoned
                // resource selected (finding 1.11's search-level half). The
                // burst from test 3 has already ended by this point, so
                // this cannot ALSO prove capture stops - see the block
                // comment above for why that half is proven separately.
                printf("[testhost] test 1: forcing a swapchain resize mid-session...\n");
                for (UINT i = 0; i < kBackBufferCount; ++i) {
                    backBuffers[i].Reset(); // DXGI requires no outstanding back-buffer refs before ResizeBuffers
                }
                queue->Signal(fence.Get(), ++fenceValue);
                if (fence->GetCompletedValue() < fenceValue) {
                    fence->SetEventOnCompletion(fenceValue, fenceEvent);
                    WaitForSingleObject(fenceEvent, 5000);
                }
                const UINT resizedWidth = kWidth + 128;
                const UINT resizedHeight = kHeight + 72;
                if (!Check(
                        swapChain->ResizeBuffers(
                            kBackBufferCount, resizedWidth, resizedHeight, DXGI_FORMAT_R8G8B8A8_UNORM, 0),
                        "ResizeBuffers")) {
                    resizeTestPassed = false;
                } else {
                    for (UINT i = 0; i < kBackBufferCount; ++i) {
                        Check(swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffers[i])), "GetBuffer (post-resize)");
                        device->CreateRenderTargetView(backBuffers[i].Get(), nullptr, rtvAt(i));
                    }
                    printf(
                        "[testhost]   resized back buffer %ux%u -> %ux%u\n", kWidth, kHeight, resizedWidth,
                        resizedHeight);

                    // Polled every 30 presented frames once identified
                    // (d3d12_hook.cpp's kResizePollFrames) - generous budget
                    // for the poll to notice plus the reopen to fire.
                    const int reopenFrame = waitFor(50, [&] { return identifiedVelocityFn() == nullptr; });
                    if (reopenFrame < 0) {
                        printf("[testhost]   FAIL - velocity was never observed reopened (null) within 50 frames of "
                               "the resize\n");
                        resizeTestPassed = false;
                    } else {
                        // Same resource, same descriptor - it settles again
                        // after spec.settleFrames (90); generous margin.
                        const int reselectedFrame = waitFor(150, [&] { return identifiedVelocityFn() != nullptr; });
                        if (reselectedFrame < 0) {
                            printf("[testhost]   FAIL - velocity never reidentified after the resize (reopened at "
                                   "frame %d)\n",
                                   reopenFrame);
                            resizeTestPassed = false;
                        } else {
                            printf("[testhost]   PASS - reopened at frame %d, search recovered at frame %d\n",
                                   reopenFrame, reselectedFrame);
                        }
                    }
                }
                printf(
                    "[testhost] test 1 (resize triggers reidentification): %s\n", resizeTestPassed ? "PASS" : "FAIL");

                // ---- Test 2: a depth reallocation must produce a depth
                // answer again afterward, not get stuck AMBIGUOUS or empty.
                printf("[testhost] test 2: forcing scene depth to move to a different resource...\n");
                if (forceReopenDepth == nullptr) {
                    printf("[testhost]   FAIL - mv_hook.dll does not export MvForceReopenDepth\n");
                    depthReopenRecoveryPassed = false;
                } else {
                    void* before = identifiedDepthFn();
                    // Simulate the engine reallocating scene depth: stop
                    // feeding edges to the resource currently selected, and
                    // start feeding CustomDepth (depth look-alike 4 -
                    // structurally IDENTICAL to SceneDepth, previously
                    // excluded only by taking fewer edges/frame)
                    // SceneDepth's own edge rate instead. Usage rate, not
                    // structure, is what told them apart in the first place
                    // (see the depthLineUp comment).
                    depthLineUp[0].edgesPerFrame = 0; // SceneDepth goes silent
                    depthLineUp[4].edgesPerFrame = 4; // CustomDepth takes over
                    forceReopenDepth();

                    const int nullFrame = waitFor(10, [&] { return identifiedDepthFn() == nullptr; });
                    if (nullFrame < 0) {
                        printf("[testhost]   FAIL - depth was never observed null after the forced reopen\n");
                        depthReopenRecoveryPassed = false;
                    } else {
                        // depth's post-reopen window needs at least one
                        // empty-survivors retry cycle (2x settleFrames =
                        // 120) before CustomDepth's freshly-accumulated
                        // edges clear the behaviour gate - generous margin.
                        const int recoveredFrame = waitFor(300, [&] { return identifiedDepthFn() != nullptr; });
                        void* after = identifiedDepthFn();
                        if (recoveredFrame < 0) {
                            printf("[testhost]   FAIL - depth never produced an answer again after the reopen\n");
                            depthReopenRecoveryPassed = false;
                        } else if (after == before) {
                            printf("[testhost]   FAIL - depth re-selected the OLD (now-silent) resource, not the "
                                   "one actually taking the edge now\n");
                            depthReopenRecoveryPassed = false;
                        } else if (after == static_cast<void*>(depthLineUp[4].resource.Get())) {
                            printf("[testhost]   PASS - depth recovered onto the resource that took over the edge "
                                   "(CustomDepth) - not stuck AMBIGUOUS or empty\n");
                        } else {
                            printf("[testhost]   FAIL - depth selected an unexpected resource %p\n", after);
                            depthReopenRecoveryPassed = false;
                        }
                    }
                }
                printf(
                    "[testhost] test 2 (depth reallocation recovers): %s\n",
                    depthReopenRecoveryPassed ? "PASS" : "FAIL");
            }
        }
    }

    if (hookModule != nullptr) {
        // Drain the writer thread before counting dump files below - it
        // finishes writes on a background thread, so counting without this
        // first measures whatever fraction of the burst happened to reach
        // disk by the time we looked, not what capture actually produced.
        // The module is left loaded; a real game never unloads the hook
        // either, so there is nothing else to exercise here.
        using FlushCaptureFn = void (*)();
        auto flushCapture = reinterpret_cast<FlushCaptureFn>(
            reinterpret_cast<void*>(GetProcAddress(hookModule, "MvFlushCapture")));
        if (flushCapture != nullptr) {
            flushCapture();
        } else {
            printf("[testhost] WARNING: no MvFlushCapture export; dump file counts below may be racy\n");
        }
    }

    // ---- Test 3 (continued): the sealed manifest is the source of truth
    // for why the burst ended (see the MvForceReopenVelocity call above).
    // No JSON library in this tree for a one-field check - a substring
    // search on the (small, machine-written) file is sufficient and matches
    // how the rest of this harness avoids new dependencies. ----------------
    if (phase8Tests && hookModule != nullptr && midBurstReopenEndedBurst) {
        char dumpDirBuf[MAX_PATH]{};
        if (GetEnvironmentVariableA("MV_DUMP_DIR", dumpDirBuf, MAX_PATH) == 0) {
            printf("[testhost] test 3 (mid-burst reopen ends the burst): FAIL - MV_DUMP_DIR not set\n");
            midBurstReopenEndedBurst = false;
        } else {
            std::string dir = dumpDirBuf;
            if (!dir.empty() && dir.back() != '\\' && dir.back() != '/') {
                dir += '\\';
            }
            std::ifstream manifestFile(dir + "manifest.json");
            const std::string content(
                (std::istreambuf_iterator<char>(manifestFile)), std::istreambuf_iterator<char>());
            if (content.empty()) {
                printf("[testhost] test 3 (mid-burst reopen ends the burst): FAIL - manifest.json missing or empty\n");
                midBurstReopenEndedBurst = false;
            } else if (content.find("\"surface_reopened\"") != std::string::npos) {
                printf("[testhost] test 3 (mid-burst reopen ends the burst): PASS - "
                       "burst.ended_because=surface_reopened\n");
            } else {
                printf("[testhost] test 3 (mid-burst reopen ends the burst): FAIL - burst did not end with "
                       "surface_reopened; check manifest.json's burst.ended_because\n");
                midBurstReopenEndedBurst = false;
            }
        }
    }

    // ---- Did the capture actually write depth and the View buffer? ------
    //
    // "Identification picked the right resource" and "the bytes reached disk"
    // are different claims, and the gap between them is not theoretical. A
    // change that stopped recording depth copies once the velocity edge arrived
    // left this harness reporting PASS on both identification verdicts and zero
    // debug-layer errors, while producing a dump with no depth in it at all -
    // because here the depth edges come after the velocity edge. Nothing was
    // checking the files.
    //
    // This has to run AFTER the flush above, not before it. The writer thread is
    // still draining when the last frame is presented, so counting files any
    // earlier counts a directory that is still being written - which is exactly
    // what the first version of this check did, reporting a phantom off-by-one
    // that cost a round of chasing a capture bug that was not there.
    // Read the same way the hook reads it, so the harness and the thing it is
    // testing cannot disagree about what the switch is set to.
    bool depthCaptureEnabled = true;
    {
        char value[8]{};
        const DWORD length = GetEnvironmentVariableA("MV_CAPTURE_DEPTH", value, sizeof(value));
        depthCaptureEnabled = !(length > 0 && length < sizeof(value) && std::string(value, length) == "0");
    }

    bool dumpContentsPassed = true;
    if (hookModule != nullptr && GetEnvironmentVariableA("MV_AUTOCAPTURE", nullptr, 0) != 0) {
        char dumpDir[MAX_PATH]{};
        if (GetEnvironmentVariableA("MV_DUMP_DIR", dumpDir, MAX_PATH) != 0) {
            std::string dir = dumpDir;
            if (!dir.empty() && dir.back() != '\\' && dir.back() != '/') {
                dir += '\\';
            }
            auto count = [&](const char* pattern) {
                WIN32_FIND_DATAA found{};
                HANDLE h = FindFirstFileA((dir + pattern).c_str(), &found);
                if (h == INVALID_HANDLE_VALUE) {
                    return 0;
                }
                int n = 0;
                do {
                    ++n;
                } while (FindNextFileA(h, &found));
                FindClose(h);
                return n;
            };
            const int velocities = count("vel_*.bin");
            const int depths = count("depth_*.bin");
            const int normalsFrames = count("normals_*.bin");
            printf("\n[testhost] ==== dump contents ====\n");
            printf("[testhost]   velocity frames: %d\n", velocities);
            printf("[testhost]   depth frames:    %d\n", depths);
            printf("[testhost]   normals frames:  %d\n", normalsFrames);
            // Kept apart from dumpContentsPassed, which the branches below
            // assign outright; combined after them.
            bool normalsDumpOk = true;
            constexpr int kMaxNormalsMissed = 15;
            if (!normals && normalsFrames != 0) {
                // Off by default: one normals blob means the opt-in leaked.
                printf("[testhost] FAIL - normals was captured without MV_SURFACES=normals\n");
                normalsDumpOk = false;
            } else if (normals && !expectNothingSelected &&
                       (normalsFrames > velocities || velocities - normalsFrames > kMaxNormalsMissed)) {
                // GBufferA takes its edge before velocity every frame. The
                // only frames allowed to miss normals are the first
                // allocation switch, before the rival is adopted (stall +
                // frames_before_adopt in the profile, plus slack); every
                // later switch is between two tracked allocations and free.
                printf("[testhost] FAIL - %d velocity frames but %d normals frames (at most %d may miss, the first "
                       "rival-adoption window)\n", velocities, normalsFrames, kMaxNormalsMissed);
                normalsDumpOk = false;
            } else if (normals && !expectNothingSelected) {
                printf("[testhost] PASS - normals on %d of %d captured frames (%d missed: the first rival-adoption "
                       "window, before the 2nd allocation was adopted)\n", normalsFrames, velocities,
                       velocities - normalsFrames);
            }
            if (expectNothingSelected) {
                // Identification selects nothing in this mode, so capturing
                // NOTHING is the correct outcome and an empty dump is the
                // assertion. A file here would mean the hook captured from a
                // resource it had not identified.
                dumpContentsPassed = velocities == 0 && depths == 0;
                printf("[testhost] %s\n",
                       dumpContentsPassed
                           ? "PASS - nothing was captured, which is correct when identification selected nothing"
                           : "FAIL - something was captured despite identification selecting nothing");
            } else if (GetEnvironmentVariableA("MV_CAPTURE_DEPTH", nullptr, 0) != 0 && !depthCaptureEnabled) {
                // MV_CAPTURE_DEPTH=0 is the bandwidth control test: the hook is
                // asked not to copy depth, so a dump with depth in it is the
                // failure and a dump without it is the pass. Asserted rather
                // than assumed, because "the switch did nothing" and "the switch
                // worked" produce the same console output otherwise - which is
                // the exact shape of the bug this harness exists to catch.
                dumpContentsPassed = velocities > 0 && depths == 0;
                printf("[testhost] %s\n",
                       dumpContentsPassed
                           ? "PASS - MV_CAPTURE_DEPTH=0 suppressed depth and left the rest of the frame intact"
                           : "FAIL - MV_CAPTURE_DEPTH=0 did not produce a velocity dump with no depth");
            } else if (velocities > 0 && depths == velocities) {
                printf("[testhost] PASS - every captured frame has velocity and depth\n");
            } else {
                printf("[testhost] FAIL - the dump is missing one of the per-frame artefacts\n");
                dumpContentsPassed = false;
            }
            dumpContentsPassed = dumpContentsPassed && normalsDumpOk;
        }
    }

    printf("\n[testhost] ==== result ====\n");
    printf("[testhost] debug-layer errors during setup (before the hook loaded): %d\n", errorsBeforeHook);
    printf("[testhost] debug-layer errors during %d hooked frames:               %d\n", frames, totalErrors);
    printf("[testhost] %s\n", totalErrors == 0 ? "PASS - the capture path produced no debug-layer errors"
                                               : "FAIL - see the messages above");
    printf("[testhost] %s\n", identificationPassed ? "PASS - velocity identification"
                                                   : "FAIL - velocity identification");
    printf("[testhost] %s\n", depthIdentificationPassed ? "PASS - depth identification"
                                                        : "FAIL - depth identification");
    printf("[testhost] %s\n", dumpContentsPassed ? "PASS - dump contents"
                                                 : "FAIL - dump contents");
    if (normals) {
        printf("[testhost] %s\n", normalsIdentificationPassed ? "PASS - normals identification"
                                                              : "FAIL - normals identification");
    }
    if (phase8Tests) {
        printf("[testhost] %s\n", resizeTestPassed ? "PASS - Phase 8 test 1 (resize stops capture until reidentified)"
                                                     : "FAIL - Phase 8 test 1 (resize stops capture until reidentified)");
        printf("[testhost] %s\n",
               depthReopenRecoveryPassed ? "PASS - Phase 8 test 2 (depth reallocation recovers)"
                                          : "FAIL - Phase 8 test 2 (depth reallocation recovers)");
        printf("[testhost] %s\n",
               midBurstReopenEndedBurst ? "PASS - Phase 8 test 3 (mid-burst reopen ends the burst)"
                                        : "FAIL - Phase 8 test 3 (mid-burst reopen ends the burst)");
    }

    CloseHandle(fenceEvent);
    DestroyWindow(hwnd);
    const int exitCode =
        (totalErrors == 0 && identificationPassed && depthIdentificationPassed && dumpContentsPassed &&
         normalsIdentificationPassed &&
         (!phase8Tests || (resizeTestPassed && depthReopenRecoveryPassed && midBurstReopenEndedBurst)))
            ? 0
            : 1;
    printf("[testhost] exiting with code %d\n", exitCode);
    fflush(stdout);
    return exitCode;
}
