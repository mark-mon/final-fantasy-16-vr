// Frame warp (VRMode Warp): one eye drawn per game tick, the other rebuilt from depth.
//
// The method is Rift Apart VR's "Clank's Space & Frame Warp, Depth Based" (MIT, scubasteve2365); its shader is in
// warp_rebuild.inc. Each tick:
//   - At the upscaler (DLSS / FSR), the drawn eye's depth and motion vectors (the rendered extent) are copied into
//     textures of our own (stereo_edge::Capture, on the game's command list).
//   - At the present (xr_screen.cpp), the drawn eye's image, depth, motion vectors and camera become that eye's held
//     state; the other eye is rebuilt for its camera of this tick: first from its own held image (a tick old), moved by
//     its depth; where that has nothing, from the drawn eye through its depth. The result goes to the other eye's slice.
// Cameras come from the game's camera constants (cam_prev): camera-to-world rows 11..13 (columns: right, up, back,
// position), projection c[7].x / c[8].y, near c[9].w; depth is near / view distance.
//
// Lab (WarpLab, in Native stereo): the right eye is rebuilt from the left one and shown instead of the real right eye,
// with the real right camera, to check the rebuild against what the game draws.
#include "warp.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

#include "cam_prev.h"
#include "eye_history.h"
#include "mvfix.h"
#include "config.h"
#include "log.h"
#include "stereo_edge.h"
#include "stereo_render.h"
#include "vr_shared.h"

namespace warp {
namespace {

#include "warp_rebuild.inc"

struct RebuildConstants {
    float dRight[4], dUp[4], dBack[4], dPos[4];
    float sRight[4], sUp[4], sBack[4], sPos[4];
    float hRight[4], hUp[4], hBack[4];
    float dTan[4], sTan[4], hTan[4];
    float misc[4];
    float pRight[4], pUp[4], pBack[4], pTan[4];
    float motion[4];
    float fill[4];
    float band[4];
    float objm[4];
    float objmv[4];
};
static_assert(sizeof(RebuildConstants) == 96 * 4, "constants");

ID3D12Device* g_dev = nullptr;
ID3D12CommandQueue* g_queue = nullptr;
std::mutex g_mutex;
bool g_failed = false;
ID3D12RootSignature* g_root = nullptr;
ID3D12PipelineState *g_clear = nullptr, *g_splat = nullptr, *g_splatOwn = nullptr, *g_splatOwnMove = nullptr, *g_resolve = nullptr;
ID3D12DescriptorHeap* g_heap = nullptr;
UINT g_inc = 0;
constexpr UINT kTable = 11;
ID3D12Resource* g_cb = nullptr;
uint8_t* g_cbMapped = nullptr;
constexpr UINT kCbSlot = 512, kCbSlots = 16;
UINT g_cbNext = 0;
ID3D12Resource* g_stats = nullptr;
ID3D12CommandAllocator* g_alloc[3] = {};
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12Fence* g_fence = nullptr;
UINT64 g_fenceValue = 0, g_allocFence[3] = {};
HANDLE g_event = nullptr;
int g_frame = 0;

// Per eye: depth and motion vectors of its last render (captured at the upscaler) and as held with its image.
UINT g_w = 0, g_h = 0;  // rendered extent (render resolution)
// Captures are a ring per eye: the engine presents an image one or more frames after rendering it, so the depth and
// camera that belong to the image being presented are the k-th capture of that eye for its k-th present, not the latest.
constexpr int kCaps = 3;
ID3D12Resource *g_depth[2][kCaps] = {}, *g_mv[2][kCaps] = {}, *g_obj[2][kCaps] = {}, *g_heldDepth[2] = {}, *g_heldMv[2] = {}, *g_heldObj[2] = {};
bool g_heldObjValid[2] = {};
bool g_heldValid[2] = {};
struct Eye;
UINT g_capSeq[2] = {}, g_presSeq[2] = {};  // captures so far / presents so far (1-based number of the current one)
int Cur(int eye) { return (int)((g_presSeq[eye] + kCaps - 1) % kCaps); }
ID3D12Resource *g_map = nullptr, *g_ownMap = nullptr, *g_ownMove = nullptr;
ID3D12Resource* g_out = nullptr;
UINT64 g_outW = 0;
UINT g_outH = 0;
DXGI_FORMAT g_colorFmt = DXGI_FORMAT_UNKNOWN;

struct Eye {
    float right[3], up[3], back[3], pos[3];
    float tan[4];  // left, right, up, down
    float nearZ;
    bool valid = false;
};
Eye g_held[2], g_heldPrev[2];
float g_heldMvUv[2][2] = {};
struct Cap {
    Eye cam, prev;
    uint8_t sl[464] = {};  // dev: the sl::Constants of the evaluate (dumps)
    bool haveSl = false;
    float mvUv[2];  // motion vector units -> image units (uv)
    uint32_t ord = 0;     // this eye's captures so far at this one
    bool valid = false;
    bool haveObj = false;  // g_obj holds this pass's object motion (mvfix)
};
// Camera-update serial of a capture: the k-th capture of an eye is its k-th render, which used the k-th camera update,
// so serial = base + ord. The game thread may already have made the next update when a capture happens, so the
// latest serial seen at capture time is this render's or the next one's; the base is calibrated to the smallest
// difference seen (0 when the game thread was level at least once, otherwise 1 behind the latest).
uint32_t g_capOrd[2] = {}, g_serialBase[2] = {};
int g_serialMinD[2] = {1 << 30, 1 << 30};
uint32_t CapSerial(int eye, const Cap& c) { return g_serialBase[eye] + c.ord + (uint32_t)g_serialMinD[eye]; }
Cap g_cap[2][kCaps];
const Cap& CurCap(int eye) { return g_cap[eye][Cur(eye)]; }
std::atomic<long> g_rebuilds{0}, g_captures{0};
std::atomic<int> g_backSign{1};  // dev: +1 column 2 of the camera is "back", -1 it is "forward"
std::atomic<bool> g_motion{false};
std::atomic<bool> g_objects{true};     // moving things from the drawn eye, found by its motion vectors (WarpObjects)
std::atomic<int> g_objThrTenths{15};   // their threshold, tenths of a render pixel (dev "objthr")
std::atomic<int> g_mvSign{-1};  // FF16 motion vectors point away from the previous image (run 259: -1 cancels the still world); dev +1
std::atomic<int> g_view{0};
std::atomic<int> g_prevMode{0};
std::atomic<bool> g_pairBySerial{false};
std::atomic<int> g_order{3};  // source order (Rebuild); 3 catches moving things by depth alone (no motion vectors)
bool g_selfCheck = false;  // lab views 5 / 6 (under g_mutex)


ID3D12Resource* Tex(UINT64 w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st, const wchar_t* name) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    ID3D12Resource* r = nullptr;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    r->SetName(name);
    return r;
}

void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    l->ResourceBarrier(1, &x);
}

