#include "taa.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "mvfix.h"
#include "log.h"

// TAA off (DisableTAA).
//
// The game's TAA is one compute pass, ProcessTAACandidatesFullScreenCS (RenderDoc hash ae5132ad67, run 159): it reads
// the lit HDR scene (t5, RGBA16F), the history (t3), velocity, depth and an edge mask, and writes the anti-aliased
// scene (u0, RGBA16F). The next pass copies that output into the history; motion blur / bloom / tonemap read it.
// With DisableTAA on, the mod swaps that pipeline for one that copies t5 to u0 unchanged (same root signature,
// so all of the game's bindings stay valid). The swap happens at SetPipelineState, so the option works live.
// Pipelines are recognised at creation (CreateComputePipelineState, CreatePipelineState, pipeline libraries) by the
// shader container digest. With DLSS / FSR / XeSS active the game runs the upscaler instead of this pass; the
// option then has no effect. The camera jitter stays (sub-pixel; see notes).
namespace taa {
namespace {

// DXIL container digest (bytes 4..20) of ProcessTAACandidatesFullScreenCS, game version 1.03 (capture 3532).
const uint8_t kTaaDigest[16] = {0x13, 0xc3, 0x40, 0xf9, 0x8d, 0xdb, 0x91, 0xd4,
                                0x56, 0x04, 0xc0, 0xd3, 0xf2, 0xd5, 0x19, 0xd0};

// t5 = scene, u0 = output. Each thread of the original covers a block of pixels (dispatch 91 x 76 groups of 8 x 8 for
// 2184 x 1816, i.e. 3 x 3 pixels); the group count is in the per-frame constants (cbuffer1 row 30 .zw), so the
// block size is derived from it and the output size.
const char* kPassThroughHlsl = R"(
Texture2D<float4> g_scene : register(t5);
RWTexture2D<float4> g_out : register(u0);
cbuffer Frame : register(b1) { uint4 g_rows[36]; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    g_out.GetDimensions(w, h);
    uint gx = max(g_rows[30].z, 1u), gy = max(g_rows[30].w, 1u);
    uint bx = (w + gx * 8 - 1) / (gx * 8), by = (h + gy * 8 - 1) / (gy * 8);
    bx = clamp(bx, 1u, 8u);
    by = clamp(by, 1u, 8u);
    for (uint y = 0; y < by; ++y)
        for (uint x = 0; x < bx; ++x) {
            uint2 p = uint2(id.x * bx + x, id.y * by + y);
#ifdef TAA_DEBUG
            if (p.x < w && p.y < h) g_out[p] = float4(g_scene[p].r + 0.5, 0, g_scene[p].b + 0.5, 1);  // magenta tint
#else
            if (p.x < w && p.y < h) g_out[p] = g_scene[p];
#endif
        }
}
)";

// ---- Per-eye character motion (PerEyeObjectMotion) ----
// resolveUvSpaceVelocityCS (RenderDoc hash b53ee7e2df) turns the G-buffer velocity (t0, RG16F, NDC units, 65504 =
// static pixel) and depth (t1, near / view z) into the uv-space velocity (u0) that TAA and the upscalers read.
// Skinned / animated objects write their own velocity in the G-buffer from the *previous* CameraParam of cCamera
// (b0 +0x400), which in native stereo is the other eye: ~20 px of false horizontal motion on characters (runs
// 168 / 170), the fuzzy halo. The error is exactly the disparity between the two eye cameras at the pixel's depth:
// e = P00 * (lateral(current eye) - lateral(previous eye)) / z (NDC), so right after the game's resolve the mod runs a
// small pass on the same bindings that adds it back for object pixels (G-buffer velocity not 65504) whose velocity
// isn't the camera-only reprojection (objects without a previous pose take that path and are already right).
// Two variants of the resolve: with TAA (b53ee7e2df: t0 velocity, t1 depth R16F) and with the upscalers
// (FSR 3 / DLSS, 3ec0c2941a, run 195: t0 depth D32, t1 stencil, t2 velocity). Same camera / frame cbuffers.
const uint8_t kVelDigest[16] = {0xd9, 0x78, 0xef, 0x28, 0x02, 0x21, 0xa4, 0xca,
                                0xf7, 0x98, 0x46, 0x0b, 0xea, 0x23, 0x11, 0xc7};
const uint8_t kVelDigestUpscaler[16] = {0x4b, 0x56, 0x37, 0x37, 0x1f, 0x02, 0xac, 0x01,
                                        0xb0, 0x9f, 0xee, 0xe6, 0x21, 0xfb, 0x62, 0xc8};
const char* kVelFixHlsl = R"(
cbuffer Camera : register(b0) { float4 c[138]; };
cbuffer Frame : register(b1) { uint4 g_rows[36]; };
#ifdef UPSCALER_VARIANT
Texture2D<float> g_depth : register(t0);
Texture2D<float2> g_vel : register(t2);
#else
Texture2D<float2> g_vel : register(t0);
Texture2D<float> g_depth : register(t1);
#endif
RWTexture2D<float2> g_out : register(u0);
float Lateral(uint b) {  // eye offset from the camera node along the camera's right axis (CameraParam at row b)
    float3 right = float3(c[b + 11].x, c[b + 12].x, c[b + 13].x);
    return dot(c[b + 38].xyz - c[b + 37].xyz, right);
}
#ifdef DISOCC
// Stereo edge fix (StereoEdgeFix, shared upscaler history: DlssGhostingFix off). The previous frame is the other eye,
// so at every depth edge a strip of background was hidden behind the nearer object from the other eye's viewpoint;
// the upscaler blends the wrong (occluder's) history there: the halo. Those pixels are found from depth alone: p was
// hidden if a nearer pixel q on the same row lands on the same spot in the previous (other eye's) frame. Their motion
// vector is pointed off screen, so the upscaler treats them as newly revealed and uses the current frame.
float2 PrevNdc(float2 ndc, float d) {
    float4 v = float4(ndc, d, 1.0);
    float pw = dot(c[131], v);
    return float2(dot(c[128], v), dot(c[129], v)) / (abs(pw) > 1e-6 ? pw : 1.0);
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    g_out.GetDimensions(w, h);
#if EDGE_MODE == 4
    // dev: no checks at all, every texel of the thread's block (does anything downstream read this texture?)
    {
        uint gx4 = max(g_rows[30].z, 1u), gy4 = max(g_rows[30].w, 1u);
        uint bx4 = clamp((w + gx4 * 8 - 1) / (gx4 * 8), 1u, 8u), by4 = clamp((h + gy4 * 8 - 1) / (gy4 * 8), 1u, 8u);
        for (uint y = 0; y < by4; ++y)
            for (uint x = 0; x < bx4; ++x) {
                uint2 p = uint2(id.x * bx4 + x, id.y * by4 + y);
                if (p.x < w && p.y < h) g_out[p] = float2(0.05, 0.0);
            }
        return;
    }
#endif
    float ipd = 2.0 * length(c[38].xyz - c[37].xyz);
    float3 right = float3(c[11].x, c[12].x, c[13].x);
    float3 curPos = float3(c[11].w, c[12].w, c[13].w), prevPos = float3(c[75].w, c[76].w, c[77].w);
    float side = dot(curPos - prevPos, right);
    if (ipd < 0.005 || ipd > 2.0 || abs(side) < 0.5 * ipd || abs(side) > 1.5 * ipd) return;  // previous = other eye only
    uint gx = max(g_rows[30].z, 1u), gy = max(g_rows[30].w, 1u);
    uint bx = clamp((w + gx * 8 - 1) / (gx * 8), 1u, 8u), by = clamp((h + gy * 8 - 1) / (gy * 8), 1u, 8u);
    float rs = asfloat(g_rows[6].x);
    rs = (rs > 0.05 && rs <= 1.0) ? rs : 1.0;
    float wS = w * rs, hS = h * rs;  // the rendered part of the textures
    for (uint y = 0; y < by; ++y)
        for (uint x = 0; x < bx; ++x) {
            uint2 p = uint2(id.x * bx + x, id.y * by + y);
            if (p.x >= (uint)wS || p.y >= (uint)hS) continue;
            float dp = g_depth[p];
            float2 ndcP = float2((p.x + 0.5) / wS * 2.0 - 1.0, 1.0 - (p.y + 0.5) / hS * 2.0);
            float prevP = PrevNdc(ndcP, dp).x;
            bool hidden = false;
            [loop] for (int dir = -1; dir <= 1 && !hidden; dir += 2) {
                // coarse look for anything nearer within the search range
                bool near = false;
                [loop] for (int k = 8; k <= EDGE_RANGE && !near; k += 8) {
                    int qx = (int)p.x + dir * k;
                    if (qx < 0 || qx >= (int)wS) break;
                    near = g_depth[uint2(qx, p.y)] > dp * 1.02 + 1e-7;
                }
                if (!near) continue;
                [loop] for (int k = 1; k <= EDGE_RANGE && !hidden; ++k) {
                    int qx = (int)p.x + dir * k;
                    if (qx < 0 || qx >= (int)wS) break;
                    float dq = g_depth[uint2(qx, p.y)];
                    if (!(dq > dp * 1.02 + 1e-7)) continue;
                    float2 ndcQ = float2((qx + 0.5) / wS * 2.0 - 1.0, ndcP.y);
                    float prevQ = PrevNdc(ndcQ, dq).x;
                    hidden = abs(prevQ - prevP) * 0.5 * wS < 0.75;  // same spot in the other eye's frame (pixels)
                }
            }
#if EDGE_MODE == 2
            if (hidden) g_out[p] = float2(0.03, 0.0);  // dev: visible smear where the mask is
#elif EDGE_MODE == 3
            g_out[p] = float2(4.0, 4.0);  // dev: no history anywhere (does the upscaler react to off-screen vectors?)
#else
            if (hidden) g_out[p] = float2(4.0, 4.0);  // off screen: no history here
#endif
        }
}
#elif defined(UPSCALER_VARIANT)
// Upscaler variant (run 196): the previous CameraParam (+0x400) is the other eye, and the resolve reprojects every
// pixel (static and animated) with it, so the whole image carries the eye disparity as motion (+-10 px at 50 % FSR
// with a 2 m scene). The error is exact: e = P00 * (lateral(cur) - lateral(prev)) / z in NDC, i.e. -e / 2 in uv x.
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    // Eye separation from this eye's offset to the camera node; the previous camera's position is the translation of
    // its camera-to-world rows (75..77 .w; rows 11..13 .w for the current one). The previous camera is the other eye
    // when it sits about an IPD to the side (camera motion per tick is far smaller).
    float ipd = 2.0 * length(c[38].xyz - c[37].xyz);
    float3 right = float3(c[11].x, c[12].x, c[13].x);
    float3 curPos = float3(c[11].w, c[12].w, c[13].w), prevPos = float3(c[75].w, c[76].w, c[77].w);
    float side = dot(curPos - prevPos, right);
    if (ipd < 0.005 || ipd > 2.0 || abs(side) > 1.5 * ipd) return;
    // Previous camera = this eye (the mod's per-eye motion vectors for per-eye DLSS, run 211): static pixels are right;
    // animated objects still carry the eye disparity in one eye (their previous pose was stored by the other eye's
    // pass), so only those are fixed, with the sign that shrinks the motion.
    bool sameEye = abs(side) < 0.5 * ipd;
    float dl = side > 0.0 ? ipd : -ipd;
    uint w, h;
    g_out.GetDimensions(w, h);
    uint gx = max(g_rows[30].z, 1u), gy = max(g_rows[30].w, 1u);
    uint bx = clamp((w + gx * 8 - 1) / (gx * 8), 1u, 8u), by = clamp((h + gy * 8 - 1) / (gy * 8), 1u, 8u);
    float p00 = c[7].x, nearZ = c[9].w;
    // uv velocity is relative to the whole texture; the scene covers the render-scale part of it (frame row 6 .x:
    // 0.5 with FSR performance, run 196)
    float rs = asfloat(g_rows[6].x);
    rs = (rs > 0.05 && rs <= 1.0) ? rs : 1.0;
    if (p00 <= 0.0 || nearZ <= 0.0) return;
    for (uint y = 0; y < by; ++y)
        for (uint x = 0; x < bx; ++x) {
            uint2 p = uint2(id.x * bx + x, id.y * by + y);
            if (p.x >= w || p.y >= h) continue;
            float d = g_depth[p];
            if (d <= 0.0) continue;  // sky / outside the rendered area: no disparity
            float e = p00 * dl * d / nearZ;
            // Static pixels (G-buffer velocity 65504) are reprojected by the resolve with the other eye's camera: always
            // off by e. Animated objects carry their own G-buffer velocity, which has the error only in one eye (the
            // pass whose skinning used the other eye's previous camera; run 200): fix those when removing e helps.
            float2 g = g_vel[p];
            if (sameEye) {
                if (g.x > 60000.0) continue;
                e = p00 * ipd * d / nearZ;
                e = abs(g.x + e) < abs(g.x - e) ? e : -e;
                if (abs(g.x) < 0.25 * abs(e) || !(abs(g.x + e) < abs(g.x))) continue;
            } else if (g.x < 60000.0 && !(abs(g.x + e) < abs(g.x))) continue;
            float2 o = g_out[p];
            o.x -= 0.5 * rs * e;
            g_out[p] = o;
        }
}
#else
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    // The resolve's camera holds this eye only (run 190): the eye separation is twice the eye's offset from the camera
    // node (rows 38 / 37); which way the error points is taken per pixel (the sign that removes it).
    float ipd = 2.0 * length(c[38].xyz - c[37].xyz);
    if (ipd < 0.005 || ipd > 2.0) return;
    uint w, h;
    g_out.GetDimensions(w, h);
    uint gx = max(g_rows[30].z, 1u), gy = max(g_rows[30].w, 1u);
    uint bx = clamp((w + gx * 8 - 1) / (gx * 8), 1u, 8u), by = clamp((h + gy * 8 - 1) / (gy * 8), 1u, 8u);
    float p00 = c[7].x, nearZ = c[9].w;
    // uv velocity is relative to the whole texture; the scene covers the render-scale part of it (frame row 6 .x:
    // 0.5 with FSR performance, run 196)
    float rs = asfloat(g_rows[6].x);
    rs = (rs > 0.05 && rs <= 1.0) ? rs : 1.0;
    if (p00 <= 0.0 || nearZ <= 0.0) return;
    for (uint y = 0; y < by; ++y)
        for (uint x = 0; x < bx; ++x) {
            uint2 p = uint2(id.x * bx + x, id.y * by + y);
            if (p.x >= w || p.y >= h) continue;
            float2 g = g_vel[p];
            if (g.x > 60000.0) continue;  // static pixel: camera reprojection, already right
            float d = g_depth[p];
            if (d <= 0.0) continue;
            float e = p00 * ipd * d / nearZ;  // eye disparity in NDC x at this depth (z = near / d)
            // camera-only velocity (what objects without a previous pose write): already right
            float4 ndc = float4((p.x + 0.5) / w * 2.0 - 1.0, 1.0 - (p.y + 0.5) / h * 2.0, d, 1.0);
            float pw = dot(c[131], ndc);
            float2 prevNdc = float2(dot(c[128], ndc), dot(c[129], ndc)) / (abs(pw) > 1e-6 ? pw : 1.0);
            float2 gcam = -(prevNdc - ndc.xy + c[137].xy);
            if (abs(g.x - gcam.x) < 0.3 * e && abs(g.y - gcam.y) < 0.3 * e) continue;
            float s = abs(g.x + e) < abs(g.x - e) ? 1.0 : -1.0;
            if (abs(g.x + s * e) >= abs(g.x)) continue;  // removing the disparity would not help
            float2 o = g_out[p];
            o.x -= 0.5 * rs * s * e;  // uv = -ndc / 2 in x (x render scale)
            g_out[p] = o;
        }
}
#endif
)";
ID3DBlob* g_velCs = nullptr;
ID3DBlob* g_velCsUp = nullptr;
struct VelPair {
    ID3D12PipelineState* orig;
    ID3D12PipelineState* fix;
};
VelPair g_vel[16];
ID3D12PipelineState* g_edge[16][4] = {};  // stereo edge fix pipelines per upscaler resolve: normal, dev 2, dev 3
ID3DBlob* g_edgeCs[4] = {};
std::atomic<long> g_edgePasses{0};
std::atomic<int> g_nVel{0}, g_velFixes{0};

