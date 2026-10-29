#include "motion_blur.h"

#include <windows.h>

#include <cstring>

#include <algorithm>

#include <atomic>
#include <cstdint>
#include <cstdio>

#include "config.h"
#include "log.h"
#include "patches.h"
#include "stereo_render.h"
#include "render_size.h"
#include "vrkit_input.h"

// Motion blur off (DisableMotionBlur).
//
// With motion blur set to Off the game still runs its motion-blur passes, with the shutter scale divided by 1000
// (0.0035 instead of 3.5), so the blur is invisible. After a load the game builds its motion-blur parameters at full
// strength again and only re-applies the Off scale when the pause menu closes (game bug, unnoticed in flat play). In
// stereo the eye offset reads as motion, so the whole image smears until the player opens and closes the menu.
//
// The parameters are rebuilt from code every frame (patching the heap copies, runs 136-139, or the constant buffers,
// runs 141-155, did not reach the values the GPU uses), so the fix does what the player does: once the gameplay
// camera has been live for a few seconds after a load, a background thread checks the game's motion-blur parameter
// blocks in the heap (six floats: shutter, max blur, 3, 10, 4, 16). If they are at full strength, it opens and closes
// the pause menu (Start, Start through the XInput hook) and checks again. At most two tries per load.
namespace motion_blur {
namespace {

std::atomic<bool> g_started{false};

// ---- Code patch (run 221): the motion-blur strength lives in a float at [obj + 0x68] that the per-frame constant
// builder (+0xAF4088) reads at +0xAF44F3 (frame constants row 11 .w = 1 / it). Motion blur Off = 0.001; after a load
// the game resets it to 1.0 and only the pause menu's close puts 0.001 back. The hook at that read writes 0.001 when
// DisableMotionBlur is on, every frame, so no menu round trip is needed.
std::atomic<bool> g_patched{false};
std::atomic<long> g_forced{0};
constexpr char kScaleSite[] = "8B 81 24 12 00 00 89 87 B8 00 00 00 C4 C1 7A 10 46 68 C5 F8 54 C7";
constexpr int kScaleReadOffset = 12;  // vmovss xmm0, [r14 + 0x68]; vandps xmm0, xmm0, xmm7 (10 bytes)

extern "C" void BlurScaleHook(uint8_t* obj) {
    if (!obj || !vr::g_config.disableMotionBlur) return;
    float* v = reinterpret_cast<float*>(obj + 0x68);
    if (*v > 0.0011f && *v <= 10.0f) {
        const long n = ++g_forced;
        if (n <= 5 || n % 10000 == 0) LOG("Motion blur: strength %.4g -> 0.001 (Off) at %p (%ld times)", *v, (void*)v, n);
        *v = 0.001f;
    }
}

bool InstallScalePatch() {
    int count = 0;
    uint8_t* sig = patches::FindCode(kScaleSite, &count);
    if (!sig) {
        LOG("Motion blur: strength read site not found (%d matches); using the menu round trip", count);
        return false;
    }
    uint8_t* at = sig + kScaleReadOffset;
    uint8_t* stub = patches::AllocCodeNear(at, 256);
    if (!stub) return false;
    void* fn = reinterpret_cast<void*>(&BlurScaleHook);
    uint8_t code[256];
    size_t n = 0;
    auto put = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) code[n++] = x; };
    put({0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00});  // sub rsp, 0C0h (rsp stays 16-aligned)
    for (uint8_t i = 0; i < 6; ++i) put({0xC5, 0xFA, 0x7F, (uint8_t)(0x44 | (i << 3)), 0x24, (uint8_t)(0x20 + 16 * i)});  // vmovdqu [rsp+20h+16i], xmm_i
    put({0x48, 0x89, 0x84, 0x24, 0x80, 0, 0, 0});  // mov [rsp+80h], rax
    put({0x48, 0x89, 0x8C, 0x24, 0x88, 0, 0, 0});  // rcx
    put({0x48, 0x89, 0x94, 0x24, 0x90, 0, 0, 0});  // rdx
    put({0x4C, 0x89, 0x84, 0x24, 0x98, 0, 0, 0});  // r8
    put({0x4C, 0x89, 0x8C, 0x24, 0xA0, 0, 0, 0});  // r9
    put({0x4C, 0x89, 0x94, 0x24, 0xA8, 0, 0, 0});  // r10
    put({0x4C, 0x89, 0x9C, 0x24, 0xB0, 0, 0, 0});  // r11
    put({0x4C, 0x89, 0xF1});                        // mov rcx, r14
    put({0x48, 0xB8}); memcpy(code + n, &fn, 8); n += 8;  // mov rax, BlurScaleHook
    put({0xFF, 0xD0});                              // call rax
    for (uint8_t i = 0; i < 6; ++i) put({0xC5, 0xFA, 0x6F, (uint8_t)(0x44 | (i << 3)), 0x24, (uint8_t)(0x20 + 16 * i)});  // vmovdqu xmm_i, [...]
    put({0x48, 0x8B, 0x84, 0x24, 0x80, 0, 0, 0});
    put({0x48, 0x8B, 0x8C, 0x24, 0x88, 0, 0, 0});
    put({0x48, 0x8B, 0x94, 0x24, 0x90, 0, 0, 0});
    put({0x4C, 0x8B, 0x84, 0x24, 0x98, 0, 0, 0});
    put({0x4C, 0x8B, 0x8C, 0x24, 0xA0, 0, 0, 0});
    put({0x4C, 0x8B, 0x94, 0x24, 0xA8, 0, 0, 0});
    put({0x4C, 0x8B, 0x9C, 0x24, 0xB0, 0, 0, 0});
    put({0x48, 0x81, 0xC4, 0xC0, 0x00, 0x00, 0x00});  // add rsp, 0C0h
    memcpy(code + n, at, 10); n += 10;               // the original vmovss / vandps
    put({0xFF, 0x25, 0, 0, 0, 0});                   // jmp [rip]
    uint8_t* back = at + 10;
    memcpy(code + n, &back, 8); n += 8;
    memcpy(stub, code, n);
    FlushInstructionCache(GetCurrentProcess(), stub, n);
    uint8_t patch[10] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
    const int32_t rel = static_cast<int32_t>(stub - (at + 5));
    memcpy(patch + 1, &rel, 4);
    if (!patches::WriteCodeBytes(at, patch, sizeof(patch))) return false;
    LOG("Motion blur: strength hook at ffxvi.exe+0x%llX (stub %p)", (unsigned long long)(at - patches::GameBase()), (void*)stub);
    return true;
}
std::atomic<int> g_roundTrips{0};
std::atomic<int> g_lastFull{-1};  // full-strength blocks found by the last check
char g_state[64] = "waiting for gameplay";

