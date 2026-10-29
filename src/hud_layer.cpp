#include "hud_layer.h"

#include <string>

#include <windows.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "config.h"
#include "log.h"

namespace hud {
namespace {

using CreateUAV_t = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, ID3D12Resource*,
                                              const D3D12_UNORDERED_ACCESS_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
CreateUAV_t g_origCreateUAV = nullptr;

std::mutex g_mutex;
ID3D12Device* g_device = nullptr;
ID3D12CommandQueue* g_queue = nullptr;
std::atomic<UINT> g_scW{0}, g_scH{0};

// The redirect target (written by the game's UI composite) and the per-frame copy the VR side shows.
ID3D12Resource* g_hud = nullptr;      // RGBA8 typeless, kept in UNORDERED_ACCESS
ID3D12Resource* g_zero = nullptr;     // same size, all zero, COPY_SOURCE (clears g_hud by copy)
ID3D12Resource* g_out = nullptr;      // last frame's HUD, COPY_SOURCE at rest
ID3D12Resource* g_retired[8] = {};    // older textures after a resize (kept alive: the GPU may still use them)
int g_nRetired = 0;
UINT g_w = 0, g_h = 0;
std::atomic<bool> g_redirected{false};
std::atomic<DWORD> g_uiThread{0};  // thread that created the first redirected (UI pass) UAV
std::atomic<int> g_skips{0};
std::atomic<uint64_t> g_redirects{0};

// command recording
constexpr int kFrames = 3;
ID3D12CommandAllocator* g_alloc[kFrames] = {};
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12Fence* g_fence = nullptr;
HANDLE g_event = nullptr;
uint64_t g_fenceValue = 0, g_allocFence[kFrames] = {};
int g_frame = 0;

// desktop composite (premultiplied over)
ID3D12RootSignature* g_rootSig = nullptr;
ID3D12PipelineState* g_pso = nullptr;
DXGI_FORMAT g_psoFormat = DXGI_FORMAT_UNKNOWN;
ID3D12DescriptorHeap* g_srvHeap = nullptr;
ID3D12DescriptorHeap* g_rtvHeap = nullptr;
ID3DBlob* g_vs = nullptr;
ID3DBlob* g_ps = nullptr;
ID3DBlob* g_psConv = nullptr;
ID3D12PipelineState* g_psoConv = nullptr;  // HUD PQ -> SDR into g_out (no blending)
bool g_gpuFailed = false;

D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    return x;
}

ID3D12Resource* MakeTexture(UINT w, UINT h, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    ID3D12Resource* r = nullptr;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

// SRV 0 = g_out (desktop composite), SRV 1 = g_hud (conversion input); RTV 0 = backbuffer (set per frame), RTV 1 = g_out
void CreateViews() {
    if (!g_srvHeap || !g_rtvHeap || !g_out || !g_hud) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    const UINT inc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    g_device->CreateShaderResourceView(g_out, &sd, h);
    h.ptr += inc;
    g_device->CreateShaderResourceView(g_hud, &sd, h);
    D3D12_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE r = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    r.ptr += g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_device->CreateRenderTargetView(g_out, &rv, r);
}

// Create the HUD textures for a w x h target (called from the UAV hook, any thread).
bool EnsureTextures(UINT w, UINT h) {
    if (g_hud && g_w == w && g_h == h) return true;
    if (g_hud && g_nRetired + 3 <= 8) {
        g_retired[g_nRetired++] = g_hud;
        g_retired[g_nRetired++] = g_zero;
        g_retired[g_nRetired++] = g_out;
    }
    g_hud = MakeTexture(w, h, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_zero = MakeTexture(w, h, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_SOURCE);  // zero-initialised
    g_out = MakeTexture(w, h, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (!g_hud || !g_zero || !g_out) {
        LOG("HUD: texture creation failed (%u x %u)", w, h);
        g_hud = g_zero = g_out = nullptr;
        return false;
    }
    g_w = w;
    g_h = h;
    CreateViews();
    LOG("HUD: layer textures created (%u x %u)", w, h);
    return true;
}

// Who is creating this UAV? The game's UI pass and an upscaler (DLSS / FSR / XeSS) can both create UAVs on the same
// swap-chain-sized R10G10B10A2 target: the upscaler writes the scene into it, the UI pass then composites the HUD on
// top. Only the UI pass may be redirected; redirecting the upscaler's output put the whole scene into the HUD layer
// (a head-locked flat panel; 4K + DLSS on the A: machine). Returns the upscaler module name, or "" for the game.
std::string UpscalerInStack(std::string* modules) {
    void* frames[32];
    const USHORT n = RtlCaptureStackBackTrace(1, 32, frames, nullptr);
    HMODULE last = nullptr;
    std::string found;
    for (USHORT i = 0; i < n; ++i) {
        HMODULE m = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCWSTR>(frames[i]), &m) || m == last)
            continue;
        last = m;
        char path[MAX_PATH] = {};
        GetModuleFileNameA(m, path, MAX_PATH);
        const char* name = strrchr(path, '\\');
        name = name ? name + 1 : path;
        if (modules && modules->size() < 200) { if (!modules->empty()) *modules += " < "; *modules += name; }
        if (found.empty() && (!_strnicmp(name, "nvngx", 5) || !_strnicmp(name, "_nvngx", 6) || !_strnicmp(name, "sl.dlss", 7) ||
                              !_strnicmp(name, "amd_fidelityfx", 14) || !_strnicmp(name, "libxess", 7)))
            found = name;
    }
    return found;
}

void STDMETHODCALLTYPE Hook_CreateUAV(ID3D12Device* dev, ID3D12Resource* res, ID3D12Resource* counter,
                                      const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    if (res && vr::g_config.hudLayer) {
        D3D12_RESOURCE_DESC rd = res->GetDesc();
        const UINT sw = g_scW.load(), sh = g_scH.load();
        const DXGI_FORMAT vf = desc ? desc->Format : rd.Format;
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.Format == DXGI_FORMAT_R10G10B10A2_UNORM &&
            (vf == DXGI_FORMAT_R10G10B10A2_UNORM || vf == DXGI_FORMAT_UNKNOWN) && sw && rd.Width == sw &&
            rd.Height == sh) {
            std::string mods;
            const std::string upscaler = UpscalerInStack(&mods);
            const DWORD tid = GetCurrentThreadId();
            DWORD first = g_uiThread.load();
            const bool otherThread = first && first != tid;
            if (!upscaler.empty() || otherThread) {
                if (g_skips.fetch_add(1) < 16)
                    LOG("HUD: UAV on the swap-chain-sized target %p NOT redirected (%s; thread %lu, UI thread %lu): %s", res,
                        !upscaler.empty() ? upscaler.c_str() : "other thread", tid, first, mods.c_str());
                g_origCreateUAV(dev, res, counter, desc, dest);
                return;
            }
            std::lock_guard<std::mutex> lock(g_mutex);
            if (EnsureTextures((UINT)rd.Width, rd.Height)) {
                if (!first) g_uiThread = tid;
                D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                g_origCreateUAV(dev, g_hud, nullptr, &ud, dest);
                g_redirected = true;
                if (g_redirects.fetch_add(1) < 8)
                    LOG("HUD: UI composite target %p (%llu x %u) redirected to the HUD layer (thread %lu): %s", res,
                        (unsigned long long)rd.Width, rd.Height, tid, mods.c_str());
                return;
            }
        }
    }
    g_origCreateUAV(dev, res, counter, desc, dest);
}

const char* kHlsl = R"(
Texture2D hudTex : register(t0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 PSMain(VSOut i) : SV_Target { return hudTex.Load(int3(i.pos.xy, 0)); }

// The game composites its UI into an HDR10 (PQ-encoded) image; its final pass converts that to the SDR swap chain
// (ffxvi.exe final-draw pixel shader, ported 1:1 from the capture: PQ -> ICtCp tone map -> sRGB-ish gamma 2.2).
// The HUD layer holds the UI in the same PQ code values (premultiplied), so it is converted the same way here;
// without this the logos and HUD look washed out (PQ shown as SDR: raised blacks).
float3 GameFinal(float3 c) {
    float3 r0 = saturate(c * 1.569664) * 0.637079;
    r0 = exp2(log2(r0) * 0.012683);
    float3 r1 = max(r0 - 0.835938, 0);
    r0 = r1 / (-r0 * 18.6875 + 18.851563);
    r0 = exp2(log2(r0) * 6.277395);
    float3 t = float3(dot(float3(1688, 2146, 262), r0), dot(float3(683, 2951, 462), r0), dot(float3(99, 309, 3688), r0));
    r0 = exp2(log2(t * 0.000244) * 0.159302);
    r0 = (r0 * 18.851563 + 0.835938) / (r0 * 18.6875 + 1);
    r0 = exp2(log2(r0) * 78.84375);
    r1 = float3(dot(float2(0.5, 0.5), r0.xy), dot(float3(1.613770, -3.323486, 1.709717), r0),
                dot(float3(4.378174, -4.245605, -0.132568), r0));
    float3 tm = log2(r1 * 2.068866 - 0.4) * 0.201022 + 0.631503;
    r0 = float3(r1.x > 0.483356 ? tm.x : r1.x, r1.y > 0.483356 ? tm.y : r1.y, r1.z > 0.483356 ? tm.z : r1.z);
    float3 j = float3(dot(float3(1, 0.008609, 0.111030), r0), dot(float3(1, -0.008609, -0.111030), r0),
                      dot(float3(1, 0.560031, -0.320627), r0));
    float3 sgn = float3(j.x >= 0 ? 1 : -1, j.y >= 0 ? 1 : -1, j.z >= 0 ? 1 : -1);
    r1 = exp2(log2(abs(j)) * 0.012683);
    float3 r2 = max(r1 - 0.835938, 0);
    r1 = r2 / (-r1 * 18.6875 + 18.851563);
    r1 = exp2(log2(abs(r1)) * 6.277395);
    r0 = sgn * r1;
    float3 k = float3(dot(float3(3.436607, -2.506452, 0.069845), r0), dot(float3(-0.791330, 1.983600, -0.192271), r0),
                      dot(float3(-0.025950, -0.098914, 1.124864), r0));
    float3 m = float3(dot(float3(1.660496, -0.587656, -0.072840), k), dot(float3(-0.124547, 1.132895, -0.008348), k),
                      dot(float3(-0.018154, -0.100597, 1.118751), k));
    return exp2(log2(saturate(m * 38.461540)) * 0.454545);
}
// HUD (PQ, premultiplied) -> SDR premultiplied
float4 PSConvert(VSOut i) : SV_Target {
    float4 c = hudTex.Load(int3(i.pos.xy, 0));
    if (c.a <= 0.0) return float4(0, 0, 0, 0);
    float3 sdr = GameFinal(saturate(c.rgb / c.a));
    return float4(sdr * c.a, c.a);
}
)";

bool EnsureGpu() {
    if (g_list) return true;
    if (g_gpuFailed) return false;
    g_gpuFailed = true;
    for (auto& a : g_alloc)
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return false;
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, IID_PPV_ARGS(&g_list))))
        return false;
    g_list->Close();
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc ? reinterpret_cast<pD3DCompile>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    if (!compile) return false;
    ID3DBlob* err = nullptr;
    if (FAILED(compile(kHlsl, strlen(kHlsl), "ff16vr_hud", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &g_vs, &err)) ||
        FAILED(compile(kHlsl, strlen(kHlsl), "ff16vr_hud", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &g_ps, &err)) ||
        FAILED(compile(kHlsl, strlen(kHlsl), "ff16vr_hud", nullptr, nullptr, "PSConvert", "ps_5_0", 0, 0, &g_psConv, &err))) {
        LOG("HUD: shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    D3D12_ROOT_PARAMETER p{};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.DescriptorTable.NumDescriptorRanges = 1;
    p.DescriptorTable.pDescriptorRanges = &range;
    p.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs{1, &p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob* blob = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) return false;
    HRESULT hr = g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_rootSig));
    blob->Release();
    if (FAILED(hr)) return false;
    D3D12_DESCRIPTOR_HEAP_DESC sh{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(g_device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g_srvHeap))) ||
        FAILED(g_device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_rtvHeap))))
        return false;
    CreateViews();
    g_gpuFailed = false;
    return true;
}

