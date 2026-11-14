// Foveated DLSS - see fovdlss.h. Own implementation (public NGX SDK parameter names / interface layout).
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cmath>
#include "fovdlss.h"
#include "fovdlss_host.h"   // logging hooks of the host mod (see fovdlss_host.h)
#include "MinHook.h"

namespace fovdlss {

// NVSDK_NGX_Parameter (DLSS SDK nvsdk_ngx_params.h, public). Same declaration -> same vtable as the SDK's.
struct NgxParam {
    virtual void Set(const char*, unsigned long long) = 0;
    virtual void Set(const char*, float) = 0;
    virtual void Set(const char*, double) = 0;
    virtual void Set(const char*, unsigned int) = 0;
    virtual void Set(const char*, int) = 0;
    virtual void Set(const char*, ID3D11Resource*) = 0;
    virtual void Set(const char*, ID3D12Resource*) = 0;
    virtual void Set(const char*, void*) = 0;
    virtual unsigned Get(const char*, unsigned long long*) const = 0;
    virtual unsigned Get(const char*, float*) const = 0;
    virtual unsigned Get(const char*, double*) const = 0;
    virtual unsigned Get(const char*, unsigned int*) const = 0;
    virtual unsigned Get(const char*, int*) const = 0;
    virtual unsigned Get(const char*, ID3D11Resource**) const = 0;
    virtual unsigned Get(const char*, ID3D12Resource**) const = 0;
    virtual unsigned Get(const char*, void**) const = 0;
    virtual void Reset() = 0;
};
struct NgxHandle { unsigned id; };
static bool Ok(unsigned r) { return (r & 0xFFF00000u) != 0xBAD00000u; }
static unsigned GetUI(const NgxParam* p, const char* n, unsigned def = 0) {
    unsigned v = 0; if (p && Ok(p->Get(n, &v))) return v;
    int i = 0; if (p && Ok(p->Get(n, &i))) return (unsigned)i;
    return def;
}
static bool Has(const NgxParam* p, const char* n) { unsigned v; return p && Ok(p->Get(n, &v)); }

typedef unsigned(__cdecl* PFN_Create)(ID3D12GraphicsCommandList*, unsigned, NgxParam*, NgxHandle**);
typedef unsigned(__cdecl* PFN_Eval)(ID3D12GraphicsCommandList*, const NgxHandle*, NgxParam*, void*);
static PFN_Create o_Create; static PFN_Eval o_Eval, o_EvalC;

static std::mutex g_mu;
static Config g_cfg;
static float g_center[2][2] = {{0.5f, 0.5f}, {0.5f, 0.5f}};
static std::unordered_map<unsigned, unsigned> g_feature;   // handle id -> feature id (1 = DLSS-SR), create flags in high bits
static std::unordered_map<unsigned, unsigned> g_flags, g_quality;
// FF16 (Streamline 2.4): the evaluate's parameter block carries OutWidth / OutHeight = the render size (log of
// Nov 5: "of 1456x1211" with a 4368x3632 output -> picture in the top-left quadrant). The output size the feature
// was created with is kept per handle and used instead when the evaluate's value is not larger than the input.
static std::unordered_map<unsigned, unsigned> g_createOutW, g_createOutH;
static thread_local int t_eye = -1;
static std::atomic<unsigned> g_lastRW{0}, g_lastRH{0};
bool LastRenderSize(unsigned& w, unsigned& h) { w = g_lastRW.load(); h = g_lastRH.load(); return w && h; }
static std::atomic<uint64_t> s_eval{0}, s_cropped{0}, s_skipped{0}, s_moved{0};
static std::atomic<uint64_t> s_sizeChanges{0}, s_centreCreates{0}, s_viewsFreed{0};
static unsigned g_lastCropW = 0, g_lastCropH = 0;   // render-size changes (dynamic resolution) seen by the centre crop
static std::atomic<bool> g_logged{false};

static const unsigned kSR = 1;              // NVSDK_NGX_Feature_SuperSampling
static const unsigned kMVLowRes = 1u << 1;  // NVSDK_NGX_DLSS_Feature_Flags_MVLowRes

void SetConfig(const Config& c) {
    std::lock_guard<std::mutex> l(g_mu);
    bool changed = c.enabled != g_cfg.enabled || c.width != g_cfg.width || c.height != g_cfg.height;
    g_cfg = c;
    if (changed) LOGI("fovdlss: %s, centre %.2f x %.2f of each eye", c.enabled ? "ON" : "off", c.width, c.height);
}
void SetEyeCenter(int eye, float u, float v) { if (eye < 0 || eye > 1) return; std::lock_guard<std::mutex> l(g_mu); g_center[eye][0] = u; g_center[eye][1] = v; }
void SetThreadEye(int eye) { t_eye = eye; }

static unsigned __cdecl Hk_Create(ID3D12GraphicsCommandList* cl, unsigned feature, NgxParam* p, NgxHandle** out) {
    unsigned r = o_Create(cl, feature, p, out);
    if (Ok(r) && out && *out) {
        std::lock_guard<std::mutex> l(g_mu);
        g_feature[(*out)->id] = feature;
        g_flags[(*out)->id] = GetUI(p, "DLSS.Feature.Create.Flags");
        g_quality[(*out)->id] = GetUI(p, "PerfQualityValue");
        g_createOutW[(*out)->id] = GetUI(p, "OutWidth");
        g_createOutH[(*out)->id] = GetUI(p, "OutHeight");
        LOGI("fovdlss: NGX feature %u created (handle %u, %ux%u -> %ux%u, flags %X)", feature, (*out)->id,
             GetUI(p, "Width"), GetUI(p, "Height"), GetUI(p, "OutWidth"), GetUI(p, "OutHeight"), GetUI(p, "DLSS.Feature.Create.Flags"));
    }
    return r;
}

// Names of the input base-offset parameters moved by the crop (render-resolution inputs).
static const char* kInBase[][2] = {
    {"DLSS.Input.Color.Subrect.Base.X", "DLSS.Input.Color.Subrect.Base.Y"},
    {"DLSS.Input.Depth.Subrect.Base.X", "DLSS.Input.Depth.Subrect.Base.Y"},
    {"DLSS.Input.Translucency.Subrect.Base.X", "DLSS.Input.Translucency.Subrect.Base.Y"},
    {"DLSS.Input.Bias.Current.Color.Subrect.Base.X", "DLSS.Input.Bias.Current.Color.Subrect.Base.Y"},
};

// Per DLSS handle: the private centre output (the game's feature reconstructs only the crop into it; test 152 showed
// that output sub-rectangles on the game's output are rejected: BAD00005). Dynamic resolution lets the game's feature
// take a smaller Width/Height + OutWidth/OutHeight than it was created with.
struct View { ID3D12Resource* out = nullptr; UINT w = 0, h = 0; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
              NgxHandle* priv = nullptr; unsigned pcw = 0, pch = 0; bool failed = false;
              unsigned pow = 0, poh = 0; bool exactOnly = true;    // priv's output size; exactOnly: one feature per crop size
              // (a 1:1 feature evaluated with a smaller subrect was refused with BAD00005 on the test PC, runs 334/335;
              // false re-enables that attempt, with a fallback to exact size)
              ID3D12Resource* pout = nullptr; NgxHandle* ph = nullptr; unsigned pw = 0, phh = 0; bool pfailed = false; unsigned ppreset = 0;
              unsigned cpreset = 0;
              ID3D12Resource* hist[2] = {}; unsigned hw = 0, hh = 0, hcur = 0; uint64_t hframe = 0;
              ID3D12Resource* mvc = nullptr; unsigned mvw = 0, mvh = 0; int pbx = -1, pby = -1; };   // moving crop: own MV crop, previous base   // periphery TAA history (output size)   // periphery: preset E DLAA at render size   // priv: our own DLSS feature sized for the crop
typedef unsigned(__cdecl* PFN_Release)(NgxHandle*);
static PFN_Release p_Release;   // the original (trampoline once hooked): used for our own features
static std::unordered_map<unsigned, View> g_views;

// Deferred release (user 2026-11-05: two bluescreens, both while changing the centre size in game). Our textures and NGX
// features are referenced by command lists the game has recorded but the GPU has not run yet (several frames queued,
// plus the replay); releasing them at once freed memory the GPU still used. They now wait 5 s (and 30 more evaluates).
struct Dead { ID3D12Resource* res; NgxHandle* ngx; DWORD t; uint64_t ev; };
static std::mutex g_deadMu; static std::vector<Dead> g_dead; static std::atomic<uint64_t> g_evCount{0};
static void DeferRes(ID3D12Resource* r) { if (!r) return; std::lock_guard<std::mutex> l(g_deadMu); g_dead.push_back({r, nullptr, GetTickCount(), g_evCount.load()}); }
static void DeferNgx(NgxHandle* h) { if (!h) return; std::lock_guard<std::mutex> l(g_deadMu); g_dead.push_back({nullptr, h, GetTickCount(), g_evCount.load()}); }
static void FlushDead() {
    const uint64_t ev = ++g_evCount; const DWORD now = GetTickCount();
    std::vector<Dead> go;
    { std::lock_guard<std::mutex> l(g_deadMu);
      for (size_t i = 0; i < g_dead.size();) { if (now - g_dead[i].t >= 5000 && ev - g_dead[i].ev >= 30) { go.push_back(g_dead[i]); g_dead[i] = g_dead.back(); g_dead.pop_back(); } else i++; } }
    for (auto& d : go) { if (d.res) d.res->Release(); if (d.ngx && p_Release) p_Release(d.ngx); }
}

static ID3D12Resource* PrivateOut(ID3D12GraphicsCommandList* cl, unsigned id, ID3D12Resource* gameOut, UINT w, UINT h) {
    D3D12_RESOURCE_DESC gd = gameOut->GetDesc();
    View& v = g_views[id];
    if (v.out && v.w == w && v.h == h && v.fmt == gd.Format) return v.out;
    DeferRes(v.out);   // the GPU may still use it (queued frames)
    v.out = nullptr; v.w = v.h = 0;
    ID3D12Device* dev = nullptr;
    if (FAILED(cl->GetDevice(__uuidof(ID3D12Device), (void**)&dev)) || !dev) return nullptr;
    D3D12_RESOURCE_DESC d = gd; d.Width = w; d.Height = h; d.MipLevels = 1; d.DepthOrArraySize = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; d.Alignment = 0;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource), (void**)&v.out);
    dev->Release();
    if (FAILED(hr)) { LOGE("fovdlss: centre output %ux%u fmt %d failed %08lX", w, h, (int)gd.Format, hr); v.out = nullptr; return nullptr; }
    v.w = w; v.h = h; v.fmt = gd.Format;
    LOGI("fovdlss: handle %u centre output %ux%u fmt %d", id, w, h, (int)gd.Format);
    return v.out;
}
static void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; x.Transition.StateBefore = a; x.Transition.StateAfter = b;
    cl->ResourceBarrier(1, &x);
}


