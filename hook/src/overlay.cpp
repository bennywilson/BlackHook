#include "overlay.h"

#include "logging.h"
#include "surface_registry.h"

#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace mv
{
namespace
{

// Frames of command-list resources in flight. If the next one is still busy,
// the overlay waits up to `kSlotWaitMs` for it, then skips the frame and
// counts it in `g_skippedFrames`.
constexpr UINT kFrames = 3;
constexpr DWORD kSlotWaitMs = 100;
uint64_t g_skippedFrames = 0;

// SRV heap slot i is surface i (`capture.h`), matching t0..t4 in the shader. The
// colour slot holds the snapshot of the finished frame taken at Present. The
// last slot is the status panel (t5).
constexpr UINT kPanelSrv = kSurfaceCount;
constexpr UINT kSrvCount = kSurfaceCount + 1;

// The status panel: lines of Consolas drawn by GDI into a CPU bitmap, uploaded
// to an R8 texture when the text changes, at most every `kPanelRefreshMs`. The
// width is a multiple of `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT`, so the upload
// rows need no padding. The shader places it 16 pixels from the top left.
constexpr UINT kPanelWidth = 768;
constexpr UINT kPanelLines = 7;
constexpr UINT kPanelLineHeight = 20;
constexpr UINT kPanelHeight = kPanelLines * kPanelLineHeight + 8;
constexpr DWORD kPanelRefreshMs = 250;

// Must match `OverlayMode` in overlay.h, including its count.
static_assert(static_cast<int>(OverlayMode::Count) == 4, "kModeNames is out of sync with OverlayMode");
const char* const kModeNames[] = {
    "off", "SceneVelocity (raw channels, engine view)", "SceneVelocity over gameplay",
    "GBuffer grid (frame | depth / normals | specular)"};

// Opacity of the velocity colours when composited over the live frame.
constexpr float kOverGameplayBlend = 0.75f;

const char* kShaderSource = R"HLSL(
Texture2D<float4> Velocity   : register(t0);
Texture2D<float>  Depth      : register(t1); // plane 0 only (depth, not stencil)
Texture2D<float4> SceneColor : register(t2);
Texture2D<float4> Normals    : register(t3);
Texture2D<float4> Specular   : register(t4);
Texture2D<float>  PanelText  : register(t5); // text coverage, drawn by GDI
SamplerState Samp            : register(s0);

cbuffer Params : register(b0)
{
    float4 StaleBySurface;  // 1 = not refreshed this frame: x velocity, y depth, z normals, w specular
    float  Mode;            // 0 = panel only, 1 = velocity only, 2 = over gameplay, 3 = GBuffer grid
    float  Blend;           // opacity when composited over gameplay
    float2 VelocityRes;     // velocity texture dimensions, in pixels
    float  GridRows;        // 2, or 1 without normals and specular
};
#define Stale StaleBySurface.x

// Point fetch at `uv` from a texture of any size. A null SRV (surface never
// copied) reports 0x0 and reads as black.
float4 FetchAt(Texture2D<float4> t, float2 uv)
{
    uint w, h;
    t.GetDimensions(w, h);
    if (w == 0 || h == 0) return 0.0.xxxx;
    int2 p = min(int2(uv * float2(w, h)), int2(w - 1, h - 1));
    return t.Load(int3(p, 0));
}

float FetchDepth(float2 uv)
{
    uint w, h;
    Depth.GetDimensions(w, h);
    if (w == 0 || h == 0) return 0.0;
    int2 p = min(int2(uv * float2(w, h)), int2(w - 1, h - 1));
    return Depth.Load(int3(p, 0));
}

// 2x2: frame | depth / normals | specular. Raw channels, no decode.
float4 GBufferGrid(float2 uv)
{
    // One row: the top row's cells, at the same size so neither image is
    // stretched, centred vertically.
    if (GridRows < 1.5)
    {
        if (uv.y < 0.25 || uv.y >= 0.75) return float4(0.0.xxx, 1.0);
        uv.y -= 0.25;
    }

    float2 cell = floor(uv * 2.0);
    float2 local = frac(uv * 2.0);
    int index = (int)(cell.y * 2.0 + cell.x);
    float stale = index == 0 ? 0.0 // the frame snapshot is taken every Present
                : index == 1 ? StaleBySurface.y
                : index == 2 ? StaleBySurface.z
                             : StaleBySurface.w;

    // Quadrant border: red when that surface was not refreshed this frame.
    float2 edge = min(local, 1.0 - local);
    if (min(edge.x, edge.y) < 0.004)
    {
        return float4(stale > 0.5 ? float3(0.75, 0.0, 0.0) : 0.08.xxx, 1.0);
    }

    float3 c = 0.0.xxx;
    if (index == 0)
    {
        // The game's finished frame, snapshotted before drawing.
        c = SceneColor.SampleLevel(Samp, local, 0).rgb;
    }
    else if (index == 1)
    {
        // Reversed Z: 1 at the near plane, falling towards 0 with distance, so
        // log-scale it. On Skyrunner scene depth spans log2 -11..-1 (median
        // -6); 12 stops puts the median at mid-grey. 0 (sky) is black.
        float d = FetchDepth(local);
        c = d > 0.0 ? saturate((log2(d) + 12.0) / 12.0).xxx : 0.0.xxx;
    }
    else if (index == 2)
    {
        c = FetchAt(Normals, local).rgb;
    }
    else
    {
        c = FetchAt(Specular, local).rgb; // R metallic, G specular, B roughness
    }

    return float4(c, 1.0);
}

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID)
{
    // Fullscreen triangle; no vertex buffer.
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

// The status panel at the top left: text over a translucent dark box. Alpha 0
// outside the box.
float4 Panel(float2 pixel)
{
    uint w, h;
    PanelText.GetDimensions(w, h);
    int2 p = int2(pixel) - 16;
    if (any(p < 0) || p.x >= (int)w || p.y >= (int)h) return 0.0.xxxx;
    float text = PanelText.Load(int3(p, 0));
    return float4(text * float3(1.0, 1.0, 0.85), lerp(0.7, 1.0, text));
}

float4 ModeColor(VSOut i)
{
    if (Mode > 2.5)
    {
        return GBufferGrid(i.uv);
    }

    float3 scene = SceneColor.SampleLevel(Samp, i.uv, 0).rgb;

    // Velocity is copied only at the identified resource's RT -> SRV edge; a
    // frame without one has no new velocity.
    if (Stale > 0.5)
    {
        bool border = i.uv.x < 0.004 || i.uv.x > 0.996 || i.uv.y < 0.007 || i.uv.y > 0.993;
        float3 base = (Mode < 1.5) ? 0.0.xxx : scene;
        return float4(border ? float3(0.55, 0.0, 0.0) : base, 1.0);
    }

    // Point-sample: velocity is at render resolution and the back buffer at
    // output resolution, and filtering at the edge of a written region would
    // blend in the cleared value and show motion that is not there.
    float2 vres = max(VelocityRes, 1.0.xx);
    float2 texel = (floor(i.uv * vres) + 0.5) / vres;
    float3 raw = Velocity.SampleLevel(Samp, texel, 0).rgb;

    // Unwritten pixels are 0, while zero motion encodes near 0.5, so they are
    // masked out.
    bool written = any(raw.rg > 0.0);

    // Channels straight to RGB, as the engine's visualizetexture does. Channel 2
    // is not a motion axis: under `VELOCITY_ENCODE_DEPTH` it holds the high 16
    // bits of the float32 DeviceZ delta (Common.ush), the low 16 in channel 3.
    // Its sign bit gives the engine view its olive and lavender families.
    if (Mode < 1.5)
    {
        return float4(written ? raw : 0.0.xxx, 1.0);
    }

    return float4(written ? lerp(scene, raw, Blend) : scene, 1.0);
}

// The pipeline blends by source alpha. Off, only the panel is drawn over the
// game's frame; in the other modes the output is opaque.
float4 PSMain(VSOut i) : SV_Target
{
    float4 panel = Panel(i.pos.xy);
    if (Mode < 0.5) return panel;
    return float4(lerp(ModeColor(i).rgb, panel.rgb, panel.a), 1.0);
}
)HLSL";

struct FrameResources
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    // Per slot, so a panel upload is never overwritten while the GPU reads it.
    ComPtr<ID3D12Resource> panelUpload;
    uint8_t* panelMapped = nullptr;
    UINT64 fenceValue = 0;
};