bool Pipelines() {
    if (g_root) return true;
    if (g_failed || !g_dev) return false;
    g_failed = true;
    using Compile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT,
                                       UINT, ID3DBlob**, ID3DBlob**);
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = m ? reinterpret_cast<Compile_t>(GetProcAddress(m, "D3DCompile")) : nullptr;
    if (!compile) return false;
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 0, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 5, 0, 0, 6};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {2, ranges};
    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rsd{2, params, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob *blob = nullptr, *err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(g_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_root)))) {
        LOG("Frame warp: root signature failed");
        g_root = nullptr;
        return false;
    }
    const struct { const char* entry; ID3D12PipelineState** out; } stages[5] = {
        {"Clear", &g_clear}, {"Splat", &g_splat}, {"SplatOwn", &g_splatOwn}, {"SplatOwnMove", &g_splatOwnMove}, {"Resolve", &g_resolve}};
    for (const auto& st : stages) {
        ID3DBlob *cs = nullptr, *e = nullptr;
        if (FAILED(compile(kRebuildHlsl, sizeof(kRebuildHlsl) - 1, "warp_rebuild", nullptr, nullptr, st.entry, "cs_5_1", 0, 0, &cs, &e))) {
            LOG("Frame warp: shader %s failed: %s", st.entry, e ? (const char*)e->GetBufferPointer() : "?");
            g_root = nullptr;
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_root;
        pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        if (FAILED(g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(st.out)))) {
            LOG("Frame warp: pipeline %s failed", st.entry);
            g_root = nullptr;
            return false;
        }
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2 * kTable, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap)))) { g_root = nullptr; return false; }
    g_inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD}, dh{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (UINT64)kCbSlot * kCbSlots;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_cb))) ||
        FAILED(g_cb->Map(0, nullptr, reinterpret_cast<void**>(&g_cbMapped)))) { g_root = nullptr; return false; }
    bd.Width = 32 * 12;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(g_dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_stats)))) {
        g_root = nullptr;
        return false;
    }
    for (auto& a : g_alloc)
        if (FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) { g_root = nullptr; return false; }
    if (FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, IID_PPV_ARGS(&g_list)))) { g_root = nullptr; return false; }
    g_list->Close();
    if (FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) { g_root = nullptr; return false; }
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_failed = false;
    LOG("Frame warp: rebuild pipelines ready");
    return true;
}

// Per-size textures (the rendered extent).
bool EnsureExtent(UINT w, UINT h) {
    if (g_w == w && g_h == h && g_map) return true;
    // a resolution change: start over (old textures may still be in flight: kept, a few MB, rare)
    g_w = w;
    g_h = h;
    const auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto kUav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    bool texOk = true;
    for (int e = 0; e < 2; ++e) {
        for (int k = 0; k < kCaps; ++k) {
            g_depth[e][k] = Tex(w, h, DXGI_FORMAT_R32_FLOAT, kUav, kRead, L"ff16vr warp depth");
            g_mv[e][k] = Tex(w, h, DXGI_FORMAT_R16G16_FLOAT, kUav, kRead, L"ff16vr warp motion");
            g_obj[e][k] = Tex(w, h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, kRead, L"ff16vr warp object motion");
            g_cap[e][k].valid = false;
            g_cap[e][k].haveObj = false;
            texOk = texOk && g_depth[e][k] && g_mv[e][k];
        }
        g_capSeq[e] = g_presSeq[e] = g_capOrd[e] = 0;
        g_serialMinD[e] = 1 << 30;
        g_heldDepth[e] = Tex(w, h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, kRead, L"ff16vr warp held depth");
        g_heldMv[e] = Tex(w, h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, kRead, L"ff16vr warp held motion");
        g_heldObj[e] = Tex(w, h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, kRead, L"ff16vr warp held object motion");
        g_heldObjValid[e] = false;
        g_heldValid[e] = false;
        g_held[e].valid = g_heldPrev[e].valid = false;
    }
    g_map = Tex(w, h, DXGI_FORMAT_R32_UINT, kUav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"ff16vr warp map");
    g_ownMap = Tex(w, h, DXGI_FORMAT_R32_UINT, kUav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"ff16vr warp own map");
    g_ownMove = Tex(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, kUav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"ff16vr warp own move");
    const bool ok = texOk && g_heldDepth[0] && g_heldDepth[1] && g_heldMv[0] && g_heldMv[1] && g_map && g_ownMap && g_ownMove;
    LOG("Frame warp: depth maps %u x %u%s", w, h, ok ? "" : " FAILED");
    if (!ok) g_map = nullptr;
    return ok;
}