// ---- composite (+ cheap periphery TAA): output = centre DLSS crop inside, temporally accumulated input colour outside.
// Test 157: a second DLSS feature for the periphery costs as much as the crop saves with DLSS 310.6 (presets E / F / K / M
// all equal), so the periphery is stabilised by our own TAA (history per eye at output size, motion-vector reprojection,
// 3x3 neighbourhood clamp) - one compute pass.
static const char* kCompHlsl = R"(
Texture2D<float4> col  : register(t0);   // periphery source: game's input colour (render size) or a periphery DLSS result
Texture2D<float4> cen  : register(t1);   // centre DLSS output (crop size)
Texture2D<float2> mv   : register(t2);   // game's motion vectors (render size, low-res)
Texture2D<float4> hist : register(t3);   // previous output of this eye (output size)
RWTexture2D<float4> outp  : register(u0);   // game's DLSS output
RWTexture2D<float4> hnext : register(u1);   // next history
SamplerState lin : register(s0);
cbuffer C : register(b0) {
    uint ox, oy, cw, ch, ow, oh, feather, flags;   // flags: 1 debug border, 2 TAA on, 4 history valid
    float4 inRect;                                 // colour subrect: uv scale.xy, offset.zw
    float2 renderSize; float2 mvScale;             // MV * mvScale = render pixels
    float4 mvRect;                                 // MV subrect: uv scale.xy, offset.zw
    float blend; float3 pad;
};
float3 Tm(float3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }   // tonemap-weighted blending (HDR fireflies)
float3 Itm(float3 c) { return c / max(1e-4, 1.0 - max(c.r, max(c.g, c.b))); }
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= ow || id.y >= oh) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(ow, oh);
    float2 cuv = uv * inRect.xy + inRect.zw;
    float4 c = col.SampleLevel(lin, cuv, 0);
    if (flags & 2) {
        float2 texel = inRect.xy / renderSize;
        float3 mn = Tm(c.rgb), mx = mn;
        [unroll] for (int y = -1; y <= 1; y++) [unroll] for (int x = -1; x <= 1; x++) {
            float3 n = Tm(col.SampleLevel(lin, cuv + float2(x, y) * texel, 0).rgb); mn = min(mn, n); mx = max(mx, n); }
        float2 m = mv.SampleLevel(lin, uv * mvRect.xy + mvRect.zw, 0) * mvScale / renderSize;   // uv offset to the previous frame
        float2 puv = uv + m;
        if ((flags & 4) && all(puv >= 0) && all(puv <= 1)) {
            float3 h = clamp(Tm(hist.SampleLevel(lin, puv, 0).rgb), mn, mx);
            c.rgb = Itm(lerp(h, Tm(c.rgb), blend));
        }
    }
    int2 q = int2(id.xy) - int2(ox, oy);
    if (q.x >= 0 && q.y >= 0 && q.x < (int)cw && q.y < (int)ch) {
        float4 d = cen[q];
        float e = min(min(q.x, q.y), min((int)cw - 1 - q.x, (int)ch - 1 - q.y));
        float w = feather > 0 ? saturate(e / (float)feather) : 1.0;
        c = lerp(c, d, w);
        if ((flags & 1) && e < max(4.0, cw / 150.0)) c = float4(4, 0, 0, 1);   // thick + bright (HDR): visible in the headset
    }
    outp[id.xy] = c;
    hnext[id.xy] = c;
}
)";
// Moving centre (eye tracking): the centre feature's history lives in crop coordinates, so when the crop moves by B1-B0
// the motion vectors of the crop get +(B1-B0): mv_crop = mv + (B1 - B0) (DLSS: previous position = current + mv).
static const char* kMvHlsl = R"(
Texture2D<float2> mvsrc : register(t0);
RWTexture2D<float2> mvout : register(u0);
cbuffer C : register(b0) { uint bx, by, w, h; float dx, dy; uint p0, p1; };
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    mvout[id.xy] = mvsrc[id.xy + uint2(bx, by)] + float2(dx, dy);
}
)";
static ID3D12PipelineState* g_psoMv;
typedef HRESULT(WINAPI* PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
static ID3D12RootSignature* g_rs; static ID3D12PipelineState* g_pso; static ID3D12DescriptorHeap* g_heap;
static UINT g_inc = 0, g_ring = 0; static const UINT kSets = 64, kPerSet = 6;
static bool g_compFailed = false;
static bool InitComposite(ID3D12Device* dev) {
    if (g_pso) return true;
    if (g_compFailed) return false;
    g_compFailed = true;
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc ? (PFN_D3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile) { LOGE("fovdlss: d3dcompiler_47 missing"); return false; }
    ID3DBlob *cs = nullptr, *err = nullptr;
    if (FAILED(compile(kCompHlsl, strlen(kCompHlsl), "fovdlss", nullptr, nullptr, "main", "cs_5_0", 1 << 15, 0, &cs, &err))) {
        LOGE("fovdlss: composite shader: %s", err ? (const char*)err->GetBufferPointer() : "?"); if (err) err->Release(); return false; }
    D3D12_DESCRIPTOR_RANGE r[2] = {};
    r[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r[0].NumDescriptors = 4; r[0].BaseShaderRegister = 0; r[0].OffsetInDescriptorsFromTableStart = 0;
    r[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; r[1].NumDescriptors = 2; r[1].BaseShaderRegister = 0; r[1].OffsetInDescriptorsFromTableStart = 4;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[0].DescriptorTable.NumDescriptorRanges = 2; rp[0].DescriptorTable.pDescriptorRanges = r;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[1].Constants.Num32BitValues = 24; rp[1].Constants.ShaderRegister = 0;
    D3D12_STATIC_SAMPLER_DESC ss{}; ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; ss.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters = 2; rd.pParameters = rp; rd.NumStaticSamplers = 1; rd.pStaticSamplers = &ss;
    ID3DBlob* sig = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) { LOGE("fovdlss: root signature"); cs->Release(); return false; }
    dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&g_rs);
    sig->Release();
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = g_rs; pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&g_pso);
    cs->Release();
    if (SUCCEEDED(compile(kMvHlsl, strlen(kMvHlsl), "fovdlss_mv", nullptr, nullptr, "main", "cs_5_0", 1 << 15, 0, &cs, &err))) {
        pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&g_psoMv);
        cs->Release();
    } else { LOGE("fovdlss: MV shader: %s", err ? (const char*)err->GetBufferPointer() : "?"); if (err) err->Release(); }
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = kSets * kPerSet; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&g_heap);
    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!g_rs || !g_pso || !g_heap) { LOGE("fovdlss: composite pipeline failed"); return false; }
    g_compFailed = false;
    LOGI("fovdlss: composite pipeline ready (periphery TAA)");
    return true;
}
static DXGI_FORMAT Typed(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    default: return f;
    }
}
struct CompositeArgs {
    ID3D12Resource* color; const float* inRect; ID3D12Resource* centre; ID3D12Resource* out;
    ID3D12Resource* mv; float mvRect[4]; float renderW, renderH, mvScaleX, mvScaleY;
    ID3D12Resource* histPrev; ID3D12Resource* histNext; bool histValid; bool taa; float blend;
    unsigned ox, oy, cw, ch, ow, oh, feather; bool debug;
};
// Records the composite on the game's command list right after the centre NGX evaluate (NGX itself changes the
// descriptor heaps / pipeline there, so the caller already re-binds its state afterwards). Inputs (colour, MV) are in
// NON_PIXEL_SHADER_RESOURCE around the NGX evaluate; centre / history textures are ours (kept in UAV between uses).
static bool Composite(ID3D12GraphicsCommandList* cl, const CompositeArgs& a) {
    ID3D12Device* dev = nullptr;
    if (FAILED(cl->GetDevice(__uuidof(ID3D12Device), (void**)&dev)) || !dev) return false;
    bool ok = InitComposite(dev);
    if (ok) {
        UINT set = (g_ring++ % kSets) * kPerSet;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_heap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += (SIZE_T)set * g_inc;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_heap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += (UINT64)set * g_inc;
        auto srv = [&](ID3D12Resource* r, DXGI_FORMAT fallback) {
            D3D12_SHADER_RESOURCE_VIEW_DESC d{}; d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; d.Texture2D.MipLevels = 1;
            if (r) { d.Format = Typed(r->GetDesc().Format); dev->CreateShaderResourceView(r, &d, cpu); }
            else { d.Format = fallback; dev->CreateShaderResourceView(nullptr, &d, cpu); }   // null descriptor
            cpu.ptr += g_inc;
        };
        auto uav = [&](ID3D12Resource* r) {   // null resource -> null descriptor (writes are discarded)
            D3D12_UNORDERED_ACCESS_VIEW_DESC u{}; u.Format = r ? Typed(r->GetDesc().Format) : DXGI_FORMAT_R16G16B16A16_FLOAT; u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            dev->CreateUnorderedAccessView(r, nullptr, &u, cpu); cpu.ptr += g_inc;
        };
        srv(a.color, DXGI_FORMAT_R16G16B16A16_FLOAT); srv(a.centre, DXGI_FORMAT_R16G16B16A16_FLOAT);
        srv(a.mv, DXGI_FORMAT_R16G16_FLOAT); srv(a.histPrev, DXGI_FORMAT_R16G16B16A16_FLOAT);
        uav(a.out); uav(a.histNext);
        const D3D12_RESOURCE_STATES srvState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Barrier(cl, a.centre, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srvState);
        if (a.histPrev) Barrier(cl, a.histPrev, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srvState);
        ID3D12DescriptorHeap* heaps[] = {g_heap};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(g_rs);
        cl->SetPipelineState(g_pso);
        cl->SetComputeRootDescriptorTable(0, gpu);
        uint32_t k[24] = {a.ox, a.oy, a.cw, a.ch, a.ow, a.oh, a.feather,
                          (a.debug ? 1u : 0u) | (a.taa && a.mv && a.histPrev ? 2u : 0u) | (a.histValid ? 4u : 0u)};
        float f[16] = {a.inRect[0], a.inRect[1], a.inRect[2], a.inRect[3], a.renderW, a.renderH, a.mvScaleX, a.mvScaleY,
                       a.mvRect[0], a.mvRect[1], a.mvRect[2], a.mvRect[3], a.blend, 0, 0, 0};
        memcpy(&k[8], f, sizeof(f));
        cl->SetComputeRoot32BitConstants(1, 24, k, 0);
        cl->Dispatch((a.ow + 7) / 8, (a.oh + 7) / 8, 1);
        if (a.histPrev) Barrier(cl, a.histPrev, srvState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(cl, a.centre, srvState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        D3D12_RESOURCE_BARRIER ub[2] = {}; ub[0].Type = ub[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; ub[0].UAV.pResource = a.out; ub[1].UAV.pResource = a.histNext;
        cl->ResourceBarrier(a.histNext ? 2 : 1, ub);
    }
    dev->Release();
    return ok;
}

// Writes the crop of the game's MVs + offset into our own crop-size MV texture (left in NON_PIXEL_SHADER_RESOURCE).
static bool MvShift(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, ID3D12Resource* dst, unsigned bx, unsigned by,
                    unsigned w, unsigned h, float dx, float dy) {
    ID3D12Device* dev = nullptr;
    if (FAILED(cl->GetDevice(__uuidof(ID3D12Device), (void**)&dev)) || !dev) return false;
    bool ok = InitComposite(dev) && g_psoMv;
    if (ok) {
        UINT set = (g_ring++ % kSets) * kPerSet;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_heap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += (SIZE_T)set * g_inc;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_heap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += (UINT64)set * g_inc;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1; sd.Format = Typed(src->GetDesc().Format);
        dev->CreateShaderResourceView(src, &sd, cpu); cpu.ptr += g_inc;
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        for (int i = 0; i < 3; i++) { dev->CreateShaderResourceView(nullptr, &sd, cpu); cpu.ptr += g_inc; }
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{}; u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; u.Format = Typed(dst->GetDesc().Format);
        dev->CreateUnorderedAccessView(dst, nullptr, &u, cpu); cpu.ptr += g_inc;
        u.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; dev->CreateUnorderedAccessView(nullptr, nullptr, &u, cpu);
        ID3D12DescriptorHeap* heaps[] = {g_heap};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(g_rs);
        cl->SetPipelineState(g_psoMv);
        cl->SetComputeRootDescriptorTable(0, gpu);
        uint32_t k[24] = {bx, by, w, h}; memcpy(&k[4], &dx, 4); memcpy(&k[5], &dy, 4);
        cl->SetComputeRoot32BitConstants(1, 24, k, 0);
        cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        Barrier(cl, dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    dev->Release();
    return ok;
}

static const char* kPresetKeys[] = {"DLSS.Hint.Render.Preset.DLAA", "DLSS.Hint.Render.Preset.Quality", "DLSS.Hint.Render.Preset.Balanced",
    "DLSS.Hint.Render.Preset.Performance", "DLSS.Hint.Render.Preset.UltraPerformance", "DLSS.Hint.Render.Preset.UltraQuality"};
static ID3D12Resource* MakeTex(ID3D12GraphicsCommandList* cl, ID3D12Resource* like, UINT w, UINT h) {
    ID3D12Device* dev = nullptr; ID3D12Resource* t = nullptr;
    if (FAILED(cl->GetDevice(__uuidof(ID3D12Device), (void**)&dev)) || !dev) return nullptr;
    D3D12_RESOURCE_DESC d = like->GetDesc(); d.Width = w; d.Height = h; d.MipLevels = 1; d.DepthOrArraySize = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; d.Alignment = 0;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource), (void**)&t);
    dev->Release();
    return t;
}
// Periphery: the whole eye through a second, cheap DLSS feature (preset E) as DLAA at render size - temporally stable
// edges without the cost of the big model at output size. Returns the render-size result or nullptr (-> raw colour).
static ID3D12Resource* Periphery(PFN_Eval orig, ID3D12GraphicsCommandList* cl, const NgxHandle* h, NgxParam* p, void* cb,
                                 ID3D12Resource* gameOut, unsigned rw, unsigned rh, unsigned outW, unsigned outH, unsigned gameFlags) {
    View* v;
    { std::lock_guard<std::mutex> l(g_mu); v = &g_views[h->id]; }
    if (!v->pout || v->pw != rw || v->phh != rh) {
        DeferRes(v->pout); v->pout = nullptr;
        DeferNgx(v->ph); v->ph = nullptr;
        v->pout = MakeTex(cl, gameOut, rw, rh); v->pw = rw; v->phh = rh; v->pfailed = false;
        if (!v->pout) { LOGE("fovdlss: periphery texture %ux%u failed", rw, rh); return nullptr; }
    }
    if (v->ph && v->ppreset != g_cfg.periphPreset) { DeferNgx(v->ph); v->ph = nullptr; v->pfailed = false; }
    p->Set("Output", v->pout); p->Set("OutWidth", rw); p->Set("OutHeight", rh);
    if (!v->ph && !v->pfailed) {
        v->ppreset = g_cfg.periphPreset;
        unsigned oldQ = GetUI(p, "PerfQualityValue"), oldP[6];
        for (int i = 0; i < 6; i++) { oldP[i] = GetUI(p, kPresetKeys[i]); p->Set(kPresetKeys[i], g_cfg.periphPreset); }
        p->Set("PerfQualityValue", 5);   // NVSDK_NGX_PerfQuality_Value_DLAA
        p->Set("DLSS.Feature.Create.Flags", (int)gameFlags);
        unsigned cr = o_Create(cl, kSR, p, &v->ph);
        LOGI("fovdlss: periphery DLSS (preset %c, DLAA %ux%u) for handle %u: result %08X", (char)('A' - 1 + g_cfg.periphPreset), rw, rh, h->id, cr);
        if (!Ok(cr)) { v->ph = nullptr; v->pfailed = true; }
        p->Set("PerfQualityValue", (int)oldQ);
        for (int i = 0; i < 6; i++) p->Set(kPresetKeys[i], oldP[i]);
    }
    unsigned r = v->ph ? orig(cl, v->ph, p, cb) : 0xBAD00005u;
    p->Set("Output", gameOut); p->Set("OutWidth", outW); p->Set("OutHeight", outH);
    return Ok(r) ? v->pout : nullptr;
}

static unsigned Evaluate(PFN_Eval orig, ID3D12GraphicsCommandList* cl, const NgxHandle* h, NgxParam* p, void* cb) {
    FlushDead();
    s_eval++;
    Config cfg; float c[2] = {0.5f, 0.5f}; unsigned feature = 0, flags = 0; bool known = false;
    {
        std::lock_guard<std::mutex> l(g_mu);
        cfg = g_cfg;
        int e = t_eye; if (e >= 0 && e < 2) { c[0] = g_center[e][0]; c[1] = g_center[e][1]; }
        if (h) { auto it = g_feature.find(h->id); if (it != g_feature.end()) { known = true; feature = it->second; flags = g_flags[h->id]; } }
    }
    const bool sr = known && feature == kSR;
    if (sr && p) {   // scene (render) size for other foveation users (VRS)
        unsigned w = GetUI(p, "DLSS.Render.Subrect.Dimensions.Width"), hh = GetUI(p, "DLSS.Render.Subrect.Dimensions.Height");
        if (!w) w = GetUI(p, "Width");
        if (!hh) hh = GetUI(p, "Height");
        g_lastRW = w; g_lastRH = hh;
    }
    if (!cfg.enabled || !sr || !p || !h) { if (cfg.enabled && !sr) s_skipped++; return orig(cl, h, p, cb); }
    unsigned outW = GetUI(p, "OutWidth"), outH = GetUI(p, "OutHeight");
    {
        unsigned cw0 = 0, ch0 = 0;
        {
            std::lock_guard<std::mutex> l(g_mu);
            auto iw = g_createOutW.find(h->id), ih = g_createOutH.find(h->id);
            if (iw != g_createOutW.end()) cw0 = iw->second;
            if (ih != g_createOutH.end()) ch0 = ih->second;
        }
        const unsigned evW = outW, evH = outH, inW0 = GetUI(p, "Width"), inH0 = GetUI(p, "Height");
        if (cw0 && ch0 && (outW <= inW0 || outH <= inH0 || !outW || !outH)) { outW = cw0; outH = ch0; }
        static std::atomic<int> s_sizeLogs{0};
        if (cfg.enabled && s_sizeLogs.fetch_add(1) < 2)
            LOGI("fovdlss: evaluate sizes: input %ux%u, evaluate OutWidth/Height %ux%u, created %ux%u -> using %ux%u", inW0, inH0,
                 evW, evH, cw0, ch0, outW, outH);
    }
    const unsigned inW = GetUI(p, "Width"), inH = GetUI(p, "Height");
    unsigned rw = GetUI(p, "DLSS.Render.Subrect.Dimensions.Width"), rh = GetUI(p, "DLSS.Render.Subrect.Dimensions.Height");
    const unsigned rwOrig = rw, rhOrig = rh;
    if (!rw) rw = inW;
    if (!rh) rh = inH;
    ID3D12Resource* gameOut = nullptr;
    if (!Ok(p->Get("Output", &gameOut)) || !gameOut || !outW || !outH || !rw || !rh) { s_skipped++; return orig(cl, h, p, cb); }
    const float sx = (float)outW / rw, sy = (float)outH / rh;
    // The centre's OUTPUT size is a fixed share of the game's output, so dynamic resolution (render size changing every
    // few frames) never changes it; the crop in render pixels follows the render size. Before 0.9.3 the output was
    // derived from the crop and wobbled by 1-2 px, which recreated the centre DLSS feature dozens of times a second
    // (player's 0x139 bluescreens, 2026-11-12).
    const unsigned ow = std::min(outW, std::max(16u, (unsigned)std::lround(outW * std::clamp(cfg.width, 0.1f, 1.0f)) & ~1u));
    const unsigned oh = std::min(outH, std::max(16u, (unsigned)std::lround(outH * std::clamp(cfg.height, 0.1f, 1.0f)) & ~1u));
    unsigned cw = std::min(rw, std::max(16u, (unsigned)std::lround(ow / sx) & ~1u));
    unsigned ch = std::min(rh, std::max(16u, (unsigned)std::lround(oh / sy) & ~1u));
    int bx = (int)std::lround(c[0] * rw - cw * 0.5f), by = (int)std::lround(c[1] * rh - ch * 0.5f);
    bx = std::clamp(bx, 0, (int)(rw - cw)); by = std::clamp(by, 0, (int)(rh - ch));
    const unsigned obx = std::min((unsigned)std::lround(bx * sx), outW - ow), oby = std::min((unsigned)std::lround(by * sy), outH - oh);
    if (cw != g_lastCropW || ch != g_lastCropH) { if (g_lastCropW) s_sizeChanges++; g_lastCropW = cw; g_lastCropH = ch; }
    ID3D12Resource* periph = cfg.periphDlss && rw == inW && rh == inH ? Periphery(orig, cl, h, p, cb, gameOut, rw, rh, outW, outH, flags) : nullptr;
    ID3D12Resource* priv = nullptr;
    { std::lock_guard<std::mutex> l(g_mu); priv = PrivateOut(cl, h->id, gameOut, ow, oh); }
    if (!priv) { s_skipped++; return orig(cl, h, p, cb); }
    unsigned saved[5][2] = {};
    ID3D12Resource* gameMv = nullptr; ID3D12Resource* mvCrop = nullptr;
    auto restore = [&] {   // the caller reuses the parameter block
        for (int i = 0; i < 4; i++) { p->Set(kInBase[i][0], saved[i][0]); p->Set(kInBase[i][1], saved[i][1]); }
        p->Set("DLSS.Input.MV.Subrect.Base.X", saved[4][0]); p->Set("DLSS.Input.MV.Subrect.Base.Y", saved[4][1]);
        p->Set("Output", gameOut);
        if (gameMv) p->Set("MotionVectors", gameMv);
        p->Set("Width", inW); p->Set("Height", inH); p->Set("OutWidth", outW); p->Set("OutHeight", outH);
        p->Set("DLSS.Render.Subrect.Dimensions.Width", rwOrig ? rwOrig : rw); p->Set("DLSS.Render.Subrect.Dimensions.Height", rhOrig ? rhOrig : rh);
    };
    for (int i = 0; i < 4; i++) {
        saved[i][0] = GetUI(p, kInBase[i][0]); saved[i][1] = GetUI(p, kInBase[i][1]);
        p->Set(kInBase[i][0], saved[i][0] + (unsigned)bx); p->Set(kInBase[i][1], saved[i][1] + (unsigned)by);
    }
    const bool mvLow = (flags & kMVLowRes) != 0;
    saved[4][0] = GetUI(p, "DLSS.Input.MV.Subrect.Base.X"); saved[4][1] = GetUI(p, "DLSS.Input.MV.Subrect.Base.Y");
    p->Set("DLSS.Input.MV.Subrect.Base.X", saved[4][0] + (mvLow ? (unsigned)bx : obx));
    p->Set("DLSS.Input.MV.Subrect.Base.Y", saved[4][1] + (mvLow ? (unsigned)by : oby));
    // moved crop (eye tracking): shifted MVs into our own crop texture
    {
        int pbx, pby;
        { std::lock_guard<std::mutex> l(g_mu); View& v = g_views[h->id]; pbx = v.pbx; pby = v.pby; v.pbx = bx; v.pby = by; }
        if (pbx >= 0 && (pbx != bx || pby != by) && Ok(p->Get("MotionVectors", &gameMv)) && gameMv) {
            const unsigned mw = mvLow ? cw : ow, mh = mvLow ? ch : oh;
            float scx = 1, scy = 1; p->Get("MV.Scale.X", &scx); p->Get("MV.Scale.Y", &scy);
            float dpx = (float)(bx - pbx) * (mvLow ? 1.0f : sx), dpy = (float)(by - pby) * (mvLow ? 1.0f : sy);
            {
                std::lock_guard<std::mutex> l(g_mu); View& v = g_views[h->id];
                if (!v.mvc || v.mvw < mw || v.mvh < mh) {   // allocated at the largest size: dynamic resolution only shrinks the used part
                    const unsigned aw = std::max(mw, ow), ah = std::max(mh, oh);
                    DeferRes(v.mvc); v.mvc = MakeTex(cl, gameMv, aw, ah); v.mvw = aw; v.mvh = ah; }
                mvCrop = v.mvc;
            }
            if (mvCrop && MvShift(cl, gameMv, mvCrop, saved[4][0] + (mvLow ? bx : obx), saved[4][1] + (mvLow ? by : oby), mw, mh,
                                  scx != 0 ? dpx / scx : dpx, scy != 0 ? dpy / scy : dpy)) {
                p->Set("MotionVectors", mvCrop);
                p->Set("DLSS.Input.MV.Subrect.Base.X", 0u); p->Set("DLSS.Input.MV.Subrect.Base.Y", 0u);
                s_moved++;
            } else mvCrop = nullptr;
        }
    }
    p->Set("Output", priv);
    p->Set("Width", cw); p->Set("Height", ch);
    p->Set("OutWidth", ow); p->Set("OutHeight", oh);
    p->Set("DLSS.Render.Subrect.Dimensions.Width", cw); p->Set("DLSS.Render.Subrect.Dimensions.Height", ch);
    // The game's feature rejects a smaller output (BAD00005, test 153): reconstruct the crop with our own feature,
    // created from the game's parameter block (its quality / flags / preset settings) at the crop's sizes.
    NgxHandle* ph = nullptr;
    {
        std::lock_guard<std::mutex> l(g_mu);
        View& v = g_views[h->id];
        // Created once for the LARGEST crop (the output size, input 1:1) and evaluated with the current crop as the
        // render subrect: dynamic resolution changes only the subrect (how DLSS itself expects dynamic resolution).
        // If the driver refuses a smaller subrect, this view falls back to features of the exact crop size.
        const unsigned mw2 = v.exactOnly ? cw : ow, mh2 = v.exactOnly ? ch : oh;
        const bool stale = v.priv && (v.cpreset != cfg.centrePreset || v.pow != ow || v.poh != oh ||
                                      (v.exactOnly ? (v.pcw != cw || v.pch != ch) : (v.pcw < cw || v.pch < ch)));
        if (stale) { DeferNgx(v.priv); v.priv = nullptr; v.failed = false; }
        if (!v.priv && !(v.failed && v.pcw == mw2 && v.pch == mh2)) {
            p->Set("Width", mw2); p->Set("Height", mh2);   // creation size; the evaluate below uses cw x ch
            unsigned gameFlags = g_flags[h->id], gameQ = g_quality[h->id];
            p->Set("DLSS.Feature.Create.Flags", (int)gameFlags); p->Set("PerfQualityValue", (int)gameQ);
            unsigned oldP[6];
            for (int i = 0; i < 6; i++) { oldP[i] = GetUI(p, kPresetKeys[i]); if (cfg.centrePreset) p->Set(kPresetKeys[i], cfg.centrePreset); }
            unsigned cr = o_Create(cl, kSR, p, &v.priv);
            p->Set("Width", cw); p->Set("Height", ch);
            s_centreCreates++;
            for (int i = 0; i < 6; i++) p->Set(kPresetKeys[i], oldP[i]);
            v.cpreset = cfg.centrePreset;
            LOGI("fovdlss: centre preset %u", cfg.centrePreset);
            LOG("fovdlss: centre DLSS feature for handle %u: created for %ux%u -> %ux%u (crop now %ux%u%s), result %08X", h->id, mw2, mh2, ow, oh,
                cw, ch, v.exactOnly ? ", exact size" : "", cr);
            if (!Ok(cr)) v.priv = nullptr;
            v.failed = !v.priv;
            v.pcw = mw2; v.pch = mh2; v.pow = ow; v.poh = oh;
        }
        ph = v.priv;
    }
    if (!ph) { restore(); s_skipped++; return orig(cl, h, p, cb); }   // no centre feature: the game's full DLSS
    unsigned r = orig(cl, ph, p, cb);
    if (!Ok(r)) {
        bool retry = false;
        { std::lock_guard<std::mutex> l(g_mu); View& v = g_views[h->id];
          if (!v.exactOnly && (v.pcw != cw || v.pch != ch)) { v.exactOnly = true; retry = true; } }
        if (retry) {   // smaller subrect refused: this frame gets the game's full DLSS, later frames exact-size features
            LOG("fovdlss: centre evaluate with a %ux%u subrect of a %ux%u feature failed (%08X): handle %u uses exact-size features", cw, ch, ow, oh, r, h->id);
            if (mvCrop) Barrier(cl, mvCrop, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            restore(); s_skipped++; return orig(cl, h, p, cb);
        }
    }
    if (mvCrop) Barrier(cl, mvCrop, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // composite: centre + upscaled periphery into the game's output (UAV state around the NGX evaluate)
    if (Ok(r)) {
        ID3D12Resource* color = nullptr; ID3D12Resource* mvt = nullptr;
        if (!periph) p->Get("Color", &color);
        p->Get("MotionVectors", &mvt);
        float inRect[4] = {1, 1, 0, 0};
        if (color) { D3D12_RESOURCE_DESC cd = color->GetDesc();
            inRect[0] = (float)rw / (float)cd.Width; inRect[1] = (float)rh / (float)cd.Height;
            inRect[2] = (float)saved[0][0] / (float)cd.Width; inRect[3] = (float)saved[0][1] / (float)cd.Height; }
        if (periph) Barrier(cl, periph, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        unsigned feather = (unsigned)(std::min(ow, oh) * 0.08f);
        CompositeArgs ca{};
        ca.color = periph ? periph : color; ca.inRect = inRect; ca.centre = priv; ca.out = gameOut;
        ca.mv = mvt; ca.renderW = (float)rw; ca.renderH = (float)rh;
        float sxm = 1, sym = 1; p->Get("MV.Scale.X", &sxm); p->Get("MV.Scale.Y", &sym); ca.mvScaleX = sxm; ca.mvScaleY = sym;
        if (mvt) { D3D12_RESOURCE_DESC md = mvt->GetDesc(); float mw = mvLow ? (float)rw : (float)outW, mh = mvLow ? (float)rh : (float)outH;
            ca.mvRect[0] = mw / (float)md.Width; ca.mvRect[1] = mh / (float)md.Height;
            ca.mvRect[2] = (float)saved[4][0] / (float)md.Width; ca.mvRect[3] = (float)saved[4][1] / (float)md.Height;
            if (!mvLow) { ca.mvScaleX *= (float)rw / outW; ca.mvScaleY *= (float)rh / outH; } }   // high-res MVs: output pixels -> render pixels
        ca.taa = cfg.periphTaa && !periph; ca.blend = cfg.taaBlend;
        if (ca.taa) {
            std::lock_guard<std::mutex> l(g_mu);
            View& v = g_views[h->id];
            if (!v.hist[0] || v.hw != outW || v.hh != outH) {
                for (auto& t : v.hist) { DeferRes(t); t = MakeTex(cl, gameOut, outW, outH); }
                v.hw = outW; v.hh = outH; v.hframe = 0;
            }
            ca.histPrev = v.hist[v.hcur]; ca.histNext = v.hist[v.hcur ^ 1];
            ca.histValid = v.hframe > 0 && GetUI(p, "Reset") == 0;
            v.hcur ^= 1; v.hframe++;
            if (!ca.histPrev || !ca.histNext) ca.taa = false;
        }
        if (!ca.taa) { ca.histPrev = nullptr; ca.histNext = nullptr; }
        ca.ox = obx; ca.oy = oby; ca.cw = ow; ca.ch = oh; ca.ow = outW; ca.oh = outH; ca.feather = feather; ca.debug = cfg.debugBorder;
        bool comp = ca.color && Composite(cl, ca);
        if (periph) Barrier(cl, periph, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!comp) {
            // fallback: plain copy of the centre (periphery stale)
            Barrier(cl, priv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cl, gameOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = gameOut; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = priv; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cl->CopyTextureRegion(&dst, obx, oby, 0, &src, nullptr);
            Barrier(cl, gameOut, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(cl, priv, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
    restore();
    s_cropped++;
    static std::atomic<int> logged{0};
    {   // which motion vectors this eye's evaluate got (per eye, a few times): the texture, its size, the scale, the reset flag
        static std::atomic<int> s_mvLogs[2]{{0}, {0}};
        const int e = t_eye >= 0 && t_eye < 2 ? t_eye : 0;
        if (const int n = s_mvLogs[e].fetch_add(1); n < 2 || (n >= 900 && n < 902) || (n >= 3600 && n < 3602)) {
            ID3D12Resource* mvt = nullptr; p->Get("MotionVectors", &mvt);
            float sxm = 0, sym = 0; p->Get("MV.Scale.X", &sxm); p->Get("MV.Scale.Y", &sym);
            unsigned long long mw = 0; unsigned mh = 0; int fmt = -1;
            if (mvt) { D3D12_RESOURCE_DESC md = mvt->GetDesc(); mw = md.Width; mh = md.Height; fmt = (int)md.Format; }
            LOGI("fovdlss: eye %d handle %u motion vectors %p (%llu x %u fmt %d) base %u,%u scale %.3f %.3f reset %u, exposure %p, jitter %.3f %.3f",
                 t_eye, h->id, (void*)mvt, mw, mh, fmt, saved[4][0], saved[4][1], sxm, sym, GetUI(p, "Reset"), [&] { ID3D12Resource* x = nullptr; p->Get("ExposureTexture", &x); return (void*)x; }(),
                 [&] { float j = 0; p->Get("Jitter.Offset.X", &j); return j; }(), [&] { float j = 0; p->Get("Jitter.Offset.Y", &j); return j; }());
        }
    }
    if (logged.fetch_add(1) < 2 || !Ok(r))
        { static std::atomic<int> errs{0}; if (Ok(r) || errs.fetch_add(1) < 5)
          LOGI("fovdlss: cropped evaluate: eye %d, render %ux%u crop %ux%u at (%d,%d) -> %ux%u at (%u,%u) of %ux%u, MV %s-res, result %08X",
               t_eye, rw, rh, cw, ch, bx, by, ow, oh, obx, oby, outW, outH, mvLow ? "low" : "high", r); }
    return r;
}
static unsigned __cdecl Hk_Eval(ID3D12GraphicsCommandList* cl, const NgxHandle* h, NgxParam* p, void* cb) { return Evaluate(o_Eval, cl, h, p, cb); }

// The game (Streamline) releases one of its features: everything we keep for it (our centre / periphery features, the
// centre output, history and MV textures) goes to the deferred queue. Before 0.9.3 it was never freed: with a game that
// recreates its DLSS feature often (dynamic resolution, settings changes) that leaked hundreds of MB each time.
static unsigned __cdecl Hk_Release(NgxHandle* h) {
    if (h) {
        View dead; bool had = false;
        {
            std::lock_guard<std::mutex> l(g_mu);
            auto it = g_views.find(h->id);
            if (it != g_views.end()) { dead = it->second; g_views.erase(it); had = true; }
            g_feature.erase(h->id); g_flags.erase(h->id); g_quality.erase(h->id); g_createOutW.erase(h->id); g_createOutH.erase(h->id);
        }
        if (had) {
            DeferNgx(dead.priv); DeferNgx(dead.ph);
            DeferRes(dead.out); DeferRes(dead.pout); DeferRes(dead.hist[0]); DeferRes(dead.hist[1]); DeferRes(dead.mvc);
            s_viewsFreed++;
            LOG("fovdlss: game released DLSS handle %u: its centre feature and textures are released after 5 s", h->id);
        }
    }
    return p_Release ? p_Release(h) : 0;
}
static unsigned __cdecl Hk_EvalC(ID3D12GraphicsCommandList* cl, const NgxHandle* h, NgxParam* p, void* cb) { return Evaluate(o_EvalC, cl, h, p, cb); }

static DWORD WINAPI InstallThread(void*) {
    HMODULE m = nullptr;
    for (int i = 0; i < 6000 && !(m = GetModuleHandleW(L"_nvngx.dll")); i++) Sleep(20);
    if (!m) { LOGW("fovdlss: _nvngx.dll not loaded within 120 s - foveated DLSS unavailable"); return 0; }
    struct H { const char* n; void* d; void** o; } hs[] = {
        {"NVSDK_NGX_D3D12_CreateFeature", (void*)Hk_Create, (void**)&o_Create},
        {"NVSDK_NGX_D3D12_EvaluateFeature", (void*)Hk_Eval, (void**)&o_Eval},
        {"NVSDK_NGX_D3D12_EvaluateFeature_C", (void*)Hk_EvalC, (void**)&o_EvalC},
    };
    {
        void* t = (void*)GetProcAddress(m, "NVSDK_NGX_D3D12_ReleaseFeature");
        void* tramp = nullptr;
        MH_STATUS st = t ? MH_CreateHook(t, (void*)Hk_Release, &tramp) : MH_ERROR_FUNCTION_NOT_FOUND;
        if (st == MH_OK) { p_Release = (PFN_Release)tramp; st = MH_EnableHook(t); }
        else p_Release = (PFN_Release)t;
        LOGI("fovdlss: NVSDK_NGX_D3D12_ReleaseFeature %s", st == MH_OK ? "hooked" : "not hooked");
    }
    for (auto& x : hs) {
        void* t = (void*)GetProcAddress(m, x.n);
        MH_STATUS st = t ? MH_CreateHook(t, x.d, x.o) : MH_ERROR_FUNCTION_NOT_FOUND;
        if (st == MH_OK) st = MH_EnableHook(t);
        LOGI("fovdlss: %s %s", x.n, st == MH_OK ? "hooked" : "not hooked");
    }
    return 0;
}
void Install() { CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr); }

void Report() {
    uint64_t e = s_eval.exchange(0), c = s_cropped.exchange(0), k = s_skipped.exchange(0);
    uint64_t mv = s_moved.exchange(0);
    float c0[2], c1[2]; { std::lock_guard<std::mutex> l(g_mu); c0[0] = g_center[0][0]; c0[1] = g_center[0][1]; c1[0] = g_center[1][0]; c1[1] = g_center[1][1]; }
    const uint64_t sc = s_sizeChanges.exchange(0), cc = s_centreCreates.exchange(0), vf = s_viewsFreed.exchange(0);
    size_t pending = 0; { std::lock_guard<std::mutex> l(g_deadMu); pending = g_dead.size(); }
    if (e) LOGI("fovdlss: %llu NGX evaluates, %llu cropped to the centre (%llu with a moved centre), %llu not DLSS-SR | centre L (%.2f %.2f) R (%.2f %.2f) (10 s)",
                (unsigned long long)e, (unsigned long long)c, (unsigned long long)mv, (unsigned long long)k, c0[0], c0[1], c1[0], c1[1]);
    if (sc || cc || vf)
        LOG("fovdlss: render size changed %llu times (dynamic resolution?), %llu centre features created, %llu game handles released, %zu objects waiting for release (10 s)",
            (unsigned long long)sc, (unsigned long long)cc, (unsigned long long)vf, pending);
}

}  // namespace fovdlss