std::mutex g_mutex;
std::atomic<int> g_mode{0};

// Joinable with a stop flag, but only ever detached: it is stopped from
// `DLL_PROCESS_DETACH`, after Windows has already ended it.
std::atomic<bool> g_hotkeyStop{false};
std::thread g_hotkeyThread;

ResourceBarrierFn g_originalResourceBarrier = nullptr;
ExecuteCommandListsFn g_originalExecuteCommandLists = nullptr;

ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12RootSignature> g_rootSignature;
ComPtr<ID3D12PipelineState> g_pipelineState;
ComPtr<ID3D12DescriptorHeap> g_srvHeap;
ComPtr<ID3D12DescriptorHeap> g_rtvHeap;
ComPtr<ID3D12Fence> g_fence;
UINT64 g_nextFenceValue = 1;
UINT g_frame = 0;
FrameResources g_frames[kFrames];

// One GPU-side copy per surface (the colour slot is unused): the texture, the
// desc it was created from (`CopyResource` needs identical layouts, so a
// resolution change recreates it), and which presented frame it holds.
struct SourceCopy
{
    ComPtr<ID3D12Resource> tex;
    D3D12_RESOURCE_DESC desc{};
    uint64_t copyFrame = 0;
    bool valid = false;
};
SourceCopy g_sources[kSurfaceCount];

