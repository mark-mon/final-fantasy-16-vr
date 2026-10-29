#include "render_size.h"
#include "cam_prev.h"
#include "motion_blur.h"
#include "taa.h"
#include "camera_hook.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "config.h"
#include "log.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include "patches.h"
#include "stereo_render.h"
#include "vrs.h"
#include "xr_screen.h"

#include <algorithm>
#include <cmath>

namespace render_size {
namespace {

HWND g_hwnd = nullptr;
uint8_t* g_base = nullptr;
size_t g_size = 0;
DWORD g_lastPoll = 0;

void FindGameModule() {
    if (g_base) return;
    g_base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_base + reinterpret_cast<IMAGE_DOS_HEADER*>(g_base)->e_lfanew);
    g_size = nt->OptionalHeader.SizeOfImage;
}

// "+0x123456 +0x..." for the return addresses inside ffxvi.exe (skipping our own frames)
std::string GameStack() {
    FindGameModule();
    void* frames[24];
    USHORT n = RtlCaptureStackBackTrace(1, 24, frames, nullptr);
    std::string s;
    int shown = 0;
    for (USHORT i = 0; i < n && shown < 8; ++i) {
        auto* p = static_cast<uint8_t*>(frames[i]);
        if (p < g_base || p >= g_base + g_size) continue;
        char b[24];
        snprintf(b, sizeof(b), " +0x%zX", (size_t)(p - g_base));
        s += b;
        ++shown;
    }
    return s.empty() ? std::string(" (no game frames)") : s;
}

// ---- user32 import logging (first calls and every change of the result)
using GetClientRect_t = BOOL(WINAPI*)(HWND, LPRECT);
using SetWindowPos_t = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using AdjustWindowRectEx_t = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD);
using GetSystemMetrics_t = int(WINAPI*)(int);
using CreateWindowExA_t = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
GetClientRect_t o_GetClientRect = nullptr;
SetWindowPos_t o_SetWindowPos = nullptr;
AdjustWindowRectEx_t o_AdjustWindowRectEx = nullptr;
GetSystemMetrics_t o_GetSystemMetrics = nullptr;
CreateWindowExA_t o_CreateWindowExA = nullptr;

LONG g_gcrCalls = 0;
RECT g_lastClient{};

BOOL WINAPI Hk_GetClientRect(HWND h, LPRECT r) {
    BOOL ok = o_GetClientRect(h, r);
    LONG n = InterlockedIncrement(&g_gcrCalls);
    if (ok && r && (n <= 3 || memcmp(r, &g_lastClient, sizeof(RECT)) != 0)) {
        g_lastClient = *r;
        LOG("Size probe: GetClientRect(%p) = %ld x %ld (call %ld) from%s", h, r->right - r->left, r->bottom - r->top, n,
            GameStack().c_str());
    }
    return ok;
}

BOOL WINAPI Hk_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags) {
    LOG("Size probe: SetWindowPos(%p, %d,%d %d x %d, flags 0x%X) from%s", h, x, y, cx, cy, flags, GameStack().c_str());
    return o_SetWindowPos(h, after, x, y, cx, cy, flags);
}

BOOL WINAPI Hk_AdjustWindowRectEx(LPRECT r, DWORD style, BOOL menu, DWORD ex) {
    RECT in = r ? *r : RECT{};
    BOOL ok = o_AdjustWindowRectEx(r, style, menu, ex);
    LOG("Size probe: AdjustWindowRectEx(%ld x %ld, style 0x%08lX ex 0x%08lX) from%s", in.right - in.left, in.bottom - in.top,
        style, ex, GameStack().c_str());
    return ok;
}

int WINAPI Hk_GetSystemMetrics(int i) {
    int v = o_GetSystemMetrics(i);
    static LONG n = 0;
    if ((i == SM_CXSCREEN || i == SM_CYSCREEN) && InterlockedIncrement(&n) <= 6)
        LOG("Size probe: GetSystemMetrics(%d) = %d from%s", i, v, GameStack().c_str());
    return v;
}

