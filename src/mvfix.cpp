// Per-eye motion vectors rebuilt for the upscaler (MotionRebuild; per-eye DLSS history, DlssGhostingFix on).
//
// FF16 renders the two eyes as two frame passes per tick (time frozen for the second), and the engine's "previous
// camera" for a pass is simply the previous pass: the other eye. Dumps (runs 279 / 280, eye 1 drawn first):
//   - the resolved motion vectors of a static pixel are the move to the other eye's camera, not to this eye's previous
//     frame (per-eye history then blends from the wrong place: the fuzzy image), and
//   - the second pass of a tick carries no object motion at all: a skinned object's "previous pose" is the one the
//     first pass just stored, this tick's. Characters are static for DLSS in that eye while they move: smear / halo.
// Substituting this eye's previous camera in the constants (cam_prev.cpp) fixes the static part in principle, but the
// vectors come out mirrored (run 279), and it cannot restore the missing object motion.
//
// So the vectors are rebuilt here, at the upscaler's evaluate, from the depth and the cameras the mod knows exactly:
//   camera part  = where this pixel's point was in this eye's previous frame (this eye's previous camera, kept here),
//   object part  = game vector - the engine's own camera part (the camera the engine used for this pass: known), i.e.
//                  the object's displacement; for the second pass (none) taken from the first pass of the same tick,
//                  at the same surface point (reprojected through the other eye's camera, depth checked).
// The result is handed to the upscaler instead of the game's texture (sl_hooks: a re-tag). Same units as the game's
// vectors: uv of the whole texture, previous = current - vector (mvcheck.py: 0.00 px residual against the cameras).
#include "mvfix.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "cam_prev.h"
#include "vr_shared.h"
#include "config.h"
#include "log.h"
#include "sl_hooks.h"
#include "ffx_hooks.h"
#include "stereo_render.h"
#include "taa.h"

namespace mvfix {
namespace {

const char* kHlsl = R"(
#pragma pack_matrix(row_major)
// Everything in Streamline's terms (sl::Constants of this evaluate, row-vector matrices: clip = v * M).
cbuffer P : register(b0) {
    int2 g_off; int2 g_size;        // rendered extent in the game's textures
    float2 g_tex; float g_thr; float g_shift;   // texture size; static threshold (pixels); the previous pass's camera: view x shift
    float4 g_flags;                  // x: mode, y: have other, z: object sign, w: second pass of the tick
    float4x4 g_viewToClip;           // sl cameraViewToClip
    float4x4 g_clipToView;           // sl clipToCameraView
    float4x4 g_clipToPrev;           // sl clipToPrevClip (this eye: its previous frame)
    float4x4 g_clipToOther;          // the other eye's view (the pass whose textures are bound at t2 / t3): view x shift
};
Texture2D<float> g_depth : register(t0);
Texture2D<float2> g_mv : register(t1);
Texture2D<float> g_otherDepth : register(t2);
Texture2D<float2> g_otherObj : register(t3);
RWTexture2D<float2> g_out : register(u0);
RWTexture2D<float2> g_obj : register(u1);
RWTexture2D<float> g_depthOut : register(u2);

float2 NdcOf(float4 clip) { return clip.xy / (abs(clip.w) > 1e-9 ? clip.w : 1e-9); }
// the game's vector for a pixel that was at prevNdc: previous uv - current uv (rendered area), in texture uv units
float2 Vec(float2 ndc, float2 prevNdc) { return float2(prevNdc.x - ndc.x, ndc.y - prevNdc.y) * 0.5 * ((float2)g_size / g_tex); }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    int2 p = (int2)id.xy;
    if (p.x >= g_size.x || p.y >= g_size.y) return;
    int2 t = g_off + p;
    float d = g_depth[t];
    float2 game = g_mv[t];
    float2 uv = ((float2)p + 0.5) / (float2)g_size;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 clip = float4(ndc, d, 1.0);
    int mode = (int)g_flags.x;
    // static pixels: this eye's previous frame (what the game's resolve computes for them)
    float2 camStatic = Vec(ndc, NdcOf(mul(clip, g_clipToPrev)));
    float pxScale = g_tex.x / (float)g_size.x;  // thresholds are in pixels of the rendered image; vectors in texture uv
    bool isStatic = length((game - camStatic) * g_tex) < g_thr * pxScale;
    // objects (own previous transform, stored by the previous pass = the other eye): their vector holds that eye's
    // disparity; the rest is the object's own motion
    float4 view = mul(clip, g_clipToView);
    view /= (abs(view.w) > 1e-9 ? view.w : 1e-9);
    float2 obj = 0.0;
    if (!isStatic) {
        // the previous pass's camera: the other eye (view x shift); for the tick's first pass it is also a tick ago
        float4 vo = view + float4(g_shift, 0.0, 0.0, 0.0);
        float4 co = mul(vo, g_viewToClip);
        float2 prevPass = NdcOf(g_flags.w > 0.5 ? co : mul(co, g_clipToPrev));
        obj = game - Vec(ndc, prevPass);
        float m = length(obj * g_tex);
        if (m < g_thr * pxScale) obj = 0.0;  // noise
    }
    g_obj[p] = obj;  // the raw object part (the frame warp marks moving things by it, including fast ones)
    if (length(obj * g_tex) > 12.0 * pxScale) obj = 0.0;  // no plausible object motion for the upscaler (a bad frame)
    g_depthOut[p] = d;
    if (mode == 3 && g_flags.y > 0.5) {
        // second pass of the tick: no object motion of its own; take the first pass's at the same surface point
        float4 vo = view + float4(g_shift, 0.0, 0.0, 0.0);  // the other eye's camera (this tick's first pass)
        float4 co = mul(vo, g_viewToClip);
        float2 no = NdcOf(co);
        float2 uvo = float2(no.x * 0.5 + 0.5, 0.5 - no.y * 0.5);
        int2 q = (int2)floor(uvo * (float2)g_size);
        obj = 0.0;
        if (co.w > 0.0 && q.x >= 0 && q.y >= 0 && q.x < g_size.x && q.y < g_size.y) {
            float dq = g_otherDepth[q];
            float de = co.z / co.w;  // the point's depth-buffer value as seen by the other eye
            if (abs(dq - de) < 0.05 * de + 1e-5) obj = g_otherObj[q];  // same surface
            if (length(obj * g_tex) > 12.0 * pxScale) obj = 0.0;
        }
    } else if (mode == 1) {
        obj = 0.0;
    }
    float2 o = camStatic + g_flags.z * obj;
    if (mode == 0) o = game;
    g_out[t] = o;
}
)";

