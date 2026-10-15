// D3D12 validation harness for the capture path.
//
// The capture path cannot run under the debug layer inside a Shipping game, so
// this stands up a device with the debug layer AND GPU-based validation, builds
// a line-up of PLACED render targets (UE5's scene textures come from its
// transient allocator), loads mv_hook.dll into itself and renders frames. The
// hook cannot tell the difference. Two independent verdicts:
//   - zero debug-layer errors over the run, and
//   - identification picked the RIGHT resource out of the line-up, checked by
//     pointer (MvIdentifiedSurface), not by "a dump appeared" - that was true
//     throughout the days the filter was selecting the wrong resource.
// Each decoy is rejected by exactly one filter, so no filter can be quietly
// load-bearing. Then the dump itself is checked (every frame carries every
// surface it should).
//
// USAGE
//   mv_testhost.exe [--hook <dll>] [--frames N] [--no-gbv] [--pipeline oxi|skyrunner]
//                   [--ambiguous] [--normals] [--specular] [--velocity-realloc]
//                   [--phase8-tests] [--phase9-negative]
//   mv_testhost.exe --wpo-encode-test <output_dir>
//
//   --pipeline skyrunner  velocity's 5-transition, UNORDERED_ACCESS pass measured
//                         on a UE 5.2 title (default oxi: the strict 2-cycle)
//   --ambiguous           adds a perfect twin; the only right answer is to refuse
//   --normals/--specular  GBufferA/GBufferB look-alikes and decoys (MV_SURFACES)
//   --velocity-realloc    SceneVelocity through three allocations (rival adoption)
//   --phase8-tests        resize, depth reallocation, mid-burst reopen
//   --phase9-negative     a profile narrowed to the wrong format must find nothing
//
// MV_AUTOCAPTURE=1 makes the hook start a burst by itself (no one to press F8);
// tools/run_testhost_suite.ps1 runs every mode. --wpo-encode-test is separate
// (wpo_encode_test.cpp): it renders a synthetic velocity grid through the
// engine's encode for tools/wpo_synthetic_test.py, without the hook.

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

// Deliberately not any title's resolution: the hook must derive it.
constexpr UINT64 kVelocityWidth = 1120;
constexpr UINT kVelocityHeight = 630;
constexpr DXGI_FORMAT kVelocityFormat = DXGI_FORMAT_R16G16B16A16_UNORM;

constexpr UINT kBackBufferCount = 3;
constexpr UINT kWidth = 1280;
constexpr UINT kHeight = 720;

// What every look-alike is cleared to, so all four channels decode to
// something checkable:
//   x, y = 0.5  -> exactly UE5's encoded zero motion (32767/65535): 0.00000 px.
//   z, w        -> the halves of a float32, as VELOCITY_ENCODE_DEPTH packs
//                  them: 0x3580 and 0 reassemble to +9.5367e-07, the ~1e-6
//                  magnitude a real DeviceZ delta has (not a NaN).
constexpr float kVelocityClear[4] = {0.5f, 0.5f, 13696.0f / 65535.0f, 0.0f};

// How each look-alike behaves at the barrier level. The decoys are chosen so
// that each one is rejected by exactly one of the two independent filters - a
// filter that is right for the wrong reason fails here rather than in a game.
enum class Behaviour {
    Velocity,       // RT -> shader-resource -> RT. Exactly 2 events per frame.
    // RT -> SRV in ONE ResourceBarrier call with scene depth's DEPTH_WRITE ->
    // DEPTH_READ: how GBufferA and GBufferB become readable on Skyrunner.
    ReadWithDepth,
    ComputeWritten, // goes through UNORDERED_ACCESS and takes a UAV barrier
    ExtraPass,      // the same RT <-> SRV cycle, but twice per frame
    // SceneVelocity as measured on a UE 5.2 title: five transitions a frame,
    // through UNORDERED_ACCESS:
    //     RT->UAV   UAV->NON_PS_SRV   NON_PS_SRV->RT   RT->NON_PS_SRV   ...->RT
    // Every behavioural rule once measured on the first title rejects it.
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
    // A TYPELESS resource (UE's 8-bit GBuffers) needs a typed format for its
    // RTV and clear value; UNKNOWN means "same as format".
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
    bool shouldBeSpecular = false; // --specular: the one specular must select