void Rb(float* o3, const float* v) { o3[0] = v[0], o3[1] = v[1], o3[2] = v[2], o3[3] = 0; }
void PutEye(float* r, float* u, float* b, float* p, float* t, const Eye& e) {
    Rb(r, e.right);
    Rb(u, e.up);
    Rb(b, e.back);
    if (p) Rb(p, e.pos);
    memcpy(t, e.tan, 16);
}

void Descriptors(int table, ID3D12Resource* srcColor, ID3D12Resource* histColor, int source, int dest) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)table * kTable * g_inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    sd.Format = g_colorFmt;
    g_dev->CreateShaderResourceView(srcColor, &sd, h);
    h.ptr += g_inc;
    g_dev->CreateShaderResourceView(histColor, &sd, h);
    h.ptr += g_inc;
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    g_dev->CreateShaderResourceView(g_depth[source][Cur(source)], &sd, h);
    h.ptr += g_inc;
    g_dev->CreateShaderResourceView(g_selfCheck ? g_depth[source][Cur(source)] : g_heldDepth[dest], &sd, h);
    h.ptr += g_inc;
    sd.Format = DXGI_FORMAT_R16G16_FLOAT;
    const bool heldObj = !g_selfCheck && !g_motion.load() && g_objects.load() && g_heldObjValid[dest] && g_heldObj[dest];
    g_dev->CreateShaderResourceView(g_selfCheck ? g_mv[source][Cur(source)] : heldObj ? g_heldObj[dest] : g_heldMv[dest], &sd, h);
    h.ptr += g_inc;
    g_dev->CreateShaderResourceView(CurCap(source).haveObj ? g_obj[source][Cur(source)] : g_mv[source][Cur(source)], &sd, h);
    h.ptr += g_inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    ud.Format = DXGI_FORMAT_R32_UINT;
    g_dev->CreateUnorderedAccessView(g_map, nullptr, &ud, h);
    h.ptr += g_inc;
    ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    g_dev->CreateUnorderedAccessView(g_out, nullptr, &ud, h);
    h.ptr += g_inc;
    ud.Format = DXGI_FORMAT_R32_UINT;
    g_dev->CreateUnorderedAccessView(g_ownMap, nullptr, &ud, h);
    h.ptr += g_inc;
    ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    g_dev->CreateUnorderedAccessView(g_ownMove, nullptr, &ud, h);
    h.ptr += g_inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC bu{};
    bu.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    bu.Format = DXGI_FORMAT_R32_TYPELESS;
    bu.Buffer.NumElements = 32 * 3;
    bu.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    g_dev->CreateUnorderedAccessView(g_stats, nullptr, &bu, h);
}

// Lab dump (trigger "dump 1"): the drawn eye's depth and motion vectors and its camera constants, written to the game
// folder (ff16vr_dump_*) for offline checks of the motion vector convention.
std::atomic<int> g_dumpReq{0};  // 1 + eye to dump (0 none)
int g_dumpCount = 0;
int g_dumpState = 0;  // 1 copies recorded, 2 submitted (waiting for g_dumpFence)
UINT64 g_dumpFence = 0;
ID3D12Resource* g_dumpBuf = nullptr;
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_dumpFp[2] = {};
float g_dumpRows[2][cam_prev::kRows][4];
int g_dumpEye = 0;