struct Cb {
    int32_t off[2], size[2];
    float tex[2], thr, shift;
    float flags[4];
    float viewToClip[16], clipToView[16], clipToPrev[16], clipToOther[16];
};
static_assert(sizeof(Cb) == 304, "cb layout");

std::mutex g_mutex;
ID3D12Device* g_dev = nullptr;
ID3D12RootSignature* g_rs = nullptr;
ID3D12PipelineState* g_pso = nullptr;
ID3D12DescriptorHeap* g_heap = nullptr;
ID3D12Resource* g_cb = nullptr;
uint8_t* g_cbMapped = nullptr;
constexpr UINT kCbSlot = 512, kSlots = 16, kDescPerSlot = 7;
UINT g_inc = 0, g_slot = 0;
bool g_failed = false;
struct EyeRes {
    ID3D12Resource* out = nullptr;    // the rebuilt vectors (the game's texture size and format)
    ID3D12Resource* obj = nullptr;    // object part (extent, RG16F)
    ID3D12Resource* depth = nullptr;  // depth copy (extent, R32F)
    UINT64 w = 0;
    UINT h = 0, ew = 0, eh = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool haveCur = false;  // this eye's obj / depth textures hold its last pass
    unsigned passes = 0;
};
EyeRes g_eye[2];
std::atomic<long> g_passes{0}, g_skips{0}, g_second{0}, g_noMatch{0};
std::atomic<int> g_ipdSign{1}, g_objSign{1}, g_thrTenths{10}, g_modeOverride{-1}, g_trace{0};  // threshold 1.0 px

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

D3D12_RESOURCE_BARRIER Tr(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    return x;
}

template <typename T>
T Orig(void* cl, int slot) {
    return reinterpret_cast<T>(taa::OrigListFn(cl, slot));
}

const D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
const D3D12_RESOURCE_STATES kOut = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

