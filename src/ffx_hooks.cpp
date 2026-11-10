// Per-eye FSR 3 history (PerEyeFsr, part of DlssGhostingFix).
//
// The game uses AMD FidelityFX through the "ffx API" of amd_fidelityfx_dx12.dll (imports ffxCreateContext,
// ffxDestroyContext, ffxConfigure, ffxQuery, ffxDispatch). In native stereo both eyes go through one upscale context,
// so FSR blends each eye with the other eye's history. As with DLSS (sl_hooks.cpp), the right eye gets a context of its
// own: when the game creates an upscale context, a second one is created from the same description; right-eye upscale
// dispatches go to it; configure calls are mirrored; destroy frees both.
//
// ffx API (ffx_api.h): every desc starts with { uint64_t type; header* pNext; }. Upscale: create 0x00010000,
// dispatch 0x00010001 (ffx_upscale.h).
#include "ffx_hooks.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "config.h"
#include "log.h"
#include <d3d12.h>

#include "stereo_edge.h"
#include "mvfix.h"
#include "cam_prev.h"
#include "eye_history.h"
#include "warp.h"
#include "stereo_render.h"

namespace ffx_hooks {
namespace {

struct Header {
    uint64_t type;
    Header* pNext;
};
using Ctx = void*;
using Create_t = uint32_t (*)(Ctx* ctx, Header* desc, const void* mem);
using Destroy_t = uint32_t (*)(Ctx* ctx, const void* mem);
using Configure_t = uint32_t (*)(Ctx* ctx, const Header* desc);
using Query_t = uint32_t (*)(Ctx* ctx, Header* desc);
using Dispatch_t = uint32_t (*)(Ctx* ctx, const Header* desc);
Create_t o_Create = nullptr;
Destroy_t o_Destroy = nullptr;
Configure_t o_Configure = nullptr;
Query_t o_Query = nullptr;
Dispatch_t o_Dispatch = nullptr;

constexpr uint64_t kCreateUpscale = 0x00010000ull, kDispatchUpscale = 0x00010001ull;

std::mutex g_mutex;
std::unordered_map<Ctx, Ctx*> g_right;  // game's upscale context -> the right eye's
std::atomic<long> g_evals[2] = {}, g_logs{0};
std::atomic<ULONGLONG> g_lastEval{0};

bool Enabled() { return vr::g_config.perEyeFsr; }

void LogType(const char* what, uint64_t type) {
    static uint64_t seen[32];
    static int n = 0;
    for (int i = 0; i < n; ++i)
        if (seen[i] == type) return;
    if (n < 32) seen[n++] = type;
    LOG("FFX: %s desc type 0x%08llX", what, (unsigned long long)type);
}

uint32_t Hk_Create(Ctx* ctx, Header* desc, const void* mem) {
    const uint32_t r = o_Create(ctx, desc, mem);
    if (desc) LogType("create", desc->type);
    if (r != 0 || !ctx || !*ctx || !desc || desc->type != kCreateUpscale) return r;
    if (!Enabled()) {  // the second context costs VRAM (run 235: ~1.2 GB at 2880 output): only with PerEyeFsr
        LOG("FFX: upscale context %p created (PerEyeFsr off: no right eye context)", *ctx);
        return r;
    }
    Ctx* right = new Ctx{};
    const uint32_t r2 = o_Create(right, desc, mem);
    if (r2 != 0 || !*right) {
        LOG("FFX: second upscale context for the right eye FAILED (%u)", r2);
        delete right;
        return r;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_right[*ctx] = right;
    LOG("FFX: upscale context %p created; right eye context %p", *ctx, *right);
    return r;
}

uint32_t Hk_Destroy(Ctx* ctx, const void* mem) {
    Ctx* right = nullptr;
    if (ctx && *ctx) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_right.find(*ctx);
        if (it != g_right.end()) {
            right = it->second;
            g_right.erase(it);
        }
    }
    if (right) {
        o_Destroy(right, mem);
        delete right;
        LOG("FFX: right eye upscale context destroyed");
    }
    return o_Destroy(ctx, mem);
}

Ctx* RightOf(Ctx* ctx) {
    if (!ctx || !*ctx) return nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_right.find(*ctx);
    return it == g_right.end() ? nullptr : it->second;
}

uint32_t Hk_Configure(Ctx* ctx, const Header* desc) {
    const uint32_t r = o_Configure(ctx, desc);
    if (desc) LogType("configure", desc->type);
    if (Ctx* right = RightOf(ctx)) o_Configure(right, desc);
    return r;
}

uint32_t Hk_Query(Ctx* ctx, Header* desc) {
    if (desc) LogType("query", desc->type);
    return o_Query(ctx, desc);
}

// ffxDispatchDescUpscale (ffx_upscale.h): header 0, commandList 16, then FfxApiResource (48 bytes: resource 0,
// description 8 {type, format, width, height, depth, mipCount, flags, usage}, state 40) color 24, depth 72,
// motionVectors 120, exposure 168, reactive 216, transparencyAndComposition 264, output 312; jitterOffset 360,
// motionVectorScale 368, renderSize 376, upscaleSize 384.
D3D12_RESOURCE_STATES FfxState(uint32_t s) {
    switch (s) {
        case 1u << 0: return D3D12_RESOURCE_STATE_COMMON;
        case 1u << 1: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case 1u << 2: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        case 1u << 3: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        case (1u << 2) | (1u << 3): return (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        case 1u << 4: return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case 1u << 5: return D3D12_RESOURCE_STATE_COPY_DEST;
        case (1u << 4) | (1u << 2): return D3D12_RESOURCE_STATE_GENERIC_READ;
        default: return (D3D12_RESOURCE_STATES)0xFFFFFFFF;
    }
}

uint8_t g_descCopy[448];

// The depth and motion vectors of an upscale dispatch, and its command list.
bool DescInputs(const Header* desc, stereo_edge::Inputs* out, ID3D12GraphicsCommandList** outCl) {
    const auto* b = reinterpret_cast<const uint8_t*>(desc);
    auto res = [&](int off) { return *reinterpret_cast<ID3D12Resource* const*>(b + off); };
    auto u32 = [&](int off) { return *reinterpret_cast<const uint32_t*>(b + off); };
    ID3D12Resource* depth = res(72);
    ID3D12Resource* mv = res(120);
    auto* cl = *reinterpret_cast<ID3D12GraphicsCommandList* const*>(b + 16);
    const uint32_t rw = u32(376), rh = u32(380);
    const D3D12_RESOURCE_STATES ds = FfxState(u32(72 + 40)), ms = FfxState(u32(120 + 40));
    static int s_logs = 0;
    if (s_logs++ < 2)
        LOG("FFX: dispatch inputs: depth %p (%u x %u fmt %u state 0x%X), motion %p (%u x %u fmt %u state 0x%X), render %u x %u, "
            "motion scale %.3f %.3f", (void*)depth, u32(72 + 16), u32(72 + 20), u32(72 + 12), u32(72 + 40), (void*)mv, u32(120 + 16),
            u32(120 + 20), u32(120 + 12), u32(120 + 40), rw, rh, *reinterpret_cast<const float*>(b + 368),
            *reinterpret_cast<const float*>(b + 372));
    if (!depth || !mv || !cl || !rw || !rh || rw > 16384 || rh > 16384 || ds == (D3D12_RESOURCE_STATES)0xFFFFFFFF ||
        ms == (D3D12_RESOURCE_STATES)0xFFFFFFFF)
        return false;
    stereo_edge::Inputs in;
    in.depth = depth;
    in.depthState = ds;
    in.mv = mv;
    in.mvState = ms;
    in.w = (int)rw;
    in.h = (int)rh;
    in.pixPerMvX = *reinterpret_cast<const float*>(b + 368);
    in.pixPerMvY = *reinterpret_cast<const float*>(b + 372);
    in.jitter[0] = *reinterpret_cast<const float*>(b + 360);
    in.jitter[1] = *reinterpret_cast<const float*>(b + 364);
    in.haveJitter = true;
    in.depthInverted = true;  // FF16 uses reversed depth (taa.cpp: z = near / d)
    in.eye = stereo_render::FrameEye() & 1;
    *out = in;
    *outCl = cl;
    return true;
}

// StereoEdgeFix with shared FSR history: returns a copy of the dispatch description with the fixed motion vectors.
const Header* EdgeDesc(const Header* desc) {
    if (!stereo_edge::Wanted() || Enabled() || !stereo_render::NativeActive()) return desc;
    stereo_edge::Inputs in;
    ID3D12GraphicsCommandList* cl = nullptr;
    if (!DescInputs(desc, &in, &cl)) return desc;
    ID3D12Resource* out = stereo_edge::Run(cl, in);
    if (!out) return desc;
    memcpy(g_descCopy, desc, sizeof(g_descCopy));
    *reinterpret_cast<ID3D12Resource**>(g_descCopy + 120) = out;
    *reinterpret_cast<uint32_t*>(g_descCopy + 120 + 40) = (1u << 2) | (1u << 3);  // PIXEL_COMPUTE_READ
    return reinterpret_cast<const Header*>(g_descCopy);
}

uint8_t g_rebuildDesc[448];
std::atomic<long> g_rebuilt{0}, g_rebuildMiss{0};

// MotionRebuild with per-eye FSR: the dispatch gets this eye's rebuilt motion vectors (mvfix, constants from the camera
// buffer). Returns the description to dispatch; *mvOut the rebuilt texture (for the warp capture) or nullptr.
const Header* RebuildDesc(const Header* desc, ID3D12Resource** mvOut) {
    *mvOut = nullptr;
    if (!Enabled() || !stereo_render::NativeActive() || vr::g_config.motionRebuild <= 0) return desc;
    stereo_edge::Inputs in;
    ID3D12GraphicsCommandList* cl = nullptr;
    if (!DescInputs(desc, &in, &cl)) return desc;
    if (!mvfix::ConstantsFromCamera(in.eye, &in, in.jitter[0], in.jitter[1], in.w, in.h)) {
        if (g_rebuildMiss++ < 3) LOG("FFX: motion rebuild: no camera for eye %d yet", in.eye);
        return desc;
    }
    {
        static std::atomic<int> s_unitLogs{0};
        const auto* b = reinterpret_cast<const uint8_t*>(desc);
        auto u32 = [&](int off) { return *reinterpret_cast<const uint32_t*>(b + off); };
        if (s_unitLogs.fetch_add(1) < 2)
            LOG("FFX: motion rebuild eye %d: motion texture %u x %u, render %u x %u, motion scale %.3f %.3f (vectors in texture uv: scale "
                "= texture size expected)", in.eye, u32(120 + 16), u32(120 + 20), in.w, in.h, in.pixPerMvX, in.pixPerMvY);
    }
    ID3D12Resource* out = mvfix::Run(cl, in);
    if (!out) return desc;
    ++g_rebuilt;
    memcpy(g_rebuildDesc, desc, sizeof(g_rebuildDesc));
    *reinterpret_cast<ID3D12Resource**>(g_rebuildDesc + 120) = out;
    *reinterpret_cast<uint32_t*>(g_rebuildDesc + 120 + 40) = (1u << 2) | (1u << 3);  // PIXEL_COMPUTE_READ
    *mvOut = out;
    return reinterpret_cast<const Header*>(g_rebuildDesc);
}

uint32_t Hk_Dispatch(Ctx* ctx, const Header* desc) {
    if (desc) LogType("dispatch", desc->type);
    eye_history::Suspend noHistory;  // copies inside the upscale (ours and FSR's own) are not the game's history copies
    ID3D12Resource* rebuilt = nullptr;
    if (desc && desc->type == kDispatchUpscale) desc = RebuildDesc(desc, &rebuilt);
    if (desc && desc->type == kDispatchUpscale && warp::Wanted() && stereo_render::FrameEye() >= 0) {  // frame warp
        stereo_edge::Inputs in;
        ID3D12GraphicsCommandList* cl = nullptr;
        if (DescInputs(desc, &in, &cl)) warp::OnUpscalerInputs(cl, in, in.eye);  // the rebuilt vectors when there are any
    }
    if (desc && desc->type == kDispatchUpscale && !rebuilt) desc = EdgeDesc(desc);
    if (!desc || desc->type != kDispatchUpscale || !Enabled() || !stereo_render::NativeActive())
        return o_Dispatch(ctx, desc);
    const int eye = stereo_render::FrameEye();
    if (eye >= 0) ++g_evals[eye & 1];
    g_lastEval = GetTickCount64();
    Ctx* right = eye == 1 ? RightOf(ctx) : nullptr;
    if (right && g_logs.fetch_add(1) < 4) LOG("FFX: right eye upscale dispatch -> context %p", *right);
    return o_Dispatch(right ? right : ctx, desc);
}

}  // namespace

void Install() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;
    int patched = 0;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), "amd_fidelityfx_dx12.dll") != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* addrs = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addrs) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const char* n = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name;
            void* hook = nullptr;
            void** orig = nullptr;
            if (!strcmp(n, "ffxCreateContext")) hook = (void*)&Hk_Create, orig = (void**)&o_Create;
            else if (!strcmp(n, "ffxDestroyContext")) hook = (void*)&Hk_Destroy, orig = (void**)&o_Destroy;
            else if (!strcmp(n, "ffxConfigure")) hook = (void*)&Hk_Configure, orig = (void**)&o_Configure;
            else if (!strcmp(n, "ffxQuery")) hook = (void*)&Hk_Query, orig = (void**)&o_Query;
            else if (!strcmp(n, "ffxDispatch")) hook = (void*)&Hk_Dispatch, orig = (void**)&o_Dispatch;
            if (!hook) continue;
            DWORD old;
            if (!VirtualProtect(&addrs->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) continue;
            *orig = reinterpret_cast<void*>(addrs->u1.Function);
            addrs->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
            VirtualProtect(&addrs->u1.Function, sizeof(void*), old, &old);
            ++patched;
        }
    }
    LOG("FFX: %d of 5 FidelityFX imports redirected (PerEyeFsr=%d)", patched, vr::g_config.perEyeFsr);
}

bool FsrEvaluatingRecently() { return GetTickCount64() - g_lastEval.load() < 500; }

const char* Status() {
    static char buf[256];
    snprintf(buf, sizeof(buf), "FSR per eye: %ld / %ld upscales, %ld rebuilt; %s", g_evals[0].load(), g_evals[1].load(), g_rebuilt.load(),
             cam_prev::JitterMatchStatus());
    return buf;
}

}  // namespace ffx_hooks
