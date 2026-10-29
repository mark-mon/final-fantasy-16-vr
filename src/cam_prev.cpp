// Per-eye previous camera in the GPU camera constants (PerEyeMotionVectors).
//
// RenderView::Update builds the cCamera constant buffer (0x900 bytes) for each view at ffxvi.exe+0xAA44EE..+0xAA46C4:
//   cb+0x000 = CameraParam ring[+0xE75] (current), cb+0x400 = ring[+0xE76] (previous), cb+0x800.. reprojection,
//   cb+0x890 = jitter delta (current - previous CameraParam +0x2F0), then +0x9E08F8 finalises it.
// The ring has two entries, so in native stereo "previous" is always the other eye's camera. The upscaler's velocity
// resolve and the G-buffer's object motion use that previous CameraParam, so every motion vector carries the eye
// separation. With shared upscaler history (FSR 3, DLSS without PerEyeDlss) that is roughly what the history needs,
// but with per-eye DLSS history (viewport + 0x100) each eye needs its own previous frame, or DLSS rejects its history
// (noisy, low-resolution image).
//
// The hook sits on the finalise call: when per-eye DLSS is evaluating, the previous half of the main view's buffer is
// replaced with the same eye's previous CameraParam (kept here per eye), and the jitter delta is recomputed.
// The eye is taken from the CameraParam itself (eye position row 38 vs camera node row 37 along the right axis).
#include "cam_prev.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "camera_hook.h"
#include "config.h"
#include "log.h"
#include "patches.h"
#include "sl_hooks.h"
#include "stereo_render.h"

