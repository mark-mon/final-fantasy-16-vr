#include "vrs.h"

#include <windows.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <climits>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "log.h"
#include "stereo_render.h"
#include "vr_shared.h"

namespace vrs {
namespace {

using CreateRTV_t = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*,
                                              D3D12_CPU_DESCRIPTOR_HANDLE);
using OMSetRT_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                           const D3D12_CPU_DESCRIPTOR_HANDLE*);
CreateRTV_t o_CreateRTV = nullptr;
OMSetRT_t o_OMSetRT = nullptr;
using RSSetViewports_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
RSSetViewports_t o_RSSetViewports = nullptr;

ID3D12Device* g_device = nullptr;
ID3D12CommandQueue* g_queue = nullptr;
bool g_supported = false;
UINT g_tile = 16;
std::atomic<UINT> g_scW{0}, g_scH{0};
ID3D12Resource* g_backbuffers[4] = {};

struct Rt { ID3D12Resource* res; UINT w, h; };
std::shared_mutex g_rtvLock;
std::unordered_map<SIZE_T, Rt> g_rtvs;  // RTV descriptor -> resource and size

// ---- shading-rate images
// One image per (render-target size, viewport, foveation centre, settings). The scene is often drawn into a sub-rect
// of a larger target (resolution scale / DLSS input in the top-left corner), so the rates are laid out around the
// *viewport*, not the texture (run 162 on the 5090: centring on the texture put the whole viewport in the outer ring).
// With eye tracking (FoveatedGaze) the centre follows the gaze, so images are built asynchronously on a queue of our
// own and only used once the GPU has finished uploading them; until then the closest ready image is used.
struct SriKey {
    UINT rtW, rtH;
    int x0, y0, x1, y1;  // viewport in tiles
    int cx, cy;          // foveation centre in tiles
    bool operator==(const SriKey& o) const {
        return rtW == o.rtW && rtH == o.rtH && x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1 && cx == o.cx && cy == o.cy;
    }
};
struct Sri {
    SriKey key;
    ID3D12Resource* tex = nullptr;
    ID3D12Resource* upload = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    UINT64 fence = 0;
    bool ready = false;
    uint64_t lastUse = 0;
};
std::mutex g_sriLock;
std::vector<Sri> g_sris;
struct Retired {
    ID3D12Resource* tex;
    uint64_t at;
};
std::vector<Retired> g_retired;
float g_builtInner = -1, g_builtMiddle = -1;
int g_builtOuter = -1;
ID3D12CommandQueue* g_ownQueue = nullptr;
ID3D12Fence* g_fence = nullptr;
UINT64 g_fenceNext = 1;
std::atomic<uint64_t> g_applied{0}, g_gazeApplied{0};
char g_status[128] = "off";
constexpr size_t kMaxSris = 40;

void ReleaseSri(Sri& e, uint64_t now) {
    if (e.tex) g_retired.push_back({e.tex, now});  // may still be referenced by recorded command lists
    if (e.upload) e.upload->Release();
    if (e.list) e.list->Release();
    if (e.alloc) e.alloc->Release();
    e = Sri{};
}

