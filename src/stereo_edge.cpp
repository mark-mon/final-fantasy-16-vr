// Stereo edge fix (StereoEdgeFix): shared upscaler history (DlssGhostingFix off) without the halo.
//
// With one DLSS / FSR history for both eyes, the previous frame is the other eye. At every depth edge a strip of
// background was hidden behind the nearer object from the other eye's viewpoint; the upscaler blends the occluder's
// history into it: the halo around characters. Right before the upscaler runs, this pass copies the motion vectors
// it was given and points them off screen for those pixels (a nearer pixel q on the same row lands on the same spot
// in the previous frame), so the upscaler treats them as newly revealed and uses the current frame there. The copy is
// handed to the upscaler instead of the game's texture (DLSS: a re-tag of the motion vectors; FSR: the dispatch).
//
// Run 249 / the user's log of Nov 4: changing the game's velocity-resolve output does nothing (the upscalers read
// another texture), hence the work at the upscaler's own inputs.
//
// The pass records into the game's command list, so the compute state it changes (descriptor heaps, root signature,
// root arguments, pipeline) is tracked per command list and put back afterwards.
#include "stereo_edge.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "config.h"
#include "log.h"
#include "taa.h"

namespace stereo_edge {
namespace {

// ---- compute state of the game's command list (per recording thread)
struct RootArg {
    int kind = 0;  // 0 none, 1 table, 2 constants, 3 CBV, 4 SRV, 5 UAV
    D3D12_GPU_DESCRIPTOR_HANDLE table{};
    D3D12_GPU_VIRTUAL_ADDRESS va = 0;
    uint32_t consts[64] = {};
    uint32_t constMask[2] = {};  // which constants were set (64 bits)
};
struct ListState {
    ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12DescriptorHeap* heaps[2] = {};
    UINT nHeaps = 0;
    ID3D12RootSignature* rootSig = nullptr;
    RootArg args[32];
};
thread_local ListState t_state;

ListState& StateFor(ID3D12GraphicsCommandList* cl) {
    if (t_state.cl != cl) {
        t_state = ListState{};
        t_state.cl = cl;
    }
    return t_state;
}

template <typename T>
T Orig(void* cl, int slot) {
    return reinterpret_cast<T>(taa::OrigListFn(cl, slot));
}

void STDMETHODCALLTYPE Hk_Reset(ID3D12GraphicsCommandList* cl, ID3D12CommandAllocator* a, ID3D12PipelineState* p) {
    if (t_state.cl == cl) t_state = ListState{};
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*)>(cl, 10)(cl, a, p);
}
void STDMETHODCALLTYPE Hk_SetHeaps(ID3D12GraphicsCommandList* cl, UINT n, ID3D12DescriptorHeap* const* h) {
    ListState& s = StateFor(cl);
    s.nHeaps = (std::min)(n, 2u);
    for (UINT i = 0; i < s.nHeaps; ++i) s.heaps[i] = h[i];
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*)>(cl, 28)(cl, n, h);
}
void STDMETHODCALLTYPE Hk_SetRootSig(ID3D12GraphicsCommandList* cl, ID3D12RootSignature* rs) {
    ListState& s = StateFor(cl);
    if (s.rootSig != rs)
        for (auto& a : s.args) a = RootArg{};
    s.rootSig = rs;
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*)>(cl, 29)(cl, rs);
}
void STDMETHODCALLTYPE Hk_SetTable(ID3D12GraphicsCommandList* cl, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    ListState& s = StateFor(cl);
    if (i < 32) s.args[i].kind = 1, s.args[i].table = h;
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE)>(cl, 31)(cl, i, h);
}
void STDMETHODCALLTYPE Hk_SetConst(ID3D12GraphicsCommandList* cl, UINT i, UINT v, UINT off) {
    ListState& s = StateFor(cl);
    if (i < 32 && off < 64) {
        s.args[i].kind = 2;
        s.args[i].consts[off] = v;
        s.args[i].constMask[off / 32] |= 1u << (off % 32);
    }
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT)>(cl, 33)(cl, i, v, off);
}
void STDMETHODCALLTYPE Hk_SetConsts(ID3D12GraphicsCommandList* cl, UINT i, UINT n, const void* v, UINT off) {
    ListState& s = StateFor(cl);
    if (i < 32 && v)
        for (UINT k = 0; k < n && off + k < 64; ++k) {
            s.args[i].kind = 2;
            s.args[i].consts[off + k] = static_cast<const uint32_t*>(v)[k];
            s.args[i].constMask[(off + k) / 32] |= 1u << ((off + k) % 32);
        }
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT)>(cl, 35)(cl, i, n, v, off);
}
void STDMETHODCALLTYPE Hk_SetCbv(ID3D12GraphicsCommandList* cl, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) {
    ListState& s = StateFor(cl);
    if (i < 32) s.args[i].kind = 3, s.args[i].va = va;
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS)>(cl, 37)(cl, i, va);
}
void STDMETHODCALLTYPE Hk_SetSrv(ID3D12GraphicsCommandList* cl, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) {
    ListState& s = StateFor(cl);
    if (i < 32) s.args[i].kind = 4, s.args[i].va = va;
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS)>(cl, 39)(cl, i, va);
}
void STDMETHODCALLTYPE Hk_SetUav(ID3D12GraphicsCommandList* cl, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) {
    ListState& s = StateFor(cl);
    if (i < 32) s.args[i].kind = 5, s.args[i].va = va;
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS)>(cl, 41)(cl, i, va);
}