bool EnsurePso(DXGI_FORMAT fmt) {
    if (g_pso && g_psoFormat == fmt) return true;
    if (g_pso) { g_pso->Release(); g_pso = nullptr; }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_rootSig;
    pd.VS = {g_vs->GetBufferPointer(), g_vs->GetBufferSize()};
    pd.PS = {g_ps->GetBufferPointer(), g_ps->GetBufferSize()};
    auto& rt = pd.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_ONE;               // premultiplied over
    rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = fmt;
    pd.SampleDesc.Count = 1;
    if (FAILED(g_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_pso)))) {
        LOG("HUD: composite PSO for format %d failed", (int)fmt);
        return false;
    }
    g_psoFormat = fmt;
    return true;
}

bool EnsureConvPso() {
    if (g_psoConv) return true;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_rootSig;
    pd.VS = {g_vs->GetBufferPointer(), g_vs->GetBufferSize()};
    pd.PS = {g_psConv->GetBufferPointer(), g_psConv->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    if (FAILED(g_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_psoConv)))) {
        LOG("HUD: conversion PSO failed");
        return false;
    }
    return true;
}

ID3D12GraphicsCommandList* BeginList() {
    int f = g_frame;
    g_frame = (g_frame + 1) % kFrames;
    if (g_fence->GetCompletedValue() < g_allocFence[f]) {
        g_fence->SetEventOnCompletion(g_allocFence[f], g_event);
        WaitForSingleObject(g_event, 1000);
    }
    g_alloc[f]->Reset();
    g_list->Reset(g_alloc[f], nullptr);
    g_allocFence[f] = ~0ull;  // set at submit
    return g_list;
}