// Fills and submits the upload of one image; returns false on failure.
bool StartBuild(Sri& e) {
    const auto& c = vr::g_config;
    const SriKey& k = e.key;
    const UINT w = (k.rtW + g_tile - 1) / g_tile, h = (k.rtH + g_tile - 1) / g_tile;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8_UINT;
    d.SampleDesc.Count = 1;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&e.tex))))
        return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    g_device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&e.upload))))
        return false;
    uint8_t* p = nullptr;
    if (FAILED(e.upload->Map(0, nullptr, reinterpret_cast<void**>(&p)))) return false;
    const uint8_t outer = c.foveatedOuterRate ? 0xA /*4X4*/ : 0x5 /*2X2*/;
    // radii are fractions of the viewport's half height (circles in pixels)
    const float halfH = std::max(1.f, (k.y1 - k.y0) * 0.5f);
    for (UINT y = 0; y < h; ++y) {
        for (UINT x = 0; x < w; ++x) {
            const float u = ((float)x + 0.5f - ((float)k.cx + 0.5f)) / halfH;
            const float v = ((float)y + 0.5f - ((float)k.cy + 0.5f)) / halfH;
            const float r = sqrtf(u * u + v * v);
            p[fp.Offset + y * fp.Footprint.RowPitch + x] = r < c.foveatedInner ? 0x0 : r < c.foveatedMiddle ? 0x5 : outer;
        }
    }
    e.upload->Unmap(0, nullptr);
    if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&e.alloc))) ||
        FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, e.alloc, nullptr, IID_PPV_ARGS(&e.list))))
        return false;
    D3D12_TEXTURE_COPY_LOCATION dst{e.tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{e.upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint = fp;
    e.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = e.tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
    e.list->ResourceBarrier(1, &b);
    e.list->Close();
    ID3D12CommandList* lists[] = {e.list};
    g_ownQueue->ExecuteCommandLists(1, lists);
    e.fence = g_fenceNext++;
    g_ownQueue->Signal(g_fence, e.fence);
    return true;
}

// Ready image for the key; starts building it if needed and meanwhile returns the closest ready one (same target and
// viewport, nearest centre), or nullptr (full rate).
ID3D12Resource* SriFor(const SriKey& key) {
    const auto& c = vr::g_config;
    const uint64_t now = PresentCount();
    std::lock_guard<std::mutex> lock(g_sriLock);
    if (c.foveatedInner != g_builtInner || c.foveatedMiddle != g_builtMiddle || c.foveatedOuterRate != g_builtOuter) {
        for (auto& e : g_sris) ReleaseSri(e, now);
        g_sris.clear();
        g_builtInner = c.foveatedInner;
        g_builtMiddle = c.foveatedMiddle;
        g_builtOuter = c.foveatedOuterRate;
    }
    // housekeeping: finished uploads, old retired images
    const UINT64 done = g_fence->GetCompletedValue();
    for (auto& e : g_sris)
        if (!e.ready && e.fence && e.fence <= done) {
            e.ready = true;
            e.upload->Release();
            e.upload = nullptr;
            e.list->Release();
            e.list = nullptr;
            e.alloc->Release();
            e.alloc = nullptr;
        }
    for (size_t i = 0; i < g_retired.size();)
        if (now - g_retired[i].at > 16) {
            g_retired[i].tex->Release();
            g_retired[i] = g_retired.back();
            g_retired.pop_back();
        } else {
            ++i;
        }
    Sri* best = nullptr;
    int bestD = INT_MAX;
    bool building = false;
    for (auto& e : g_sris) {
        if (e.key == key) {
            if (e.ready) {
                e.lastUse = now;
                return e.tex;
            }
            building = true;
        }
        if (e.ready && e.key.rtW == key.rtW && e.key.rtH == key.rtH && e.key.x0 == key.x0 && e.key.y0 == key.y0 &&
            e.key.x1 == key.x1 && e.key.y1 == key.y1) {
            const int d = abs(e.key.cx - key.cx) + abs(e.key.cy - key.cy);
            if (d < bestD) bestD = d, best = &e;
        }
    }
    if (!building) {
        if (g_sris.size() >= kMaxSris) {  // evict the least recently used ready image
            size_t lru = SIZE_MAX;
            for (size_t i = 0; i < g_sris.size(); ++i)
                if (g_sris[i].ready && (lru == SIZE_MAX || g_sris[i].lastUse < g_sris[lru].lastUse)) lru = i;
            if (lru != SIZE_MAX && &g_sris[lru] != best) {
                ReleaseSri(g_sris[lru], now);
                g_sris.erase(g_sris.begin() + lru);
                best = nullptr;  // pointers into the vector are invalid now
            }
        }
        if (g_sris.size() < kMaxSris) {
            Sri e;
            e.key = key;
            e.lastUse = now;
            if (StartBuild(e)) {
                static int s_logged = 0;
                if (s_logged++ < 12)
                    LOG("VRS: shading-rate image for %u x %u, viewport tiles %d,%d-%d,%d, centre %d,%d: full rate within %.2f, "
                        "2x2 to %.2f, %s beyond",
                        key.rtW, key.rtH, key.x0, key.y0, key.x1, key.y1, key.cx, key.cy, c.foveatedInner, c.foveatedMiddle,
                        c.foveatedOuterRate ? "4x4" : "2x2");
                g_sris.push_back(e);
            } else {
                ReleaseSri(e, now);
            }
        }
    }
    if (best) {
        for (auto& e : g_sris)  // re-find after a possible erase
            if (e.ready && e.key.rtW == key.rtW && e.key.rtH == key.rtH && e.key.x0 == key.x0 && e.key.y0 == key.y0 &&
                e.key.x1 == key.x1 && e.key.y1 == key.y1 && abs(e.key.cx - key.cx) + abs(e.key.cy - key.cy) == bestD) {
                e.lastUse = now;
                return e.tex;
            }
    }
    return nullptr;
}

bool Wanted(const Rt& rt) {
    const UINT sw = g_scW.load(), sh = g_scH.load();
    if (!sw || !sh || !rt.w || !rt.h) return false;
    for (auto* b : g_backbuffers)
        if (b && b == rt.res) return false;
    const float a = (float)rt.w / rt.h, sa = (float)sw / sh;
    return fabsf(a - sa) < sa * 0.015f && rt.w * 4 >= sw && rt.w <= sw;
}

// Gaze for the eye being rendered, as an offset of the foveation centre in the viewport's NDC (-1..1, y up).
bool GazeNdc(float vpAspect, float* gx, float* gy) {
    const auto& c = vr::g_config;
    if (!c.foveatedGaze) return false;
    const HeadSample h = LatestHead();
    if (!h.valid || !h.gazeValid || h.gazeDir[2] > -0.05f) return false;
    const int view = stereo_render::FrameEye();
    const int eye = view >= 0 ? (view & 1) : -1;
    float tanV, tanH;
    if (eye == 0 || eye == 1) {
        tanV = std::max(h.eyeTan[eye][0], h.eyeTan[eye][1]) * c.fovScale;
        tanH = std::max(h.eyeTan[eye][2], h.eyeTan[eye][3]) * c.fovScale;
    } else {
        tanV = std::max(h.tanUp, h.tanDown) * c.fovScale;
        tanH = std::max(h.tanLeft, h.tanRight) * c.fovScale;
    }
    if (vpAspect > 0.1f && tanH > tanV * vpAspect) tanV = tanH / vpAspect;  // as the camera hook does
    tanH = tanV * vpAspect;
    *gx = std::clamp((h.gazeDir[0] / -h.gazeDir[2]) / tanH, -1.f, 1.f);
    *gy = std::clamp((h.gazeDir[1] / -h.gazeDir[2]) / tanV, -1.f, 1.f);
    return true;
}

// Per recording thread: the render target and viewport last set on its command list.
struct ListState {
    ID3D12GraphicsCommandList* cl = nullptr;
    Rt rt{};
    bool wanted = false;
    D3D12_VIEWPORT vp{};
    bool vpValid = false;
};
thread_local ListState t_ls;

void Apply(ID3D12GraphicsCommandList* cl) {
    ID3D12Resource* sri = nullptr;
    if (t_ls.wanted) {
        const Rt& rt = t_ls.rt;
        float vx = 0, vy = 0, vw = (float)rt.w, vh = (float)rt.h;
        if (t_ls.vpValid && t_ls.vp.Width >= rt.w * 0.2f && t_ls.vp.Height >= rt.h * 0.2f && t_ls.vp.TopLeftX >= 0 &&
            t_ls.vp.TopLeftY >= 0 && t_ls.vp.TopLeftX + t_ls.vp.Width <= rt.w + 1 && t_ls.vp.TopLeftY + t_ls.vp.Height <= rt.h + 1) {
            vx = t_ls.vp.TopLeftX, vy = t_ls.vp.TopLeftY, vw = t_ls.vp.Width, vh = t_ls.vp.Height;
        }
        SriKey k{};
        k.rtW = rt.w;
        k.rtH = rt.h;
        k.x0 = (int)(vx / g_tile);
        k.y0 = (int)(vy / g_tile);
        k.x1 = (int)ceilf((vx + vw) / g_tile);
        k.y1 = (int)ceilf((vy + vh) / g_tile);
        float cxp = vx + vw * 0.5f, cyp = vy + vh * 0.5f;
        float gx, gy;
        const bool gaze = GazeNdc(vw / vh, &gx, &gy);
        if (gaze) {
            cxp += gx * vw * 0.5f;
            cyp -= gy * vh * 0.5f;
        }
        // centre quantised to 2 tiles: fewer images while the gaze jitters
        k.cx = ((int)(cxp / g_tile) / 2) * 2;
        k.cy = ((int)(cyp / g_tile) / 2) * 2;
        sri = SriFor(k);
        if (sri && gaze) ++g_gazeApplied;
    }
    auto* cl5 = reinterpret_cast<ID3D12GraphicsCommandList5*>(cl);  // same object; List5 checked at start-up
    static const D3D12_SHADING_RATE_COMBINER comb[2] = {D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                                                        D3D12_SHADING_RATE_COMBINER_OVERRIDE};
    cl5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, sri ? comb : nullptr);
    cl5->RSSetShadingRateImage(sri);
    if (sri) ++g_applied;
}