HWND WINAPI Hk_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent,
                               HMENU menu, HINSTANCE inst, LPVOID param) {
    HWND r = o_CreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    LOG("Size probe: CreateWindowExA('%s', style 0x%08lX ex 0x%08lX, %d,%d %d x %d) = %p from%s",
        HIWORD((ULONG_PTR)name) ? name : "#", style, ex, x, y, w, h, r, GameStack().c_str());
    return r;
}

void PatchUser32() {
    FindGameModule();
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_base + reinterpret_cast<IMAGE_DOS_HEADER*>(g_base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;
    struct Want { const char* name; void* hook; void** orig; } want[] = {
        {"GetClientRect", (void*)&Hk_GetClientRect, (void**)&o_GetClientRect},
        {"SetWindowPos", (void*)&Hk_SetWindowPos, (void**)&o_SetWindowPos},
        {"AdjustWindowRectEx", (void*)&Hk_AdjustWindowRectEx, (void**)&o_AdjustWindowRectEx},
        {"GetSystemMetrics", (void*)&Hk_GetSystemMetrics, (void**)&o_GetSystemMetrics},
        {"CreateWindowExA", (void*)&Hk_CreateWindowExA, (void**)&o_CreateWindowExA},
    };
    int patched = 0;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(g_base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(g_base + imp->Name), "user32.dll") != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(g_base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* addrs = reinterpret_cast<IMAGE_THUNK_DATA*>(g_base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addrs) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const char* fn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(g_base + names->u1.AddressOfData)->Name;
            for (auto& w : want) {
                if (strcmp(fn, w.name) != 0 || *w.orig) continue;
                DWORD old;
                if (!VirtualProtect(&addrs->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) continue;
                *w.orig = reinterpret_cast<void*>(addrs->u1.Function);
                addrs->u1.Function = reinterpret_cast<ULONG_PTR>(w.hook);
                VirtualProtect(&addrs->u1.Function, sizeof(void*), old, &old);
                ++patched;
            }
        }
    }
    LOG("Size probe: %d user32 imports redirected", patched);
}

// ---- video memory (IDXGIAdapter3 of the game's GPU) and churn counters for the Perf log
IDXGIAdapter3* g_adapter = nullptr;
volatile long g_resizeBuffersCount = 0;  // ResizeBuffers calls so far
volatile long g_windowResizes = 0;       // window resizes by the mod so far
bool g_sizingStopped = false;            // safety brake: the VR size kept being resized; stay at the game's size

struct Vram { double localUsed = 0, localBudget = 0, sharedUsed = 0; bool ok = false; };
Vram QueryVram() {
    Vram v;
    if (!g_adapter) return v;
    DXGI_QUERY_VIDEO_MEMORY_INFO l{}, n{};
    if (FAILED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &l))) return v;
    g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &n);
    const double gb = 1024.0 * 1024.0 * 1024.0;
    v.localUsed = l.CurrentUsage / gb;
    v.localBudget = l.Budget / gb;
    v.sharedUsed = n.CurrentUsage / gb;
    v.ok = true;
    return v;
}
void LogVram(const char* when) {
    const Vram v = QueryVram();
    if (!v.ok) return;
    LOG("VRAM %s: %.2f of %.2f GB budget in use, %.2f GB in system memory%s", when, v.localUsed, v.localBudget, v.sharedUsed,
        v.localUsed > v.localBudget ? "  WARNING: over budget (Windows pages video memory: expect stalls)" : "");
}