void RestoreState(ID3D12GraphicsCommandList* cl) {
    const ListState& s = t_state;
    if (s.cl != cl) return;
    if (s.nHeaps) Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*)>(cl, 28)(cl, s.nHeaps, s.heaps);
    if (s.rootSig) {
        Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*)>(cl, 29)(cl, s.rootSig);
        for (UINT i = 0; i < 32; ++i) {
            const RootArg& a = s.args[i];
            switch (a.kind) {
                case 1: Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE)>(cl, 31)(cl, i, a.table); break;
                case 2:
                    for (UINT k = 0; k < 64; ++k)
                        if (a.constMask[k / 32] & (1u << (k % 32)))
                            Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT)>(cl, 33)(cl, i, a.consts[k], k);
                    break;
                case 3: Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS)>(cl, 37)(cl, i, a.va); break;
                case 4: Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS)>(cl, 39)(cl, i, a.va); break;
                case 5: Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS)>(cl, 41)(cl, i, a.va); break;
                default: break;
            }
        }
    }
    if (ID3D12PipelineState* p = taa::CurrentPso(cl)) Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*)>(cl, 25)(cl, p);
}

// ---- the pass
const char* kHlsl = R"(
cbuffer P : register(b0) {
    int2 g_off;        // extent origin in the textures
    int2 g_size;       // extent size (render resolution)
    float2 g_pixPerMv; // pixels of motion per motion-vector unit
    uint g_mode;       // 1 fix, 2 smear the mask, 3 no history anywhere, 4 smear everything
    uint g_inverted;   // depth: 1 = reversed (bigger is nearer)
    int g_range;       // search range in pixels
};
Texture2D<float> g_depth : register(t0);
Texture2D<float2> g_mv : register(t1);
RWTexture2D<float2> g_out : register(u0);
bool Nearer(float a, float b) {  // a nearer than b
    return g_inverted ? a > b * 1.02 + 1e-7 : a < b * 0.98;
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)g_size)) return;
    int2 p = g_off + (int2)id.xy;
    float2 mv = g_mv[p];
    float2 off = 4.0 * (float2)g_size / g_pixPerMv;  // well off screen
    if (g_mode == 3) { g_out[p] = off; return; }
    if (g_mode == 4) { g_out[p] = mv + 0.05 * (float2)g_size / g_pixPerMv; return; }
    float dp = g_depth[p];
    float prevP = p.x + mv.x * g_pixPerMv.x;
    bool hidden = false;
    [loop] for (int dir = -1; dir <= 1 && !hidden; dir += 2) {
        bool any_ = false;
        [loop] for (int k = 4; k <= g_range && !any_; k += 4) {
            int qx = p.x + dir * k;
            if (qx < g_off.x || qx >= g_off.x + g_size.x) break;
            any_ = Nearer(g_depth[int2(qx, p.y)], dp);
        }
        if (!any_) continue;
        [loop] for (int k = 1; k <= g_range && !hidden; ++k) {
            int qx = p.x + dir * k;
            if (qx < g_off.x || qx >= g_off.x + g_size.x) break;
            float dq = g_depth[int2(qx, p.y)];
            if (!Nearer(dq, dp)) continue;
            float prevQ = qx + g_mv[int2(qx, p.y)].x * g_pixPerMv.x;
            hidden = abs(prevQ - prevP) < 0.75;
        }
    }
    if (hidden) g_out[p] = g_mode == 2 ? mv + 0.03 * (float2)g_size / g_pixPerMv : off;
}
)";