ID3DBlob* g_cs = nullptr;

// Tag on the game's pipeline objects we map: a pipeline address reused by an unrelated pipeline after the game
// released ours carries no tag and is ignored.
const GUID kTagGuid = {0x6f1e2a40, 0x5b7c, 0x4c1d, {0x9a, 0x31, 0x2e, 0x44, 0x80, 0x17, 0xc3, 0x5d}};
void Tag(ID3D12PipelineState* p, uint32_t kind) { p->SetPrivateData(kTagGuid, sizeof(kind), &kind); }
bool Tagged(ID3D12PipelineState* p, uint32_t kind) {
    uint32_t v = 0;
    UINT size = sizeof(v);
    return SUCCEEDED(p->GetPrivateData(kTagGuid, &size, &v)) && size == sizeof(v) && v == kind;
}
std::atomic<int> g_created{0}, g_swaps{0};
std::atomic<long> g_taaRuns{0};

struct Pair {
    ID3D12PipelineState* orig;
    ID3D12PipelineState* repl;
};
Pair g_pairs[16];
std::atomic<int> g_nPairs{0};
SRWLOCK g_lock = SRWLOCK_INIT;

bool IsTaa(const void* bytecode, SIZE_T len) {
    return bytecode && len > 20 && !memcmp(bytecode, "DXBC", 4) &&
           !memcmp(static_cast<const uint8_t*>(bytecode) + 4, kTaaDigest, 16);
}