void DumpRecord(ID3D12GraphicsCommandList* l, int source) {
    ID3D12Resource *dTex = g_depth[source][Cur(source)], *mTex = g_mv[source][Cur(source)];
    D3D12_RESOURCE_DESC dd = dTex->GetDesc(), md = mTex->GetDesc();
    UINT64 sz0 = 0, sz1 = 0;
    g_dev->GetCopyableFootprints(&dd, 0, 1, 0, &g_dumpFp[0], nullptr, nullptr, &sz0);
    g_dev->GetCopyableFootprints(&md, 0, 1, 0, &g_dumpFp[1], nullptr, nullptr, &sz1);
    g_dumpFp[1].Offset = (sz0 + 65535) & ~65535ull;
    if (!g_dumpBuf) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = g_dumpFp[1].Offset + sz1;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_dumpBuf)))) return;
    }
    if (!cam_prev::EyeCamera(source, g_dumpRows[0], g_dumpRows[1])) return;  // live rows (the camera used is CurCap)
    const auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* srcs[2] = {dTex, mTex};
    for (int i = 0; i < 2; ++i) {
        Barrier(l, srcs[i], kRead, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{g_dumpBuf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, from{srcs[i], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.PlacedFootprint = g_dumpFp[i];
        from.SubresourceIndex = 0;
        l->CopyTextureRegion(&dst, 0, 0, 0, &from, nullptr);
        Barrier(l, srcs[i], D3D12_RESOURCE_STATE_COPY_SOURCE, kRead);
    }
    g_dumpEye = source;
    g_dumpState = 1;
}

void DumpWrite() {
    uint8_t* p = nullptr;
    if (FAILED(g_dumpBuf->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return;
    const int k = ++g_dumpCount;
    const std::wstring base = vr::g_gameDir + L"\\ff16vr_dump" + std::to_wstring(k);
    const std::wstring names[2] = {base + L"_depth.bin", base + L"_mv.bin"};
    for (int i = 0; i < 2; ++i) {
        const auto& fp = g_dumpFp[i];
        const UINT rowBytes = fp.Footprint.Width * (i == 0 ? 4 : 4);
        if (FILE* f = _wfopen(names[i].c_str(), L"wb")) {
            for (UINT y = 0; y < fp.Footprint.Height; ++y) fwrite(p + fp.Offset + (size_t)y * fp.Footprint.RowPitch, 1, rowBytes, f);
            fclose(f);
        }
    }
    g_dumpBuf->Unmap(0, nullptr);
    if (FILE* f = _wfopen((base + L"_cam.txt").c_str(), L"w")) {
        fprintf(f, "eye %d size %u %u mvuv %.6f %.6f\n", g_dumpEye, g_dumpFp[0].Footprint.Width, g_dumpFp[0].Footprint.Height,
                CurCap(g_dumpEye).mvUv[0], CurCap(g_dumpEye).mvUv[1]);
        for (int b = 0; b < 2; ++b)
            for (int r = 0; r < cam_prev::kRows; ++r)
                fprintf(f, "%s %d %.9g %.9g %.9g %.9g\n", b ? "prev" : "cur", r, g_dumpRows[b][r][0], g_dumpRows[b][r][1],
                        g_dumpRows[b][r][2], g_dumpRows[b][r][3]);
        const Cap& cc = CurCap(g_dumpEye);
        fprintf(f, "cap %d %.9g %.9g %.9g %.9g\n", cc.haveSl ? 1 : 0, cc.cam.pos[0], cc.cam.pos[1], cc.cam.pos[2], cc.cam.nearZ);
        for (int i = 0; i < 29; ++i) {  // the sl::Constants as float4 rows (byte offset 16 i)
            const float* v = reinterpret_cast<const float*>(cc.sl + 16 * i);
            fprintf(f, "sl %d %.9g %.9g %.9g %.9g\n", i, v[0], v[1], v[2], v[3]);
        }
        fclose(f);
    }
    LOG("Frame warp: dump %d written (eye %d, %u x %u)", k, g_dumpEye, g_dumpFp[0].Footprint.Width, g_dumpFp[0].Footprint.Height);
}  // dev: where the held image's previous camera comes from (see Rebuild)    // dev: 1 tints the rebuilt eye by source (green own image, cyan / yellow drawn eye, yellow = moving)

void FillEye(float c[cam_prev::kRows][4], Eye* e);
// The camera of the pass with this upscaler jitter (the game finalises cameras ahead of the render thread: the newest
// one can be the next pass's, which under fast head turns reprojects the drawn eye from a wrong camera).
bool CameraOfPass(int eye, const stereo_edge::Inputs& in, Eye* out, Eye* prev) {
    float last[cam_prev::kRows][4], used[cam_prev::kRows][4];
    int back = 0;
    if (!in.haveJitter || !cam_prev::FindCameraByJitter(eye, in.jitter[0], in.jitter[1], in.w, in.h, last, used, &back)) return false;
    FillEye(last, out);
    if (prev) FillEye(used, prev);
    return out->valid;
}
bool CameraOf(int eye, Eye* out, Eye* prev) {
    float last[cam_prev::kRows][4], before[cam_prev::kRows][4];
    if (!cam_prev::EyeCamera(eye, last, before)) return false;
    FillEye(last, out);
    if (prev) FillEye(before, prev);
    return out->valid;
}
void FillEye(float c[cam_prev::kRows][4], Eye* e) {
    {
        const float s = (float)g_backSign.load();
        for (int i = 0; i < 3; ++i) {
            e->right[i] = c[11 + i][0];
            e->up[i] = c[11 + i][1];
            e->back[i] = s * c[11 + i][2];
            e->pos[i] = c[11 + i][3];
        }
        const float tx = c[7][0] != 0 ? 1.f / c[7][0] : 1.f, ty = c[8][1] != 0 ? 1.f / c[8][1] : 1.f;
        e->tan[0] = -fabsf(tx);
        e->tan[1] = fabsf(tx);
        e->tan[2] = fabsf(ty);
        e->tan[3] = -fabsf(ty);
        e->nearZ = c[9][3];
        e->valid = e->nearZ > 0;
    }
}

}  // namespace

void Init(ID3D12Device* dev, ID3D12CommandQueue* queue) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_dev) return;
    g_dev = dev;
    g_queue = queue;
    g_backSign = vr::g_config.warpBackSign < 0 ? -1 : 1;
    g_motion = vr::g_config.warpMotion;
    g_objects = vr::g_config.warpObjects;
}

bool Wanted() {
    return stereo_render::WarpActive() || ((vr::g_config.warpLab || vr::g_config.devCapture) && stereo_render::NativeActive());
}

// Dev: write the current capture of `eye` (depth, motion vectors, camera) on a list of our own (no rebuild needed).
void DumpNow(int eye) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_dev || eye < 0 || eye > 1 || !Pipelines() || !g_map || !CurCap(eye).valid) return;
    if (g_dumpState == 2) {  // a dump in flight: wait for it (small readback)
        if (g_fence->GetCompletedValue() < g_dumpFence) {
            g_fence->SetEventOnCompletion(g_dumpFence, g_event);
            WaitForSingleObject(g_event, 200);
        }
        if (g_fence->GetCompletedValue() >= g_dumpFence) {
            DumpWrite();
            g_dumpState = 0;
        }
    }
    if (g_dumpState != 0) return;
    const int f = g_frame;
    g_frame = (g_frame + 1) % 3;
    if (g_fence->GetCompletedValue() < g_allocFence[f]) {
        g_fence->SetEventOnCompletion(g_allocFence[f], g_event);
        WaitForSingleObject(g_event, 1000);
    }
    g_alloc[f]->Reset();
    g_list->Reset(g_alloc[f], nullptr);
    DumpRecord(g_list, eye);
    g_list->Close();
    ID3D12CommandList* lists[] = {g_list};
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_fence, ++g_fenceValue);
    g_allocFence[f] = g_fenceValue;
    if (g_dumpState == 1) {
        g_dumpFence = g_fenceValue;
        g_dumpState = 2;
    }
}