// Frame warp (warp.cpp): copy the upscaler's depth and motion vectors (the rendered extent) into textures of its own.
const char* kCaptureHlsl = R"(
cbuffer P : register(b0) { int2 g_off; int2 g_size; };
Texture2D<float> g_depth : register(t0);
Texture2D<float2> g_mv : register(t1);
RWTexture2D<float> g_outDepth : register(u0);
RWTexture2D<float2> g_outMv : register(u1);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)g_size)) return;
    g_outDepth[id.xy] = g_depth[g_off + (int2)id.xy];
    g_outMv[id.xy] = g_mv[g_off + (int2)id.xy];
}
)";
ID3D12RootSignature* g_capRs = nullptr;
ID3D12PipelineState* g_capPso = nullptr;
bool g_capFailed = false;

std::mutex g_mutex;
ID3D12Device* g_dev = nullptr;
ID3D12RootSignature* g_rs = nullptr;
ID3D12PipelineState* g_pso = nullptr;
ID3D12DescriptorHeap* g_heap = nullptr;
UINT g_inc = 0, g_slot = 0;
constexpr UINT kSlots = 32;  // 32 sets of 3 descriptors (ring)
struct OutTex {
    ID3D12Resource* tex = nullptr;
    D3D12_RESOURCE_DESC desc{};
};
OutTex g_out[2];
bool g_failed = false;
std::atomic<long> g_passes{0}, g_skips{0};

DXGI_FORMAT SrvFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
        default: return f;
    }
}

bool Init(ID3D12Device* dev) {
    if (g_pso) return true;
    if (g_failed) return false;
    g_failed = true;  // until it works
    using Compile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                       UINT, ID3DBlob**, ID3DBlob**);
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = m ? reinterpret_cast<Compile_t>(GetProcAddress(m, "D3DCompile")) : nullptr;
    if (!compile) return false;
    ID3DBlob *cs = nullptr, *err = nullptr;
    if (FAILED(compile(kHlsl, strlen(kHlsl), "stereo_edge", nullptr, nullptr, "main", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                       &cs, &err))) {
        LOG("Stereo edge fix: shader failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return false;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2}};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 12};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC rsd{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob* blob = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_rs)))) {
        LOG("Stereo edge fix: root signature failed");
        return false;
    }
    blob->Release();
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_rs;
    pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso)))) {
        LOG("Stereo edge fix: pipeline failed");
        return false;
    }
    cs->Release();
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3 * kSlots, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap)))) return false;
    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_dev = dev;
    g_failed = false;
    LOG("Stereo edge fix: pass ready");
    return true;
}

ID3D12Resource* OutFor(int eye, const D3D12_RESOURCE_DESC& mvDesc) {
    OutTex& o = g_out[eye & 1];
    D3D12_RESOURCE_DESC d = mvDesc;
    d.Format = SrvFormat(mvDesc.Format);
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.MipLevels = 1;
    d.DepthOrArraySize = 1;
    if (o.tex && o.desc.Width == d.Width && o.desc.Height == d.Height && o.desc.Format == d.Format) return o.tex;
    if (o.tex) o.tex->Release();  // only on a resolution change (the GPU may still use it: rare, accepted for a test option)
    o = OutTex{};
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&o.tex)))) {
        LOG("Stereo edge fix: motion texture %llu x %u format %d failed", (unsigned long long)d.Width, d.Height, (int)d.Format);
        return nullptr;
    }
    o.tex->SetName(L"ff16vr stereo edge motion");
    o.desc = d;
    LOG("Stereo edge fix: motion texture %llu x %u format %d for eye %d", (unsigned long long)d.Width, d.Height, (int)d.Format, eye & 1);
    return o.tex;
}

D3D12_RESOURCE_BARRIER Tr(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    return x;
}

}  // namespace

void Install() {
    taa::HookListSlot(10, (void*)&Hk_Reset);
    taa::HookListSlot(28, (void*)&Hk_SetHeaps);
    taa::HookListSlot(29, (void*)&Hk_SetRootSig);
    taa::HookListSlot(31, (void*)&Hk_SetTable);
    taa::HookListSlot(33, (void*)&Hk_SetConst);
    taa::HookListSlot(35, (void*)&Hk_SetConsts);
    taa::HookListSlot(37, (void*)&Hk_SetCbv);
    taa::HookListSlot(39, (void*)&Hk_SetSrv);
    taa::HookListSlot(41, (void*)&Hk_SetUav);
}