namespace cam_prev {
namespace {

constexpr char kFinaliseSite[] = "48 89 83 90 08 00 00 48 8B 8F 10 0E 00 00 C5 F8 77 E8";
constexpr int kFinaliseCallOffset = 17;
constexpr size_t kParam = 0x400;

struct EyeHist {
    alignas(16) uint8_t last[kParam];
    alignas(16) uint8_t prev[kParam];
    bool haveLast = false, havePrev = false;
    unsigned long long serial = 0;  // frame pass of the last update
};
EyeHist g_eye[2];
std::mutex g_mutex;
std::atomic<int> g_mode{1};  // dev: 0 off, 1 auto (per-eye DLSS evaluating), 2 always
std::atomic<long> g_calls{0}, g_patched{0}, g_logs{0};

const float* Row(const uint8_t* p, int r) { return reinterpret_cast<const float*>(p + r * 16); }

// eye of a CameraParam: -1 unknown, 0 left, 1 right
int EyeOf(const uint8_t* cp, float* lateral) {
    const float* r37 = Row(cp, 37);
    const float* r38 = Row(cp, 38);
    const float right[3] = {Row(cp, 11)[0], Row(cp, 12)[0], Row(cp, 13)[0]};
    const float lat = (r38[0] - r37[0]) * right[0] + (r38[1] - r37[1]) * right[1] + (r38[2] - r37[2]) * right[2];
    *lateral = lat;
    if (!std::isfinite(lat) || std::fabs(lat) < 0.005f || std::fabs(lat) > 1.0f) return -1;
    return lat < 0 ? 0 : 1;
}

float Dist(const uint8_t* a, const uint8_t* b) {  // camera positions (camera-to-world rows 11..13 .w)
    float d2 = 0;
    for (int r = 11; r <= 13; ++r) {
        const float d = Row(a, r)[3] - Row(b, r)[3];
        d2 += d * d;
    }
    return std::sqrt(d2);
}

}  // namespace

bool Active() {
    const int m = g_mode.load();
    if (m == 0 || !vr::g_config.perEyeMotionVectors || !stereo_render::NativeActive()) return false;
    return m == 2 || (vr::g_config.perEyeDlss && sl_hooks::DlssEvaluatingRecently());
}

extern "C" void CamCbFinalise(uint8_t* cb, uint8_t* view) {
    if (!cb || !view || view != camera_hook::MainView()) return;
    ++g_calls;
    if (!stereo_render::NativeActive()) return;
    float lat = 0;
    const int eye = EyeOf(cb, &lat);
    if (eye < 0) return;  // not a stereo eye camera (other views, mono)
    if (!memcmp(cb, cb + kParam, kParam)) return;  // the view runs without history (previous == current)
    std::lock_guard<std::mutex> lock(g_mutex);
    EyeHist& h = g_eye[eye];
    // Keep each eye's history whether or not the patch is active, so switching it on works at once.
    const unsigned long long serial = camera_hook::PassSerial();
    if (!h.haveLast || memcmp(cb, h.last, kParam)) {
        if (h.haveLast) {
            memcpy(h.prev, h.last, kParam);
            h.havePrev = true;
        }
        memcpy(h.last, cb, kParam);
        h.haveLast = true;
        h.serial = serial;
    } else if (serial != h.serial) {
        memcpy(h.prev, h.last, kParam);  // a new frame with the same camera: no motion
        h.havePrev = true;
        h.serial = serial;
    }  // else: the same buffer built again in this pass: same previous
    if (!h.havePrev || !Active()) return;
    const float before = Dist(cb, cb + kParam), after = Dist(cb, h.prev);
    if (after > 2.0f) return;  // a camera cut / teleport: leave the game's value
    memcpy(cb + kParam, h.prev, kParam);
    // jitter delta (cb+0x890): current - previous jitter (CameraParam +0x2F0)
    const float* jc = reinterpret_cast<const float*>(cb + 0x2F0);
    const float* jp = reinterpret_cast<const float*>(h.prev + 0x2F0);
    float* jd = reinterpret_cast<float*>(cb + 0x890);
    jd[0] = jc[0] - jp[0];
    jd[1] = jc[1] - jp[1];
    const long n = ++g_patched;
    if (g_logs.load() < 8 || n % 5000 == 0) {
        ++g_logs;
        LOG("Per-eye motion vectors: eye %d (lateral %.4f m) previous camera %.4f m away -> %.4f m (same eye), jitter "
            "%.3f %.3f -> delta %.3f %.3f; %ld patched of %ld (thread %lu)",
            eye, lat, before, after, jc[0], jc[1], jd[0], jd[1], n, g_calls.load(), GetCurrentThreadId());
    }
}

void SetMode(int m) {
    g_mode = m;
    LOG("Per-eye motion vectors: mode %d (0 off, 1 with per-eye DLSS, 2 always)", m);
}

const char* Status() {
    static char buf[96];
    snprintf(buf, sizeof(buf), "per-eye motion vectors %s, %ld patched", Active() ? "on" : "idle", g_patched.load());
    return buf;
}

void Install() {
    int count = 0;
    uint8_t* sig = patches::FindCode(kFinaliseSite, &count);
    if (!sig) {
        LOG("Per-eye motion vectors: camera buffer site not found (%d matches)", count);
        return;
    }
    uint8_t* call = sig + kFinaliseCallOffset;
    int32_t rel = 0;
    memcpy(&rel, call + 1, 4);
    void* orig = call + 5 + rel;
    uint8_t* stub = patches::AllocCodeNear(call, 128);
    if (!stub) return;
    void* fn = reinterpret_cast<void*>(&CamCbFinalise);
    uint8_t code[96];
    size_t n = 0;
    auto put = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) code[n++] = x; };
    // entry: rsp = 8 mod 16 (return address); 4 pushes + 0x28 -> aligned call with shadow space
    put({0x51});                          // push rcx
    put({0x52});                          // push rdx
    put({0x41, 0x50});                    // push r8
    put({0x41, 0x51});                    // push r9
    put({0x48, 0x83, 0xEC, 0x28});        // sub rsp, 28h
    put({0x48, 0x89, 0xD9});              // mov rcx, rbx (cbuffer)
    put({0x48, 0x89, 0xFA});              // mov rdx, rdi (RenderView)
    put({0x48, 0xB8}); memcpy(code + n, &fn, 8); n += 8;    // mov rax, CamCbFinalise
    put({0xFF, 0xD0});                    // call rax
    put({0x48, 0x83, 0xC4, 0x28});        // add rsp, 28h
    put({0x41, 0x59});                    // pop r9
    put({0x41, 0x58});                    // pop r8
    put({0x5A});                          // pop rdx
    put({0x59});                          // pop rcx
    put({0x48, 0xB8}); memcpy(code + n, &orig, 8); n += 8;  // mov rax, original
    put({0xFF, 0xE0});                    // jmp rax
    memcpy(stub, code, n);
    FlushInstructionCache(GetCurrentProcess(), stub, n);
    uint8_t patch[5] = {0xE8};
    const int32_t newRel = static_cast<int32_t>(stub - (call + 5));
    memcpy(patch + 1, &newRel, 4);
    if (!patches::WriteCodeBytes(call, patch, 5)) return;
    LOG("Per-eye motion vectors: hook at ffxvi.exe+0x%llX (finalise ffxvi.exe+0x%llX)",
        (unsigned long long)(call - patches::GameBase()), (unsigned long long)((uint8_t*)orig - patches::GameBase()));
}

}  // namespace cam_prev