void OnUpscalerInputs(ID3D12GraphicsCommandList* cl, const stereo_edge::Inputs& in, int eye) {
    if (!Wanted() || eye < 0 || eye > 1 || !g_dev) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!EnsureExtent((UINT)in.w, (UINT)in.h)) return;
    const int slot = (int)(g_capSeq[eye] % kCaps);
    if (!stereo_edge::Capture(cl, in, g_depth[eye][slot], g_mv[eye][slot])) return;
    Cap& cap = g_cap[eye][slot];
    cap.haveObj = false;
    {   // the object part of this eye's rebuilt vectors (mvfix), when the rebuild ran for this pass
        UINT ow = 0, oh = 0;
        ID3D12Resource* obj = mvfix::LastObject(eye, &ow, &oh);
        if (obj && g_obj[eye][slot] && ow == (UINT)in.w && oh == (UINT)in.h) {
            eye_history::Suspend noHistory;  // a same-description copy: not one of the game's history copies
            const auto kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            Barrier(cl, obj, kSrv, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cl, g_obj[eye][slot], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            cl->CopyResource(g_obj[eye][slot], obj);
            Barrier(cl, obj, D3D12_RESOURCE_STATE_COPY_SOURCE, kSrv);
            Barrier(cl, g_obj[eye][slot], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cap.haveObj = true;
        }
    }
    cap.valid = CameraOfPass(eye, in, &cap.cam, &cap.prev) || CameraOf(eye, &cap.cam, &cap.prev);  // this pass's camera
    memcpy(cap.sl, in.sl, sizeof(cap.sl));
    cap.haveSl = in.haveSl;
    cap.mvUv[0] = in.pixPerMvX / (float)in.w;
    cap.mvUv[1] = in.pixPerMvY / (float)in.h;
    cap.ord = g_capOrd[eye]++;
    {
        const uint32_t latest = vrs::LatestSerial(eye);
        if (cap.ord == 0) g_serialBase[eye] = latest;
        const int d = (int)(latest - (g_serialBase[eye] + cap.ord));
        if (d < g_serialMinD[eye]) g_serialMinD[eye] = d;
    }
    ++g_capSeq[eye];
    if (g_captures++ == 0)
        LOG("Frame warp: first capture (eye %d, %d x %d at %d,%d, %.1f px per motion unit)", eye, in.w, in.h, in.x, in.y, in.pixPerMvX);
}

ID3D12Resource* Rebuild(int source, ID3D12Resource* srcColor, ID3D12Resource* histColor, bool haveHist, DXGI_FORMAT colorFmt,
                        const float eyeOffset[3], bool useRealDest) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_dev || !g_queue || source < 0 || source > 1 || !srcColor || !Pipelines() || !g_map || !CurCap(source).valid) return nullptr;
    const int dest = source ^ 1;
    if (g_dumpState == 2 && g_fence->GetCompletedValue() >= g_dumpFence) {
        DumpWrite();
        g_dumpState = 0;
    }
    Eye src = CurCap(source).cam, srcPrev = CurCap(source).prev, dst{};
    if (useRealDest) {
        if (!CameraOf(dest, &dst, nullptr)) return nullptr;
    } else {
        dst = src;  // the other eye of this tick: the drawn eye moved by the eye separation, in the camera's axes
        for (int i = 0; i < 3; ++i)
            dst.pos[i] = src.pos[i] + src.right[i] * eyeOffset[0] + src.up[i] * eyeOffset[1] + src.back[i] * eyeOffset[2];
        static ULONGLONG s_last = 0;
        if (GetTickCount64() - s_last > 5000) {  // check: where the moved camera lands against the other eye's own camera
            s_last = GetTickCount64();
            Eye real{};
            if (CameraOf(dest, &real, nullptr)) {
                float d[3] = {dst.pos[0] - real.pos[0], dst.pos[1] - real.pos[1], dst.pos[2] - real.pos[2]};
                float s2[3] = {src.pos[0] - real.pos[0], src.pos[1] - real.pos[1], src.pos[2] - real.pos[2]};
                auto on = [](const float* v, const float* ax) { return v[0] * ax[0] + v[1] * ax[1] + v[2] * ax[2]; };
                LOG("Frame warp: eye %d from %d: separation %.4f %.4f %.4f; moved camera - eye %d's last camera: right %.4f up "
                    "%.4f back %.4f (drawn eye - it: right %.4f)",
                    dest, source, eyeOffset[0], eyeOffset[1], eyeOffset[2], dest, on(d, real.right), on(d, real.up),
                    on(d, real.back), on(s2, real.right));
            }
        }
    }
    const D3D12_RESOURCE_DESC cd = srcColor->GetDesc();
    if (!g_out || g_outW != cd.Width || g_outH != cd.Height) {
        g_out = Tex(cd.Width, cd.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, L"ff16vr warp rebuilt eye");
        g_outW = cd.Width;
        g_outH = cd.Height;
        if (!g_out) return nullptr;
        LOG("Frame warp: rebuilt eye image %llu x %u", (unsigned long long)cd.Width, cd.Height);
    }
    g_colorFmt = colorFmt;

    const int f = g_frame;
    g_frame = (g_frame + 1) % 3;
    if (g_fence->GetCompletedValue() < g_allocFence[f]) {
        g_fence->SetEventOnCompletion(g_allocFence[f], g_event);
        WaitForSingleObject(g_event, 1000);
    }
    g_alloc[f]->Reset();
    g_list->Reset(g_alloc[f], nullptr);
    ID3D12GraphicsCommandList* l = g_list;
    const auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    RebuildConstants c{};
    PutEye(c.dRight, c.dUp, c.dBack, c.dPos, c.dTan, dst);
    PutEye(c.sRight, c.sUp, c.sBack, c.sPos, c.sTan, src);
    const bool own = haveHist && g_held[dest].valid && g_heldValid[dest];
    // Lab views 5 / 6: the drawn eye's own render checked the same way (its depth, motion vectors and camera as the "held"
    // ones; previous camera 5: the other eye's held camera, the render before; 6: the previous camera in its constants).
    const int view = g_view.load();
    const bool selfCheck = (view == 5 || view == 6) && own;
    const Eye& held = selfCheck ? src : own ? g_held[dest] : src;
    PutEye(c.hRight, c.hUp, c.hBack, nullptr, c.hTan, held);
    c.hRight[3] = held.pos[0];
    c.hUp[3] = held.pos[1];
    c.hBack[3] = held.pos[2];
    c.misc[0] = src.nearZ;
    c.misc[1] = (float)g_w;
    c.misc[2] = (float)g_h;
    // source order: 1 own image first, the drawn eye where it sees something nearer and for gaps; 3 also where it sees
    // something clearly farther (what the own image shows there has moved away). 0 without an own image.
    c.misc[3] = own ? (float)g_order.load() : 0.f;
    const float* mvUv = own ? g_heldMvUv[dest] : CurCap(source).mvUv;
    c.motion[0] = mvUv[0] * (float)g_mvSign.load();
    c.motion[1] = mvUv[1] * (float)g_mvSign.load();
    // The camera the held image's motion vectors point to. 0: the previous camera in that render's constants (cam_prev);
    // 1: the drawn eye's held camera, i.e. the render before (the game's previous frame in warp).
    const bool prevFromSource = g_prevMode.load() == 1 && g_held[source].valid;
    if (g_motion.load() && own && (prevFromSource || g_heldPrev[dest].valid)) {
        const Eye& p = selfCheck ? (view == 5 ? g_held[dest] : srcPrev) : prevFromSource ? g_held[source] : g_heldPrev[dest];
        PutEye(c.pRight, c.pUp, c.pBack, nullptr, c.pTan, p);
        c.pRight[3] = p.pos[0];
        c.pUp[3] = p.pos[1];
        c.pBack[3] = p.pos[2];
        c.motion[2] = 1.f;
        c.motion[3] = 1.f;
    }
    {  // lab: how far the held image's previous camera is from it (the other eye's camera of the tick before?)
        static long s_n = 0;
        static ULONGLONG s_last = 0;
        const Eye &a0 = g_held[dest], &p0 = g_heldPrev[dest], &q0 = g_held[source];
        const float rd = a0.right[0] * q0.right[0] + a0.right[1] * q0.right[1] + a0.right[2] * q0.right[2];
        const bool turning = q0.valid && rd < 0.99999f;
        if (turning && GetTickCount64() - s_last > 500) {
            s_last = GetTickCount64();
            const float rp = a0.right[0] * p0.right[0] + a0.right[1] * p0.right[1] + a0.right[2] * p0.right[2];
            const float rs = src.right[0] * a0.right[0] + src.right[1] * a0.right[1] + src.right[2] * a0.right[2];
            LOG("Frame warp: turning: right axes dot: held eye %d vs its previous camera %.6f, vs the drawn eye's held camera "
                "%.6f, drawn eye now vs held eye %.6f", dest, rp, rd, rs);
        }
        if (own && g_heldPrev[dest].valid && (s_n++ % 600) == 0) {
            const Eye &a = g_held[dest], &p = g_heldPrev[dest];
            float d[3] = {a.pos[0] - p.pos[0], a.pos[1] - p.pos[1], a.pos[2] - p.pos[2]};
            float onRight = d[0] * a.right[0] + d[1] * a.right[1] + d[2] * a.right[2];
            float onUp = d[0] * a.up[0] + d[1] * a.up[1] + d[2] * a.up[2];
            float onBack = d[0] * a.back[0] + d[1] * a.back[1] + d[2] * a.back[2];
            float sd[3] = {src.pos[0] - a.pos[0], src.pos[1] - a.pos[1], src.pos[2] - a.pos[2]};
            float sRightOff = sd[0] * a.right[0] + sd[1] * a.right[1] + sd[2] * a.right[2];
            float rotDot = a.right[0] * p.right[0] + a.right[1] * p.right[1] + a.right[2] * p.right[2];
            LOG("Frame warp: eye %d held camera - its previous camera: right %.4f up %.4f back %.4f (right axes dot %.5f); drawn eye "
                "- held eye along right %.4f; motion units -> uv %.3f %.3f",
                dest, onRight, onUp, onBack, rotDot, sRightOff, g_heldMvUv[dest][0], g_heldMvUv[dest][1]);
        }
    }
    c.fill[0] = 1.f;    // gaps from the background side
    c.fill[1] = 8.f;    // search radius
    c.fill[2] = g_motion.load() ? 0.003f : 0.f;
    c.fill[3] = 1.f;
    c.band[0] = (float)vr::g_config.warpBand;  // around anything that moved, the drawn eye (map texels)
    // ff16vr: moving things found in the drawn eye: the object part of its rebuilt motion vectors (mvfix), captured with
    // its depth and vectors
    if (CurCap(source).haveObj) {
        c.objm[0] = 0.1f * (float)g_objThrTenths.load();
        c.objm[1] = g_objects.load() ? 1.f : 0.f;
        c.objm[2] = (float)vr::g_config.warpBand;
        c.objmv[0] = CurCap(source).mvUv[0];
        c.objmv[1] = CurCap(source).mvUv[1];
    }
    if (!g_selfCheck && !g_motion.load() && g_objects.load() && g_heldObjValid[dest]) {  // t4 = the held eye's object motion
        c.objm[3] = 1.f;
        c.objmv[2] = g_heldMvUv[dest][0];
        c.objmv[3] = g_heldMvUv[dest][1];
    }
    c.band[1] = selfCheck ? 3.f : (float)view;
    if (selfCheck) {
        c.motion[0] = CurCap(source).mvUv[0] * (float)g_mvSign.load();
        c.motion[1] = CurCap(source).mvUv[1] * (float)g_mvSign.load();
    }
    g_selfCheck = selfCheck;
    Descriptors(source, srcColor, own ? histColor : srcColor, source, dest);

    // the colour images are in PIXEL_SHADER_RESOURCE (xr_screen): readable by compute too while this runs
    const auto kPs = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, kAll = kPs | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* hist = own ? histColor : nullptr;
    Barrier(l, srcColor, kPs, kAll);
    if (hist && hist != srcColor) Barrier(l, hist, kPs, kAll);
    ID3D12DescriptorHeap* heaps[] = {g_heap};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(g_root);
    const UINT slot = g_cbNext++ % kCbSlots;
    memcpy(g_cbMapped + (size_t)slot * kCbSlot, &c, sizeof(c));
    l->SetComputeRootConstantBufferView(0, g_cb->GetGPUVirtualAddress() + (UINT64)slot * kCbSlot);
    D3D12_GPU_DESCRIPTOR_HANDLE table = g_heap->GetGPUDescriptorHandleForHeapStart();
    table.ptr += (UINT64)source * kTable * g_inc;
    l->SetComputeRootDescriptorTable(1, table);
    const UINT mx = (g_w + 15) / 16, my = (g_h + 15) / 16;
    D3D12_RESOURCE_BARRIER uav[3]{};
    for (auto& u : uav) u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav[0].UAV.pResource = g_map;
    uav[1].UAV.pResource = g_ownMap;
    uav[2].UAV.pResource = g_ownMove;
    l->SetPipelineState(g_clear);
    l->Dispatch(mx, my, 1);
    l->ResourceBarrier(3, uav);
    l->SetPipelineState(g_splat);
    l->Dispatch(mx, my, 1);
    l->ResourceBarrier(1, &uav[0]);
    if (own) {
        l->SetPipelineState(g_splatOwn);
        l->Dispatch(mx, my, 1);
        l->ResourceBarrier(1, &uav[1]);
        l->SetPipelineState(g_splatOwnMove);
        l->Dispatch(mx, my, 1);
        l->ResourceBarrier(1, &uav[2]);
    }
    Barrier(l, g_out, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l->SetPipelineState(g_resolve);
    l->Dispatch((UINT)((g_outW + 15) / 16), (g_outH + 15) / 16, 1);
    Barrier(l, g_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Barrier(l, srcColor, kAll, kPs);
    if (hist && hist != srcColor) Barrier(l, hist, kAll, kPs);
    (void)kRead;
    if (const int req = g_dumpReq.exchange(0); req && g_dumpState == 0) DumpRecord(l, (req - 1) & 1);
    l->Close();
    ID3D12CommandList* lists[] = {l};
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_fence, ++g_fenceValue);
    g_allocFence[f] = g_fenceValue;
    if (g_dumpState == 1) {
        g_dumpFence = g_fenceValue;
        g_dumpState = 2;
    }
    const long n = ++g_rebuilds;
    if (n == 1 || n % 2000 == 0)
        LOG("Frame warp: %ld rebuilds (eye %d from eye %d, %s; near %.3f, tan %.3f %.3f, back sign %d, motion %d)", n, dest, source,
            own ? "own image + drawn eye" : "drawn eye only", src.nearZ, src.tan[1], src.tan[2], g_backSign.load(), (int)g_motion.load());
    return g_out;
}