bool Wanted() { return vr::g_config.stereoEdgeFix > 0; }

ID3D12Resource* Run(ID3D12GraphicsCommandList* cl, const Inputs& in) {
    if (!cl || !in.depth || !in.mv || in.w <= 0 || in.h <= 0 || in.pixPerMvX == 0.f || in.pixPerMvY == 0.f) return nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_dev) {
        ID3D12Device* dev = nullptr;
        if (FAILED(in.mv->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return nullptr;
        const bool ok = Init(dev);
        dev->Release();
        if (!ok) return nullptr;
    }
    const D3D12_RESOURCE_DESC md = in.mv->GetDesc(), dd = in.depth->GetDesc();
    if (md.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        (UINT64)(in.x + in.w) > md.Width || (UINT)(in.y + in.h) > md.Height || (UINT64)(in.x + in.w) > dd.Width ||
        (UINT)(in.y + in.h) > dd.Height) {
        if (g_skips++ < 5)
            LOG("Stereo edge fix: skipped (motion %llu x %u fmt %d, depth %llu x %u fmt %d, extent %d,%d %d x %d)",
                (unsigned long long)md.Width, md.Height, (int)md.Format, (unsigned long long)dd.Width, dd.Height, (int)dd.Format,
                in.x, in.y, in.w, in.h);
        return nullptr;
    }
    ID3D12Resource* out = OutFor(in.eye, md);
    if (!out) return nullptr;
    const UINT slot = (g_slot++ % kSlots) * 3;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)slot * g_inc;
    gpu.ptr += (UINT64)slot * g_inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    sd.Format = SrvFormat(dd.Format);
    g_dev->CreateShaderResourceView(in.depth, &sd, cpu);
    sd.Format = SrvFormat(md.Format);
    g_dev->CreateShaderResourceView(in.mv, &sd, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + g_inc});
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = SrvFormat(md.Format);
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_dev->CreateUnorderedAccessView(out, nullptr, &ud, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + 2 * g_inc});

    const D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES kOut = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    // game textures: to COPY_SOURCE (motion) / shader read (depth); ours: copy destination, then UAV
    D3D12_RESOURCE_BARRIER pre[3];
    UINT np = 0;
    if (in.mvState != D3D12_RESOURCE_STATE_COPY_SOURCE) pre[np++] = Tr(in.mv, in.mvState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (!(in.depthState & kRead)) pre[np++] = Tr(in.depth, in.depthState, (D3D12_RESOURCE_STATES)(in.depthState | kRead));
    pre[np++] = Tr(out, kOut, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->ResourceBarrier(np, pre);
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*)>(cl, 17)(cl, out, in.mv);
    D3D12_RESOURCE_BARRIER mid[2] = {Tr(in.mv, D3D12_RESOURCE_STATE_COPY_SOURCE, kRead),
                                     Tr(out, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    cl->ResourceBarrier(2, mid);

    ID3D12DescriptorHeap* heaps[] = {g_heap};
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*)>(cl, 28)(cl, 1, heaps);
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*)>(cl, 29)(cl, g_rs);
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*)>(cl, 25)(cl, g_pso);
    struct { int ox, oy, w, h; float px, py; uint32_t mode, inverted; int range; uint32_t pad[3]; } c{
        in.x, in.y, in.w, in.h, in.pixPerMvX, in.pixPerMvY, (uint32_t)std::clamp(vr::g_config.stereoEdgeFix, 1, 4),
        in.depthInverted ? 1u : 0u, 128, {}};
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT)>(cl, 35)(cl, 0, 12, &c, 0);
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE)>(cl, 31)(cl, 1, gpu);
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT)>(cl, 14)(cl, (in.w + 7) / 8, (in.h + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER post[3];
    UINT nq = 0;
    post[nq++] = Tr(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kOut);
    if (in.mvState != kRead) post[nq++] = Tr(in.mv, kRead, in.mvState);
    if (!(in.depthState & kRead)) post[nq++] = Tr(in.depth, (D3D12_RESOURCE_STATES)(in.depthState | kRead), in.depthState);
    cl->ResourceBarrier(nq, post);
    RestoreState(cl);
    if (g_passes++ == 0)
        LOG("Stereo edge fix: first pass (mode %d, extent %d,%d %d x %d, %.2f px per motion unit, depth %s, motion fmt %d, depth fmt %d)",
            vr::g_config.stereoEdgeFix, in.x, in.y, in.w, in.h, in.pixPerMvX, in.depthInverted ? "reversed" : "normal",
            (int)md.Format, (int)dd.Format);
    return out;
}

long Passes() { return g_passes.load(); }
void RestoreListState(ID3D12GraphicsCommandList* cl) { RestoreState(cl); }

namespace {
bool InitCapture() {
    if (g_capPso) return true;
    if (g_capFailed || !g_dev) return false;
    g_capFailed = true;
    using Compile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                       UINT, ID3DBlob**, ID3DBlob**);
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = m ? reinterpret_cast<Compile_t>(GetProcAddress(m, "D3DCompile")) : nullptr;
    ID3DBlob *cs = nullptr, *err = nullptr, *blob = nullptr;
    if (!compile || FAILED(compile(kCaptureHlsl, strlen(kCaptureHlsl), "warp_capture", nullptr, nullptr, "main", "cs_5_1",
                                   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err))) {
        LOG("Frame warp: capture shader failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 2}};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 4};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC rsd{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(g_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_capRs))))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_capRs;
    pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    if (FAILED(g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_capPso)))) return false;
    g_capFailed = false;
    LOG("Frame warp: capture pass ready");
    return true;
}
}  // namespace