// 0 = not a velocity resolve, 1 = TAA variant, 2 = upscaler variant
int VelKind(const void* bytecode, SIZE_T len) {
    if (!bytecode || len <= 20 || memcmp(bytecode, "DXBC", 4)) return 0;
    const uint8_t* d = static_cast<const uint8_t*>(bytecode) + 4;
    return !memcmp(d, kVelDigest, 16) ? 1 : !memcmp(d, kVelDigestUpscaler, 16) ? 2 : 0;
}
bool IsVel(const void* bytecode, SIZE_T len) { return VelKind(bytecode, len) != 0; }

ID3DBlob* Compile(const char* src, const char* name, const D3D_SHADER_MACRO* defines = nullptr) {
    using Compile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                       UINT, ID3DBlob**, ID3DBlob**);
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = m ? reinterpret_cast<Compile_t>(GetProcAddress(m, "D3DCompile")) : nullptr;
    if (!compile) return nullptr;
    ID3DBlob* out = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = compile(src, strlen(src), name, defines, nullptr, "main", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &err);
    if (FAILED(hr)) LOG("Shader %s failed to compile 0x%08lX %s", name, hr, err ? (const char*)err->GetBufferPointer() : "");
    if (err) err->Release();
    return out;
}

void NoteVelPso(ID3D12Device* dev, ID3D12RootSignature* rs, ID3D12PipelineState* orig, const char* how, int kind) {
    if (!orig || !rs) return;
    ID3DBlob*& cs = kind == 2 ? g_velCsUp : g_velCs;
    if (!cs) {
        const D3D_SHADER_MACRO up[] = {{"UPSCALER_VARIANT", "1"}, {nullptr, nullptr}};
        cs = Compile(kVelFixHlsl, kind == 2 ? "velocity_fix_upscaler" : "velocity_fix", kind == 2 ? up : nullptr);
    }
    if (!cs) return;
    D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
    d.pRootSignature = rs;
    d.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    ID3D12PipelineState* fix = nullptr;
    HRESULT hr = dev->CreateComputePipelineState(&d, IID_PPV_ARGS(&fix));
    if (FAILED(hr) || !fix) {
        LOG("Per-eye character motion: fix pipeline FAILED 0x%08lX (%s)", hr, how);
        return;
    }
    // A pipeline object's address can be reused after the game releases it (run 197: a TAA-path resolve and the later
    // upscaler-path resolve at the same address): replace an entry for the same pointer.
    AcquireSRWLockExclusive(&g_lock);
    const int n = g_nVel.load();
    int slot = -1;
    for (int i = 0; i < n; ++i)
        if (g_vel[i].orig == orig) slot = i;
    if (slot >= 0) {
        g_vel[slot].fix = fix;  // the old fix pipeline is leaked on purpose (it may still be in a command list)
    } else if (n < 16) {
        g_vel[n] = {orig, fix};
        g_nVel.store(n + 1);
    } else {
        fix->Release();
    }
    ReleaseSRWLockExclusive(&g_lock);
    Tag(orig, 2);
    ID3D12PipelineState* edge = nullptr;
    if (kind == 2) {  // StereoEdgeFix pipelines for the upscaler resolve
        ID3D12PipelineState* e3[4] = {};
        for (int m = 0; m < 4; ++m) {
            if (!g_edgeCs[m]) {
                const char* modes[4] = {"1", "2", "3", "4"};
                const D3D_SHADER_MACRO dm[] = {{"UPSCALER_VARIANT", "1"}, {"DISOCC", "1"}, {"EDGE_RANGE", "128"},
                                               {"EDGE_MODE", modes[m]}, {nullptr, nullptr}};
                g_edgeCs[m] = Compile(kVelFixHlsl, "stereo_edge_fix", dm);
            }
            if (!g_edgeCs[m]) continue;
            D3D12_COMPUTE_PIPELINE_STATE_DESC ed{};
            ed.pRootSignature = rs;
            ed.CS = {g_edgeCs[m]->GetBufferPointer(), g_edgeCs[m]->GetBufferSize()};
            if (FAILED(dev->CreateComputePipelineState(&ed, IID_PPV_ARGS(&e3[m])))) e3[m] = nullptr;
        }
        edge = e3[0];
        LOG("Stereo edge fix: pipelines %p %p %p %p", (void*)e3[0], (void*)e3[1], (void*)e3[2], (void*)e3[3]);
        AcquireSRWLockExclusive(&g_lock);
        for (int i = 0; i < g_nVel.load(); ++i)
            if (g_vel[i].orig == orig)
                for (int m = 0; m < 4; ++m) g_edge[i][m] = e3[m];
        ReleaseSRWLockExclusive(&g_lock);
    }
    LOG("Per-eye character motion: game velocity resolve pipeline %p (%s, %s variant) -> fix pass %p, edge pass %p", (void*)orig,
        how, kind == 2 ? "upscaler" : "TAA", (void*)fix, (void*)edge);
}