void ResizeWindow(int cw, int ch, bool toMonitorOrigin = false) {
    InterlockedIncrement(&g_windowResizes);
    if (!g_hwnd || !IsWindow(g_hwnd)) {
        LOG("Resize trigger: no game window yet");
        return;
    }
    DWORD style = (DWORD)GetWindowLongPtrW(g_hwnd, GWL_STYLE), ex = (DWORD)GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    RECT r{0, 0, cw, ch};
    AdjustWindowRectEx(&r, style, FALSE, ex);
    RECT wr{};
    GetWindowRect(g_hwnd, &wr);
    if (toMonitorOrigin) {  // borderless: client = window; top-left corner of the game's monitor
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            wr.left = mi.rcMonitor.left + r.left;  // r.left <= 0: border, so the client area starts at the corner
            wr.top = mi.rcMonitor.top + r.top;
        }
    }
    LOG("Window resize: client %d x %d -> window %ld x %ld (style 0x%08lX)", cw, ch, r.right - r.left, r.bottom - r.top, style);
    (o_SetWindowPos ? o_SetWindowPos : &SetWindowPos)(g_hwnd, nullptr, wr.left, wr.top, r.right - r.left, r.bottom - r.top,
                                                      SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

// ---- 16:9 fitting. The game fits its views into a 16:9 rectangle (letterbox / pillarbox) in three helpers that
// compare width / height with 16/9 (ffxvi.exe 1.03: +0x20A041, +0x20A733, +0x69C7C4). At the VR render size (headset
// aspect, e.g. square) that letterboxes the 3D view, so while the VR size is applied these fits are skipped.
struct FitPatch {
    const char* name;
    const char* sig;
    int offset;
    uint8_t bytes[2];
    int len;
    uint8_t* addr = nullptr;
    uint8_t orig[2] = {};
};
FitPatch g_fit[] = {
    {"fit A (flagged, view)", "44 38 AC 24 C0 00 00 00 74 74 C5 F0 57 C9", 8, {0xEB}, 1},       // je -> jmp
    {"fit B (flagged)", "45 84 C9 0F 84 8A 00 00 00 8B C2 C5 E8 57 D2", 3, {0x90, 0xE9}, 2},     // je rel32 -> jmp
    {"fit C", "45 8B E1 41 8B F8 45 8B E8 41 8B F1 C5 F0 57 C9 C5 E8 57 D2", 12, {0xEB, 0x5E}, 2},  // jmp past the fit
};
int g_fitMask = 7;      // which fits to skip while the VR size is applied (dev trigger "fit MASK")
int g_fitApplied = 0;   // currently patched

void FindFitPatches() {
    for (auto& f : g_fit) {
        int n = 0;
        uint8_t* p = patches::FindCode(f.sig, &n);
        if (!p) {
            LOG("Render size: WARNING %s not found (%d matches); views may be letterboxed at non-16:9 sizes", f.name, n);
            continue;
        }
        f.addr = p + f.offset;
        memcpy(f.orig, f.addr, f.len);
    }
}

void SetFitPatches(int mask) {
    if (mask == g_fitApplied) return;
    for (int i = 0; i < 3; ++i) {
        FitPatch& f = g_fit[i];
        if (!f.addr) continue;
        const bool on = (mask >> i) & 1, was = (g_fitApplied >> i) & 1;
        if (on != was) patches::WriteCodeBytes(f.addr, on ? f.bytes : f.orig, f.len);
    }
    LOG("Render size: 16:9 fit patches 0x%X (was 0x%X)", mask, g_fitApplied);
    g_fitApplied = mask;
}

// ---- VR render size
struct Size { int w = 0, h = 0; };
bool g_changed = false;       // the mod resized the window (restore when VR rendering is switched off)
Size g_applied;               // last size the mod asked for
DWORD g_attemptTimes[4] = {};
int g_attemptIdx = 0;
bool g_gaveUpLogged = false;
char g_status[96] = "game resolution";

Size ClientSize() {
    RECT r{};
    if (g_hwnd && (o_GetClientRect ? o_GetClientRect : &GetClientRect)(g_hwnd, &r)) return {r.right - r.left, r.bottom - r.top};
    return {};
}

// Per-eye size for the field of view the camera hook renders (symmetric frustum covering both eyes, x FovScale),
// at the pixel density of the headset's recommended eye image, x RenderScale. Width : height = tanH : tanV, so the
// camera hook's FOV matches the headset exactly.
bool g_capped = false;
Size Desired() {
    const auto& c = vr::g_config;
    if (!c.vrOutput || c.vrMode == 0 || g_sizingStopped) return {};
    // Title and first load stay at the game's own size (16:9 menus). (Resizing already at the title was tried: the
    // first gameplay blur stayed the same, so it is not the view's creation size.)
    if (!stereo_render::GameplayReached()) return {};
    if (c.renderWidth > 0 && c.renderHeight > 0) return {c.renderWidth & ~1, c.renderHeight & ~1};
    if (c.renderScale <= 0.f) return {};
    unsigned rw = 0, rh = 0;
    xr::RecommendedEyeSize(&rw, &rh);
    float t[2][4];
    if (!rw || !rh || !xr::HeadsetEyeTangents(t)) return {};
    float tanV = 0, tanH = 0, spanX = 0, spanY = 0;
    for (int e = 0; e < 2; ++e) {
        tanV = std::max(tanV, std::max(t[e][0], t[e][1]));
        tanH = std::max(tanH, std::max(t[e][2], t[e][3]));
    }
    spanY = t[0][0] + t[0][1];
    spanX = t[0][2] + t[0][3];
    if (spanX <= 0.01f || spanY <= 0.01f) return {};
    float ppt = std::max(rw / spanX, rh / spanY) * c.renderScale;  // pixels per unit of tangent
    tanV *= c.fovScale;
    tanH *= c.fovScale;
    int w = (int)lroundf(2.f * tanH * ppt / 8.f) * 8;
    int h = (int)lroundf(2.f * tanV * ppt / 8.f) * 8;
    unsigned mw = 0, mh = 0;
    xr::MaxSwapchainSize(&mw, &mh);
    const int maxW = mw ? (int)mw : 4096, maxH = mh ? (int)mh : 4096;  // per-eye array slices: full max width each
    g_capped = w > maxW || h > maxH;  // higher RenderScale gives no more pixels (runtime's max swapchain size)
    if (w > maxW) { h = h * maxW / w; w = maxW; }
    if (h > maxH) { w = w * maxH / h; h = maxH; }
    return {std::max(w, 320) & ~7, std::max(h, 240) & ~7};
}

void Restore() {
    // back to the game's own size: the monitor (the game runs borderless; its own resolution setting is applied
    // the next time it changes display mode)
    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi)) return;
    const RECT& m = mi.rcMonitor;
    LOG("Render size: VR rendering off; window back to the monitor size %ld x %ld", m.right - m.left, m.bottom - m.top);
    (o_SetWindowPos ? o_SetWindowPos : &SetWindowPos)(g_hwnd, nullptr, m.left, m.top, m.right - m.left, m.bottom - m.top,
                                                      SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

void UpdateVrSize() {
    if (!g_hwnd || !IsWindow(g_hwnd)) return;
    const Size want = Desired();
    const Size cur = ClientSize();
    if (!want.w) {
        if (g_changed) {
            g_changed = false;
            g_applied = {};
            SetFitPatches(0);
            Restore();
        }
        snprintf(g_status, sizeof(g_status), "game resolution %d x %d", cur.w, cur.h);
        return;
    }
    snprintf(g_status, sizeof(g_status), "%d x %d per eye%s", want.w, want.h, g_capped ? " (max)" : "");
    static bool s_cappedLogged = false;
    if (g_capped && !s_cappedLogged) {
        s_cappedLogged = true;
        LOG("Render size: at the maximum per-eye size %d x %d (the runtime's max swapchain size); a higher "
            "RenderScale changes nothing", want.w, want.h);
    }
    SetFitPatches(g_fitMask);
    if (cur.w == want.w && cur.h == want.h) {
        g_gaveUpLogged = false;
        return;
    }
    // don't fight the game: at most 4 attempts per 20 s
    const DWORD now = GetTickCount();
    if (now - g_attemptTimes[g_attemptIdx] < 20000 && g_attemptTimes[g_attemptIdx] != 0) {
        if (!g_gaveUpLogged) {
            g_gaveUpLogged = true;
            LOG("Render size: WARNING the window keeps returning to %d x %d (wanted %d x %d); retrying later", cur.w, cur.h,
                want.w, want.h);
        }
        return;
    }
    g_attemptTimes[g_attemptIdx] = now;
    g_attemptIdx = (g_attemptIdx + 1) % 4;
    if (want.w != g_applied.w || want.h != g_applied.h) {
        unsigned rw = 0, rh = 0;
        xr::RecommendedEyeSize(&rw, &rh);
        LOG("Render size: per-eye %d x %d (headset recommends %u x %u, RenderScale %.2f, FovScale %.2f); window was %d x %d",
            want.w, want.h, rw, rh, vr::g_config.renderScale, vr::g_config.fovScale, cur.w, cur.h);
    }
    g_applied = want;
    g_changed = true;
    LogVram("before the resize");
    ResizeWindow(want.w, want.h, true);
}

}  // namespace