bool Active() {
    const auto& c = vr::g_config;
    return g_supported && c.foveatedRendering && c.vrOutput && c.vrMode != 0 && stereo_render::GameplayReached();
}

void STDMETHODCALLTYPE Hk_CreateRTV(ID3D12Device* dev, ID3D12Resource* res, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                    D3D12_CPU_DESCRIPTOR_HANDLE h) {
    o_CreateRTV(dev, res, desc, h);
    Rt rt{res, 0, 0};
    if (res) {
        D3D12_RESOURCE_DESC d = res->GetDesc();
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) { rt.w = (UINT)d.Width; rt.h = d.Height; }
    }
    std::unique_lock<std::shared_mutex> lock(g_rtvLock);
    g_rtvs[h.ptr] = rt;
}

void STDMETHODCALLTYPE Hk_OMSetRT(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,
                                  BOOL range, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    o_OMSetRT(cl, n, rtvs, range, dsv);
    if (!Active()) return;
    if (t_ls.cl != cl) {
        t_ls = ListState{};
        t_ls.cl = cl;
    }
    t_ls.rt = Rt{};
    if (n >= 1 && rtvs) {
        std::shared_lock<std::shared_mutex> lock(g_rtvLock);
        auto it = g_rtvs.find(rtvs[0].ptr);
        if (it != g_rtvs.end()) t_ls.rt = it->second;
    }
    t_ls.wanted = Wanted(t_ls.rt);
    Apply(cl);
}