void BeginPresent(int eye, uint32_t serial) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (eye < 0 || eye > 1) return;
    ++g_presSeq[eye];
    // Pairing by camera-update serial (dev "pair 1"): FF16's per-eye camera updates do not run 1:1 with renders (runs
    // 277 / 278), so counting renders against presents is the default.
    if (serial && g_pairBySerial.load()) {  // pair by identity: the capture whose render used this present's camera update
        for (int k = 0; k < kCaps; ++k) {
            const UINT seq = g_capSeq[eye] - 1 - (UINT)k;  // latest first
            if (seq >= g_capSeq[eye] || seq + kCaps < g_capSeq[eye]) break;
            if (g_cap[eye][seq % kCaps].valid && CapSerial(eye, g_cap[eye][seq % kCaps]) == serial) {
                static long s_hits = 0, s_miss = 0;
                const UINT bySeq = g_presSeq[eye];
                g_presSeq[eye] = seq + 1;  // Cur() = this slot
                if (++s_hits == 1 || (bySeq != seq + 1 && (++s_miss % 500) == 1))
                    LOG("Frame warp: eye %d present paired by camera serial %u (capture %u; counting would give %u)", eye,
                        serial, seq, bySeq - 1);
                return;
            }
        }
        static long s_none = 0;
        if ((++s_none % 300) == 1) LOG("Frame warp: eye %d present: no capture with camera serial %u (counting instead)", eye, serial);
    }
    static int s_lag[2] = {-1, -1};
    int lag = (int)g_capSeq[eye] - (int)g_presSeq[eye];  // captures the game is ahead of this present
    if (lag < 0) {  // more presents than renders (menus, mode switches): this present shows the latest render
        g_presSeq[eye] = g_capSeq[eye];
        lag = 0;
    } else if (lag >= kCaps) {  // fell behind the ring (the mode was off for a while): assume one frame of pipelining
        g_presSeq[eye] = g_capSeq[eye] - 1;
        lag = 1;
    }
    if (lag != s_lag[eye]) {
        s_lag[eye] = lag;
        LOG("Frame warp: eye %d presents run %d frame%s behind its renders", eye, lag, lag == 1 ? "" : "s");
    }
}