ID3D12PipelineState* VelFixFor(ID3D12PipelineState* p, ID3D12PipelineState** edge = nullptr) {
    const int n = g_nVel.load();
    for (int i = 0; i < n; ++i)
        if (g_vel[i].orig == p) {
            if (!Tagged(p, 2)) return nullptr;
            if (edge) {
                const int m = std::clamp(vr::g_config.stereoEdgeFix, 1, 4) - 1;
                *edge = g_edge[i][m];
            }
            return g_vel[i].fix;
        }
    return nullptr;
}

// Pipelines seen at creation: TAA (swap) or the velocity resolve (fix pass after it).
void NotePso(ID3D12Device* dev, ID3D12RootSignature* rs, ID3D12PipelineState* p, const void* bc, SIZE_T len, const char* how);

ID3DBlob* PassThroughCs() {
    if (g_cs) return g_cs;
    using Compile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                       UINT, ID3DBlob**, ID3DBlob**);
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = m ? reinterpret_cast<Compile_t>(GetProcAddress(m, "D3DCompile")) : nullptr;
    if (!compile) {
        LOG("TAA: d3dcompiler_47.dll not available");
        return nullptr;
    }
    ID3DBlob* err = nullptr;
    // TaaDebug (dev): the replacement tints the image magenta, so a capture-free test shows whether the swapped pass
    // reaches the screen
    const D3D_SHADER_MACRO dbg[] = {{"TAA_DEBUG", "1"}, {nullptr, nullptr}};
    HRESULT hr = compile(kPassThroughHlsl, strlen(kPassThroughHlsl), "taa_off", vr::g_config.taaDebug ? dbg : nullptr, nullptr, "main", "cs_5_1",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &g_cs, &err);
    if (FAILED(hr)) LOG("TAA: pass-through shader failed to compile 0x%08lX %s", hr, err ? (const char*)err->GetBufferPointer() : "");
    if (err) err->Release();
    return g_cs;
}