bool Init(ID3D12Device* dev) {
    if (g_pso) return true;
    if (g_failed) return false;
    g_failed = true;
    using Compile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                       UINT, ID3DBlob**, ID3DBlob**);
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = m ? reinterpret_cast<Compile_t>(GetProcAddress(m, "D3DCompile")) : nullptr;
    if (!compile) return false;
    ID3DBlob *cs = nullptr, *err = nullptr, *blob = nullptr;
    if (FAILED(compile(kHlsl, strlen(kHlsl), "motion_rebuild", nullptr, nullptr, "main", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                       &cs, &err))) {
        LOG("Motion rebuild: shader failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return false;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 4}};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor = {0, 0};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC rsd{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_rs)))) {
        LOG("Motion rebuild: root signature failed");
        return false;
    }
    blob->Release();
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_rs;
    pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso)))) {
        LOG("Motion rebuild: pipeline failed");
        return false;
    }
    cs->Release();
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kDescPerSlot * kSlots, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap)))) return false;
    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (UINT64)kCbSlot * kSlots;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_cb))) ||
        FAILED(g_cb->Map(0, nullptr, reinterpret_cast<void**>(&g_cbMapped))))
        return false;
    g_dev = dev;
    g_failed = false;
    LOG("Motion rebuild: pass ready");
    return true;
}

ID3D12Resource* MakeTex(UINT64 w, UINT h, DXGI_FORMAT fmt, const wchar_t* name) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
    d.Format = fmt;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    ID3D12Resource* t = nullptr;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, kOut, nullptr, IID_PPV_ARGS(&t)))) {
        LOG("Motion rebuild: texture %llu x %u format %d failed", (unsigned long long)w, h, (int)fmt);
        return nullptr;
    }
    t->SetName(name);
    return t;
}

bool EnsureRes(int eye, const D3D12_RESOURCE_DESC& md, UINT ew, UINT eh) {
    EyeRes& r = g_eye[eye];
    const DXGI_FORMAT fmt = SrvFormat(md.Format);
    if (r.out && r.w == md.Width && r.h == md.Height && r.fmt == fmt && r.ew == ew && r.eh == eh) return true;
    for (ID3D12Resource** t : {&r.out, &r.obj, &r.depth})
        if (*t) (*t)->Release(), *t = nullptr;  // on a resolution change only (the GPU may still read it: accepted)
    r.haveCur = false;
    r.out = MakeTex(md.Width, md.Height, fmt, L"ff16vr motion rebuild");
    r.obj = MakeTex(ew, eh, DXGI_FORMAT_R16G16_FLOAT, L"ff16vr motion rebuild object part");
    r.depth = MakeTex(ew, eh, DXGI_FORMAT_R32_FLOAT, L"ff16vr motion rebuild depth");
    if (!r.out || !r.obj || !r.depth) return false;
    r.w = md.Width;
    r.h = md.Height;
    r.fmt = fmt;
    r.ew = ew;
    r.eh = eh;
    LOG("Motion rebuild: textures %llu x %u (extent %u x %u) format %d for eye %d", (unsigned long long)md.Width, md.Height, ew, eh,
        (int)fmt, eye);
    return true;
}

}  // namespace

bool Wanted() {
    // DLSS (Streamline evaluates) or FSR 3 (FFX upscale dispatches, constants from the camera buffer)
    if (vr::g_config.motionRebuild <= 0 || !stereo_render::NativeActive()) return false;
    return (vr::g_config.perEyeDlss && sl_hooks::DlssEvaluatingRecently()) ||
           (vr::g_config.perEyeFsr && ffx_hooks::FsrEvaluatingRecently());
}

// 4x4 row-major helpers (column-vector convention: v' = M v), for the camera constants' matrices.
void Mul4(const float a[16], const float b[16], float out[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
}
bool Inv4(const float m[16], float out[16]) {
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) a[r][c] = m[r * 4 + c], a[r][c + 4] = r == c ? 1.0 : 0.0;
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
        if (std::fabs(a[piv][c]) < 1e-12) return false;
        if (piv != c)
            for (int k = 0; k < 8; ++k) std::swap(a[piv][k], a[c][k]);
        const double d = a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] /= d;
        for (int r = 0; r < 4; ++r)
            if (r != c) {
                const double f = a[r][c];
                for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
            }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = (float)a[r][c + 4];
    return true;
}
void Transpose4(const float m[16], float out[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[c * 4 + r] = m[r * 4 + c];
}
// The camera constants (cam_prev rows): rows 0..2 world-to-view (3 x 4), rows 7..10 the projection with the jitter in
// column 2 of rows 0 / 1 (dump 302, run Nov 9: the unjittered projection equals Streamline's cameraViewToClip, and
// prevViewProj * inverse(viewProj) equals its clipToPrevClip to 1e-6 in the rotation terms).
void ViewProj(const float c[cam_prev::kRows][4], float view[16], float proj[16], float vp[16]) {
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k) view[r * 4 + k] = c[r][k];
    view[12] = view[13] = view[14] = 0.f;
    view[15] = 1.f;
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k) proj[r * 4 + k] = c[7 + r][k];
    proj[2] = proj[6] = 0.f;  // no jitter
    Mul4(proj, view, vp);
}