void Install() {
    DeleteFileW((vr::g_gameDir + L"\\ff16vr_resize.trigger").c_str());  // stale trigger from an earlier run
    PatchUser32();
    FindFitPatches();
}

const char* Status() { return g_status; }

float VrAspect() {
    const Size a = g_applied;
    if (!g_changed || !a.w || !a.h) return 0.f;
    const Size cur = ClientSize();
    return cur.w == a.w && cur.h == a.h ? (float)a.w / (float)a.h : 0.f;
}

void OnDevice(ID3D12Device* dev) {
    if (g_adapter || !dev) return;
    const LUID luid = dev->GetAdapterLuid();
    IDXGIFactory4* f = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) {
        if (FAILED(f->EnumAdapterByLuid(luid, IID_PPV_ARGS(&g_adapter)))) g_adapter = nullptr;
        f->Release();
    }
    if (g_adapter) {
        DXGI_ADAPTER_DESC d{};
        g_adapter->GetDesc(&d);
        LOG("VRAM: GPU %ls, %.2f GB dedicated", d.Description, d.DedicatedVideoMemory / (1024.0 * 1024.0 * 1024.0));
        LogVram("at start");
    } else {
        LOG("VRAM: could not open the game's adapter; no video memory logging");
    }
}

long ResizeCount() { return g_resizeBuffersCount; }