void Hold(int eye) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_dev || eye < 0 || eye > 1 || !Pipelines() || !g_map || !CurCap(eye).valid) return;
    const Eye cam = CurCap(eye).cam, prev = CurCap(eye).prev;
    memcpy(g_heldMvUv[eye], CurCap(eye).mvUv, sizeof(g_heldMvUv[eye]));
    const int f = g_frame;
    g_frame = (g_frame + 1) % 3;
    if (g_fence->GetCompletedValue() < g_allocFence[f]) {
        g_fence->SetEventOnCompletion(g_allocFence[f], g_event);
        WaitForSingleObject(g_event, 1000);
    }
    g_alloc[f]->Reset();
    g_list->Reset(g_alloc[f], nullptr);
    const auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto copy = [&](ID3D12Resource* dst, ID3D12Resource* src) {
        Barrier(g_list, src, kRead, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(g_list, dst, kRead, D3D12_RESOURCE_STATE_COPY_DEST);
        g_list->CopyResource(dst, src);
        Barrier(g_list, dst, D3D12_RESOURCE_STATE_COPY_DEST, kRead);
        Barrier(g_list, src, D3D12_RESOURCE_STATE_COPY_SOURCE, kRead);
    };
    copy(g_heldDepth[eye], g_depth[eye][Cur(eye)]);
    copy(g_heldMv[eye], g_mv[eye][Cur(eye)]);
    g_heldObjValid[eye] = CurCap(eye).haveObj && g_heldObj[eye] && g_obj[eye][Cur(eye)];
    if (g_heldObjValid[eye]) copy(g_heldObj[eye], g_obj[eye][Cur(eye)]);
    g_list->Close();
    ID3D12CommandList* lists[] = {g_list};
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_fence, ++g_fenceValue);
    g_allocFence[f] = g_fenceValue;
    g_heldPrev[eye] = prev;
    g_held[eye] = cam;
    g_heldValid[eye] = true;
}

