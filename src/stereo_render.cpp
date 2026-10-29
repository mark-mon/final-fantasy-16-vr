#include "stereo_render.h"
#include "camera_hook.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "config.h"
#include "log.h"
#include "xr_screen.h"
#include "patches.h"

namespace stereo_render {
namespace {

// mov rcx,[rdi+5B0h] ; test rcx,rcx ; je +6 ; mov rax,[rcx] ; call [rax+40h] ; lea rsi,[rdi+538h]
constexpr char kSite[] = "48 8B 8F B0 05 00 00 48 85 C9 74 06 48 8B 01 FF 50 40 48 8D B7 38 05 00 00";
constexpr int kPatchOffset = 12;  // the 6 bytes "mov rax,[rcx] ; call [rax+40h]"

// main loop: mov rcx,[app] ; call Frame ; test al,al ; je ...   (patch the 5-byte call)
constexpr char kFrameSite[] = "48 8B 0D 19 4D E6 00 E8 5C E2 FF FF 84 C0 74 0E";
constexpr int kFrameCallOffset = 7;
// frame function: mov rbx,[rdi+5B8h] ; call GameTime ; test rax,rax   (patch the 5-byte call)
constexpr char kTimeSite[] = "48 8B 9F B8 05 00 00 E8 91 26 FF FF 48 85 C0";
constexpr int kTimeCallOffset = 7;

using Render_t = void (*)(void* renderer);

using Frame_t = bool (*)(void* app);
using Time_t = int64_t (*)();

Frame_t g_frameFn = nullptr;
Time_t g_timeFn = nullptr;
std::atomic<int> g_mode{0};  // 0 normal, 1 Render() twice, 2 whole frame twice (game clock frozen for the 2nd)
bool g_freezeTime = false;
std::atomic<bool> g_skip0{false};
// Frame-twice only runs in gameplay (main camera updated recently). Boot logos, movies and menus stay single-frame:
// the logo sequence stalls when every tick is rendered twice.
std::atomic<uint64_t> g_frameCalls{0};
std::atomic<uint64_t> g_lastMainView{0};
std::atomic<bool> g_twiceNow{false};
std::atomic<uint64_t> g_mainViewRun{0};
std::atomic<ULONGLONG> g_mainViewRunStart{0};  // GetTickCount64 when the current run began
constexpr ULONGLONG kMainViewStableMs = 1000;  // ...and for at least this long (menu pages with a 3D camera, fast menus)    // consecutive frame calls with a gameplay camera update
constexpr uint64_t kMainViewWindow = 3;    // camera must have been updated within this many frame calls
constexpr uint64_t kMainViewStable = 120;  // ...and in each of this many calls before both eyes are rendered
int64_t g_lastTime = 0;
std::atomic<bool> g_twice{false};
std::atomic<bool> g_gameplaySeen{false};
std::atomic<long> g_toggles{0};  // switches between one and two frames per tick  // the gameplay camera has been stable once (past title / loading)
thread_local int t_pass = 0;
std::atomic<int> g_viewCount{1};  // frame passes per tick: 1 or 2 (stereo)
std::atomic<int> g_frameEye{-1};  // eye of the frame being rendered (any thread): -1 single frame, 0 left, 1 right
std::atomic<int> g_framesLogged{0};
std::atomic<unsigned long> g_gameThread{0};
SRWLOCK g_totLock = SRWLOCK_INIT;
PassTotals g_tot{};
void AddPass(int slot, double ms, ULONG64 cyc) {
    AcquireSRWLockExclusive(&g_totLock);
    g_tot.wallMs[slot] += ms;
    g_tot.cycles[slot] += (double)cyc;
    ++g_tot.calls[slot];
    ReleaseSRWLockExclusive(&g_totLock);
}
double g_freqMs = 0;

// timing accumulators (game thread only)
uint64_t g_frames = 0;
double g_sum[2] = {0, 0};
ULONGLONG g_lastLog = 0;
// frame phases: render end -> present begin, present duration, present end -> next render begin
double g_renderEnd = 0, g_presentBegin = 0, g_presentEnd = 0;
double g_phase[3] = {0, 0, 0};
uint64_t g_phaseN[3] = {0, 0, 0};

double NowMs() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / g_freqMs;
}

extern "C" void RenderHook(void* renderer) {
    Render_t fn = (*reinterpret_cast<Render_t**>(renderer))[8];
    double t0 = NowMs();
    if (g_presentEnd > 0) { g_phase[2] += t0 - g_presentEnd; ++g_phaseN[2]; g_presentEnd = 0; }
    fn(renderer);
    double t1 = NowMs();
    g_sum[0] += t1 - t0;
    if (g_twice.load(std::memory_order_relaxed)) {
        t_pass = 1;
        fn(renderer);
        t_pass = 0;
        g_sum[1] += NowMs() - t1;
    }
    g_renderEnd = NowMs();
    ++g_frames;
    ULONGLONG now = GetTickCount64();
    if (!g_lastLog) g_lastLog = now;
    if (now - g_lastLog >= (g_mode.load() ? 2000u : 10000u) && g_framesLogged.fetch_add(1) < 2000) {
        auto avg = [](int i) { return g_phaseN[i] ? g_phase[i] / g_phaseN[i] : -1.0; };
        LOG("STEREO Render(): %llu frames in %.1f s, CPU per call: first %.2f ms, second %.2f ms | "
            "render->present %.2f ms, present %.2f ms, present->render %.2f ms",
            (unsigned long long)g_frames, (now - g_lastLog) / 1000.0, g_sum[0] / g_frames,
            g_twice ? g_sum[1] / g_frames : 0.0, avg(0), avg(1), avg(2));
        g_frames = 0;
        g_sum[0] = g_sum[1] = 0;
        for (int i = 0; i < 3; ++i) g_phase[i] = 0, g_phaseN[i] = 0;
        g_lastLog = now;
    }
}

extern "C" int64_t TimeHook() {
    if (g_freezeTime && g_lastTime) return g_lastTime;
    g_lastTime = g_timeFn();
    return g_lastTime;
}

extern "C" bool FrameHook(void* app) {
    t_pass = 0;
    const uint64_t n = ++g_frameCalls;
    const uint64_t last = g_lastMainView.load(std::memory_order_relaxed);
    const bool stableLongEnough = g_twiceNow.load(std::memory_order_relaxed) ||
                                  GetTickCount64() - g_mainViewRunStart.load(std::memory_order_relaxed) >= kMainViewStableMs;
    const bool twice = g_mode.load(std::memory_order_relaxed) == 2 && last && n - last <= kMainViewWindow &&
                       g_mainViewRun.load(std::memory_order_relaxed) >= kMainViewStable && stableLongEnough &&
                       !xr::HudMovieMode();
    if (!g_gameplaySeen.load(std::memory_order_relaxed) && last && n - last <= kMainViewWindow &&
        g_mainViewRun.load(std::memory_order_relaxed) >= kMainViewStable) {
        g_gameplaySeen = true;
        LOG("STEREO: gameplay camera stable for %llu frames (first gameplay)", (unsigned long long)kMainViewStable);
    }
    if (twice != g_twiceNow.load(std::memory_order_relaxed)) {
        g_twiceNow = twice;
        ++g_toggles;
        // a flood of these means the gameplay-camera gate flickers (every switch recreates the stereo swapchain)
        if (g_toggles <= 50 || g_toggles % 100 == 0)
            LOG("STEREO: %s (switch %ld)", twice ? "gameplay camera active: rendering both eyes" : "no gameplay camera: single frame",
                (long)g_toggles);
    }
    // Views per tick: 2 (left, right) in native stereo.
    const int views = twice ? 2 : 1;
    if (views != g_viewCount.load(std::memory_order_relaxed)) {
        if (views > 1) LOG("STEREO: %d views per tick", views);
        g_viewCount = views;
    }
    g_frameEye = twice ? 0 : -1;
    g_gameThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    camera_hook::OnFramePass();
    ULONG64 c0 = 0, c1 = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c0);
    double w0 = NowMs();
    bool r = g_frameFn(app);
    QueryThreadCycleTime(GetCurrentThread(), &c1);
    AddPass(twice ? 0 : 2, NowMs() - w0, c1 - c0);
    for (int v = 1; twice && r && v < views; ++v) {
        t_pass = v;
        g_frameEye = v;
        g_freezeTime = true;
        camera_hook::OnFramePass();
        QueryThreadCycleTime(GetCurrentThread(), &c0);
        w0 = NowMs();
        r = g_frameFn(app);
        QueryThreadCycleTime(GetCurrentThread(), &c1);
        AddPass(1, NowMs() - w0, c1 - c0);
        g_freezeTime = false;
    }
    t_pass = 0;
    return r;
}