void STDMETHODCALLTYPE Hk_RSSetViewports(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_VIEWPORT* vps) {
    o_RSSetViewports(cl, n, vps);
    if (!Active() || !n || !vps) return;
    if (t_ls.cl != cl) {
        t_ls = ListState{};
        t_ls.cl = cl;
    }
    const bool changed = !t_ls.vpValid || memcmp(&t_ls.vp, &vps[0], sizeof(D3D12_VIEWPORT)) != 0;
    t_ls.vp = vps[0];
    t_ls.vpValid = true;
    if (t_ls.wanted && changed) Apply(cl);
}

}  // namespace

void OnDevice(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (g_device || !device) return;
    g_device = device;
    g_device->AddRef();
    g_queue = queue;
    g_queue->AddRef();
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6))) ||
        o6.VariableShadingRateTier < D3D12_VARIABLE_SHADING_RATE_TIER_2) {
        LOG("VRS: Tier 2 variable rate shading not supported; foveated rendering unavailable");
        snprintf(g_status, sizeof(g_status), "not supported by the GPU");
        return;
    }
    g_tile = o6.ShadingRateImageTileSize ? o6.ShadingRateImageTileSize : 16;
    if (!o6.AdditionalShadingRatesSupported && vr::g_config.foveatedOuterRate) {
        LOG("VRS: 4x4 rate not supported; using 2x2 for the outer ring");
        vr::g_config.foveatedOuterRate = 0;
    }
    // device: CreateRenderTargetView (vtable 20); command list: OMSetRenderTargets (46), via a list of our own
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)))) {
        if (alloc) alloc->Release();
        LOG("VRS: could not create a command list; foveated rendering unavailable");
        return;
    }
    ID3D12GraphicsCommandList5* l5 = nullptr;
    if (FAILED(list->QueryInterface(IID_PPV_ARGS(&l5)))) {
        list->Release();
        alloc->Release();
        LOG("VRS: ID3D12GraphicsCommandList5 not available");
        return;
    }
    l5->Release();
    list->Close();
    void** lvt = *reinterpret_cast<void***>(list);
    void** dvt = *reinterpret_cast<void***>(device);
    DWORD old;
    if (VirtualProtect(&dvt[20], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        o_CreateRTV = reinterpret_cast<CreateRTV_t>(dvt[20]);
        dvt[20] = reinterpret_cast<void*>(&Hk_CreateRTV);
        VirtualProtect(&dvt[20], sizeof(void*), old, &old);
    }
    if (VirtualProtect(&lvt[46], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        o_OMSetRT = reinterpret_cast<OMSetRT_t>(lvt[46]);
        lvt[46] = reinterpret_cast<void*>(&Hk_OMSetRT);
        VirtualProtect(&lvt[46], sizeof(void*), old, &old);
    }
    if (VirtualProtect(&lvt[21], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        o_RSSetViewports = reinterpret_cast<RSSetViewports_t>(lvt[21]);
        lvt[21] = reinterpret_cast<void*>(&Hk_RSSetViewports);
        VirtualProtect(&lvt[21], sizeof(void*), old, &old);
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_ownQueue))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
        LOG("VRS: could not create the upload queue; foveated rendering unavailable");
        list->Release();
        alloc->Release();
        return;
    }
    list->Release();  // the vtable stays (shared by all of the driver's command lists)
    alloc->Release();
    g_supported = o_CreateRTV && o_OMSetRT && o_RSSetViewports;
    LOG("VRS: Tier 2 supported (tile %u, extra rates %d); hooks %s", g_tile, (int)o6.AdditionalShadingRatesSupported,
        g_supported ? "installed" : "FAILED");
}

void OnSwapChainSize(unsigned w, unsigned h) {
    g_scW = w;
    g_scH = h;
}

void SetBackBuffer(ID3D12Resource* const* buffers, unsigned count) {
    for (unsigned i = 0; i < 4; ++i) g_backbuffers[i] = i < count ? buffers[i] : nullptr;
}

const char* Status() {
    const auto& c = vr::g_config;
    if (!g_supported) return g_status;
    static uint64_t s_last = 0;
    static ULONGLONG s_t = 0;
    static unsigned s_rate = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_t >= 1000) {
        const uint64_t a = g_applied.load();
        s_rate = (unsigned)(a - s_last);
        s_last = a;
        s_t = now;
    }
    const HeadSample h = LatestHead();
    snprintf(g_status, sizeof(g_status), "%s, %u passes/s, gaze %s", c.foveatedRendering ? "on" : "off", s_rate,
             !c.foveatedGaze ? "off" : h.gazeValid ? "tracking" : "not available");
    return g_status;
}

}  // namespace vrs