bool Capture(ID3D12GraphicsCommandList* cl, const Inputs& in, ID3D12Resource* depthOut, ID3D12Resource* mvOut) {
    if (!cl || !in.depth || !in.mv || !depthOut || !mvOut || in.w <= 0 || in.h <= 0) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_dev) {
        ID3D12Device* dev = nullptr;
        if (FAILED(in.mv->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return false;
        const bool ok = Init(dev);
        dev->Release();
        if (!ok) return false;
    }
    if (!InitCapture()) return false;
    const D3D12_RESOURCE_DESC md = in.mv->GetDesc(), dd = in.depth->GetDesc();
    if ((UINT64)(in.x + in.w) > md.Width || (UINT)(in.y + in.h) > md.Height || (UINT64)(in.x + in.w) > dd.Width ||
        (UINT)(in.y + in.h) > dd.Height)
        return false;
    const UINT base = (g_slot++ % (kSlots - 1)) * 3;  // 4 descriptors from a 3-descriptor slot and the next
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)base * g_inc;
    gpu.ptr += (UINT64)base * g_inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    sd.Format = SrvFormat(dd.Format);
    g_dev->CreateShaderResourceView(in.depth, &sd, cpu);
    sd.Format = SrvFormat(md.Format);
    g_dev->CreateShaderResourceView(in.mv, &sd, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + g_inc});
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    ud.Format = DXGI_FORMAT_R32_FLOAT;
    g_dev->CreateUnorderedAccessView(depthOut, nullptr, &ud, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + 2 * g_inc});
    ud.Format = DXGI_FORMAT_R16G16_FLOAT;
    g_dev->CreateUnorderedAccessView(mvOut, nullptr, &ud, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + 3 * g_inc});
    const D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_BARRIER pre[4];
    UINT np = 0;
    if (!(in.mvState & kRead)) pre[np++] = Tr(in.mv, in.mvState, (D3D12_RESOURCE_STATES)(in.mvState | kRead));
    if (!(in.depthState & kRead)) pre[np++] = Tr(in.depth, in.depthState, (D3D12_RESOURCE_STATES)(in.depthState | kRead));
    pre[np++] = Tr(depthOut, kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    pre[np++] = Tr(mvOut, kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->ResourceBarrier(np, pre);
    ID3D12DescriptorHeap* heaps[] = {g_heap};
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*)>(cl, 28)(cl, 1, heaps);
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*)>(cl, 29)(cl, g_capRs);
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*)>(cl, 25)(cl, g_capPso);
    const int c[4] = {in.x, in.y, in.w, in.h};
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT)>(cl, 35)(cl, 0, 4, c, 0);
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE)>(cl, 31)(cl, 1, gpu);
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT)>(cl, 14)(cl, (in.w + 7) / 8, (in.h + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER post[4];
    UINT nq = 0;
    post[nq++] = Tr(depthOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
    post[nq++] = Tr(mvOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
    if (!(in.mvState & kRead)) post[nq++] = Tr(in.mv, (D3D12_RESOURCE_STATES)(in.mvState | kRead), in.mvState);
    if (!(in.depthState & kRead)) post[nq++] = Tr(in.depth, (D3D12_RESOURCE_STATES)(in.depthState | kRead), in.depthState);
    cl->ResourceBarrier(nq, post);
    RestoreState(cl);
    return true;
}

}  // namespace stereo_edge