// Parameter block: shutter s, max blur m = 30 s, then four small whole numbers (sample counts / tile sizes; 3, 10, 4, 16
// on the test PC's settings, other quality settings may differ). Full strength: s >= 0.05 (3.5); Off: s / 1000.
struct Counts {
    int full = 0, off = 0;
    uint32_t tail[4] = {};    // tail of the first full-strength block (diagnostics)
    float s = 0;
};

bool SmallInt(float v) { return v >= 1.0f && v <= 256.0f && v == (float)(int)v; }

void CountRange(const uint8_t* base, size_t size, Counts* c) {
    const float* f = reinterpret_cast<const float*>(base);
    const size_t n = size / 4;
    for (size_t i = 0; i + 6 <= n; ++i) {
        const float s = f[i];
        if (!(s > 0.0005f && s < 100.0f) || s == 1.0f) continue;  // s = 1: other data (run 174: 1487 hits, 1 / 30 / 20 16 16 1)
        const float m = f[i + 1];
        if (!(m > 29.9f * s && m < 30.1f * s)) continue;
        if (!SmallInt(f[i + 2]) || !SmallInt(f[i + 3]) || !SmallInt(f[i + 4]) || !SmallInt(f[i + 5])) continue;
        // Other data also has m = 30 s and whole numbers after it (runs 163-164: 0.819 / 1, 1, 1, 1 and 1 / 20, 1, 1, 1).
        // The motion-blur tail is four different settings (3, 10, 4, 16): require at least 3 distinct values.
        {
            const float t[4] = {f[i + 2], f[i + 3], f[i + 4], f[i + 5]};
            int distinct = 0;
            for (int a = 0; a < 4; ++a) {
                bool seen = false;
                for (int b = 0; b < a; ++b) seen |= t[a] == t[b];
                distinct += !seen;
            }
            if (distinct < 3) continue;
        }
        if (s >= 0.05f) {
            if (!c->full++) {
                c->s = s;
                for (int k = 0; k < 4; ++k) c->tail[k] = (uint32_t)f[i + 2 + k];
            }
        } else {
            ++c->off;
        }
    }
}