std::atomic<long> g_calls{0};
void Why(const char* why) {
    static std::atomic<int> s_n{0};
    if (s_n++ < 6) LOG("Motion rebuild: no pass (%s) at call %ld", why, g_calls.load());
}

ID3D12Resource* Run(ID3D12GraphicsCommandList* cl, const stereo_edge::Inputs& in) {
    ++g_calls;
    if (!cl || !in.depth || !in.mv || in.w <= 0 || in.h <= 0 || in.eye < 0 || in.eye > 1) { Why("inputs"); return nullptr; }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_dev) {
        ID3D12Device* dev = nullptr;
        if (FAILED(in.mv->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return nullptr;
        const bool ok = Init(dev);
        dev->Release();
        if (!ok) { Why("init"); return nullptr; }
    }
    const D3D12_RESOURCE_DESC md = in.mv->GetDesc(), dd = in.depth->GetDesc();
    if (md.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        (UINT64)(in.x + in.w) > md.Width || (UINT)(in.y + in.h) > md.Height || (UINT64)(in.x + in.w) > dd.Width ||
        (UINT)(in.y + in.h) > dd.Height) {
        if (g_skips++ < 5)
            LOG("Motion rebuild: skipped (motion %llu x %u fmt %d, depth %llu x %u fmt %d, extent %d,%d %d x %d)",
                (unsigned long long)md.Width, md.Height, (int)md.Format, (unsigned long long)dd.Width, dd.Height, (int)dd.Format,
                in.x, in.y, in.w, in.h);
        return nullptr;
    }
    // The cameras: the one Streamline was given for this evaluate (its position), found among the recent passes;
    // the same eye's previous one; the pass before it (the other eye).
    // The eye: the view being rendered (view 0 = the tick's first pass; the token / frame eye disagreement counter in
    // sl_hooks shows this to be reliable at the evaluate). Streamline's camera position cannot tell the eyes apart: it
    // is the camera node's, the same for both. Runs 295-297: view 0's objects encode a previous camera at -separation
    // (view x) of this tick, view 1's one at +separation a tick ago: view 1 is drawn first.
    if (!in.haveSl) { Why("no Streamline constants"); return nullptr; }
    const int eye = in.eye;
    const bool second = eye == 0;  // runs 296 / 297: view 1's objects have a tick-old previous pass, view 0's this tick's
    EyeRes& r = g_eye[eye];
    EyeRes& o = g_eye[eye ^ 1];
    if (!EnsureRes(eye, md, (UINT)in.w, (UINT)in.h)) { Why("textures"); return nullptr; }
    const bool haveOther = o.haveCur && o.ew == r.ew && o.eh == r.eh;  // the other eye's last pass: this tick's first (second pass) or the tick before
    int mode = std::clamp(vr::g_config.motionRebuild, 0, 3);
    if (g_modeOverride.load() >= 0) mode = g_modeOverride.load();
    if (mode == 3 && !second) mode = 2;  // the first pass has its own object motion
    if (second) ++g_second;
    r.haveCur = true;
    r.passes++;
    // eye separation in game units: the headset's IPD (the two eye poses) times the world scale; the camera constants'
    // lateral offsets are not used (the eyes' values come from different ticks: wrong while the head moves)
    float sep = 0.063f * vr::g_config.worldScale;
    {
        const vrs::HeadSample hs = vrs::LatestHead();
        if (hs.valid) {
            const float dx = hs.eye[1].p[0] - hs.eye[0].p[0], dy = hs.eye[1].p[1] - hs.eye[0].p[1], dz = hs.eye[1].p[2] - hs.eye[0].p[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz) * vr::g_config.worldScale;
            if (d > 0.03f && d < 0.12f) sep = d;
        }
    }
    sep *= (float)g_ipdSign.load();
    // where a point moves in view x when seen from the previous pass's camera: view 0 (right eye) -> the left eye
    // sits at -sep, so the point shifts by +sep; view 1 the other way
    const float shift = eye == 0 ? sep : -sep;

    const UINT slot = g_slot++ % kSlots;
    Cb cb{};
    cb.off[0] = in.x;
    cb.off[1] = in.y;
    cb.size[0] = in.w;
    cb.size[1] = in.h;
    cb.tex[0] = (float)md.Width;
    cb.tex[1] = (float)md.Height;
    cb.thr = 0.1f * (float)g_thrTenths.load();
    cb.shift = shift;
    cb.flags[0] = (float)mode;
    cb.flags[1] = haveOther ? 1.f : 0.f;
    cb.flags[2] = (float)g_objSign.load();
    cb.flags[3] = second ? 1.f : 0.f;
    memcpy(cb.viewToClip, in.sl + 32, 64);
    memcpy(cb.clipToView, in.sl + 96, 64);
    memcpy(cb.clipToPrev, in.sl + 224, 64);
    memcpy(cb.clipToOther, in.sl + 288, 64);  // (unused: prevClipToClip)
    memcpy(g_cbMapped + (size_t)slot * kCbSlot, &cb, sizeof(cb));

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)slot * kDescPerSlot * g_inc;
    gpu.ptr += (UINT64)slot * kDescPerSlot * g_inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    auto srv = [&](ID3D12Resource* t, DXGI_FORMAT f, int i) {
        sd.Format = f;
        g_dev->CreateShaderResourceView(t, &sd, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + (SIZE_T)i * g_inc});
    };
    srv(in.depth, SrvFormat(dd.Format), 0);
    srv(in.mv, SrvFormat(md.Format), 1);
    srv(o.depth ? o.depth : r.depth, DXGI_FORMAT_R32_FLOAT, 2);
    srv(o.obj ? o.obj : r.obj, DXGI_FORMAT_R16G16_FLOAT, 3);
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    auto uav = [&](ID3D12Resource* t, DXGI_FORMAT f, int i) {
        ud.Format = f;
        g_dev->CreateUnorderedAccessView(t, nullptr, &ud, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + (SIZE_T)i * g_inc});
    };
    uav(r.out, r.fmt, 4);
    uav(r.obj, DXGI_FORMAT_R16G16_FLOAT, 5);
    uav(r.depth, DXGI_FORMAT_R32_FLOAT, 6);

    D3D12_RESOURCE_BARRIER pre[5];
    UINT np = 0;
    if (!(in.mvState & kRead)) pre[np++] = Tr(in.mv, in.mvState, (D3D12_RESOURCE_STATES)(in.mvState | kRead));
    if (!(in.depthState & kRead)) pre[np++] = Tr(in.depth, in.depthState, (D3D12_RESOURCE_STATES)(in.depthState | kRead));
    pre[np++] = Tr(r.out, kOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    pre[np++] = Tr(r.obj, kOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    pre[np++] = Tr(r.depth, kOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->ResourceBarrier(np, pre);
    ID3D12DescriptorHeap* heaps[] = {g_heap};
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*)>(cl, 28)(cl, 1, heaps);
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*)>(cl, 29)(cl, g_rs);
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*)>(cl, 25)(cl, g_pso);
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS)>(cl, 37)(
        cl, 0, g_cb->GetGPUVirtualAddress() + (UINT64)slot * kCbSlot);
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE)>(cl, 31)(cl, 1, gpu);
    Orig<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT)>(cl, 14)(cl, (in.w + 7) / 8, (in.h + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER post[5];
    UINT nq = 0;
    post[nq++] = Tr(r.out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kOut);
    post[nq++] = Tr(r.obj, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kOut);
    post[nq++] = Tr(r.depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kOut);
    if (!(in.mvState & kRead)) post[nq++] = Tr(in.mv, (D3D12_RESOURCE_STATES)(in.mvState | kRead), in.mvState);
    if (!(in.depthState & kRead)) post[nq++] = Tr(in.depth, (D3D12_RESOURCE_STATES)(in.depthState | kRead), in.depthState);
    cl->ResourceBarrier(nq, post);
    stereo_edge::RestoreListState(cl);
    const long n = g_passes++;
    if (g_trace.load() > 0) {
        --g_trace;
        const float* j = reinterpret_cast<const float*>(in.sl + 352);
        const float* m = reinterpret_cast<const float*>(in.sl + 224);  // clipToPrevClip rows
        const uint8_t* fl = in.sl + 444;
        LOG("Motion rebuild trace: eye %d (%s pass) node %.4f %.4f %.4f jitter %.3f %.3f shift %.4f mode %d | c2p row2 %.5f %.5f row3 %.5f %.5f %.5f "
            "| flags depthInv %d camMotion %d mv3d %d reset %d",
            eye, second ? "second" : "first", in.camPos[0], in.camPos[1], in.camPos[2], j[0], j[1], shift, mode, m[8], m[9], m[12], m[13],
            m[14], fl[0], fl[1], fl[2], fl[3]);
    }
    if (n < 4 || n % 3600 == 0)
        LOG("Motion rebuild: pass %ld eye %d (%s pass of the tick, mode %d, ipd sign %d, object sign %g, threshold %.1f px, shift %.4f; "
            "extent %d,%d %d x %d of %llu x %u)",
            n, eye, second ? "second" : "first", mode, g_ipdSign.load(), cb.flags[2], cb.thr, shift, in.x, in.y, in.w, in.h,
            (unsigned long long)md.Width, md.Height);
    return r.out;
}

ID3D12Resource* LastObject(int eye, UINT* w, UINT* h) {
    if (eye < 0 || eye > 1 || !g_eye[eye].haveCur || !g_eye[eye].obj) return nullptr;
    *w = g_eye[eye].ew;
    *h = g_eye[eye].eh;
    return g_eye[eye].obj;
}

bool ConstantsFromCamera(int eye, stereo_edge::Inputs* in, float jitterX, float jitterY, int renderW, int renderH) {
    float last[cam_prev::kRows][4], used[cam_prev::kRows][4];
    int back = 0;
    if (!cam_prev::FindCameraByJitter(eye, jitterX, jitterY, renderW, renderH, last, used, &back)) return false;
    float view[16], proj[16], vpCur[16], vpPrev[16], inv[16], tmp[16], out[16];
    ViewProj(last, view, proj, vpCur);
    ViewProj(used, view, tmp, vpPrev);
    if (!Inv4(vpCur, inv)) return false;
    memset(in->sl, 0, sizeof(in->sl));
    Transpose4(proj, out);
    memcpy(in->sl + 32, out, 64);  // cameraViewToClip (row-vector convention)
    if (!Inv4(proj, tmp)) return false;
    Transpose4(tmp, out);
    memcpy(in->sl + 96, out, 64);  // clipToCameraView
    Mul4(vpPrev, inv, tmp);
    Transpose4(tmp, out);
    memcpy(in->sl + 224, out, 64);  // clipToPrevClip
    Mul4(vpCur, inv, tmp);          // identity: prevClipToClip is not used
    Transpose4(tmp, out);
    memcpy(in->sl + 288, out, 64);
    const float scale[2] = {1.f, 1.f};
    memcpy(in->sl + 360, scale, 8);
    memcpy(in->sl + 376, last[37], 12);  // camera node position
    in->sl[444] = 1;                     // depthInverted
    in->sl[445] = 1;                     // cameraMotionIncluded
    in->haveSl = in->haveCamPos = true;
    memcpy(in->camPos, last[37], sizeof(in->camPos));
    return true;
}

bool DevCommand(const char* cmd, int v) {
    if (!strcmp(cmd, "mvcam")) g_ipdSign = v < 0 ? -1 : 1;
    else if (!strcmp(cmd, "mvobj")) g_objSign = v < 0 ? -1 : 1;
    else if (!strcmp(cmd, "mvthr")) g_thrTenths = std::max(v, 0);
    else if (!strcmp(cmd, "mvmode")) g_modeOverride = v;
    else if (!strcmp(cmd, "mvtrace")) g_trace = v;
    else return false;
    LOG("Motion rebuild: %s %d (ipd sign %d, object sign %d, threshold %.1f px, mode override %d)", cmd, v, g_ipdSign.load(),
        g_objSign.load(), 0.1f * g_thrTenths.load(), g_modeOverride.load());
    return true;
}

const char* Status() {
    static char buf[96];
    snprintf(buf, sizeof(buf), "motion rebuild %ld passes of %ld calls (%ld second-pass, %ld camera misses)", g_passes.load(),
             g_calls.load(), g_second.load(), g_noMatch.load());
    return buf;
}

}  // namespace mvfix