void NoteTaaPso(ID3D12Device* dev, ID3D12RootSignature* rs, ID3D12PipelineState* orig, const char* how) {
    if (!orig || !rs) return;
    ID3DBlob* cs = PassThroughCs();
    if (!cs) return;
    D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
    d.pRootSignature = rs;
    d.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    ID3D12PipelineState* repl = nullptr;
    // Creates through the device the game used; our own hooks see a non-TAA shader and pass it on.
    HRESULT hr = dev->CreateComputePipelineState(&d, IID_PPV_ARGS(&repl));
    if (FAILED(hr) || !repl) {
        LOG("TAA: pass-through pipeline FAILED 0x%08lX (%s)", hr, how);
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    const int n = g_nPairs.load();
    int slot = -1;
    for (int i = 0; i < n; ++i)
        if (g_pairs[i].orig == orig) slot = i;
    if (slot >= 0) {
        g_pairs[slot].repl = repl;
    } else if (n < 16) {
        g_pairs[n] = {orig, repl};
        g_nPairs.store(n + 1);
    } else {
        repl->Release();
    }
    ReleaseSRWLockExclusive(&g_lock);
    Tag(orig, 1);
    LOG("TAA: game TAA pipeline %p (%s) -> pass-through %p (DisableTAA=%d)", (void*)orig, how, (void*)repl,
        vr::g_config.disableTaa);
}

ID3D12PipelineState* ReplacementFor(ID3D12PipelineState* p) {
    const int n = g_nPairs.load();
    for (int i = 0; i < n; ++i)
        if (g_pairs[i].orig == p) return Tagged(p, 1) ? g_pairs[i].repl : nullptr;
    return nullptr;
}

// ---- per-vtable hook bookkeeping (Streamline's proxy device and the runtime device both reach us)
struct VtHooks {
    void** vt;
    void* orig[64];
};
VtHooks g_vts[12];
int g_nvt = 0;
SRWLOCK g_hookLock = SRWLOCK_INIT;

void* OrigFor(void* self, int slot) {
    void** vt = *reinterpret_cast<void***>(self);
    for (int i = 0; i < g_nvt; ++i)
        if (g_vts[i].vt == vt && g_vts[i].orig[slot]) return g_vts[i].orig[slot];
    for (int i = 0; i < g_nvt; ++i)
        if (g_vts[i].orig[slot]) return g_vts[i].orig[slot];  // vtable copied by another layer
    return nullptr;
}

bool Hook(void** vt, int slot, void* hook) {
    if (vt[slot] == hook) return true;
    AcquireSRWLockExclusive(&g_hookLock);
    VtHooks* h = nullptr;
    for (int i = 0; i < g_nvt; ++i)
        if (g_vts[i].vt == vt) h = &g_vts[i];
    if (!h && g_nvt < 12) {
        h = &g_vts[g_nvt++];
        h->vt = vt;
    }
    bool ok = false;
    DWORD old;
    if (h && vt[slot] != hook && VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        h->orig[slot] = vt[slot];
        vt[slot] = hook;
        VirtualProtect(&vt[slot], sizeof(void*), old, &old);
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_hookLock);
    return ok;
}

// ID3D12Device slots: 11 CreateComputePipelineState, 44 CreatePipelineLibrary (Device1), 47 CreatePipelineState (Device2).
// ID3D12PipelineLibrary slots: 10 LoadComputePipeline, 13 LoadPipeline (Library1).
// ID3D12GraphicsCommandList slot 25: SetPipelineState.
using CreateCompute_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
using CreateStream_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
using CreateLibrary_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const void*, SIZE_T, REFIID, void**);
using LoadCompute_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary*, LPCWSTR, const D3D12_COMPUTE_PIPELINE_STATE_DESC*,
                                                  REFIID, void**);
