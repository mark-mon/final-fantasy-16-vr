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
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "camera_hook.h"
#include "config.h"
#include "log.h"
#include "patches.h"
#include "ffx_hooks.h"
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
    alignas(16) uint8_t used[kParam];  // the previous camera this eye's last render used (its motion vectors point there)
    alignas(16) uint8_t game[kParam];  // the previous camera the game put there (before any substitution)
    bool haveUsed = false;
    bool haveLast = false, havePrev = false;
    unsigned long long serial = 0;  // frame pass of the last update
};
EyeHist g_eye[2];
constexpr int kRing = 8;
CamEntry g_ring[2][kRing];  // recent cameras per eye (most recent at g_ringNext - 1)
int g_ringNext[2] = {};
std::mutex g_mutex;
std::atomic<int> g_mode{1};  // dev: 0 off, 1 auto (per-eye DLSS evaluating), 2 always
std::atomic<long> g_calls{0}, g_patched{0}, g_logs{0};
std::atomic<int> g_cbDump{0};
std::atomic<int> g_trace{0};  // dev: log the next N finalise calls
std::atomic<float> g_lat[2] = {0.f, 0.f};  // last lateral eye offset per eye (game units)  // dev: dump the next cCamera buffers (per eye) to the game folder, before / after our change
int g_cbDumpN = 0;
void DumpCb(const uint8_t* cb, int eye, const char* when) {
    const std::wstring path = vr::g_gameDir + L"\\ff16vr_ccamera" + std::to_wstring(g_cbDumpN) + L"_eye" + std::to_wstring(eye) +
                              L"_" + std::wstring(when, when + strlen(when)) + L".bin";
    if (FILE* f = _wfopen(path.c_str(), L"wb")) {
        fwrite(cb, 1, 0x900, f);
        fclose(f);
    }
}

const float* Row(const uint8_t* p, int r) { return reinterpret_cast<const float*>(p + r * 16); }

// eye of a CameraParam: -1 unknown, 0 left, 1 right. The lateral offset of the eye camera from the camera node is the
// eye's half IPD plus the head's own sideways offset from the node (leaning, or turning the head: the eyes swing around
// the neck), so a plain sign test misclassifies an eye once that offset passes half the IPD (then that eye's camera
// history froze while the head moved: FSR right-eye noise and the frame warp's drift at large turns, user Nov 10).
// Classify against the midpoint of the two most recent distinct offsets (consecutive cameras alternate eyes).
float g_latRecent[2] = {0, 0};
int g_latCount = 0;
std::mutex g_latMutex;
int EyeOf(const uint8_t* cp, float* lateral) {
    std::lock_guard<std::mutex> lock(g_latMutex);  // job threads
    const float* r37 = Row(cp, 37);
    const float* r38 = Row(cp, 38);
    const float right[3] = {Row(cp, 11)[0], Row(cp, 12)[0], Row(cp, 13)[0]};
    const float lat = (r38[0] - r37[0]) * right[0] + (r38[1] - r37[1]) * right[1] + (r38[2] - r37[2]) * right[2];
    *lateral = lat;
    if (!std::isfinite(lat) || std::fabs(lat) > 1.0f) return -1;
    int eye = -1;
    if (g_latCount >= 2) {
        const float mid = 0.5f * (g_latRecent[0] + g_latRecent[1]);
        const float sep = std::fabs(g_latRecent[0] - g_latRecent[1]);
        if (sep > 0.02f && sep < 0.2f && std::fabs(lat - mid) > 0.25f * sep) eye = lat < mid ? 0 : 1;
    }
    if (eye < 0 && std::fabs(lat) >= 0.005f) eye = lat < 0 ? 0 : 1;  // no usable history yet: the sign
    if (eye >= 0 && (g_latCount == 0 || std::fabs(lat - g_latRecent[0]) > 1e-5f)) {  // a new camera (not the same rebuilt)
        g_latRecent[1] = g_latRecent[0];
        g_latRecent[0] = lat;
        if (g_latCount < 2) ++g_latCount;
    }
    return eye;
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
    return m == 2 || (vr::g_config.perEyeDlss && sl_hooks::DlssEvaluatingRecently()) ||
           (vr::g_config.perEyeFsr && ffx_hooks::FsrEvaluatingRecently());
}