    ComPtr<ID3D12Heap> heap;
    ComPtr<ID3D12Resource> resource;
    UINT rtvIndex = 0;
};

// The depth line-up: each decoy excluded by exactly one of format, the
// ALLOW_DEPTH_STENCIL / DENY_SHADER_RESOURCE pair, and the extent. TYPELESS like
// UE5's scene depth, so the copy exercises the two-plane path.
struct DepthLookAlike {
    const char* name;
    const char* why;
    DXGI_FORMAT format;
    UINT64 width;
    UINT height;
    bool denyShaderResource;
    bool shouldBeSelected;
    // DEPTH_WRITE -> readable edges per frame: the only thing separating scene
    // depth from CustomDepth (same extent, format and flags in UE5).
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

// A 2D texture PLACED at offset 0 of its own heap, the heap shaped as RenderDoc
// showed SceneVelocity's on a UE 5.2 title: no restriction flags (tier 2) and
// CREATE_NOT_ZEROED - the aliasing rules the debug layer checks depend on it.
bool CreatePlacedTexture(ID3D12Device* device, DXGI_FORMAT format, UINT64 width, UINT height,
                         D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE& clear,
                         ComPtr<ID3D12Heap>& heap, ComPtr<ID3D12Resource>& resource) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;

    const D3D12_RESOURCE_ALLOCATION_INFO alloc = device->GetResourceAllocationInfo(0, 1, &desc);
    D3D12_HEAP_DESC heapDesc{};
    heapDesc.SizeInBytes = alloc.SizeInBytes;
    heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapDesc.Alignment = alloc.Alignment;
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    const bool tier2 = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) &&
                       options.ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2;
    heapDesc.Flags = (tier2 ? D3D12_HEAP_FLAG_NONE : D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES) |
                     D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
    return Check(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap)), "CreateHeap") &&
           Check(device->CreatePlacedResource(heap.Get(), 0, &desc, state, &clear, IID_PPV_ARGS(&resource)),
                 "CreatePlacedResource");
}

// An export of the loaded hook, or nullptr.
template <typename Fn>
Fn HookExport(HMODULE hook, const char* name) {
    return hook != nullptr ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(hook, name))) : nullptr;
}

// This run's lines of mv_hook.log, which is appended across runs: from the last
// "InitThread running" on.
std::vector<std::string> ThisRunsLog(HMODULE hook) {
    std::vector<std::string> lines;
    const auto logDirectory = HookExport<const char* (*)()>(hook, "MvLogDirectory");
    if (logDirectory == nullptr) {
        return lines;
    }
    std::ifstream in(std::string(logDirectory()) + "mv_hook.log", std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        if (line.find("InitThread running") != std::string::npos) {
            lines.clear();
        }
        lines.push_back(line);
    }
    return lines;
}