// Redirect a 5-byte E8 call at `call` to `hook` through a near stub; returns the original target.
void* RedirectCall(uint8_t* call, void* hook) {
    if (call[0] != 0xE8) return nullptr;
    int32_t rel;
    memcpy(&rel, call + 1, 4);
    void* orig = call + 5 + rel;
    uint8_t* stub = patches::AllocCodeNear(call, 32);
    if (!stub) return nullptr;
    uint8_t code[12] = {0x48, 0xB8};
    memcpy(code + 2, &hook, 8);
    code[10] = 0xFF;
    code[11] = 0xE0;
    memcpy(stub, code, 12);
    FlushInstructionCache(GetCurrentProcess(), stub, 12);
    uint8_t patch[5] = {0xE8};
    int32_t nrel = static_cast<int32_t>(stub - (call + 5));
    memcpy(patch + 1, &nrel, 4);
    if (!patches::WriteCodeBytes(call, patch, 5)) return nullptr;
    return orig;
}

DWORD WINAPI ControlThread(LPVOID) {
    const std::wstring trigger = vr::g_gameDir + L"\\ff16vr_render2.trigger";
    for (;;) {
        Sleep(200);
        if (GetFileAttributesW(trigger.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        char buf[16] = {};
        if (FILE* f = _wfopen(trigger.c_str(), L"r")) {
            fgets(buf, sizeof(buf), f);
            fclose(f);
        }
        DeleteFileW(trigger.c_str());
        int mode = buf[0] >= '0' && buf[0] <= '3' ? buf[0] - '0' : 0;
        g_skip0 = mode == 3;
        if (mode == 3) mode = 2;
        if (mode == 2 && !(g_frameFn && g_timeFn)) mode = 0;
        g_mode = mode;
        g_twice = mode == 1;
        LOG("STEREO mode %d: %s", mode, mode == 0 ? "normal" : mode == 1 ? "Render() twice per frame"
                                                             : "whole frame twice, game clock frozen for the second");
    }
}

}  // namespace

int CurrentPass() { return t_pass; }
void* FrameFnAddr() { return reinterpret_cast<void*>(g_frameFn); }
unsigned long GameThreadId() { return g_gameThread.load(std::memory_order_relaxed); }
PassTotals TakePassTotals() {
    AcquireSRWLockExclusive(&g_totLock);
    PassTotals t = g_tot;
    g_tot = PassTotals{};
    ReleaseSRWLockExclusive(&g_totLock);
    return t;
}
int FrameEye() { return g_frameEye.load(std::memory_order_relaxed); }
bool NativeActive() { return g_twiceNow.load(std::memory_order_relaxed); }
int ViewCount() { return g_viewCount.load(std::memory_order_relaxed); }

void SetNativeMode(bool on) {
    if (on && !(g_frameFn && g_timeFn)) return;
    g_mode = on ? 2 : 0;
    g_twice = false;
    g_skip0 = false;
    LOG("STEREO: native stereo %s (menu)", on ? "on" : "off");
}
long ModeSwitches() { return g_toggles.load(std::memory_order_relaxed); }
bool GameplayReached() { return g_gameplaySeen.load(std::memory_order_relaxed); }

void NoteMainView() {
    const uint64_t n = g_frameCalls.load(std::memory_order_relaxed);
    const uint64_t last = g_lastMainView.load(std::memory_order_relaxed);
    if (n == last) return;  // second pass of the same tick
    const bool cont = last && n == last + 1;
    if (!cont) g_mainViewRunStart.store(GetTickCount64(), std::memory_order_relaxed);
    g_mainViewRun.store(cont ? g_mainViewRun.load(std::memory_order_relaxed) + 1 : 1, std::memory_order_relaxed);
    g_lastMainView.store(n, std::memory_order_relaxed);
}
bool SkipThisPresent() { return g_skip0.load(std::memory_order_relaxed) && t_pass == 0 && NativeActive(); }

void OnPresentBegin() {
    if (g_freqMs == 0) return;
    g_presentBegin = NowMs();
    if (g_renderEnd > 0) { g_phase[0] += g_presentBegin - g_renderEnd; ++g_phaseN[0]; g_renderEnd = 0; }
}

void OnPresentEnd() {
    if (g_freqMs == 0 || g_presentBegin == 0) return;
    g_presentEnd = NowMs();
    g_phase[1] += g_presentEnd - g_presentBegin;
    ++g_phaseN[1];
    g_presentBegin = 0;
}

void Install() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freqMs = (double)f.QuadPart / 1000.0;
    int count = 0;
    uint8_t* sig = patches::FindCode(kSite, &count);
    if (!sig) {
        LOG("STEREO: Render() call site not found (%d matches)", count);
        return;
    }
    uint8_t* site = sig + kPatchOffset;
    uint8_t* stub = patches::AllocCodeNear(site, 32);
    if (!stub) return;
    uint8_t code[12] = {0x48, 0xB8};  // mov rax, RenderHook ; jmp rax
    void* target = reinterpret_cast<void*>(&RenderHook);
    memcpy(code + 2, &target, 8);
    code[10] = 0xFF;
    code[11] = 0xE0;
    memcpy(stub, code, 12);
    FlushInstructionCache(GetCurrentProcess(), stub, 12);
    uint8_t patch[6] = {0xE8, 0, 0, 0, 0, 0x90};  // call stub ; nop  (rcx = renderer is passed through)
    int32_t rel = static_cast<int32_t>(stub - (site + 5));
    memcpy(patch + 1, &rel, 4);
    if (!patches::WriteCodeBytes(site, patch, 6)) return;
    LOG("STEREO: Render() hook at ffxvi.exe+0x%llX; ff16vr_render2.trigger (1/0) renders each frame twice",
        (unsigned long long)(site - patches::GameBase()));
    if (uint8_t* f = patches::FindCode(kFrameSite, &count)) {
        if (uint8_t* tm = patches::FindCode(kTimeSite, &count)) {
            g_frameFn = reinterpret_cast<Frame_t>(RedirectCall(f + kFrameCallOffset, reinterpret_cast<void*>(&FrameHook)));
            g_timeFn = reinterpret_cast<Time_t>(RedirectCall(tm + kTimeCallOffset, reinterpret_cast<void*>(&TimeHook)));
        }
    }
    LOG("STEREO: frame hook %s, game clock hook %s (trigger 2 = whole frame twice)", g_frameFn ? "ok" : "MISSING",
        g_timeFn ? "ok" : "MISSING");
    if (vr::g_config.vrMode == 2 && vr::g_config.vrOutput) {
        if (g_frameFn && g_timeFn) {
            g_mode = 2;
            LOG("STEREO: VRMode Native: each game tick renders two frames (left eye, then right eye)");
        } else {
            LOG("STEREO: VRMode Native needs the frame hooks; running Mono");
        }
    }
    HANDLE t = CreateThread(nullptr, 0, ControlThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

}  // namespace stereo_render