extern "C" void CamCbFinalise(uint8_t* cb, uint8_t* view) {
    if (!cb || !view || view != camera_hook::MainView()) return;
    ++g_calls;
    if (!stereo_render::NativeActive()) return;
    float lat = 0;
    const int eye = EyeOf(cb, &lat);
    if (eye < 0) return;  // not a stereo eye camera (other views, mono)
    g_lat[eye] = lat;
    if (!memcmp(cb, cb + kParam, kParam)) return;  // the view runs without history (previous == current)
    std::lock_guard<std::mutex> lock(g_mutex);
    EyeHist& h = g_eye[eye];
    const bool dump = g_cbDump.load() > 0;
    if (dump) DumpCb(cb, eye, "before");
    struct AfterDump {
        const uint8_t* cb; int eye; bool on;
        ~AfterDump() { if (on) { DumpCb(cb, eye, "after"); if (eye == 1) { ++g_cbDumpN; --g_cbDump; } } }
    } afterDump{cb, eye, dump};
    static bool s_threadLogged = false;
    if (!s_threadLogged) {
        s_threadLogged = true;
        LOG("Per-eye motion vectors: camera constants are finalised on this thread (eye %d)", eye);  // vs the upscaler's
    }
    // Keep each eye's history whether or not the patch is active, so switching it on works at once.
    const unsigned long long serial = camera_hook::PassSerial();
    if (g_trace.load() > 0) {
        --g_trace;
        const float* jc = reinterpret_cast<const float*>(cb + 0x2F0);
        LOG("Camera trace: eye %d pass %llu pos %.4f %.4f %.4f (moved %.2f mm from this eye's last, game prev %.2f mm away) jitter %.3f %.3f "
            "thread %lu",
            eye, serial, Row(cb, 11)[3], Row(cb, 12)[3], Row(cb, 13)[3], h.haveLast ? Dist(cb, h.last) * 1000.f : -1.f,
            Dist(cb, cb + kParam) * 1000.f, jc[0], jc[1], GetCurrentThreadId());
    }
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
    memcpy(h.used, cb + kParam, kParam);
    memcpy(h.game, cb + kParam, kParam);
    h.haveUsed = true;
    {
        // the ring: one entry per pass (the same camera built again in a pass replaces its entry)
        const int lastIx = (g_ringNext[eye] + kRing - 1) % kRing;
        CamEntry* e = &g_ring[eye][lastIx];
        if (!e->valid || e->serial != serial) {
            e = &g_ring[eye][g_ringNext[eye] % kRing];
            ++g_ringNext[eye];
        }
        memcpy(e->rows, cb, kRows * 16);
        memcpy(e->game, cb + kParam, kRows * 16);
        e->serial = serial;
        e->eye = eye;
        e->pass = stereo_render::PassIndex();
        e->valid = true;
    }
    if (!h.havePrev || !Active()) return;
    const float before = Dist(cb, cb + kParam), after = Dist(cb, h.prev);
    if (after > 2.0f) return;  // a camera cut / teleport: leave the game's value
    memcpy(cb + kParam, h.prev, kParam);
    memcpy(h.used, h.prev, kParam);
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

bool EyeCamera(int eye, float last[kRows][4], float prev[kRows][4]) {
    if (eye < 0 || eye > 1) return false;
    static_assert(kRows * 16 <= kParam, "rows");
    std::lock_guard<std::mutex> lock(g_mutex);
    const EyeHist& h = g_eye[eye];
    if (!h.haveLast) return false;
    memcpy(last, h.last, kRows * 16);
    memcpy(prev, h.haveUsed ? h.used : h.last, kRows * 16);  // the previous camera its motion vectors point to
    return true;
}

bool FindCamera(const float nodePos[3], int pass, CamEntry* cur, CamEntry* samePrev, CamEntry* prevPass) {
    std::lock_guard<std::mutex> lock(g_mutex);
    int bestEye = -1, bestIx = -1;
    float bestD = 1e-4f;  // 0.1 mm
    for (int eye = 0; eye < 2; ++eye)
        for (int k = 1; k <= kRing; ++k) {
            const int ix = (g_ringNext[eye] + kRing - k) % kRing;
            const CamEntry& e = g_ring[eye][ix];
            if (!e.valid || e.pass != pass) continue;
            const float d = std::fabs(e.rows[37][0] - nodePos[0]) + std::fabs(e.rows[37][1] - nodePos[1]) + std::fabs(e.rows[37][2] - nodePos[2]);
            if (d < bestD) {
                bestD = d;
                bestEye = eye;
                bestIx = ix;
            }
        }
    if (bestEye < 0) {
        static int s_n = 0;
        if (s_n++ < 4)
            for (int eye = 0; eye < 2; ++eye) {
                const CamEntry& e = g_ring[eye][(g_ringNext[eye] + kRing - 1) % kRing];
                if (e.valid)
                    LOG("Camera lookup: wanted node %.4f %.4f %.4f pass %d; eye %d latest: node %.4f %.4f %.4f pass %d serial %llu", nodePos[0],
                        nodePos[1], nodePos[2], pass, eye, e.rows[37][0], e.rows[37][1], e.rows[37][2], e.pass, e.serial);
            }
        return false;
    }
    *cur = g_ring[bestEye][bestIx];
    const CamEntry& sp = g_ring[bestEye][(bestIx + kRing - 1) % kRing];
    *samePrev = sp.valid && sp.serial < cur->serial ? sp : CamEntry{};
    *prevPass = CamEntry{};
    for (int eye = 0; eye < 2; ++eye)
        for (int k = 0; k < kRing; ++k) {
            const CamEntry& e = g_ring[eye][k];
            if (e.valid && e.serial + 1 == cur->serial) *prevPass = e;
        }
    return true;
}

bool EyeCameraEx(int eye, float last[kRows][4], float used[kRows][4], float game[kRows][4]) {
    if (eye < 0 || eye > 1) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    const EyeHist& h = g_eye[eye];
    if (!h.haveLast || !h.haveUsed) return false;
    memcpy(last, h.last, kRows * 16);
    memcpy(used, h.used, kRows * 16);
    memcpy(game, h.game, kRows * 16);
    return true;
}

std::atomic<long> g_jitStats[4]{{0}, {0}, {0}, {0}}, g_jitMissEye[2]{{0}, {0}};  // latest, older, unmatched, sign-combo index of the last match
bool FindCameraByJitter(int eye, float jx, float jy, int w, int h, float last[kRows][4], float used[kRows][4], int* back) {
    if (eye < 0 || eye > 1 || w <= 0 || h <= 0) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    int bestIx = -1, bestBack = 0, bestCombo = 0;
    float best = 1e9f;
    for (int k = 0; k < kRing; ++k) {
        const int ix = (g_ringNext[eye] + kRing - 1 - k) % kRing;
        const CamEntry& e = g_ring[eye][ix];
        if (!e.valid) continue;
        // the projection rows 7 / 8 carry the jitter in column 2 (NDC); the dispatch gives render pixels
        const float px = e.rows[7][2] * 0.5f * (float)w, py = e.rows[8][2] * 0.5f * (float)h;
        const float sx[4] = {1, -1, 1, -1}, sy[4] = {1, 1, -1, -1};
        for (int c = 0; c < 4; ++c) {
            const float d = std::fabs(sx[c] * px - jx) + std::fabs(sy[c] * py - jy);
            if (d < best) best = d, bestIx = ix, bestBack = k, bestCombo = c;
        }
    }
    if (bestIx < 0) return false;
    {
        static std::atomic<int> s_logs{0}, s_missLogs{0};
        const bool miss = best > 0.05f;
        const int missN = miss ? s_missLogs.fetch_add(1) : 0;
        if (s_logs.fetch_add(1) < 6 || (miss && (missN < 8 || missN % 500 == 0))) {
            char line[900];
            int n = snprintf(line, sizeof(line), "FSR camera by jitter: eye %d dispatch jitter %.4f %.4f px (%d x %d): best %.4f at %d back combo %d; ring (newest first) ndc*w/2:",
                             eye, jx, jy, w, h, best, bestBack, bestCombo);
            for (int k = 0; k < 4; ++k) {
                const CamEntry& e = g_ring[eye][(g_ringNext[eye] + kRing - 1 - k) % kRing];
                if (!e.valid) break;
                n += snprintf(line + n, sizeof(line) - n, " [%.4f %.4f s%llu]", e.rows[7][2] * 0.5f * w, e.rows[8][2] * 0.5f * h, e.serial);
            }
            if (eye == 1) {
                n += snprintf(line + n, sizeof(line) - n, "; left ring:");
                for (int k = 0; k < 4; ++k) {
                    const CamEntry& e = g_ring[0][(g_ringNext[0] + kRing - 1 - k) % kRing];
                    if (!e.valid) break;
                    n += snprintf(line + n, sizeof(line) - n, " [%.4f %.4f s%llu]", e.rows[7][2] * 0.5f * w, e.rows[8][2] * 0.5f * h, e.serial);
                }
            }
            LOG("%s", line);
        }
    }
    float shifted[2][kRows][4];
    bool useShifted = false;
    if (best > 0.05f && eye == 1) {
        // The right eye's camera is not in the ring yet when its dispatch is recorded (its rows arrive a pass late, run
        // 327), but with the stereo jitter sync its dispatch jitter is the left eye's of the same tick: take that left
        // camera (and its previous one) moved by the eye offset, measured from the newest left / right pair in the ring.
        int leftIx = -1;
        float bestL = 1e9f;
        for (int k = 0; k < kRing; ++k) {
            const int ix = (g_ringNext[0] + kRing - 1 - k) % kRing;
            const CamEntry& e = g_ring[0][ix];
            if (!e.valid) continue;
            const float px = e.rows[7][2] * 0.5f * (float)w, py = e.rows[8][2] * 0.5f * (float)h;
            const float sx[4] = {1, -1, 1, -1}, sy[4] = {1, 1, -1, -1};
            for (int c = 0; c < 4; ++c) {
                const float d = std::fabs(sx[c] * px - jx) + std::fabs(sy[c] * py - jy);
                if (d < bestL) bestL = d, leftIx = ix;
            }
        }
        if (leftIx >= 0 && bestL <= 0.05f) {
            // the eye offset: newest right camera and the left camera of the pass before it
            float d[3] = {0, 0, 0};
            bool haveD = false;
            for (int k = 0; k < kRing && !haveD; ++k) {
                const CamEntry& r = g_ring[1][(g_ringNext[1] + kRing - 1 - k) % kRing];
                if (!r.valid) break;
                for (int m = 0; m < kRing; ++m) {
                    const CamEntry& l = g_ring[0][m];
                    if (l.valid && l.serial + 1 == r.serial) {
                        for (int i = 0; i < 3; ++i) d[i] = r.rows[11 + i][3] - l.rows[11 + i][3];
                        haveD = true;
                        break;
                    }
                }
            }
            if (haveD) {
                const CamEntry& l = g_ring[0][leftIx];
                const CamEntry& lp = g_ring[0][(leftIx + kRing - 1) % kRing];
                auto shift = [&](const float src[kRows][4], float dst[kRows][4]) {
                    memcpy(dst, src, kRows * 16);
                    for (int i = 0; i < 3; ++i) {
                        dst[11 + i][3] += d[i];                                                          // camera-to-world position
                        dst[i][3] -= src[i][0] * d[0] + src[i][1] * d[1] + src[i][2] * d[2];               // world-to-view translation
                    }
                };
                shift(l.rows, shifted[0]);
                if (lp.valid && lp.serial < l.serial && Dist(reinterpret_cast<const uint8_t*>(l.rows), reinterpret_cast<const uint8_t*>(lp.rows)) <= 2.0f)
                    shift(lp.rows, shifted[1]);
                else
                    shift(l.game, shifted[1]);
                useShifted = true;
                best = 0.f;
                bestBack = (int)((g_ringNext[0] + kRing - 1 - leftIx) % kRing);
                bestCombo = 5;  // through the left eye
            }
        }
    }
    if (best > 0.05f) {  // no camera carries this jitter: the newest one
        ++g_jitStats[2];
        ++g_jitMissEye[eye];
        bestIx = (g_ringNext[eye] + kRing - 1) % kRing;
        bestBack = 0;
    } else {
        ++g_jitStats[bestBack == 0 ? 0 : 1];
        g_jitStats[3] = bestCombo;
    }
    if (useShifted) {
        memcpy(last, shifted[0], kRows * 16);
        memcpy(used, shifted[1], kRows * 16);
        *back = bestBack;
        return true;
    }
    const CamEntry& e = g_ring[eye][bestIx];
    const CamEntry& p = g_ring[eye][(bestIx + kRing - 1) % kRing];
    memcpy(last, e.rows, kRows * 16);
    // its previous camera: the entry before it (this eye's previous pass, what the per-eye patch put there), else the
    // game's own previous
    if (p.valid && p.serial < e.serial && Dist(reinterpret_cast<const uint8_t*>(e.rows), reinterpret_cast<const uint8_t*>(p.rows)) <= 2.0f)
        memcpy(used, p.rows, kRows * 16);
    else
        memcpy(used, e.game, kRows * 16);
    *back = bestBack;
    return true;
}
const char* JitterMatchStatus() {
    static char buf[128];
    snprintf(buf, sizeof(buf), "FSR camera by jitter: %ld newest, %ld older, %ld unmatched (left %ld, right %ld; signs %ld)", g_jitStats[0].load(),
             g_jitStats[1].load(), g_jitStats[2].load(), g_jitMissEye[0].load(), g_jitMissEye[1].load(), g_jitStats[3].load());
    return buf;
}

float EyeSeparation() {
    const float d = g_lat[1].load() - g_lat[0].load();
    return d > 0.01f && d < 0.5f ? d : 0.063f;
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

void RequestTrace(int calls) { g_trace = calls; }

void RequestCbDump(int frames) {
    g_cbDump = frames;
    LOG("Per-eye motion vectors: dumping cCamera for %d frame(s)", frames);
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
