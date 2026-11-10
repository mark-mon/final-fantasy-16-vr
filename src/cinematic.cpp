// In-engine cinematics (user, 2026-11-08: an early-game cinematic rendered through the gameplay camera, so it played
// head-tracked and huge instead of on the theater screen).
//
// The game's frame-rate limiter knows when a cutscene or photo mode runs (UncapCutsceneFPS patches its 30 fps cap): at
// ffxvi.exe+0x84DB21 (1.03) it stores the cutscene query's result, nonzero while one runs, to limiter+0x1AB8, and the
// limiter function at +0x69AA18 (rcx = the limiter) picks the cutscene rate when +0x1AAD is set. The store happens with
// or without the patch. We hook the limiter function to learn the object and read +0x1AB8.
#include "cinematic.h"

#include <windows.h>

#include <atomic>
#include <cstdint>

#include "MinHook.h"
#include "config.h"
#include "log.h"
#include "patches.h"

namespace cinematic {
namespace {

// The limiter's per-frame function: mov [rsp+8],rbx; push rdi; sub rsp,30h; mov r8,[rip+..]; xor ebx,ebx; vmovaps
// [rsp+20h],xmm6; mov r11,rcx
constexpr const char* kLimiterFn = "48 89 5C 24 08 57 48 83 EC 30 4C 8B 05 ?? ?? ?? ?? 33 DB C5 F8 29 74 24 20 4C 8B D9";
constexpr size_t kCutsceneRef = 0x1AB8;

// Returns whatever it returns (rax passed through); second argument in xmm1.
using Fn = uint64_t(__fastcall*)(void*, float);
Fn o_limiter = nullptr;
std::atomic<uint8_t*> g_obj{nullptr};
std::atomic<bool> g_seenIdle{false};  // safety: only trusted once it has been zero (no cinematic) at least once
std::atomic<bool> g_active{false};
std::atomic<bool> g_postCine{false};  // the post-process flags word has the scene bit
constexpr uint32_t kSceneBit = 0x200;

bool Read(uint8_t* obj) {
    uint64_t v = 0;
    __try {
        v = *reinterpret_cast<volatile uint64_t*>(obj + kCutsceneRef);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return v != 0;
}

void HookBink();
void UpdateMovie();

uint64_t __fastcall Hk_Limiter(void* self, float x) {
    HookBink();
    UpdateMovie();
    if (self) {
        uint8_t* obj = static_cast<uint8_t*>(self);
        if (g_obj.exchange(obj) != obj) LOG("Cinematic: frame limiter at %p", self);
        const bool on = Read(obj);
        if (!on) g_seenIdle = true;
        if (g_seenIdle.load() && on != g_active.load()) {
            g_active = on;
            LOG("Cinematic: cutscene state %s", on ? "on" : "off");
        }
    }
    // Survey (user log 2026-11-08: the limiter's cutscene state stayed off through an in-engine cinematic): the
    // post-process flags word, whose bits 0x1B pick the cinematic look. Logged when it changes.
    {
        static uint32_t s_word = 0xFFFFFFFEu;
        static int s_logs = 0;
        const uint32_t w = patches::PostModeWord();
        if (w != s_word) {
            s_word = w;
            // Bit 0x200: set through the user's in-engine cinematic (0x2203, log 2026-11-08 04:39), clear in gameplay
            // on the test PC (0x2003, run 269).
            const bool cine = w != 0xFFFFFFFFu && (w & kSceneBit) != 0;
            if (cine != g_postCine.load()) LOG("Cinematic: scene bit %s (post-process flags 0x%08X)", cine ? "on" : "off", w);
            g_postCine = cine;
            if (s_logs++ < 400) LOG("Cinematic: post-process flags 0x%08X", w);
        }
    }
    return o_limiter(self, x);
}

// Pre-rendered movies (user, 2026-11-08: a sequence of two flat movies then an in-engine cinematic; the scene bit was set
// through all three). The game plays movies with Bink 2 (bink2w64.dll): a movie runs while full-size Bink frames are
// being decoded. Small Bink videos (UI) don't count.
// Bink 2 entry points that run once per decoded frame (games use BinkDoFrame or BinkDoFrameAsync, then BinkNextFrame).
// All take the HBINK first; HBINK starts with { U32 Width, Height, ... }. One thunk per entry point, three integer
// arguments passed through (x64: the same registers whatever the real count).
using BinkFn = int64_t(__fastcall*)(void*, uint64_t, uint64_t);
std::atomic<ULONGLONG> g_lastMovieFrame{0};
std::atomic<bool> g_binkHooked{false};
std::atomic<bool> g_movie{false};

void NoteBink(void* bink) {
    if (!bink) return;
    uint32_t w = 0, h = 0;
    __try {
        w = static_cast<const uint32_t*>(bink)[0];
        h = static_cast<const uint32_t*>(bink)[1];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    static std::atomic<uint64_t> s_wh{0};
    const uint64_t wh = (uint64_t)w << 32 | h;
    if (s_wh.exchange(wh) != wh) LOG("Cinematic: Bink video %u x %u", w, h);
    if (w >= 1280 && h >= 540 && w <= 16384 && h <= 16384) g_lastMovieFrame = GetTickCount64();
}

template <int N>
struct BinkThunk {
    static inline BinkFn orig = nullptr;
    static int64_t __fastcall Hook(void* bink, uint64_t a, uint64_t b) {
        NoteBink(bink);
        return orig(bink, a, b);
    }
};

template <int N>
void HookOne(HMODULE m, const char* name) {
    void* fn = reinterpret_cast<void*>(GetProcAddress(m, name));
    if (!fn) return;
    if (MH_CreateHook(fn, reinterpret_cast<void*>(&BinkThunk<N>::Hook), reinterpret_cast<void**>(&BinkThunk<N>::orig)) == MH_OK &&
        MH_EnableHook(fn) == MH_OK)
        LOG("Cinematic: %s hooked", name);
    else
        LOG("Cinematic: %s hook failed", name);
}

void HookBink() {
    if (g_binkHooked.load()) return;
    HMODULE b = GetModuleHandleW(L"bink2w64.dll");
    if (!b) return;
    g_binkHooked = true;
    HookOne<0>(b, "BinkDoFrame");
    HookOne<1>(b, "BinkDoFrameAsync");
    HookOne<2>(b, "BinkNextFrame");
}

void UpdateMovie() {
    const ULONGLONG last = g_lastMovieFrame.load();
    const bool on = last && GetTickCount64() - last < 400;  // movies decode at 24-30 fps
    if (on != g_movie.exchange(on)) LOG("Cinematic: movie %s", on ? "playing: theater screen (16:9)" : "ended");
}

}  // namespace

bool MoviePlaying() {
    UpdateMovie();
    return g_movie.load();
}

// Movies only: the limiter's cutscene state and the post-process scene bit are also set during in-engine cinematics,
// which work in VR (user, 2026-11-08); they are logged only.
bool Active() { return vr::g_config.cinematicTheater && MoviePlaying(); }

void Install() {
    int count = 0;
    uint8_t* fn = patches::FindCode(kLimiterFn, &count);
    if (!fn) {
        LOG("Cinematic: frame limiter not found (%d matches); cinematics through the gameplay camera stay head-tracked", count);
        return;
    }
    if (MH_CreateHook(fn, reinterpret_cast<void*>(&Hk_Limiter), reinterpret_cast<void**>(&o_limiter)) != MH_OK ||
        MH_EnableHook(fn) != MH_OK) {
        LOG("Cinematic: hook failed");
        return;
    }
    LOG("Cinematic: frame limiter hooked at ffxvi.exe+0x%llX", (unsigned long long)(fn - reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr))));
}

}  // namespace cinematic