void RequestDump(int eye) { g_dumpReq = 1 + (eye & 1); }

void SetBackSign(int s) {
    g_backSign = s < 0 ? -1 : 1;
    LOG("Frame warp: camera column 2 is %s", s < 0 ? "forward" : "back");
}
void SetMotion(bool on) {
    g_motion = on;
    LOG("Frame warp: object motion %s", on ? "on" : "off");
}

// Dev trigger ff16vr_warp.trigger, one command per line: "lab N", "back 1|-1", "motion 0|1", "mode N" (VRMode 0..3).
void Poll() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_dumpState == 2 && g_fence && g_fence->GetCompletedValue() >= g_dumpFence) {
            DumpWrite();
            g_dumpState = 0;
        }
    }
    static ULONGLONG s_next = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < s_next) return;
    s_next = now + 300;
    const std::wstring trig = vr::g_gameDir + L"\\ff16vr_warp.trigger";
    if (GetFileAttributesW(trig.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    char buf[256] = {};
    if (FILE* f = _wfopen(trig.c_str(), L"r")) {
        fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
    }
    DeleteFileW(trig.c_str());
    char* ctx = nullptr;
    for (char* line = strtok_s(buf, "\r\n", &ctx); line; line = strtok_s(nullptr, "\r\n", &ctx)) {
        char cmd[16] = {};
        int v = 0;
        if (sscanf_s(line, "%15s %d", cmd, (unsigned)sizeof(cmd), &v) != 2) continue;
        if (mvfix::DevCommand(cmd, v)) continue;
        if (!strcmp(cmd, "lab")) {
            vr::g_config.warpLab = v;
            LOG("Frame warp: lab %d", v);
        } else if (!strcmp(cmd, "back")) {
            SetBackSign(v);
        } else if (!strcmp(cmd, "mvsign")) {
            g_mvSign = v < 0 ? -1 : 1;
            LOG("Frame warp: motion vector sign %d", g_mvSign.load());
        } else if (!strcmp(cmd, "band")) {
            vr::g_config.warpBand = v;
            LOG("Frame warp: band %d", v);
        } else if (!strcmp(cmd, "objects")) {
            g_objects = v != 0;
            LOG("Frame warp: moving things from the drawn eye %s", v ? "on" : "off");
        } else if (!strcmp(cmd, "objthr")) {
            g_objThrTenths = v;
            LOG("Frame warp: moving threshold %.1f px", 0.1f * v);
        } else if (!strcmp(cmd, "order")) {
            g_order = v;
            LOG("Frame warp: source order %d", v);
        } else if (!strcmp(cmd, "dump")) {
            g_dumpReq = 1 + (v & 1);  // "dump <eye>"
        } else if (!strcmp(cmd, "camtrace")) {
            cam_prev::RequestTrace(v);
        } else if (!strcmp(cmd, "cbdump")) {
            cam_prev::RequestCbDump(v);
        } else if (!strcmp(cmd, "pair")) {
            g_pairBySerial = v != 0;
            LOG("Frame warp: pair captures by camera serial %d", v);
        } else if (!strcmp(cmd, "prev")) {
            g_prevMode = v;
            LOG("Frame warp: previous camera mode %d", v);
        } else if (!strcmp(cmd, "view")) {
            g_view = v;
            LOG("Frame warp: view %d", v);
        } else if (!strcmp(cmd, "motion")) {
            SetMotion(v != 0);
        } else if (!strcmp(cmd, "mode") && v >= 0 && v <= 3) {
            vr::g_config.vrMode = v;
            stereo_render::SetStereoMode(v);
        }
    }
}

const char* Status() {
    static char buf[96];
    snprintf(buf, sizeof(buf), "warp %ld rebuilds, %ld captures", g_rebuilds.load(), g_captures.load());
    return buf;
}

}  // namespace warp