bool AnyLine(const std::vector<std::string>& lines, const char* text) {
    return std::any_of(lines.begin(), lines.end(),
                       [&](const std::string& line) { return line.find(text) != std::string::npos; });
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

// wpo_encode_test.cpp: self-contained, no hook.
int RunWpoEncodeTest(const std::string& outputDir);

int main(int argc, char** argv) {
    std::wstring hookPath;
    int frames = 240;
    bool gpuValidation = true;
    bool ambiguous = false;
    bool phase8Tests = false;
    bool phase9Negative = false;
    bool normals = false;
    bool velocityRealloc = false;
    bool specular = false;
    // Velocity's pass shape: "oxi" (strict 2-cycle) or "skyrunner" (see
    // Behaviour::VelocityComputeAssisted). Both must be found.
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
        } else if (arg == "--specular") {
            specular = true;
        }
    }

    if (!wpoEncodeTestDir.empty()) {
        return RunWpoEncodeTest(wpoEncodeTestDir);
    }

    printf("[testhost] pid=%lu, %d frames, pipeline=%s%s\n", GetCurrentProcessId(), frames,
           pipeline.c_str(),
           ambiguous ? ", ambiguous line-up (identification must refuse to pick)" : "");

    // The settling window must fit inside the run, leaving room for the burst.
    // Set before LoadLibrary: the hook reads its environment once.
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
    if (normals || specular) {
        const char* surfaces = normals && specular ? "normals,specular" : normals ? "normals" : "specular";
        SetEnvironmentVariableA("MV_SURFACES", surfaces);
        printf("[testhost] MV_SURFACES=%s: GBuffer look-alikes join the line-up and must be the ones selected\n",
               surfaces);
    }
    // Every path below that expects identification to select nothing.
    const bool expectNothingSelected = ambiguous || phase9Negative;
    std::string dumpDir;
    if (char value[MAX_PATH]{}; GetEnvironmentVariableA("MV_DUMP_DIR", value, MAX_PATH) != 0) {
        dumpDir = value;
        if (dumpDir.back() != '\\' && dumpDir.back() != '/') {
            dumpDir += '\\';
        }
    }

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

    // 2. The line-up: SceneVelocity and its plausible mistakes. Every entry is a
    // PLACED resource, whose aliasing rules the debug layer checks separately.
    std::vector<LookAlike> lineUp = {
        {"SceneVelocity", "the real thing: velocity format, RT|UAV", kVelocityFormat,
         kVelocityWidth, kVelocityHeight,
         pipeline == "skyrunner" ? Behaviour::VelocityComputeAssisted : Behaviour::Velocity, true},

        // This project's most expensive bug: identical in every way but one
        // digit of the format enum (FLOAT is 10, UNORM 11). GetFormat never
        // returns FLOAT, so the structural filter must reject it.
        {"HDR pool target", "rejected on structure: R16G16B16A16_FLOAT is not a format "
                            "FVelocityRendering::GetFormat can return",
         DXGI_FORMAT_R16G16B16A16_FLOAT, kVelocityWidth, kVelocityHeight, Behaviour::Velocity, false},

        // Right format, flags and extent, written by compute: only the UAV
        // barrier tells it apart.
        {"compute-written RG16", "rejected on behaviour: goes through UNORDERED_ACCESS and takes a UAV barrier",
         DXGI_FORMAT_R16G16_UNORM, kVelocityWidth, kVelocityHeight, Behaviour::ComputeWritten, false},

        // Byte-identical descriptor to the real one. Nothing structural can
        // separate these two; only "exactly 2 barrier events per frame" does.
        {"twin, read twice", "rejected on behaviour: 4 RT<->SRV events per frame, not 2",
         kVelocityFormat, kVelocityWidth, kVelocityHeight, Behaviour::ExtraPass, false},
    };

    if (velocityRealloc) {
        // Without the "twin, read twice" decoy: it takes the velocity edge every
        // frame, which keeps resetting rival adoption's consecutive-frame run (a
        // known limit: it follows one rival at a time; no measured title has
        // such a twin).
        lineUp.erase(std::remove_if(lineUp.begin(), lineUp.end(),
                                    [](const LookAlike& t) { return t.behaviour == Behaviour::ExtraPass; }),
                     lineUp.end());
        // Two more allocations of SceneVelocity (same descriptor, new address),
        // rendering only after identification: rival adoption must follow them,
        // and the silence check must keep counting earlier-adopted ones.
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
        // UE5's GBufferA (R10G10B10A2, RT only), finished by the base pass, so
        // its edge comes before velocity's. SceneVelocity passes the normals
        // gates too - only mutual exclusion keeps it out. The post-process
        // output below tied GBufferA 10-10 on Skyrunner; only shares_call_with_
        // edge (readable in the same call as depth) separates them.
        LookAlike gbufferA{"GBufferA", "normals: R10G10B10A2, RT only, readable in the same call as depth",
                           DXGI_FORMAT_R10G10B10A2_UNORM, kVelocityWidth, kVelocityHeight, Behaviour::ReadWithDepth,
                           false};
        gbufferA.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        gbufferA.shouldBeNormals = true;
        LookAlike postOutput{"post-process output", "normals decoy: same format/flags/extent, never batched with depth",
                             DXGI_FORMAT_R10G10B10A2_UNORM, kVelocityWidth, kVelocityHeight, Behaviour::Velocity,
                             false};
        postOutput.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        // GBufferA's second allocation, taking turns with the first: normals must
        // adopt it as a rival, and must NOT adopt the decoy, which takes its
        // edge every frame.
        LookAlike gbufferA2 = gbufferA;
        gbufferA2.name = "GBufferA (2nd alloc)";
        gbufferA2.why = "normals: the same texture reallocated; must be adopted as a rival, not selected";
        gbufferA2.shouldBeNormals = false;
        gbufferA.allocation = 1;
        gbufferA2.allocation = 2;
        // Scene colour as measured: shares calls with depth too, but a different
        // descriptor (R16G16B16A16_FLOAT, RT|UAV) - refused by same_descriptor.
        LookAlike sceneColor{"scene colour", "normals rival decoy: shares calls with depth, different descriptor",
                             DXGI_FORMAT_R16G16B16A16_FLOAT, kVelocityWidth, kVelocityHeight,
                             Behaviour::ReadWithDepth, false};
        lineUp.insert(lineUp.begin(), sceneColor);
        lineUp.insert(lineUp.begin(), postOutput);
        lineUp.insert(lineUp.begin(), gbufferA2);
        lineUp.insert(lineUp.begin(), gbufferA);
    }

    if (specular) {
        // GBufferB and GBufferC as measured on Skyrunner: identical structurally;
        // GBufferB is readable in the same call as depth, GBufferC never.
        LookAlike gbufferB{"GBufferB", "specular: B8G8R8A8_TYPELESS, RT only, readable in the same call as depth",
                           DXGI_FORMAT_B8G8R8A8_TYPELESS, kVelocityWidth, kVelocityHeight,
                           Behaviour::ReadWithDepth, false};
        gbufferB.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        gbufferB.viewFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        gbufferB.shouldBeSpecular = true;
        LookAlike gbufferC = gbufferB;
        gbufferC.name = "GBufferC";
        gbufferC.why = "specular decoy: identical descriptor (base colour), never batched with depth";
        gbufferC.behaviour = Behaviour::Velocity;
        gbufferC.shouldBeSpecular = false;
        lineUp.insert(lineUp.begin(), gbufferC);
        lineUp.insert(lineUp.begin(), gbufferB);
    }

    if (pipeline == "skyrunner") {
        // Without the "twin, read twice" decoy: it exists to test the strict
        // two-events signal, which fires for neither when velocity itself is
        // not a 2-cycle - they then tie (11-11) and identification correctly
        // refuses. --ambiguous already covers refusal.
        lineUp.erase(std::remove_if(lineUp.begin(), lineUp.end(),
                                    [](const LookAlike& t) {
                                        return t.behaviour == Behaviour::ExtraPass;
                                    }),
                     lineUp.end());
    }

    if (ambiguous) {
        // A perfect twin of whichever SceneVelocity this pipeline uses: the
        // only correct answer is to refuse and say so.
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
        // Must match the clear below, or every frame warns about a slow clear.
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = target.viewFormat != DXGI_FORMAT_UNKNOWN ? target.viewFormat : target.format;
        memcpy(clearValue.Color, kVelocityClear, sizeof(kVelocityClear));
        if (!CreatePlacedTexture(device.Get(), target.format, target.width, target.height, target.flags,
                                 D3D12_RESOURCE_STATE_RENDER_TARGET, clearValue, target.heap, target.resource)) {
            return 1;
        }
        target.rtvIndex = kBackBufferCount + static_cast<UINT>(i);
        if (target.viewFormat != DXGI_FORMAT_UNKNOWN) {
            D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
            rtvDesc.Format = target.viewFormat;
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device->CreateRenderTargetView(target.resource.Get(), &rtvDesc, rtvAt(target.rtvIndex));
        } else {
            device->CreateRenderTargetView(target.resource.Get(), nullptr, rtvAt(target.rtvIndex));
        }
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
        // Structurally IDENTICAL to scene depth: only per-frame usage separates
        // them (and on a real title identification once refused to pick).
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
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        // Not 0.0: UE5's reversed-Z far plane, which the tools read as "no
        // geometry". 0.5 can only have come through the copy intact.
        clearValue.DepthStencil.Depth = 0.5f;
        clearValue.DepthStencil.Stencil = 0;
        const D3D12_RESOURCE_FLAGS flags =
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
            (target.denyShaderResource ? D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE : D3D12_RESOURCE_FLAG_NONE);
        if (!CreatePlacedTexture(device.Get(), target.format, target.width, target.height, flags,
                                 D3D12_RESOURCE_STATE_DEPTH_WRITE, clearValue, target.heap, target.resource)) {
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

    // 3. Load the hook into ourselves: same DllMain and InstallD3D12Hooks as an
    // injection; the injector is not what is under test.
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
    // The resource the hook selected for a surface, or nullptr.
    const auto identifiedFn = HookExport<void* (*)(const char*)>(hookModule, "MvIdentifiedSurface");
    auto identified = [&](const char* surface) { return identifiedFn != nullptr ? identifiedFn(surface) : nullptr; };

    auto barrier = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        cmdList->ResourceBarrier(1, &b);
    };

    // UE5 settles depth before velocity, which the capture path optimises
    // for; a title ordered the other way must still get depth. Both orderings
    // are modelled.
    const bool depthBeforeVelocity = pipeline != "skyrunner";
    printf("[testhost] depth edges are emitted %s the velocity edge\n",
           depthBeforeVelocity ? "BEFORE (the UE5 prepass ordering)"
                               : "AFTER (the ordering that exercises the fallback)");

    // The depth line-up, cleared through a DSV first (GPU-based validation
    // rejects reading uninitialised placed memory). Scene depth takes several
    // DEPTH_WRITE -> readable edges a frame, as with a partial prepass, so the
    // last-before-velocity rule is exercised.
    auto emitDepthEdges = [&]() {
        for (DepthLookAlike& target : depthLineUp) {
            const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvAt(target.dsvIndex);
            cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
            cmdList->ClearDepthStencilView(
                dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.5f, 0, 0, nullptr);
            ID3D12Resource* resource = target.resource.Get();
            // Both readable states the real title uses: DEPTH_READ and SRV.
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

    // One frame, as a lambda so the --phase8-tests can keep rendering past the
    // initial window.
    // --normals: GBufferA allocation 1 alone until identification has settled,
    // then the two alternate in 30-frame stretches.
    const int gbufferSwitchStart = identifyFrames + 20;
    auto gbufferAllocationActive = [&](int allocation, int frame) {
        const bool second = frame >= gbufferSwitchStart && ((frame - gbufferSwitchStart) / 30) % 2 == 0;
        return allocation == (second ? 2 : 1);
    };

    // --velocity-realloc: allocation 1, then 2 (adopted), 3 (adopted), then 2
    // again for 500 frames - longer than the 300-frame silence threshold, on
    // an allocation no longer the latest rival.
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
            // Write it as a render target, so it holds real bytes.
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvAt(target.rtvIndex);
            cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            cmdList->ClearRenderTargetView(rtv, kVelocityClear, 0, nullptr);

            ID3D12Resource* resource = target.resource.Get();
            switch (target.behaviour) {
                case Behaviour::Velocity:
                    // The edge the hook triggers on; its copy is recorded
                    // inside this pair, under the debug layer.
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
                    // Enters UNORDERED_ACCESS without ever taking a UAV
                    // barrier - why those are two separate signals.
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
    bool identificationPassed = true;
    if (hookModule != nullptr) {
        if (identifiedFn == nullptr) {
            printf("\n[testhost] FAIL - mv_hook.dll does not export MvIdentifiedSurface; "
                   "cannot check which resource was picked\n");
            identificationPassed = false;
        } else {
            void* picked = identified("velocity");
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
                // The narrowed profile is the one that ran (a load failure also
                // selects nothing), nothing was selected (no fallback), and the
                // hook said so.
                const auto activeProfile = HookExport<const char* (*)()>(hookModule, "MvActiveProfile");
                const std::string profile = activeProfile != nullptr ? activeProfile() : "<no MvActiveProfile export>";
                const bool profileOk = profile == kNegativeProfile;
                const bool saidNoCandidate = AnyLine(ThisRunsLog(hookModule), "identify: NO candidate");

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
                // Two indistinguishable candidates: only refusal is correct.
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
                    // Both extra allocations adopted, and no false STOPPED while
                    // the earlier-adopted allocation 2 carried the pass.
                    const std::vector<std::string> log = ThisRunsLog(hookModule);
                    const auto adoptions = std::count_if(log.begin(), log.end(), [](const std::string& line) {
                        return line.find("identify: ADOPTED") != std::string::npos;
                    });
                    const bool stopped = AnyLine(log, "identify: the pass has STOPPED");
                    const bool reallocOk = adoptions >= 2 && !stopped;
                    printf("[testhost]   velocity reallocations adopted: %d (need 2); pass declared STOPPED while "
                           "an earlier-adopted allocation carried it: %s\n", static_cast<int>(adoptions), stopped ? "YES" : "no");
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
        if (identifiedFn == nullptr) {
            printf("\n[testhost] FAIL - mv_hook.dll does not export MvIdentifiedSurface\n");
            depthIdentificationPassed = false;
        } else {
            void* picked = identified("depth");
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
                // Velocity identified nothing, and depth is sized from it.
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
        void* picked = identified("normals");
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
            const std::vector<std::string> log = ThisRunsLog(hookModule);
            const bool excludedVelocity = AnyLine(log, "already held by velocity");
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
            for (const std::string& line : log) {
                if (line.find("normals: ADOPTED") != std::string::npos) {
                    ++adoptions;
                    adoptedWrong = adoptedWrong || line.find(secondAlloc) == std::string::npos;
                }
                if (line.find("normals: NOT a rival") != std::string::npos && line.find(decoy) != std::string::npos) {
                    refusedDecoy = true;
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

    // ---- Phase 10 follow-up: did specular pick GBufferB over GBufferC? ---
    bool specularIdentificationPassed = true;
    if (specular && hookModule != nullptr) {
        void* picked = identified("specular");
        const LookAlike* expected = nullptr;
        printf("\n[testhost] ==== specular identification ====\n");
        for (const LookAlike& target : lineUp) {
            if (target.shouldBeSpecular) {
                expected = &target;
            }
            const bool isPicked = picked == static_cast<void*>(target.resource.Get());
            if (isPicked || target.format == DXGI_FORMAT_B8G8R8A8_TYPELESS) {
                printf("[testhost]   %-22s %p %s\n", target.name, static_cast<void*>(target.resource.Get()),
                       isPicked ? "<== SELECTED as specular" : "");
            }
        }
        if (expectNothingSelected) {
            specularIdentificationPassed = picked == nullptr;
            printf("[testhost] %s\n", specularIdentificationPassed
                                           ? "PASS - no specular selected, because velocity was never identified"
                                           : "FAIL - specular selected with no velocity extent to key off");
        } else {
            specularIdentificationPassed = expected != nullptr && picked == static_cast<void*>(expected->resource.Get());
            printf("[testhost] %s\n",
                   specularIdentificationPassed
                       ? "PASS - specular selected GBufferB over the identical-descriptor GBufferC (depth-call evidence)"
                       : "FAIL - specular identification (see the 'specular:' lines in the log)");
        }
    }

    // ---- --phase8-tests: resize, depth reallocation, mid-burst reopen ------
    //
    // MV_AUTOCAPTURE arms exactly one burst, and both test 3 and a real resize
    // end it (a resize via the colour layout check, before velocity's 30-frame
    // resize poll). So test 3 runs FIRST, through MvForceReopenVelocity, while
    // the burst is fresh; test 1 then checks reidentification only.
    // Must run BEFORE MvFlushCapture, which stops the writer for good.
    bool midBurstReopenEndedBurst = true;
    bool resizeTestPassed = true;
    bool depthReopenRecoveryPassed = true;
    if (phase8Tests && hookModule != nullptr && !expectNothingSelected && identificationPassed &&
        depthIdentificationPassed) {
        printf("\n[testhost] ==== Phase 8 behaviour tests ====\n");
        const std::string& dir = dumpDir;
        if (GetEnvironmentVariableA("MV_AUTOCAPTURE", nullptr, 0) == 0 || dir.empty()) {
            printf("[testhost]   FAIL - --phase8-tests requires MV_AUTOCAPTURE=1 and MV_DUMP_DIR set (same as the "
                   "dump-contents check) - nothing to observe without an active burst\n");
            midBurstReopenEndedBurst = false;
            resizeTestPassed = false;
            depthReopenRecoveryPassed = false;
        } else {
            const auto forceReopenDepth = HookExport<void (*)()>(hookModule, "MvForceReopenDepth");
            const auto forceReopenVelocity = HookExport<void (*)()>(hookModule, "MvForceReopenVelocity");

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

            // MV_AUTOCAPTURE arms 1s (wall clock) after the hook starts, which
            // may not have passed yet: wait for evidence of capture.
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
                    // Let in-flight slots drain and the burst seal; the
                    // manifest is checked after MvFlushCapture.
                    runFrames(20);
                    printf("[testhost]   reopened - burst.ended_because is checked against the sealed manifest "
                           "after MvFlushCapture below\n");
                }

                // ---- Test 1: a swapchain resize must trigger
                // reidentification, not keep the abandoned resource.
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

                    // Polled every 30 frames once identified.
                    const int reopenFrame = waitFor(50, [&] { return identified("velocity") == nullptr; });
                    if (reopenFrame < 0) {
                        printf("[testhost]   FAIL - velocity was never observed reopened (null) within 50 frames of "
                               "the resize\n");
                        resizeTestPassed = false;
                    } else {
                        // Same resource, same descriptor - it settles again
                        // after spec.settleFrames (90); generous margin.
                        const int reselectedFrame = waitFor(150, [&] { return identified("velocity") != nullptr; });
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
                    void* before = identified("depth");
                    // Scene depth reallocated: the selected one goes silent and
                    // CustomDepth (structurally identical) takes its edge rate.
                    depthLineUp[0].edgesPerFrame = 0; // SceneDepth goes silent
                    depthLineUp[4].edgesPerFrame = 4; // CustomDepth takes over
                    forceReopenDepth();

                    const int nullFrame = waitFor(10, [&] { return identified("depth") == nullptr; });
                    if (nullFrame < 0) {
                        printf("[testhost]   FAIL - depth was never observed null after the forced reopen\n");
                        depthReopenRecoveryPassed = false;
                    } else {
                        // Needs at least one empty-survivors retry (2x
                        // settleFrames = 120) first; generous margin.
                        const int recoveredFrame = waitFor(300, [&] { return identified("depth") != nullptr; });
                        void* after = identified("depth");
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
        // Drain the writer before counting files: it writes on a background
        // thread.
        const auto flushCapture = HookExport<void (*)()>(hookModule, "MvFlushCapture");
        if (flushCapture != nullptr) {
            flushCapture();
        } else {
            printf("[testhost] WARNING: no MvFlushCapture export; dump file counts below may be racy\n");
        }
    }

    // ---- Test 3 (continued): the sealed manifest says why the burst ended.
    // A substring search: no JSON library in this tree for one field.
    if (phase8Tests && hookModule != nullptr && midBurstReopenEndedBurst) {
        if (dumpDir.empty()) {
            printf("[testhost] test 3 (mid-burst reopen ends the burst): FAIL - MV_DUMP_DIR not set\n");
            midBurstReopenEndedBurst = false;
        } else {
            std::ifstream manifestFile(dumpDir + "manifest.json");
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

    // ---- Did the bytes reach disk? -----------------------------------------
    // A separate claim from "identified the right resource": a change once left
    // every identification PASS while writing no depth at all. Counted after
    // the flush. MV_CAPTURE_DEPTH read as the hook reads it.
    bool depthCaptureEnabled = true;
    {
        char value[8]{};
        const DWORD length = GetEnvironmentVariableA("MV_CAPTURE_DEPTH", value, sizeof(value));
        depthCaptureEnabled = !(length > 0 && length < sizeof(value) && std::string(value, length) == "0");
    }

    bool dumpContentsPassed = true;
    if (hookModule != nullptr && GetEnvironmentVariableA("MV_AUTOCAPTURE", nullptr, 0) != 0) {
        if (!dumpDir.empty()) {
            const std::string& dir = dumpDir;
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
            const int specularFrames = count("specular_*.bin");
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
                // Only the first allocation switch, before the rival is adopted,
                // may miss normals; later switches are between tracked copies.
                printf("[testhost] FAIL - %d velocity frames but %d normals frames (at most %d may miss, the first "
                       "rival-adoption window)\n", velocities, normalsFrames, kMaxNormalsMissed);
                normalsDumpOk = false;
            } else if (normals && !expectNothingSelected) {
                printf("[testhost] PASS - normals on %d of %d captured frames (%d missed: the first rival-adoption "
                       "window, before the 2nd allocation was adopted)\n", normalsFrames, velocities,
                       velocities - normalsFrames);
            }
            printf("[testhost]   specular frames: %d\n", specularFrames);
            if (!specular && specularFrames != 0) {
                printf("[testhost] FAIL - specular was captured without MV_SURFACES=specular\n");
                normalsDumpOk = false;
            } else if (specular && !expectNothingSelected && specularFrames != velocities) {
                // GBufferB here has one allocation: every frame must carry it.
                printf("[testhost] FAIL - %d velocity frames but %d specular frames\n", velocities, specularFrames);
                normalsDumpOk = false;
            } else if (specular && !expectNothingSelected) {
                printf("[testhost] PASS - every captured frame also has specular\n");
            }
            if (expectNothingSelected) {
                // Nothing identified, so nothing may be captured.
                dumpContentsPassed = velocities == 0 && depths == 0;
                printf("[testhost] %s\n",
                       dumpContentsPassed
                           ? "PASS - nothing was captured, which is correct when identification selected nothing"
                           : "FAIL - something was captured despite identification selecting nothing");
            } else if (GetEnvironmentVariableA("MV_CAPTURE_DEPTH", nullptr, 0) != 0 && !depthCaptureEnabled) {
                // MV_CAPTURE_DEPTH=0 (the bandwidth control test): velocity but
                // no depth is the pass.
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
    if (specular) {
        printf("[testhost] %s\n", specularIdentificationPassed ? "PASS - specular identification"
                                                               : "FAIL - specular identification");
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
         normalsIdentificationPassed && specularIdentificationPassed &&
         (!phase8Tests || (resizeTestPassed && depthReopenRecoveryPassed && midBurstReopenEndedBurst)))
            ? 0
            : 1;
    printf("[testhost] exiting with code %d\n", exitCode);
    fflush(stdout);
    return exitCode;
}