using LoadPipeline_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary*, LPCWSTR, const D3D12_PIPELINE_STATE_STREAM_DESC*,
                                                   REFIID, void**);
using SetPso_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);

ID3D12PipelineState* AsPso(void** out) {
    if (!out || !*out) return nullptr;
    ID3D12PipelineState* p = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&p))) && p) p->Release();  // keep it weak
    return p;
}

HRESULT STDMETHODCALLTYPE Hk_CreateCompute(ID3D12Device* dev, const D3D12_COMPUTE_PIPELINE_STATE_DESC* d, REFIID riid, void** out) {
    const HRESULT hr = reinterpret_cast<CreateCompute_t>(OrigFor(dev, 11))(dev, d, riid, out);
    ++g_created;
    if (SUCCEEDED(hr) && d) NotePso(dev, d->pRootSignature, AsPso(out), d->CS.pShaderBytecode, d->CS.BytecodeLength, "CreateComputePipelineState");
    return hr;
}

// Pipeline state streams: find the CS and root signature subobjects.
bool ParseStream(const D3D12_PIPELINE_STATE_STREAM_DESC* d, const D3D12_SHADER_BYTECODE** cs, ID3D12RootSignature** rs) {
    *cs = nullptr;
    *rs = nullptr;
    if (!d || !d->pPipelineStateSubobjectStream) return false;
    auto* p = static_cast<const uint8_t*>(d->pPipelineStateSubobjectStream);
    const uint8_t* end = p + d->SizeInBytes;
    while (p + sizeof(UINT) <= end) {
        // Each subobject: { type (4 bytes), payload at its natural alignment }, padded to pointer size.
        const UINT type = *reinterpret_cast<const UINT*>(p);
        size_t off, size;
        switch (type) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
                off = 8, size = sizeof(void*);
                *rs = *reinterpret_cast<ID3D12RootSignature* const*>(p + off);
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
                off = 8, size = sizeof(D3D12_SHADER_BYTECODE);
                *cs = reinterpret_cast<const D3D12_SHADER_BYTECODE*>(p + off);
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: off = 4, size = sizeof(UINT); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: off = 4, size = sizeof(UINT); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: off = 8, size = sizeof(D3D12_CACHED_PIPELINE_STATE); break;
            default: return *cs && *rs;  // graphics or unknown subobjects: not a plain compute stream
        }
        p += (off + size + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
    }
    return *cs && *rs;
}

