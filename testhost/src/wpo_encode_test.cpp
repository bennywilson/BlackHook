// Synthetic velocity-encode round trip, for `tools/wpo_synthetic_test.py`.
//
// The encode does not depend on where a pixel's motion came from:
// `EncodeVelocityToTexture()` (Common.ush) packs a clip-space delta the same way
// for camera motion, skinning or world-position offset (WPO, e.g. wind-animated
// foliage). So the decode can be tested on WPO-like values without simulating
// WPO geometry: known velocities go through the real encode and the GPU's
// UNORM16 quantization, and `mvtools.py` decodes them unmodified.
//
// A full-screen triangle's pixel shader computes a known (Vx, Vy, Vz) per pixel,
// swept to |V| = 2.0, just inside the encoding's 2.008 ceiling, so it spans the
// nonlinear range without saturating. It is encoded with
// `EncodeVelocityToTexture()` and `VELOCITY_ENCODE_DEPTH` as Common.ush has
// them, rendered to an R16G16B16A16_UNORM target, and written as
// `wpo_vel_00000.bin` with a `meta.txt` giving its layout.
//
// This tests the encode/decode format on WPO-like values. It does not render
// WPO geometry through a real velocity pass.

#include <d3d12.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

// Grid size: a small test pattern, unrelated to any title's resolution. 256
// columns give the X sweep enough distinct encoded values.
constexpr UINT kWidth = 256;
constexpr UINT kHeight = 64;
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R16G16B16A16_UNORM;

// Must match `VX_RANGE`/`VY_RANGE`/`VZ_RANGE` in `tools/wpo_synthetic_test.py`,
// which regenerates the grid to know what was encoded. Nothing checks that they
// agree.
const char* kShaderSource = R"HLSL(
struct VSOut { float4 pos : SV_POSITION; };

VSOut VSMain(uint id : SV_VertexID)
{
    // Fullscreen triangle; no vertex buffer.
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 PSMain(float4 pos : SV_POSITION) : SV_Target
{
    float2 uv = (pos.xy - 0.5) / float2(WIDTH, HEIGHT);

    // The clip-space velocity this pixel encodes, swept over [-2, 2] on both
    // axes. The encoding's ceiling is |V| = 2.008 (encodes to 1.0), so nothing
    // here saturates; the sweep covers the sqrt encoding's nonlinear span,
    // where one UNORM16 step means very different things at the two ends.
    float2 V;
    V.x = lerp(-2.0, 2.0, uv.x);
    V.y = lerp(-2.0, 2.0, uv.y);
    // DeviceZ deltas are ~1e-6 on a real capture; swept over the same size.
    float Vz = lerp(-1e-5, 1e-5, uv.x * uv.y);

    // `EncodeVelocityToTexture()`, Common.ush.
    float2 Vg = sign(V) * sqrt(abs(V)) * (2.0 / sqrt(2.0));
    float2 EncodedXY = Vg * (0.499f * 0.5f) + 32767.0f / 65535.0f;

    // `VELOCITY_ENCODE_DEPTH`, Common.ush. UE 5.7 uses channel 3's bottom bit
    // for `bHasPixelAnimation` (`VELOCITY_Z_LOW_MASK` = 0xFFFE), as
    // `mvtools.depth_encoding()` expects for 5.7.
    uint zbits = asuint(Vz);
    uint hi = (zbits >> 16) & 0xFFFFu;
    uint animBit = (uint(pos.x) ^ uint(pos.y)) & 1u;
    uint lo = (zbits & 0xFFFEu) | animBit;

    return float4(EncodedXY, hi / 65535.0, lo / 65535.0);
}
)HLSL";

bool Check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        printf("[wpo-encode-test] FAILED %s: 0x%08lX\n", what, static_cast<unsigned long>(hr));
        return false;
    }

    return true;
}

bool WriteFileBytes(const std::string& path, const void* data, size_t bytes) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }

    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    return f.good();
}

} // namespace