void OnSwapChain(HWND hwnd, UINT w, UINT h, const char* how) {
    if (hwnd) g_hwnd = hwnd;
    const long n = strncmp(how, "ResizeBuffers", 13) == 0 ? InterlockedIncrement(&g_resizeBuffersCount) : 0;
    if (n > 20 && n % 50 != 0) return;  // a resize loop: don't also flood the log with stacks
    RECT cr{}, wr{};
    if (g_hwnd) {
        GetClientRect(g_hwnd, &cr);
        GetWindowRect(g_hwnd, &wr);
    }
    HMONITOR mon = g_hwnd ? MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    MONITORINFO mi{sizeof(mi)};
    if (mon) GetMonitorInfoW(mon, &mi);
    LOG("Size probe: %s %u x %u; window %p style 0x%08lX, client %ld x %ld, window rect %ld,%ld %ld x %ld, monitor %ld x %ld; from%s",
        how, w, h, g_hwnd, g_hwnd ? (DWORD)GetWindowLongPtrW(g_hwnd, GWL_STYLE) : 0, cr.right - cr.left, cr.bottom - cr.top,
        wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, mi.rcMonitor.right - mi.rcMonitor.left,
        mi.rcMonitor.bottom - mi.rcMonitor.top, GameStack().c_str());
}