// A copy recorded during frame N is stamped N and drawn at Present N, so the
// expected age is 0. One frame of slack covers an engine that presents at a
// different point in its frame.
constexpr uint64_t kMaxAcceptableCopyAge = 1;

// How often there is no new velocity to draw, counted where the draw decision
// is made.
uint64_t g_freshCount = 0;
uint64_t g_staleCount = 0;
uint64_t g_staleRun = 0;
uint64_t g_staleRunMax = 0;
uint64_t g_maxCopyAge = 0;
uint64_t g_freshnessWindow = 0;
constexpr uint64_t kFreshnessLogInterval = 300;

ComPtr<ID3D12Resource> g_sceneColorTex;

// The desc the scene snapshot was created from; a window resize changes it.
D3D12_RESOURCE_DESC g_sceneColorDesc{};

ComPtr<ID3D12Resource> g_panelTex;
// The GDI side lives for the process: nothing in it depends on the device.
HDC g_panelDc = nullptr;
uint32_t* g_panelBits = nullptr;
// The text last uploaded, and when the panel was last checked for changes.
std::string g_panelText;
DWORD g_panelCheckedAt = 0;

bool SameLayout(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b)
{
    return a.Dimension == b.Dimension && a.Width == b.Width && a.Height == b.Height &&
           a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels && a.Format == b.Format &&
           a.SampleDesc.Count == b.SampleDesc.Count;
}

UINT g_srvDescriptorSize = 0;
UINT g_rtvDescriptorSize = 0;
bool g_initialised = false;
bool g_initFailed = false;

D3D12_CPU_DESCRIPTOR_HANDLE SrvCpu(UINT index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * g_srvDescriptorSize;
    return h;
}

// A null SRV keeps the shader defined before a surface is copied: the slot
// reads as zero.
void CreateNullSrv(UINT index)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(nullptr, &desc, SrvCpu(index));
}

// The format a shader reads a resource of `format` through. Typeless and depth
// formats have no SRV of their own: depth/stencil reads plane 0 (depth) as a
// float, and UE's typeless 8-bit GBuffers read as UNORM. `UNKNOWN`: no
// readable view (a typed D32 depth), so the slot stays null.
DXGI_FORMAT SrvFormatFor(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_UNKNOWN;
        default: return format;
    }
}

void CreateSrvFor(ID3D12Resource* resource, DXGI_FORMAT format, UINT index)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = format;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(resource, &desc, SrvCpu(index));
}

// A texture with the source's layout, so `CopyResource` works directly. Drops
// `RENDER_TARGET` (only ever read) but keeps `ALLOW_DEPTH_STENCIL`: D3D12 copies
// depth-stencil data only between depth-stencil resources.
bool CreateMatchingTexture(const D3D12_RESOURCE_DESC& sourceDesc, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = sourceDesc;
    desc.Flags = sourceDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    return SUCCEEDED(g_device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        nullptr,
        IID_PPV_ARGS(&out)));
}

