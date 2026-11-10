// Per-eye history for the game's temporal effects (PerEyeEffectHistory, part of DlssGhostingFix).
//
// Run 228 (RenderDoc, native stereo, TAA path): 12 textures are read before they are written in a frame, i.e. they carry
// the previous frame - in native stereo the other eye's: TAA history (2880 R11G11B10 / R16), a half-resolution effect
// (1440 R11G11B10 x2, R8), the volumetric fog / lighting froxels (360 x 360 x 64 / x 4 3D textures, 360 R16G16).
// Ten of them are written the same way: the effect computes into a work texture and the last thing it does in the frame
// is copy that work texture into the history texture (CopyTextureRegion / CopyResource between textures of the same
// description; run 229: the descriptions alone are ambiguous, the copy is not).
//
// So the hook sits on those copies. Right after the game's copy into history H, recorded in the eye's frame:
//   H -> shadow[eye]               (this eye's new history is kept)
//   shadow[other eye] -> H         (the next frame, the other eye, reads its own last history)
// H stays in COPY_DEST, as the game left it. Shadows are created on first use with H's description.
#include "eye_history.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <d3d12.h>

#include "camera_hook.h"
#include "config.h"
#include "log.h"
#include "stereo_render.h"
#include "taa.h"

namespace eye_history {
namespace {

struct Hist {
    D3D12_RESOURCE_DESC desc{};
    ID3D12Resource* shadow[2] = {};
    bool valid[2] = {};
    uint64_t bytes = 0;
    long swaps = 0;
    int cat = 0;            // Category() bit
    int lastEye = -1;       // eye of the last copy: the copies must alternate between the eyes
    long repeats = 0;       // copies with the same eye as the one before (eye label not alternating)
    long copies[2] = {};
    unsigned long long lastSerial = 0;
    long samePass = 0;      // more than one copy in one frame pass
    int inPass = 0;         // index of the current copy within its frame pass (1 = first)
    bool multi = false;     // copied more than once per pass
    char seq[33] = {};      // last copies: eye digit, '+' = same pass as the copy before
    int seqLen = 0;
    ULONGLONG lastUse = 0;  // GetTickCount64 of the last copy (stale entries are released)
    D3D12_BOX box{};        // region the game copied (the swap copies the same region)
    bool hasBox = false;
    UINT sw = 0, sh = 0, sd = 0;  // shadow size: the copied region only (run 238: memory on 10 GB cards)
};
std::mutex g_mutex;
std::unordered_map<ID3D12Resource*, Hist> g_hist;
std::atomic<long> g_swaps{0}, g_tracked{0};
std::atomic<uint64_t> g_bytes{0};
constexpr uint64_t kMaxBytes = 1536ull << 20;  // shadow memory cap
bool g_wasStereo = false;
std::atomic<int> g_mask{0x3F};  // dev "peehm": which categories are swapped
std::atomic<long> g_repeats{0};
std::atomic<int> g_multiMode{0};  // textures copied more than once per pass: 0 not swapped, 1 / 2 swap after copy 1 / 2
ULONGLONG g_lastPrune = 0;

// Categories (run 232, FSR 50 %): 1 froxel volumes (x 64 slices), 2 froxel volumes (x 4), 3 2D at froxel size,
// 8 half-resolution effect, 16 full-resolution history (RGBA16F), 32 full-resolution R16F.
int Category(const D3D12_RESOURCE_DESC& d) {
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) return d.DepthOrArraySize > 8 ? 1 : 2;
    if (d.Width <= 512) return 4;
    if (d.Width <= 1600) return 8;
    return d.Format == DXGI_FORMAT_R16_FLOAT ? 32 : 16;
}

uint64_t SizeOf(ID3D12Device* dev, const D3D12_RESOURCE_DESC& d);
ID3D12Resource* MakeShadow(ID3D12Resource* like, D3D12_RESOURCE_DESC d);

// Shadows may still be used by recorded command lists: they are released a few seconds later.
std::vector<std::pair<ULONGLONG, ID3D12Resource*>> g_graveyard;

void FreeShadows(Hist& h) {
    for (auto*& s : h.shadow)
        if (s) g_graveyard.emplace_back(GetTickCount64(), s), s = nullptr;
    g_bytes -= h.bytes;
    h.bytes = 0;
    h.sw = h.sh = h.sd = 0;
    h.valid[0] = h.valid[1] = false;
}

void Release(Hist& h) {
    FreeShadows(h);
    --g_tracked;
}

// Shadows the size of the region the game copies (the used part of the texture at the render resolution).
bool EnsureShadows(Hist& h, ID3D12Resource* like, UINT w, UINT hgt, UINT dep) {
    if (h.shadow[0] && h.shadow[1] && h.sw >= w && h.sh >= hgt && h.sd >= dep) return true;
    FreeShadows(h);
    D3D12_RESOURCE_DESC d = h.desc;
    d.Width = w;
    d.Height = hgt;
    d.DepthOrArraySize = (UINT16)dep;
    ID3D12Device* dev = nullptr;
    if (FAILED(like->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return false;
    const uint64_t sz = SizeOf(dev, d);
    dev->Release();
    if (g_bytes + 2 * sz > kMaxBytes) return false;
    h.shadow[0] = MakeShadow(like, d);
    h.shadow[1] = MakeShadow(like, d);
    if (!h.shadow[0] || !h.shadow[1]) {
        FreeShadows(h);
        return false;
    }
    h.bytes = 2 * sz;
    g_bytes += h.bytes;
    h.sw = w, h.sh = hgt, h.sd = dep;
    LOG("Per-eye effect history: shadows for %p: %u x %u x %u of %llu x %u x %u (%.1f MB per eye); %.0f MB in all", (void*)like,
        w, hgt, dep, (unsigned long long)h.desc.Width, h.desc.Height, h.desc.DepthOrArraySize, sz / 1048576.0,
        g_bytes / 1048576.0);
    return true;
}

// Entries not copied for 5 s belong to textures the game has dropped (resolution changes): free their shadows.
ULONGLONG g_lastStats = 0;
void PruneLocked(ULONGLONG now) {
    if (now - g_lastStats > 10000) {  // per texture: copies per eye, eye repeats, copies in one pass, first copies
        g_lastStats = now;
        for (auto& kv : g_hist) {
            Hist& h = kv.second;
            LOG("Per-eye effect history: %p cat %d %llux%u fmt %d: copies L %ld R %ld, eye repeats %ld, same pass %ld, "
                "swaps %ld, first [%s]", (void*)kv.first, h.cat, (unsigned long long)h.desc.Width, h.desc.Height,
                (int)h.desc.Format, h.copies[0], h.copies[1], h.repeats, h.samePass, h.swaps, h.seq);
        }
    }
    if (now - g_lastPrune < 2000) return;
    g_lastPrune = now;
    for (size_t i = 0; i < g_graveyard.size();) {
        if (now - g_graveyard[i].first > 3000) {
            g_graveyard[i].second->Release();
            g_graveyard[i] = g_graveyard.back();
            g_graveyard.pop_back();
        } else {
            ++i;
        }
    }
    for (auto it = g_hist.begin(); it != g_hist.end();) {
        if (now - it->second.lastUse > 5000) {
            LOG("Per-eye effect history: released %p (%llu x %u, not copied for 5 s)", (void*)it->first,
                (unsigned long long)it->second.desc.Width, it->second.desc.Height);
            Release(it->second);
            it = g_hist.erase(it);
        } else {
            ++it;
        }
    }
}

using CopyRegion_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT,
                                              UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
using CopyResource_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);

bool SameTexture(ID3D12Resource* a, ID3D12Resource* b, D3D12_RESOURCE_DESC* outDesc) {
    if (!a || !b || a == b) return false;
    const D3D12_RESOURCE_DESC da = a->GetDesc(), db = b->GetDesc();
    if (da.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D && da.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D) return false;
    if (da.Dimension != db.Dimension || da.Width != db.Width || da.Height != db.Height ||
        da.DepthOrArraySize != db.DepthOrArraySize || da.Format != db.Format || da.MipLevels != 1 || db.MipLevels != 1 ||
        da.SampleDesc.Count != 1)
        return false;
    if (da.Width < 64 || da.Height < 64) return false;
    // the game's history textures are all UAV-capable (run 229); this also keeps out the mod's own copies (swap-chain
    // images, HUD) and depth buffers
    if (!(da.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) || !(db.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        return false;
    if (da.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && da.DepthOrArraySize != 1) return false;
    *outDesc = da;
    return true;
}

uint64_t SizeOf(ID3D12Device* dev, const D3D12_RESOURCE_DESC& d) {
    const D3D12_RESOURCE_ALLOCATION_INFO ai = dev->GetResourceAllocationInfo(0, 1, &d);
    return ai.SizeInBytes;
}

ID3D12Resource* MakeShadow(ID3D12Resource* like, D3D12_RESOURCE_DESC d) {
    ID3D12Device* dev = nullptr;
    if (FAILED(like->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return nullptr;
    d.Flags &= ~(D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
    d.Flags &= ~(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);  // copy targets only (depth formats are typeless-copied)
    if (d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) d.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    ID3D12Resource* r = nullptr;
    const HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    IID_PPV_ARGS(&r));
    dev->Release();
    if (FAILED(hr)) {
        LOG("Per-eye effect history: shadow texture failed 0x%08lX (%llu x %u x %u, format %d)", hr,
            (unsigned long long)d.Width, d.Height, d.DepthOrArraySize, (int)d.Format);
        return nullptr;
    }
    r->SetName(L"ff16vr per-eye history");
    return r;
}

D3D12_RESOURCE_BARRIER Tr(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    return x;
}

thread_local int t_suspend = 0;
std::atomic<long> g_ignored{0};

// After the game's copy src -> dst (dst in COPY_DEST). box: the region copied (null: whole texture).
void AfterCopy(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, ID3D12Resource* src, const D3D12_BOX* box) {
    if (!vr::g_config.perEyeEffectHistory) return;
    if (t_suspend > 0) {  // Streamline / DLSS copying its own inputs
        ++g_ignored;
        return;
    }
    const bool stereo = stereo_render::NativeActive();
    const int eye = stereo_render::FrameEye();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (stereo != g_wasStereo) {  // stereo switched: the kept histories are stale
        for (auto& kv : g_hist) kv.second.valid[0] = kv.second.valid[1] = false;
        g_wasStereo = stereo;
    }
    const ULONGLONG now = GetTickCount64();
    PruneLocked(now);
    if (!stereo || eye < 0 || eye > 1) return;
    auto it = g_hist.find(dst);
    if (it != g_hist.end()) {
        // the address may belong to a new resource now (the game recreates its targets on resolution changes)
        const D3D12_RESOURCE_DESC cur = dst->GetDesc();
        const D3D12_RESOURCE_DESC& old = it->second.desc;
        if (cur.Width != old.Width || cur.Height != old.Height || cur.DepthOrArraySize != old.DepthOrArraySize ||
            cur.Format != old.Format || cur.Dimension != old.Dimension) {
            Release(it->second);
            g_hist.erase(it);
            it = g_hist.end();
        }
    }
    if (it == g_hist.end()) {
        D3D12_RESOURCE_DESC d;
        if (!SameTexture(dst, src, &d)) return;
        Hist h;
        h.desc = d;
        h.cat = Category(d);
        it = g_hist.emplace(dst, h).first;
        ++g_tracked;
        LOG("Per-eye effect history: tracking %p (%s %llu x %u x %u, format %d); %ld textures",
            (void*)dst, d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? "3D" : "2D", (unsigned long long)d.Width, d.Height,
            d.DepthOrArraySize, (int)d.Format, g_tracked.load());
    }
    Hist& h = it->second;
    h.lastUse = now;
    const unsigned long long serial = camera_hook::PassSerial();
    if (h.lastEye == eye) ++h.repeats, ++g_repeats;
    if (serial == h.lastSerial) {
        ++h.samePass;
        ++h.inPass;
    } else {
        h.inPass = 1;
    }
    if (h.seqLen < 31) {
        if (serial == h.lastSerial && h.seqLen) h.seq[h.seqLen++] = '+';
        if (h.seqLen < 31) h.seq[h.seqLen++] = char('0' + eye);
    }
    ++h.copies[eye];
    h.lastEye = eye;
    h.lastSerial = serial;
    if (!h.multi && h.samePass >= 4 && h.copies[0] + h.copies[1] < 400) {
        // Run 234: the full-resolution RGBA16F texture is copied into twice per pass and also used within the pass;
        // swapping it after the first copy gives that pass the other eye's image (left eye soft).
        h.multi = true;
        if (g_multiMode == 0) FreeShadows(h);
        h.valid[0] = h.valid[1] = false;
        LOG("Per-eye effect history: %p is copied more than once per pass: %s", (void*)dst,
            g_multiMode == 0 ? "not swapped" : g_multiMode == 1 ? "swapped after its first copy" : "swapped after its second copy");
    }
    if (h.multi && (g_multiMode == 0 || h.inPass != g_multiMode)) return;
    if (!(g_mask.load() & h.cat)) {
        h.valid[0] = h.valid[1] = false;
        return;
    }
    const int other = eye ^ 1;
    if (box) h.box = *box, h.hasBox = true;
    else h.hasBox = false;
    const bool is3D = h.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    const UINT needW = box ? std::min<UINT>(box->right, (UINT)h.desc.Width) : (UINT)h.desc.Width;
    const UINT needH = box ? std::min<UINT>(box->bottom, h.desc.Height) : h.desc.Height;
    const UINT needD = !is3D ? 1u : box ? std::min<UINT>(box->back, h.desc.DepthOrArraySize) : h.desc.DepthOrArraySize;
    if (!needW || !needH || !needD || !EnsureShadows(h, dst, needW, needH, needD)) return;
    // copies into / out of the shadows cover the region the game copied: the same box at the origin of both
    if (!box) h.box = D3D12_BOX{0, 0, 0, needW, needH, needD}, h.hasBox = true;
    else h.box.right = needW, h.box.bottom = needH, h.box.back = needD;
    auto copyRegion = reinterpret_cast<CopyRegion_t>(taa::OrigListFn(cl, 16));
    // only the region the game copied (a quarter of the texture at FSR 50 %)
    auto copyRes = [&](ID3D12Resource* to, ID3D12Resource* from) {
        D3D12_TEXTURE_COPY_LOCATION a{to, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, b{from, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        a.SubresourceIndex = b.SubresourceIndex = 0;
        copyRegion(cl, &a, 0, 0, 0, &b, h.hasBox ? &h.box : nullptr);
    };
    // keep this eye's new history
    D3D12_RESOURCE_BARRIER b1[2] = {Tr(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                    Tr(h.shadow[eye], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)};
    cl->ResourceBarrier(2, b1);
    copyRes(h.shadow[eye], dst);
    h.valid[eye] = true;
    if (h.valid[other]) {
        // the other eye's last history into H, for the next frame (the other eye)
        D3D12_RESOURCE_BARRIER b2[3] = {Tr(dst, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
                                        Tr(h.shadow[eye], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
                                        Tr(h.shadow[other], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE)};
        cl->ResourceBarrier(3, b2);
        copyRes(dst, h.shadow[other]);
        D3D12_RESOURCE_BARRIER b3[1] = {Tr(h.shadow[other], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON)};
        cl->ResourceBarrier(1, b3);
        ++h.swaps;
        if (++g_swaps == 1) LOG("Per-eye effect history: first swap (eye %d)", eye);
    } else {
        D3D12_RESOURCE_BARRIER b2[2] = {Tr(dst, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
                                        Tr(h.shadow[eye], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON)};
        cl->ResourceBarrier(2, b2);
    }
}

// VerboseLog: the first 300 texture copies (once per destination), with why they were or were not taken.
void LogCopy(const char* kind, ID3D12Resource* dst, ID3D12Resource* src, const char* note) {
    if (!vr::g_config.verboseLog || !dst || !src) return;
    static std::mutex m;
    static std::unordered_map<ID3D12Resource*, int> seen;
    std::lock_guard<std::mutex> lock(m);
    if (seen.size() >= 300 || !seen.emplace(dst, 1).second) return;
    const D3D12_RESOURCE_DESC a = dst->GetDesc(), b = src->GetDesc();
    LOG("Copy %s %s: dst %p %dD %llux%ux%u fmt %d mips %u flags 0x%X <- src %p %dD %llux%ux%u fmt %d mips %u flags 0x%X (eye %d)",
        kind, note, (void*)dst, (int)a.Dimension - 1, (unsigned long long)a.Width, a.Height, a.DepthOrArraySize, (int)a.Format,
        a.MipLevels, (unsigned)a.Flags, (void*)src, (int)b.Dimension - 1, (unsigned long long)b.Width, b.Height,
        b.DepthOrArraySize, (int)b.Format, b.MipLevels, (unsigned)b.Flags, stereo_render::FrameEye());
}

void STDMETHODCALLTYPE Hk_CopyTextureRegion(ID3D12GraphicsCommandList* cl, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x,
                                            UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    reinterpret_cast<CopyRegion_t>(taa::OrigListFn(cl, 16))(cl, dst, x, y, z, src, box);
    if (dst && src && dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
        char note[96];
        snprintf(note, sizeof(note), "sub %u <- %u at %u,%u,%u box %s", dst->SubresourceIndex, src->SubresourceIndex, x, y, z,
                 box ? "yes" : "no");
        if (box) snprintf(note + strlen(note), sizeof(note) - strlen(note), " [%u,%u,%u - %u,%u,%u]", box->left, box->top,
                          box->front, box->right, box->bottom, box->back);
        LogCopy("region", dst->pResource, src->pResource, note);
    }
    if (!dst || !src || x || y || z || dst->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ||
        src->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX || dst->SubresourceIndex || src->SubresourceIndex)
        return;
    // The game copies only the rendered part (run 231: [0,0,0 - 181,181,64] of a 360 x 360 x 64 volume at FSR 50 %):
    // a box from the origin onto the origin is a history copy too; the swap copies the whole textures.
    if (box && (box->left || box->top || box->front)) return;
    AfterCopy(cl, dst->pResource, src->pResource, box);
}

void STDMETHODCALLTYPE Hk_CopyResource(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, ID3D12Resource* src) {
    reinterpret_cast<CopyResource_t>(taa::OrigListFn(cl, 17))(cl, dst, src);
    LogCopy("resource", dst, src, "");
    AfterCopy(cl, dst, src, nullptr);
}

}  // namespace

Suspend::Suspend() { ++t_suspend; }
Suspend::~Suspend() { --t_suspend; }
long Ignored() { return g_ignored.load(); }

void Install() {
    taa::HookListSlot(16, (void*)&Hk_CopyTextureRegion);
    taa::HookListSlot(17, (void*)&Hk_CopyResource);
    LOG("Per-eye effect history: copy hooks registered (PerEyeEffectHistory=%d)", vr::g_config.perEyeEffectHistory);
}

const char* Status() {
    static char buf[128];
    snprintf(buf, sizeof(buf), "per-eye effect history %ld textures (%.0f MB), %ld swaps, %ld eye repeats, %ld upscaler copies ignored, mask 0x%X",
             g_tracked.load(), g_bytes.load() / 1048576.0, g_swaps.load(), g_repeats.load(), g_ignored.load(), g_mask.load());
    return buf;
}

void SetMultiMode(int mode) {
    g_multiMode = mode;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& kv : g_hist) kv.second.valid[0] = kv.second.valid[1] = false;
    LOG("Per-eye effect history: multi-copy mode %d", mode);
}

void SetMask(int mask) {
    g_mask = mask & 0x3F;
    LOG("Per-eye effect history: category mask 0x%X", mask & 0x3F);
}

}  // namespace eye_history