void Submit() {
    g_list->Close();
    ID3D12CommandList* lists[] = {g_list};
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_fence, ++g_fenceValue);
    int f = (g_frame + kFrames - 1) % kFrames;
    g_allocFence[f] = g_fenceValue;
}

}  // namespace

void OnDevice(ID3D12Device* device, ID3D12CommandQueue* queue) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_device) return;
    g_device = device;
    g_device->AddRef();
    g_queue = queue;
    g_queue->AddRef();
    if (!vr::g_config.hudLayer) {
        LOG("HUD: separate HUD layer off (HudLayer = 0)");
        return;
    }
    void** vt = *reinterpret_cast<void***>(device);
    DWORD old;
    if (VirtualProtect(&vt[19], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        g_origCreateUAV = reinterpret_cast<CreateUAV_t>(vt[19]);
        vt[19] = reinterpret_cast<void*>(&Hook_CreateUAV);
        VirtualProtect(&vt[19], sizeof(void*), old, &old);
        LOG("HUD: CreateUnorderedAccessView hooked on device %p", device);
    }
}

void OnSwapChainSize(UINT w, UINT h) {
    g_scW = w;
    g_scH = h;
}

void BeforeVr() {
    if (!g_redirected || !g_device) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_hud || !EnsureGpu() || !EnsureConvPso()) return;
    ID3D12GraphicsCommandList* cl = BeginList();
    // g_out = GameFinal(g_hud): the same PQ -> SDR conversion the game applies to the scene + UI image
    D3D12_RESOURCE_BARRIER b[2] = {
        Barrier(g_hud, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        Barrier(g_out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)};
    cl->ResourceBarrier(2, b);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_GPU_DESCRIPTOR_HANDLE srv = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    srv.ptr += g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    cl->SetGraphicsRootSignature(g_rootSig);
    cl->SetDescriptorHeaps(1, &g_srvHeap);
    cl->SetGraphicsRootDescriptorTable(0, srv);
    cl->SetPipelineState(g_psoConv);
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{0, 0, (float)g_w, (float)g_h, 0, 1};
    D3D12_RECT scr{0, 0, (LONG)g_w, (LONG)g_h};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &scr);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(3, 1, 0, 0);
    D3D12_RESOURCE_BARRIER c[2] = {
        Barrier(g_hud, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        Barrier(g_out, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE)};
    cl->ResourceBarrier(2, c);
    cl->CopyResource(g_hud, g_zero);  // clear for the next frame's UI
    D3D12_RESOURCE_BARRIER d = Barrier(g_hud, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->ResourceBarrier(1, &d);
    Submit();
}

void AfterVr(IDXGISwapChain* sc) {
    if (!g_redirected || !g_device || !g_out) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!EnsureGpu()) return;
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return;
    ID3D12Resource* bb = nullptr;
    HRESULT hr = sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
    sc3->Release();
    if (FAILED(hr) || !bb) return;
    D3D12_RESOURCE_DESC bd = bb->GetDesc();
    if (bd.Width != g_w || bd.Height != g_h || !EnsurePso(bd.Format)) {
        bb->Release();
        return;
    }
    D3D12_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = bd.Format;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    g_device->CreateRenderTargetView(bb, &rv, rtv);
    ID3D12GraphicsCommandList* cl = BeginList();
    D3D12_RESOURCE_BARRIER b[2] = {
        Barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET),
        Barrier(g_out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
    cl->ResourceBarrier(2, b);
    cl->SetGraphicsRootSignature(g_rootSig);
    cl->SetDescriptorHeaps(1, &g_srvHeap);
    cl->SetGraphicsRootDescriptorTable(0, g_srvHeap->GetGPUDescriptorHandleForHeapStart());
    cl->SetPipelineState(g_pso);
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{0, 0, (float)bd.Width, (float)bd.Height, 0, 1};
    D3D12_RECT scr{0, 0, (LONG)bd.Width, (LONG)bd.Height};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &scr);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(3, 1, 0, 0);
    D3D12_RESOURCE_BARRIER c[2] = {
        Barrier(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT),
        Barrier(g_out, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE)};
    cl->ResourceBarrier(2, c);
    Submit();
    bb->Release();
}

bool Active() { return g_redirected.load() && g_out; }
ID3D12Resource* Texture() { return g_out; }
UINT Width() { return g_w; }
UINT Height() { return g_h; }

}  // namespace hud