bool CompileShaders(ComPtr<ID3DBlob>& vs, ComPtr<ID3DBlob>& ps)
{
    ComPtr<ID3DBlob> error;
    UINT flags = 0;
    HRESULT hr = D3DCompile(
        kShaderSource, strlen(kShaderSource), "overlay", nullptr, nullptr, "VSMain", "vs_5_0", flags, 0, &vs, &error);
    if (FAILED(hr))
    {
        Log(std::string("overlay: VS compile failed: ") +
            (error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown"));
        return false;
    }

    hr = D3DCompile(
        kShaderSource, strlen(kShaderSource), "overlay", nullptr, nullptr, "PSMain", "ps_5_0", flags, 0, &ps, &error);
    if (FAILED(hr))
    {
        Log(std::string("overlay: PS compile failed: ") +
            (error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown"));
        return false;
    }

    return true;
}

bool CreateRootSignature()
{
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = kSrvCount;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = 9;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 2;
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr))
    {
        Log(std::string("overlay: SerializeRootSignature failed: ") +
            (error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown"));
        return false;
    }

    hr = g_device->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_rootSignature));
    return SUCCEEDED(hr);
}

// The panel texture, its SRV, one mapped upload buffer per frame slot, and once
// per process the GDI bitmap and font the text is drawn with.
bool CreatePanel()
{
    if (g_panelDc == nullptr)
    {
        HDC dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = kPanelWidth;
        info.bmiHeader.biHeight = -static_cast<LONG>(kPanelHeight); // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;

        // Grey antialiasing, not ClearType: the shader reads one channel.
        HFONT font = CreateFontA(
            -static_cast<int>(kPanelLineHeight - 4), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
        if (bitmap == nullptr || font == nullptr)
        {
            Log("overlay: could not create the status panel's GDI bitmap or font");
            return false;
        }

        SelectObject(dc, bitmap);
        SelectObject(dc, font);
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkMode(dc, TRANSPARENT);
        g_panelDc = dc;
        g_panelBits = static_cast<uint32_t*>(bits);
    }

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kPanelWidth;
    desc.Height = kPanelHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    if (FAILED(g_device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&g_panelTex))))
    {
        return false;
    }

    CreateSrvFor(g_panelTex.Get(), DXGI_FORMAT_R8_UNORM, kPanelSrv);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = static_cast<UINT64>(kPanelWidth) * kPanelHeight;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (FrameResources& f : g_frames)
    {
        void* mapped = nullptr;
        if (FAILED(g_device->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&f.panelUpload))) ||
            FAILED(f.panelUpload->Map(0, nullptr, &mapped)))
        {
            return false;
        }

        f.panelMapped = static_cast<uint8_t*>(mapped);
    }

    g_panelText.clear(); // a new texture starts empty, so upload at once
    g_panelCheckedAt = GetTickCount() - kPanelRefreshMs;
    return true;
}

bool Initialise(ID3D12Device* device, DXGI_FORMAT backBufferFormat)
{
    g_device = device;

    if (!CreateRootSignature())
    {
        return false;
    }

    ComPtr<ID3DBlob> vs, ps;
    if (!CompileShaders(vs, ps))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_rootSignature.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    // Blends by source alpha and keeps the back buffer's own alpha.
    D3D12_RENDER_TARGET_BLEND_DESC& blend = pso.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
    blend.DestBlendAlpha = D3D12_BLEND_ONE;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = backBufferFormat;
    pso.SampleDesc.Count = 1;

    if (FAILED(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipelineState))))
    {
        Log("overlay: CreateGraphicsPipelineState failed");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = kSrvCount;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_srvHeap))))
    {
        return false;
    }

    g_srvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // One RTV per in-flight frame: a descriptor must outlive every command list
    // that references it, and with `kFrames` submissions in flight one shared
    // slot would be overwritten while the GPU may still read it.
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = kFrames;
    if (FAILED(g_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_rtvHeap))))
    {
        return false;
    }

    g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (UINT i = 0; i < kSrvCount; ++i)
    {
        CreateNullSrv(i);
    }

    for (UINT i = 0; i < kFrames; ++i)
    {
        if (FAILED(
                g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frames[i].allocator))))
        {
            return false;
        }

        if (FAILED(g_device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                g_frames[i].allocator.Get(),
                nullptr,
                IID_PPV_ARGS(&g_frames[i].commandList))))
        {
            return false;
        }

        g_frames[i].commandList->Close();
    }

    if (!CreatePanel())
    {
        return false;
    }

    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
    {
        return false;
    }

    Log("overlay: initialised");
    return true;
}

// Blocks until `g_fence` reaches `target`, for at most `timeoutMs`. True if it
// has.
bool WaitForOverlayFence(UINT64 target, DWORD timeoutMs)
{
    if (!g_fence || g_fence->GetCompletedValue() >= target)
    {
        return true;
    }

    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr)
    {
        return false;
    }

    if (SUCCEEDED(g_fence->SetEventOnCompletion(target, event)))
    {
        WaitForSingleObject(event, timeoutMs);
    }

    CloseHandle(event);
    return g_fence->GetCompletedValue() >= target;
}