HRESULT STDMETHODCALLTYPE Hk_CreateStream(ID3D12Device* dev, const D3D12_PIPELINE_STATE_STREAM_DESC* d, REFIID riid, void** out) {
    const HRESULT hr = reinterpret_cast<CreateStream_t>(OrigFor(dev, 47))(dev, d, riid, out);
    const D3D12_SHADER_BYTECODE* cs;
    ID3D12RootSignature* rs;
    if (SUCCEEDED(hr) && ParseStream(d, &cs, &rs)) NotePso(dev, rs, AsPso(out), cs->pShaderBytecode, cs->BytecodeLength, "CreatePipelineState");
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_LoadCompute(ID3D12PipelineLibrary* lib, LPCWSTR name, const D3D12_COMPUTE_PIPELINE_STATE_DESC* d,
                                         REFIID riid, void** out) {
    const HRESULT hr = reinterpret_cast<LoadCompute_t>(OrigFor(lib, 10))(lib, name, d, riid, out);
    if (SUCCEEDED(hr) && d && (IsTaa(d->CS.pShaderBytecode, d->CS.BytecodeLength) || IsVel(d->CS.pShaderBytecode, d->CS.BytecodeLength))) {
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(lib->GetDevice(IID_PPV_ARGS(&dev))) && dev) {
            NotePso(dev, d->pRootSignature, AsPso(out), d->CS.pShaderBytecode, d->CS.BytecodeLength, "pipeline library");
            dev->Release();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_LoadPipeline(ID3D12PipelineLibrary* lib, LPCWSTR name, const D3D12_PIPELINE_STATE_STREAM_DESC* d,
                                          REFIID riid, void** out) {
    const HRESULT hr = reinterpret_cast<LoadPipeline_t>(OrigFor(lib, 13))(lib, name, d, riid, out);
    const D3D12_SHADER_BYTECODE* cs;
    ID3D12RootSignature* rs;
    if (SUCCEEDED(hr) && ParseStream(d, &cs, &rs) && (IsTaa(cs->pShaderBytecode, cs->BytecodeLength) || IsVel(cs->pShaderBytecode, cs->BytecodeLength))) {
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(lib->GetDevice(IID_PPV_ARGS(&dev))) && dev) {
            NotePso(dev, rs, AsPso(out), cs->pShaderBytecode, cs->BytecodeLength, "pipeline library stream");
            dev->Release();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hk_CreateLibrary(ID3D12Device* dev, const void* blob, SIZE_T len, REFIID riid, void** out) {
    const HRESULT hr = reinterpret_cast<CreateLibrary_t>(OrigFor(dev, 44))(dev, blob, len, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        void** lvt = *reinterpret_cast<void***>(*out);
        Hook(lvt, 10, (void*)&Hk_LoadCompute);
        ID3D12PipelineLibrary1* l1 = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&l1))) && l1) {
            Hook(*reinterpret_cast<void***>(l1), 13, (void*)&Hk_LoadPipeline);
            l1->Release();
        }
    }
    return hr;
}

void NotePso(ID3D12Device* dev, ID3D12RootSignature* rs, ID3D12PipelineState* p, const void* bc, SIZE_T len, const char* how) {
    if (IsTaa(bc, len)) NoteTaaPso(dev, rs, p, how);
    else if (const int k = VelKind(bc, len)) NoteVelPso(dev, rs, p, how, k);
}

// current pipeline per command list (recording is single-threaded per list)
thread_local ID3D12GraphicsCommandList* t_cl = nullptr;
thread_local ID3D12PipelineState* t_pso = nullptr;
using Dispatch_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
struct ExtraHook { int slot; void* fn; };
ExtraHook g_extra[16];
int g_nExtra = 0;

void STDMETHODCALLTYPE Hk_Dispatch(ID3D12GraphicsCommandList* cl, UINT x, UINT y, UINT z) {
    auto dispatch = reinterpret_cast<Dispatch_t>(OrigFor(cl, 14));
    dispatch(cl, x, y, z);
    const bool objMotion = vr::g_config.perEyeObjectMotion && !mvfix::Wanted();  // MotionRebuild handles objects itself
    // the in-resolve variant of the edge fix had no effect (the upscalers read other motion vectors): see stereo_edge.cpp
    const bool edgeFix = false;
    if (t_cl != cl || !t_pso || !(objMotion || edgeFix) || !g_nVel.load()) return;
    ID3D12PipelineState* edge = nullptr;
    ID3D12PipelineState* fix = VelFixFor(t_pso, &edge);
    if (edgeFix) fix = edge;  // the shader itself does nothing unless the previous frame is the other eye
    if (!fix) return;
    if (edgeFix && g_edgePasses++ == 0) LOG("Stereo edge fix: first pass (%u x %u groups)", x, y);
    // the fix reads and writes the resolve's output: UAV barriers around it; same root signature, so the game's
    // bindings (b0 camera, b1 frame, t0 / t1, u0) stay in place
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    cl->ResourceBarrier(1, &b);
    auto setPso = reinterpret_cast<SetPso_t>(OrigFor(cl, 25));
    setPso(cl, fix);
    dispatch(cl, x, y, z);
    cl->ResourceBarrier(1, &b);
    setPso(cl, t_pso);
    if (g_velFixes++ == 0) LOG("Per-eye character motion: first fix pass (%u x %u groups)", x, y);
}

void STDMETHODCALLTYPE Hk_SetPso(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* p) {
    t_cl = cl;
    t_pso = p;
    if (p && g_nPairs.load()) {
        if (ID3D12PipelineState* r = ReplacementFor(p)) {
            ++g_taaRuns;  // the game's TAA is about to run (with DLSS / FSR it should not)
            if (vr::g_config.disableTaa) {
                p = r;
                ++g_swaps;
            }
        }
    }
    reinterpret_cast<SetPso_t>(OrigFor(cl, 25))(cl, p);
}

struct ExtraDev { int slot; void* fn; };
ExtraDev g_extraDev[8];
int g_nExtraDev = 0;

void HookDevice(ID3D12Device* dev) {
    void** dvt = *reinterpret_cast<void***>(dev);
    Hook(dvt, 11, (void*)&Hk_CreateCompute);
    for (int i = 0; i < g_nExtraDev; ++i) Hook(dvt, g_extraDev[i].slot, g_extraDev[i].fn);
    ID3D12Device1* d1 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d1))) && d1) {
        Hook(*reinterpret_cast<void***>(d1), 44, (void*)&Hk_CreateLibrary);
        d1->Release();
    }
    ID3D12Device2* d2 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d2))) && d2) {
        Hook(*reinterpret_cast<void***>(d2), 47, (void*)&Hk_CreateStream);
        d2->Release();
    }
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    for (auto type : {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE}) {
        if (SUCCEEDED(dev->CreateCommandAllocator(type, IID_PPV_ARGS(&alloc))) &&
            SUCCEEDED(dev->CreateCommandList(0, type, alloc, nullptr, IID_PPV_ARGS(&list)))) {
            Hook(*reinterpret_cast<void***>(list), 25, (void*)&Hk_SetPso);
            Hook(*reinterpret_cast<void***>(list), 14, (void*)&Hk_Dispatch);
            for (int i = 0; i < g_nExtra; ++i) Hook(*reinterpret_cast<void***>(list), g_extra[i].slot, g_extra[i].fn);
            list->Close();
        }
        if (list) list->Release();
        if (alloc) alloc->Release();
        list = nullptr;
        alloc = nullptr;
    }
    LOG("TAA: hooks on device %p (vtable %p)", (void*)dev, (void*)dvt);
}