void SafeCount(const uint8_t* base, size_t size, Counts* c) {
    __try {
        CountRange(base, size, c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Full-strength parameter blocks in the private heap (~1-2 s for 8 GB at low priority).
Counts g_counts;

// Heap regions where parameter blocks turned up before: checked first (a few ms instead of seconds).
struct Region {
    uint8_t* base;
    size_t size;
};
Region g_regions[32];
int g_nRegions = 0;

void NoteRegion(uint8_t* base, size_t size) {
    for (int i = 0; i < g_nRegions; ++i)
        if (g_regions[i].base == base) return;
    if (g_nRegions < 32) g_regions[g_nRegions++] = {base, size};
}

// Quick count over the remembered regions (still committed and read-write). -1 if there are none.
int QuickCount(Counts* c) {
    if (!g_nRegions) return -1;
    MEMORY_BASIC_INFORMATION mbi;
    for (int i = 0; i < g_nRegions; ++i) {
        if (!VirtualQuery(g_regions[i].base, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & 0xFF) != PAGE_READWRITE || mbi.BaseAddress != g_regions[i].base)
            continue;
        SafeCount(g_regions[i].base, std::min(mbi.RegionSize, g_regions[i].size), c);
    }
    return c->full;
}

int CountFullStrength() {
    Counts c;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    auto* p = static_cast<uint8_t*>(si.lpMinimumApplicationAddress);
    auto* end = static_cast<uint8_t*>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi;
    while (p < end && VirtualQuery(p, &mbi, sizeof(mbi))) {
        auto* base = static_cast<uint8_t*>(mbi.BaseAddress);
        const DWORD prot = mbi.Protect & 0xFF;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && prot == PAGE_READWRITE &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_WRITECOMBINE | PAGE_NOCACHE)) && mbi.RegionSize < (64ull << 30))
        {
            const int before = c.full + c.off;
            SafeCount(base, mbi.RegionSize, &c);
            if (c.full + c.off > before) NoteRegion(base, mbi.RegionSize);
        }
        p = base + mbi.RegionSize;
    }
    g_counts = c;
    return c.full;
}

void SetState(const char* s) { snprintf(g_state, sizeof(g_state), "%s", s); }

constexpr unsigned short kStart = 0x0010;  // XINPUT_GAMEPAD_START
constexpr int kMinBlocks = 80;  // blurry: ~280-340 blocks; after the fix 0-50 stale ones (runs 156, 157)

// Waits until the gameplay camera has been live for ms milliseconds without a break. False if it went away for
// longer than a page flip (camera gone > 20 s counts as a new load).
void WaitLive(DWORD ms) {
    DWORD since = 0;
    for (;;) {
        Sleep(250);
        if (!stereo_render::NativeActive()) {
            since = 0;
            continue;
        }
        if (!since) since = GetTickCount();
        if (GetTickCount() - since >= ms) return;
    }
}