// Blocks until every in-flight overlay submission has retired. Used only when
// a texture an executing command list may reference is about to be released.
void WaitForOverlayIdle()
{
    UINT64 target = 0;
    for (const FrameResources& f : g_frames)
    {
        target = f.fenceValue > target ? f.fenceValue : target;
    }

    WaitForOverlayFence(target, 1000);
}

void Transition(
    ID3D12GraphicsCommandList* list,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    if (before == after)
    {
        return;
    }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    if (g_originalResourceBarrier != nullptr)
    {
        g_originalResourceBarrier(list, 1, &b);
    }
    else
    {
        list->ResourceBarrier(1, &b);
    }
}

std::string SurfaceStatusLine(const char* name)
{
    const SurfaceIdentifier* identifier = GetSurfaceRegistry().Find(name);
    std::string state = "not enabled (add it to -Surfaces)";
    if (identifier != nullptr)
    {
        UINT64 width = 0;
        UINT height = 0;
        switch (identifier->State())
        {
            case IdentifyState::Selected:
                identifier->Extent(&width, &height);
                state = "found, " + std::to_string(width) + "x" + std::to_string(height);
                break;
            case IdentifyState::Ambiguous:
                state = "ambiguous: candidates tie, nothing captured (see mv_hook.log)";
                break;
            default:
                state = "searching";
                break;
        }
    }

    char line[128]{};
    sprintf_s(line, "%-9s %s", name, state.c_str());
    return line;
}

// One string, a line per `kPanelLines`, separated by '\n'.
std::string PanelStatusText(int mode)
{
    return std::string("BlackHook   F6 restart search   F7 cycle view   F8 capture burst\n") +
           "view: " + kModeNames[mode] + "\n" + SurfaceStatusLine("velocity") + "\n" + SurfaceStatusLine("depth") +
           "\n" + SurfaceStatusLine("normals") + "\n" + SurfaceStatusLine("specular") + "\n" + CaptureStatusLine();
}