void OnPresent() {
    DWORD now = GetTickCount();
    static DWORD s_perfT0 = 0;
    static unsigned s_presents = 0;
    ++s_presents;
    if (!s_perfT0) s_perfT0 = now;
    if (now - s_perfT0 >= 10000) {
        const Size cur = ClientSize();
        static long s_rb = 0, s_wr = 0, s_xc = 0, s_sw = 0;
        const long rb = g_resizeBuffersCount - s_rb, wr = g_windowResizes - s_wr, xc = xr::SwapchainCreates() - s_xc,
                   sw = stereo_render::ModeSwitches() - s_sw;
        s_rb += rb; s_wr += wr; s_xc += xc; s_sw += sw;
        const Vram v = QueryVram();
        char vram[96] = "VRAM ?";
        if (v.ok) snprintf(vram, sizeof(vram), "VRAM %.2f / %.2f GB, %.2f GB in system memory", v.localUsed, v.localBudget, v.sharedUsed);
        LOG("Perf: %.1f game presents/s, VR %u fps, render %d x %d, %s, VRS %s, %s, %s, %s | last 10 s: %ld ResizeBuffers, %ld window "
            "resizes, %ld XR swapchain creates, %ld stereo switches", s_presents * 1000.f / (now - s_perfT0),
            xr::VrFramesPerSecond(), cur.w, cur.h, vram, vrs::Status(), motion_blur::Status(), taa::Status(), cam_prev::Status(), rb, wr, xc, sw);
        if (v.ok && v.localUsed > v.localBudget)
            LOG("WARNING: video memory over budget (%.2f of %.2f GB): lower RenderScale", v.localUsed, v.localBudget);
        if (xc >= 20 || sw >= 20)
            LOG("WARNING: the stereo image is being rebuilt constantly (%ld swapchain creates, %ld stereo switches in 10 s)", xc, sw);
        // Safety brake: the window keeps being resized -> stop VR sizing for this session (back to the game's size)
        if (!g_sizingStopped && g_changed && rb >= 8) {  // the mod itself resizes at most 4 times per 20 s
            g_sizingStopped = true;
            LOG("WARNING: %ld ResizeBuffers in 10 s; VR render sizing stopped for this session (game resolution)", rb);
        }
        s_presents = 0;
        s_perfT0 = now;
    }
    if (now - g_lastPoll < 500) return;
    g_lastPoll = now;
    const std::wstring trig = vr::g_gameDir + L"\\ff16vr_resize.trigger";
    UpdateVrSize();
    if (GetFileAttributesW(trig.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    int w = 0, h = 0;
    float scale = -1.f;
    if (FILE* f = _wfopen(trig.c_str(), L"r")) {
        char line[64] = {};
        if (fgets(line, sizeof(line), f)) {
            int mask = 0, vrsOn = 0, taaOff = 0;
            int peh = 0, pom = 0, jit = 0;
            int pmv = 0;
            if (sscanf(line, "pmv %d", &pmv) == 1) {  // dev: per-eye motion vectors 0 off / 1 auto / 2 always
                cam_prev::SetMode(pmv);
                scale = -2.f;
            } else if (sscanf(line, "jit %d", &jit) == 1) {  // dev: StereoJitterSync on / off
                vr::g_config.stereoJitterSync = jit != 0;
                LOG("Resize trigger: StereoJitterSync -> %d", jit != 0);
                scale = -2.f;
            } else if (sscanf(line, "pom %d", &pom) == 1) {  // dev: PerEyeObjectMotion on / off
                vr::g_config.perEyeObjectMotion = pom != 0;
                LOG("Resize trigger: PerEyeObjectMotion -> %d", pom != 0);
                scale = -2.f;
            } else if (sscanf(line, "peh %d", &peh) == 1) {  // dev: PerEyeHistory on / off
                vr::g_config.perEyeHistory = peh != 0;
                LOG("Resize trigger: PerEyeHistory -> %d (%ld restores so far)", peh != 0, camera_hook::HistoryRestores());
                scale = -2.f;
            } else if (sscanf(line, "taaoff %d", &taaOff) == 1) {  // dev: DisableTAA on / off (as the menu does)
                vr::g_config.disableTaa = taaOff != 0;
                LOG("Resize trigger: DisableTAA -> %d", taaOff != 0);
                scale = -2.f;
            } else if (sscanf(line, "vrs %d", &vrsOn) == 1) {  // dev: foveated rendering on / off (as the menu does)
                vr::g_config.foveatedRendering = vrsOn != 0;
                LOG("Resize trigger: FoveatedRendering -> %d", vrsOn != 0);
                scale = -2.f;
            } else if (sscanf(line, "fit %d", &mask) == 1) {
                g_fitMask = mask & 7;
                LOG("Resize trigger: fit mask -> 0x%X", g_fitMask);
                scale = -2.f;
            } else if (sscanf(line, "scale %f", &scale) != 1) scale = -1.f;
            if (scale == -1.f && sscanf(line, "%d %d", &w, &h) != 2) w = h = 0;
        }
        fclose(f);
    }
    DeleteFileW(trig.c_str());
    if (scale >= 0.f) {  // dev: RenderScale at run time (as the menu does)
        vr::g_config.renderScale = std::min(scale, 3.f);
        LOG("Resize trigger: RenderScale -> %.2f", vr::g_config.renderScale);
    } else if (scale == -2.f) {
    } else if (w >= 320 && h >= 240) ResizeWindow(w, h);
    else LOG("Resize trigger: expected \"W H\" or \"scale X\"");
}

}  // namespace render_size