bool g_installed = false;

}  // namespace

void* OrigListFn(void* self, int slot) { return OrigFor(self, slot); }

ID3D12PipelineState* CurrentPso(void* cl) { return t_cl == cl ? t_pso : nullptr; }

void HookDeviceSlot(int slot, void* fn) {
    if (g_nExtraDev < 8) g_extraDev[g_nExtraDev++] = {slot, fn};
}

void HookListSlot(int slot, void* fn) {
    if (g_nExtra < 16) g_extra[g_nExtra++] = {slot, fn};
    AcquireSRWLockShared(&g_hookLock);
    void** vts[12];
    const int n = g_nvt;
    for (int i = 0; i < n; ++i) vts[i] = g_vts[i].vt;
    ReleaseSRWLockShared(&g_hookLock);
    // vtables already seen that are command lists (they have slot 14 hooked)
    for (int i = 0; i < n; ++i)
        if (vts[i][14] == (void*)&Hk_Dispatch) Hook(vts[i], slot, fn);
}

void OnDeviceCreated(ID3D12Device* dev) {
    if (!dev || g_installed) return;
    g_installed = true;
    HookDevice(dev);
    // The runtime device behind Streamline's proxy (resources report it as their device).
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 256;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buf = nullptr;
    if (SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&buf)))) {
        ID3D12Device* inner = nullptr;
        if (SUCCEEDED(buf->GetDevice(IID_PPV_ARGS(&inner))) && inner) {
            if (*reinterpret_cast<void***>(inner) != *reinterpret_cast<void***>(dev)) HookDevice(inner);
            inner->Release();
        }
        buf->Release();
    }
    PassThroughCs();
}

const char* Status() {
    static char buf[192];
    snprintf(buf, sizeof(buf), "TAA %s (%d pipelines, ran %ld times, %d swaps), character motion fix %d passes, stereo edge fix %ld passes",
             vr::g_config.disableTaa ? "off" : "on", g_nPairs.load(), g_taaRuns.load(), g_swaps.load(), g_velFixes.load(),
             g_edgePasses.load());
    return buf;
}

}  // namespace taa