// Redraws the panel and records its upload into `list` when the text has
// changed. `fr` is a slot whose last submission has completed.
void UpdatePanel(FrameResources& fr, ID3D12GraphicsCommandList* list, int mode)
{
    const DWORD now = GetTickCount();
    if (now - g_panelCheckedAt < kPanelRefreshMs)
    {
        return;
    }

    g_panelCheckedAt = now;

    const std::string text = PanelStatusText(mode);
    if (text == g_panelText)
    {
        return;
    }

    g_panelText = text;

    memset(g_panelBits, 0, static_cast<size_t>(kPanelWidth) * kPanelHeight * sizeof(uint32_t));
    size_t start = 0;
    for (UINT line = 0; line < kPanelLines && start <= text.size(); ++line)
    {
        size_t end = text.find('\n', start);
        end = end == std::string::npos ? text.size() : end;
        TextOutA(g_panelDc, 8, static_cast<int>(4 + line * kPanelLineHeight), text.data() + start,
                 static_cast<int>(end - start));
        start = end + 1;
    }

    GdiFlush();

    // White text: every channel holds the coverage.
    for (UINT i = 0; i < kPanelWidth * kPanelHeight; ++i)
    {
        fr.panelMapped[i] = static_cast<uint8_t>(g_panelBits[i] & 0xFF);
    }

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g_panelTex.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = fr.panelUpload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8_UNORM, kPanelWidth, kPanelHeight, 1, kPanelWidth};

    Transition(list, g_panelTex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(list, g_panelTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

} // namespace

void OverlaySetOriginals(ResourceBarrierFn resourceBarrier, ExecuteCommandListsFn executeCommandLists)
{
    g_originalResourceBarrier = resourceBarrier;
    g_originalExecuteCommandLists = executeCommandLists;
}

void StartOverlayHotkeyThread()
{
    if (g_hotkeyThread.joinable())
    {
        return;
    }

    // `MV_OVERLAY=<mode>`: start in that mode (for mv_testhost, which cannot
    // press F7).
    char initial[8]{};
    const DWORD length = GetEnvironmentVariableA("MV_OVERLAY", initial, sizeof(initial));
    if (length > 0 && length < sizeof(initial))
    {
        const int mode = atoi(initial);
        if (mode > 0 && mode < static_cast<int>(OverlayMode::Count))
        {
            g_mode.store(mode);
            Log("overlay: starting in mode " + std::to_string(mode) + " (MV_OVERLAY)");
        }
        else
        {
            Log(std::string("overlay: MV_OVERLAY='") + initial + "' is not a mode 1.." +
                std::to_string(static_cast<int>(OverlayMode::Count) - 1) + " - ignored, starting off");
        }
    }

    g_hotkeyThread = std::thread(
        []
        {
            bool wasDown = false;
            while (!g_hotkeyStop.load(std::memory_order_relaxed))
            {
                const bool isDown = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
                if (isDown && !wasDown)
                {
                    const int next = (g_mode.load() + 1) % static_cast<int>(OverlayMode::Count);
                    g_mode.store(next);
                    Log(std::string("overlay: mode -> ") + kModeNames[next]);
                }
                wasDown = isDown;
                Sleep(30);
            }
        });
    Log("overlay: hotkey thread started (F7 = cycle: off, SceneVelocity, over gameplay, GBuffer grid)");
}

void StopOverlayHotkeyThread()
{
    g_hotkeyStop.store(true, std::memory_order_relaxed);
    if (!g_hotkeyThread.joinable())
    {
        return;
    }

    // Detach, never join: see `g_hotkeyThread`.
    g_hotkeyThread.detach();
}

void OverlayCaptureSurface(
    int surface, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source,
    D3D12_RESOURCE_STATES stateBefore, uint64_t frame)
{
    const int mode = g_mode.load(std::memory_order_relaxed);
    // Copy only what the current mode draws: velocity in every mode, the rest
    // for the grid. With the overlay off this is one load and a branch.
    const bool wanted = mode != 0 && surface >= 0 && surface < kSurfaceCount && surface != kSurfaceColor &&
                        (surface == kSurfaceVelocity ||
                                      mode == static_cast<int>(OverlayMode::GBufferGrid));
    if (!wanted || cmdList == nullptr || source == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialised)
    {
        return;
    }

    const UINT index = static_cast<UINT>(surface);
    SourceCopy& copy = g_sources[index];
    if (surface == kSurfaceDepth)
    {
        // last_before_surface, as in capture: depth's first edge can be a
        // partial pass (on Skyrunner, the weapon only), so copy again on each
        // edge until velocity has been copied this frame.
        const SourceCopy& velocity = g_sources[kSurfaceVelocity];
        if (copy.valid && copy.copyFrame == frame && velocity.valid && velocity.copyFrame == frame)
        {
            return;
        }
    }
    else if (copy.valid && copy.copyFrame == frame)
    {
        // First edge of the frame wins: the base pass finishing the target.
        return;
    }

    const D3D12_RESOURCE_DESC desc = source->GetDesc();
    const DXGI_FORMAT srvFormat = SrvFormatFor(desc.Format);
    if (srvFormat == DXGI_FORMAT_UNKNOWN)
    {
        return; // no shader-readable view of this format: the quadrant stays black
    }

    // Recreate whenever the source layout changes.
    if (copy.tex == nullptr || !SameLayout(copy.desc, desc))
    {
        if (copy.tex != nullptr)
        {
            Log(std::string("overlay: ") + kSurfaceNames[index] + " layout changed " + std::to_string(copy.desc.Width) +
                "x" + std::to_string(copy.desc.Height) + " -> " + std::to_string(desc.Width) + "x" +
                std::to_string(desc.Height) + "; recreating copy texture");
            WaitForOverlayIdle();
            copy.tex.Reset();
        }

        if (!CreateMatchingTexture(desc, copy.tex))
        {
            Log(std::string("overlay: failed to create ") + kSurfaceNames[index] + " copy texture");
            return;
        }

        copy.desc = desc;
        CreateSrvFor(copy.tex.Get(), srvFormat, index);
        Log(std::string("overlay: ") + kSurfaceNames[index] + " copy texture created " +
            std::to_string(desc.Width) + "x" + std::to_string(desc.Height) +
            " fmt=" + std::to_string(static_cast<int>(desc.Format)) +
            " (read as " + std::to_string(static_cast<int>(srvFormat)) + ")");
    }

    Transition(cmdList, copy.tex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(cmdList, source, stateBefore, D3D12_RESOURCE_STATE_COPY_SOURCE);

    // Identical descs: one whole-resource copy covers every plane and mip.
    cmdList->CopyResource(copy.tex.Get(), source);

    Transition(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, stateBefore);
    Transition(cmdList, copy.tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    copy.copyFrame = frame;
    copy.valid = true;
}

void OverlayOnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, uint64_t frame)
{
    // The status panel is drawn in every mode, off included.
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (!swapChain || !queue)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initFailed)
    {
        return;
    }

    ComPtr<IDXGISwapChain3> swapChain3;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))))
    {
        return;
    }

    ComPtr<ID3D12Resource> backBuffer;
    const UINT index = swapChain3->GetCurrentBackBufferIndex();
    if (FAILED(swapChain3->GetBuffer(index, IID_PPV_ARGS(&backBuffer))) || backBuffer == nullptr)
    {
        return;
    }

    const D3D12_RESOURCE_DESC backDesc = backBuffer->GetDesc();

    // A back-buffer format change also invalidates the pipeline's RTV format:
    // tear the overlay down and rebuild it next frame.
    if (g_initialised && g_sceneColorTex != nullptr && backDesc.Format != g_sceneColorDesc.Format)
    {
        Log("overlay: back buffer format changed, reinitialising");
        WaitForOverlayIdle();
        g_sceneColorTex.Reset();
        for (SourceCopy& copy : g_sources)
        {
            copy = SourceCopy{};
        }

        g_pipelineState.Reset();
        g_rootSignature.Reset();
        g_srvHeap.Reset();
        g_rtvHeap.Reset();
        g_panelTex.Reset();
        for (FrameResources& f : g_frames)
        {
            f.commandList.Reset();
            f.allocator.Reset();
            f.panelUpload.Reset();
            f.panelMapped = nullptr;
            f.fenceValue = 0;
        }

        g_fence.Reset();
        g_initialised = false;
    }

    if (!g_initialised)
    {
        ComPtr<ID3D12Device> device;
        if (FAILED(backBuffer->GetDevice(IID_PPV_ARGS(&device))))
        {
            return;
        }

        if (!Initialise(device.Get(), backDesc.Format))
        {
            g_initFailed = true;
            Log("overlay: initialisation failed, overlay disabled");
            return;
        }

        g_initialised = true;
    }

    if (g_sceneColorTex == nullptr || !SameLayout(g_sceneColorDesc, backDesc))
    {
        if (g_sceneColorTex != nullptr)
        {
            Log("overlay: back buffer resized to " + std::to_string(backDesc.Width) + "x" +
                std::to_string(backDesc.Height) + "; recreating snapshot texture");
            WaitForOverlayIdle();
            g_sceneColorTex.Reset();
        }

        if (!CreateMatchingTexture(backDesc, g_sceneColorTex))
        {
            return;
        }

        g_sceneColorDesc = backDesc;
        CreateSrvFor(g_sceneColorTex.Get(), backDesc.Format, kSurfaceColor);
    }

    // Is the velocity copy this frame's? Measured before the slot check, so
    // the freshness counters measure the game, not the overlay's pacing.
    const SourceCopy& velocity = g_sources[kSurfaceVelocity];
    const uint64_t copyAge =
        (velocity.valid && frame >= velocity.copyFrame) ? (frame - velocity.copyFrame) : UINT64_MAX;
    const bool stale = !velocity.valid || copyAge > kMaxAcceptableCopyAge;
    // The same test per grid quadrant.
    float staleBySurface[kSurfaceCount];
    for (int s = 0; s < kSurfaceCount; ++s)
    {
        const SourceCopy& c = g_sources[s];
        staleBySurface[s] = (!c.valid || frame < c.copyFrame || frame - c.copyFrame > kMaxAcceptableCopyAge) ? 1.0f : 0.0f;
    }

    // Off, nothing is drawn from the copies, so there is nothing to measure.
    if (mode != 0)
    {
        if (stale)
        {
            ++g_staleCount;
            ++g_staleRun;
            if (g_staleRun > g_staleRunMax)
            {
                g_staleRunMax = g_staleRun;
            }
        }
        else
        {
            ++g_freshCount;
            g_staleRun = 0;
        }

        if (copyAge != UINT64_MAX && copyAge > g_maxCopyAge)
        {
            g_maxCopyAge = copyAge;
        }

        if (++g_freshnessWindow >= kFreshnessLogInterval)
        {
            Log("overlay: velocity freshness over " + std::to_string(g_freshnessWindow) + " presents - " +
                std::to_string(g_freshCount) + " drawn, " + std::to_string(g_staleCount) +
                " withheld as stale, longest stale run " + std::to_string(g_staleRunMax) + " frames, max copy age " +
                std::to_string(g_maxCopyAge) + " frames");
            g_freshnessWindow = 0;
            g_freshCount = 0;
            g_staleCount = 0;
            g_staleRunMax = 0;
            g_maxCopyAge = 0;
        }
    }

    // Waits up to `kSlotWaitMs` for the next frame slot to retire. A skipped
    // frame shows the game's image without the overlay, which reads as a
    // blink. The wait can outlast a frame if the game's queue waits on a fence
    // signalled after this Present (unverified).
    const UINT slotIndex = g_frame % kFrames;
    FrameResources& fr = g_frames[slotIndex];
    if (fr.fenceValue != 0 && !WaitForOverlayFence(fr.fenceValue, kSlotWaitMs))
    {
        if (++g_skippedFrames == 1 || g_skippedFrames % 100 == 0)
        {
            Log("overlay: " + std::to_string(g_skippedFrames) + " frame(s) drawn without the overlay: the GPU had not "
                "finished the overlay's work from " + std::to_string(kFrames) + " presents earlier within " +
                std::to_string(kSlotWaitMs) + " ms");
        }

        return;
    }

    g_frame++;

    if (FAILED(fr.allocator->Reset()) || FAILED(fr.commandList->Reset(fr.allocator.Get(), nullptr)))
    {
        return;
    }

    ID3D12GraphicsCommandList* list = fr.commandList.Get();
    UpdatePanel(fr, list, mode);

    if (mode != 0)
    {
        // Snapshot the finished frame before drawing over it.
        Transition(list, backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(
            list, g_sceneColorTex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(g_sceneColorTex.Get(), backBuffer.Get());
        Transition(
            list, g_sceneColorTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(list, backBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    else
    {
        Transition(list, backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    // Composite over the whole back buffer, through this slot's own RTV.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(slotIndex) * g_rtvDescriptorSize;
    g_device->CreateRenderTargetView(backBuffer.Get(), nullptr, rtv);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    D3D12_VIEWPORT viewport{
        0.0f, 0.0f, static_cast<float>(backDesc.Width), static_cast<float>(backDesc.Height), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(backDesc.Width), static_cast<LONG>(backDesc.Height)};
    if (mode == 0)
    {
        // Only the panel is drawn: its offset in the shader plus its size.
        scissor.right = std::min<LONG>(scissor.right, 16 + kPanelWidth);
        scissor.bottom = std::min<LONG>(scissor.bottom, 16 + kPanelHeight);
    }

    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);

    ID3D12DescriptorHeap* heaps[] = {g_srvHeap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(g_rootSignature.Get());
    list->SetPipelineState(g_pipelineState.Get());
    list->SetGraphicsRootDescriptorTable(0, g_srvHeap->GetGPUDescriptorHandleForHeapStart());

    // Without normals and specular, the grid's second row would be empty.
    SurfaceRegistry& registry = GetSurfaceRegistry();
    const bool gridHasGBuffers = registry.Find("normals") != nullptr || registry.Find("specular") != nullptr;

    // Matches the shader's `cbuffer Params`.
    const float constants[9] = {
        staleBySurface[kSurfaceVelocity],
        staleBySurface[kSurfaceDepth],
        staleBySurface[kSurfaceNormals],
        staleBySurface[kSurfaceSpecular],
        static_cast<float>(mode),
        kOverGameplayBlend,
        static_cast<float>(velocity.desc.Width ? velocity.desc.Width : 1),
        static_cast<float>(velocity.desc.Height ? velocity.desc.Height : 1),
        gridHasGBuffers ? 2.0f : 1.0f,
    };
    list->SetGraphicsRoot32BitConstants(1, 9, constants, 0);

    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);

    Transition(list, backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    if (FAILED(list->Close()))
    {
        return;
    }

    ID3D12CommandList* lists[] = {list};
    if (g_originalExecuteCommandLists != nullptr)
    {
        g_originalExecuteCommandLists(queue, 1, lists);
    }
    else
    {
        queue->ExecuteCommandLists(1, lists);
    }

    fr.fenceValue = g_nextFenceValue++;
    queue->Signal(g_fence.Get(), fr.fenceValue);
}

} // namespace mv