DWORD WINAPI Thread(void*) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    long lastSwitches = -1, lastResizes = -1;
    DWORD lastCheck = 0;
    for (;;) {
        // Checks: each new gameplay stretch (after loads, menus, cutscenes), after resolution changes (the game rebuilds
        // its post-processing and also forgets the Off scale), and every 30 s.
        // 0.5 s of live gameplay camera is enough for the game to have built (or not) its motion-blur parameters.
        WaitLive(500);
        const long sw = stereo_render::ModeSwitches(), rs = render_size::ResizeCount();
        const char* why = sw != lastSwitches ? "new gameplay stretch" : rs != lastResizes ? "resolution change"
                          : GetTickCount() - lastCheck > 30000 ? "periodic" : nullptr;
        if (!why || !vr::g_config.disableMotionBlur || !vr::g_config.vrOutput) {
            Sleep(1000);
            continue;
        }
        lastSwitches = sw;
        lastResizes = rs;
        lastCheck = GetTickCount();
        int prevFull = -1;
        for (int tries = 0;; ++tries) {
            const DWORD t0 = GetTickCount();
            // Quick check in the heap regions that held parameter blocks before; a full heap scan (seconds) only
            // when that is inconclusive.
            int full;
            {
                Counts q;
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
                const int qf = QuickCount(&q);
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
                if (qf >= kMinBlocks || (qf >= 0 && q.off >= kMinBlocks)) {
                    g_counts = q;
                    full = qf;
                } else {
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);  // first scan of a session
                    full = CountFullStrength();
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
                }
            }
            g_lastFull = full;
            if (full < kMinBlocks) {
                LOG("Motion blur: %d full-strength / %d off parameter blocks (%lu ms, %s): off as set", full, g_counts.off,
                GetTickCount() - t0, why);
                SetState(tries ? "off (after menu round trip)" : "off (as set)");
                break;
            }
            // Periodic checks only report: during scripted sequences the game turns full motion blur on by itself
            // (user log Oct 28: 3000+ blocks in the new-game action sequence; menu round trips paused the game and
            // did not help).
            if (!strcmp(why, "periodic")) {
                LOG("Motion blur: %d full-strength / %d off parameter blocks (%lu ms, periodic): left alone (only new "
                    "gameplay stretches and resolution changes get the menu fix)", full, g_counts.off, GetTickCount() - t0);
                SetState("ON (periodic check, left alone)");
                break;
            }
            if (prevFull >= 0 && full * 2 > prevFull) {
                LOG("Motion blur: still %d full-strength blocks after a menu round trip (was %d): the game wants it here; "
                    "no more round trips until the next stretch", full, prevFull);
                SetState("ON (menu round trip did not help)");
                break;
            }
            prevFull = full;
            if (tries >= 2) {
                LOG("Motion blur: still %d full-strength blocks after %d menu round trips; giving up until the next load",
                    full, tries);
                SetState("ON (menu round trip did not help)");
                break;
            }
            if (g_patched) {
                LOG("Motion blur: %d full-strength / %d off parameter blocks (%lu ms, %s) with the strength patch; no menu "
                    "round trip", full, g_counts.off, GetTickCount() - t0, why);
                SetState("ON despite the patch");
                break;
            }
            if (!stereo_render::NativeActive()) break;  // camera went away during the scan: check the next stretch
            LOG("Motion blur: %d full-strength / %d off parameter blocks (%lu ms, %s; shutter %.3g, tail %u %u %u %u): the "
                "game left motion blur on; pause menu round trip %d (menu open %d ms)",
                full, g_counts.off, GetTickCount() - t0, why, g_counts.s, g_counts.tail[0], g_counts.tail[1],
                g_counts.tail[2], g_counts.tail[3], tries + 1, vr::g_config.blurMenuFixHoldMs);
            vrkit::input::InstallHooks();
            vrkit::input::Inject(kStart, 120);
            // let the menu open (BlurMenuFixHoldMs, default 1000; 700 ms needed a second round trip in run 156)
            Sleep((DWORD)std::clamp(vr::g_config.blurMenuFixHoldMs, 10, 1000));
            vrkit::input::Inject(kStart, 120);
            ++g_roundTrips;
            WaitLive(5000);  // give the stale pre-fix blocks time to be overwritten
            lastSwitches = stereo_render::ModeSwitches();  // the round trip itself is not a new stretch
            lastCheck = GetTickCount();
        }
    }
}

}  // namespace

void Start() {
    if (g_started.exchange(true)) return;
    g_patched = InstallScalePatch();
    if (HANDLE h = CreateThread(nullptr, 0, &Thread, nullptr, 0, nullptr)) CloseHandle(h);
    LOG("Motion blur: watcher started (DisableMotionBlur=%d)", vr::g_config.disableMotionBlur);
}

const char* Status() {
    static char buf[128];
    if (!vr::g_config.disableMotionBlur) return "motion blur: game setting";
    snprintf(buf, sizeof(buf), "motion blur %s (%s, forced %ld, %d menu round trips, last check %d)", g_state,
             g_patched ? "patched" : "menu fix", g_forced.load(), g_roundTrips.load(), g_lastFull.load());
    return buf;
}

}  // namespace motion_blur