// Self-contained: its own device, no window or swapchain. Returns 0 on
// success.
int RunWpoEncodeTest(const std::string& outputDir) {
    printf("[wpo-encode-test] %ux%u synthetic velocity grid -> %s\n", kWidth, kHeight, outputDir.c_str());

    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
    }

    // `D3D12CreateDevice(nullptr, ...)` takes the default adapter; there is no
    // swapchain, so no DXGI factory.
    ComPtr<ID3D12Device> device;
    if (!Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice")) {
        return 1;
    }

    ComPtr<ID3D12InfoQueue> infoQueue;
    device.As(&infoQueue);

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (!Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue")) {
        return 1;
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    if (!Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
               "CreateCommandAllocator")) {
        return 1;
    }

    ComPtr<ID3D12GraphicsCommandList> cmdList;
    if (!Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&cmdList)),
               "CreateCommandList")) {
        return 1;
    }

    // No resources bound: the shader uses only `SV_VertexID` and
    // `SV_Position`.
    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> rootBlob, rootError;
    if (!Check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &rootError),
               "D3D12SerializeRootSignature")) {
        if (rootError) {
            printf("[wpo-encode-test] %s\n", static_cast<const char*>(rootError->GetBufferPointer()));
        }

        return 1;
    }

    ComPtr<ID3D12RootSignature> rootSignature;
    if (!Check(device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
                                            IID_PPV_ARGS(&rootSignature)),
               "CreateRootSignature")) {
        return 1;
    }

    char widthText[16], heightText[16];
    snprintf(widthText, sizeof(widthText), "%u.0", kWidth);
    snprintf(heightText, sizeof(heightText), "%u.0", kHeight);
    const D3D_SHADER_MACRO macros[] = {
        {"WIDTH", widthText},
        {"HEIGHT", heightText},
        {nullptr, nullptr},
    };

    ComPtr<ID3DBlob> vsBlob, psBlob, error;
    HRESULT hr = D3DCompile(kShaderSource, strlen(kShaderSource), "wpo_encode_test", macros, nullptr, "VSMain",
                             "vs_5_0", 0, 0, &vsBlob, &error);
    if (FAILED(hr)) {
        printf("[wpo-encode-test] VS compile failed: %s\n",
               error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown");
        return 1;
    }

    hr = D3DCompile(kShaderSource, strlen(kShaderSource), "wpo_encode_test", macros, nullptr, "PSMain", "ps_5_0", 0,
                     0, &psBlob, &error);
    if (FAILED(hr)) {
        printf("[wpo-encode-test] PS compile failed: %s\n",
               error ? static_cast<const char*>(error->GetBufferPointer()) : "unknown");
        return 1;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSignature.Get();
    pso.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
    pso.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kFormat;
    pso.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipelineState;
    if (!Check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipelineState)),
               "CreateGraphicsPipelineState")) {
        return 1;
    }

    D3D12_HEAP_PROPERTIES rtHeapProps{};
    rtHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rtDesc{};
    rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rtDesc.Width = kWidth;
    rtDesc.Height = kHeight;
    rtDesc.DepthOrArraySize = 1;
    rtDesc.MipLevels = 1;
    rtDesc.Format = kFormat;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> renderTarget;
    if (!Check(device->CreateCommittedResource(&rtHeapProps, D3D12_HEAP_FLAG_NONE, &rtDesc,
                                                D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                IID_PPV_ARGS(&renderTarget)),
               "CreateCommittedResource(render target)")) {
        return 1;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    if (!Check(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)), "CreateDescriptorHeap(RTV)")) {
        return 1;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(renderTarget.Get(), nullptr, rtv);

    // Readback buffer sized by `GetCopyableFootprints`, with the row padding
    // `mvtools._rows()` strips.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 totalBytes = 0;
    device->GetCopyableFootprints(&rtDesc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);

    D3D12_HEAP_PROPERTIES rbHeapProps{};
    rbHeapProps.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rbDesc{};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width = totalBytes;
    rbDesc.Height = 1;
    rbDesc.DepthOrArraySize = 1;
    rbDesc.MipLevels = 1;
    rbDesc.Format = DXGI_FORMAT_UNKNOWN;
    rbDesc.SampleDesc.Count = 1;
    rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if (!Check(device->CreateCommittedResource(&rbHeapProps, D3D12_HEAP_FLAG_NONE, &rbDesc,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)),
               "CreateCommittedResource(readback)")) {
        return 1;
    }

    // Draw the triangle, transition to COPY_SOURCE, copy out.
    D3D12_VIEWPORT viewport{0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    cmdList->SetGraphicsRootSignature(rootSignature.Get());
    cmdList->SetPipelineState(pipelineState.Get());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->RSSetViewports(1, &viewport);
    cmdList->RSSetScissorRects(1, &scissor);
    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmdList->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER toCopySrc{};
    toCopySrc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopySrc.Transition.pResource = renderTarget.Get();
    toCopySrc.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toCopySrc.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toCopySrc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &toCopySrc);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = renderTarget.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    if (!Check(cmdList->Close(), "cmdList->Close")) {
        return 1;
    }

    ID3D12CommandList* lists[] = {cmdList.Get()};
    queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    if (!Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence")) {
        return 1;
    }

    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, event);
        WaitForSingleObject(event, INFINITE);
    }

    CloseHandle(event);

    // A shader with no vertex buffer and no bound resources is the kind of path
    // where a wrong root signature or PSO shows up as debug-layer errors.
    int errors = 0;
    if (infoQueue) {
        const UINT64 count = infoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T length = 0;
            infoQueue->GetMessage(i, nullptr, &length);
            std::vector<uint8_t> buffer(length);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
            if (SUCCEEDED(infoQueue->GetMessage(i, message, &length)) &&
                message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                printf("[debug-layer] %.*s\n", static_cast<int>(message->DescriptionByteLength),
                       message->pDescription);
                ++errors;
            }
        }

        infoQueue->ClearStoredMessages();
    }

    void* mapped = nullptr;
    if (!Check(readback->Map(0, nullptr, &mapped), "readback->Map")) {
        return 1;
    }

    CreateDirectoryA(outputDir.c_str(), nullptr);
    const std::string binPath = outputDir + "\\wpo_vel_00000.bin";
    const bool wroteBin = WriteFileBytes(binPath, mapped, static_cast<size_t>(totalBytes));
    readback->Unmap(0, nullptr);
    if (!wroteBin) {
        printf("[wpo-encode-test] failed to write %s\n", binPath.c_str());
        return 1;
    }

    std::string meta;
    meta += "velocity_width=" + std::to_string(kWidth) + "\n";
    meta += "velocity_height=" + std::to_string(kHeight) + "\n";
    meta += "velocity_row_pitch=" + std::to_string(footprint.Footprint.RowPitch) + "\n";
    meta += "velocity_format=" + std::to_string(static_cast<int>(kFormat)) + "\n";
    meta += "engine_version_major=5\n";
    meta += "engine_version_minor=7\n";
    if (!WriteFileBytes(outputDir + "\\meta.txt", meta.data(), meta.size())) {
        printf("[wpo-encode-test] failed to write meta.txt\n");
        return 1;
    }

    printf("[wpo-encode-test] wrote %llu bytes, rowPitch=%u, %d debug-layer error(s)\n",
           static_cast<unsigned long long>(totalBytes), footprint.Footprint.RowPitch, errors);
    printf("[wpo-encode-test] next: python tools/wpo_synthetic_test.py \"%s\"\n", outputDir.c_str());
    return errors == 0 ? 0 : 1;
}
