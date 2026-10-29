#include "camera_hook.h"
#include <vector>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>

#include "cam_prev.h"
#include "config.h"
#include "xr_screen.h"
#include "log.h"
#include "patches.h"
#include "vr_shared.h"
#include "frame_probe.h"
#include "stereo_render.h"
#include "render_size.h"

namespace camera_hook {
namespace {

// mov rbx, rcx ; lea rdx, [rbp+30h] ; mov rcx, [rcx+10h] ; call GetNodeWorldMatrix
constexpr char kCallSite[] = "48 8B D9 48 8D 55 30 48 8B 49 10 E8";
constexpr int kCallOffset = 11;  // offset of the E8 inside the pattern

using GetWorld_t = void (*)(void* node, float* out);
GetWorld_t g_orig = nullptr;

std::atomic<bool> g_test{false};
std::atomic<float> g_yaw{25.f}, g_pitch{0.f};
std::atomic<int> g_testPass{-1};  // test offset only on this frame pass (stereo_render), -1 = always

// Views seen (RenderView pointers), logged once each with their projection.
std::mutex g_seenMutex;
void* g_seen[32];
int g_nSeen = 0;

void* g_seenNode[32];
void NoteView(void* view, void* node) {
    std::lock_guard<std::mutex> lock(g_seenMutex);
    for (int i = 0; i < g_nSeen; ++i)
        if (g_seen[i] == view) return;
    if (g_nSeen >= 32) return;
    g_seenNode[g_nSeen] = node;
    g_seen[g_nSeen++] = view;
    const float* proj = reinterpret_cast<const float*>(static_cast<uint8_t*>(view) + 0x70);  // previous frame's projection
    float fy = proj[5];
    LOG("Camera hook: RenderView %p (node %p, vtable %p) projection fy=%.4f (fovy %.1f) near=%.3g", view, node,
        *reinterpret_cast<void**>(view), fy, fy > 0 ? 2 * atanf(1 / fy) * 57.29578f : 0.f, proj[14]);
}

// out: 4x4 row-major, row-vector convention. Rows 0..2 = camera axes in world space, row 3 = position.
[[maybe_unused]] void ApplyHead(float* m, float yawDeg, float pitchDeg, const float* offset) {
    const float y = yawDeg * 0.01745329f, p = pitchDeg * 0.01745329f;
    const float cy = cosf(y), sy = sinf(y), cp = cosf(p), sp = sinf(p);
    // R = Rx(pitch) * Ry(yaw), row-vector convention (rows are the rotated local axes)
    const float Ry[3][3] = {{cy, 0, -sy}, {0, 1, 0}, {sy, 0, cy}};
    const float Rx[3][3] = {{1, 0, 0}, {0, cp, sp}, {0, -sp, cp}};
    float R[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) R[i][j] = Rx[i][0] * Ry[0][j] + Rx[i][1] * Ry[1][j] + Rx[i][2] * Ry[2][j];
    float axes[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) axes[i][j] = m[i * 4 + j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[i * 4 + j] = R[i][0] * axes[0][j] + R[i][1] * axes[1][j] + R[i][2] * axes[2][j];
    if (offset)
        for (int j = 0; j < 3; ++j) m[12 + j] += offset[0] * axes[0][j] + offset[1] * axes[1][j] + offset[2] * axes[2][j];
}

extern "C" void HookGetWorld(void* node, float* out, void* view) {
    g_orig(node, out);
    if (view) NoteView(view, node);
}

// ---- view matrix hook ------------------------------------------------------------------------------------
// RenderView::Update builds the world->view matrix with a call (ffxvi.exe+0x29D090 in 1.03) into a local 4x4
// (rows at 16-byte stride, xyz used, row-vector convention), then copies it to view+0x30. The hook runs the
// original untouched and then post-multiplies by the inverse head transform: V' = V * H^-1.
// add rdx,40h ; vmovss [rbp-1Ch],xmm0 ; vmovss [rbp-18h],xmm1 ; call BuildView
constexpr char kViewCallSite[] = "48 83 C2 40 C5 FA 11 45 E4 C5 FA 11 4D E8 E8";
constexpr int kViewCallOffset = 14;

// The main gameplay view: near plane 0.2 (other views seen: near 0.01 / far 1000, and 90-degree probe views).
bool IsMainView(const uint8_t* view) {
    const float nearZ = *reinterpret_cast<const float*>(view + 0xE44);
    const float farZ = *reinterpret_cast<const float*>(view + 0xE48);
    return nearZ > 0.1f && nearZ < 0.5f && farZ < 10.f;
}

std::atomic<uint64_t> g_hmdUpdates{0};

// VRMode 1: game camera follows the headset. v = world->view, 4 rows (16-byte stride), row-vector convention.
// With the head pose E (column-vector rotation M, position t, in camera space): V' = V * (E^-1)^T, i.e.
// rows 0..3: a -> a * M, plus row 3: -(M^T t).
void ApplyHmd(float* v, uint8_t* view) {
    vrs::HeadSample h = vrs::LatestHead();
    if (!h.valid) return;
    // Native stereo: the frame function runs twice per game tick; pass 0 renders the left eye, pass 1 the right.
    const int pass = stereo_render::NativeActive() ? stereo_render::CurrentPass() : -1;
    const int eye = pass >= 0 ? (pass & 1) : -1;
    vrs::Pose rel = h.rel, abs = h.abs;
    if (eye >= 0) {  // eye pose = head pose * eye-in-head pose (from xrLocateViews in VIEW space)
        vrs::PoseMul(h.rel, h.eye[eye], &rel);
        vrs::PoseMul(h.abs, h.eye[eye], &abs);
    }
    float M[3][3];
    vrs::QuatToMat3(rel.q, M);
    float t[3] = {0, 0, 0};
    if (vr::g_config.positionTracking || eye >= 0) {
        float hp[3] = {0, 0, 0};
        if (vr::g_config.positionTracking) for (int i = 0; i < 3; ++i) hp[i] = h.rel.p[i];
        // eye offset (rel.p - h.rel.p) is always applied, head translation only with PositionTracking
        for (int i = 0; i < 3; ++i) t[i] = (hp[i] + (rel.p[i] - h.rel.p[i])) * vr::g_config.worldScale;
    }
    for (int row = 0; row < 4; ++row) {
        float* r = v + row * 4;
        float a[3] = {r[0], r[1], r[2]};
        for (int j = 0; j < 3; ++j) r[j] = a[0] * M[0][j] + a[1] * M[1][j] + a[2] * M[2][j];
    }
    for (int j = 0; j < 3; ++j) v[12 + j] -= M[0][j] * t[0] + M[1][j] * t[1] + M[2][j] * t[2];

    // Field of view: symmetric, covering both eyes (the runtime crops/reprojects to each eye).
    float* fovY = reinterpret_cast<float*>(view + 0xE3C);
    // The game takes the aspect from its resolution setting, not the window: with the window at the VR render size
    // (render_size), give the projection that size's aspect.
    float* aspectP = reinterpret_cast<float*>(view + 0xE40);
    if (const float va = render_size::VrAspect(); va > 0.f) *aspectP = va;
    const float aspect = *aspectP;
    const float s = vr::g_config.fovScale;
    float tanV = std::max(h.tanUp, h.tanDown) * s;
    float tanH = std::max(h.tanLeft, h.tanRight) * s;
    if (eye >= 0) {
        tanV = std::max(h.eyeTan[eye][0], h.eyeTan[eye][1]) * s;
        tanH = std::max(h.eyeTan[eye][2], h.eyeTan[eye][3]) * s;
    }
    if (aspect > 0.1f && tanH > tanV * aspect) tanV = tanH / aspect;
    *fovY = 2.f * atanf(tanV);

    vrs::UsedSample u;
    u.valid = true;
    u.abs = abs;
    u.eye = pass;  // view index (0..3); the eye is view & 1
    u.tanHalfV = tanV;
    u.tanHalfH = tanV * aspect;
    u.presentTag = vrs::PresentCount();
    vrs::RecordUsed(u);
    if (g_hmdUpdates.fetch_add(1) == 0)
        LOG("Camera hook: head tracking active on view %p (vertical FOV %.1f deg, aspect %.3f, %s)", view,
            2 * atanf(tanV) * 57.29578f, aspect, eye >= 0 ? "native stereo" : "mono");
}

extern "C" void ViewPost(float* v, uint8_t* view) {
    if (vr::g_config.vrMode >= 1 && vr::g_config.vrOutput && view && IsMainView(view)) {
        if (!xr::HudMovieMode()) ApplyHmd(v, view);  // letterboxed opening cinematic: the game's own camera, flat
        return;
    }
    if (!g_test.load(std::memory_order_relaxed)) return;
    const int tp = g_testPass.load(std::memory_order_relaxed);
    if (tp >= 0 && (!IsMainView(view) || stereo_render::CurrentPass() != tp)) return;
    const float y = g_yaw.load() * 0.01745329f, p = g_pitch.load() * 0.01745329f;
    const float cy = cosf(y), sy = sinf(y), cp = cosf(p), sp = sinf(p);
    // head rotation R = Rx(pitch) * Ry(yaw) (row-vector), inverse = transpose
    const float Ry[3][3] = {{cy, 0, -sy}, {0, 1, 0}, {sy, 0, cy}};
    const float Rx[3][3] = {{1, 0, 0}, {0, cp, sp}, {0, -sp, cp}};
    float R[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) R[i][j] = Rx[i][0] * Ry[0][j] + Rx[i][1] * Ry[1][j] + Rx[i][2] * Ry[2][j];
    for (int row = 0; row < 4; ++row) {
        float* r = v + row * 4;
        float a[3] = {r[0], r[1], r[2]};
        for (int j = 0; j < 3; ++j) r[j] = a[0] * R[j][0] + a[1] * R[j][1] + a[2] * R[j][2];  // a * R^T
    }
    (void)view;
}

void LogViewParams(uint8_t* view) {
    std::lock_guard<std::mutex> lock(g_seenMutex);
    static void* logged[32];
    static int n = 0;
    for (int i = 0; i < n; ++i)
        if (logged[i] == view) return;
    if (n >= 32) return;
    logged[n++] = view;
    const float* f = reinterpret_cast<const float*>(view + 0xE40);
    LOG("Camera hook: view %p params @+0xE40: %.4f %.4f %.4f %.4f", view, f[0], f[1], f[2], f[3]);
}

// Dev probe (ff16vr_viewprobe.trigger): where does the RenderView keep the previous frame's matrices? Snapshots the
// main view's memory after each update and logs 64-byte blocks whose new contents equal another offset's contents
// from the previous update (current -> previous copies), for the per-eye previous-camera (motion vector) fix.
constexpr size_t kProbeBytes = 0x1400;
std::vector<uint8_t> g_probePrev;
int g_probeLeft = 0;
DWORD g_probePoll = 0;

void ViewProbe(float* v, uint8_t* view) {
    const DWORD now = GetTickCount();
    if (!g_probeLeft && now - g_probePoll > 1000) {
        g_probePoll = now;
        const std::wstring trig = vr::g_gameDir + L"\\ff16vr_viewprobe.trigger";
        if (GetFileAttributesW(trig.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(trig.c_str());
            g_probeLeft = 6;
            g_probePrev.clear();
            LOG("View probe: view %p, matrix v at +0x%llX; %d updates", view, (unsigned long long)((uint8_t*)v - view), g_probeLeft);
        }
    }
    if (!g_probeLeft) return;
    std::vector<uint8_t> cur(view, view + kProbeBytes);
    if (!g_probePrev.empty()) {
        int found = 0;
        for (size_t y = 0; y + 64 <= kProbeBytes; y += 16) {
            if (!memcmp(&cur[y], &g_probePrev[y], 64)) continue;          // unchanged: not interesting
            const float* fy = reinterpret_cast<const float*>(&cur[y]);
            int nonTrivial = 0;
            for (int k = 0; k < 16; ++k) if (fy[k] != 0.f && fy[k] != 1.f && fy[k] == fy[k]) ++nonTrivial;
            if (nonTrivial < 6) continue;
            for (size_t x = 0; x + 64 <= kProbeBytes; x += 16) {
                if (x == y || memcmp(&cur[y], &g_probePrev[x], 64)) continue;
                if (found++ < 24)
                    LOG("View probe: update %d (pass %d): +0x%03zX now holds what +0x%03zX held last update", 7 - g_probeLeft,
                        stereo_render::CurrentPass(), y, x);
            }
        }
        LOG("View probe: update %d (pass %d): %d copied blocks", 7 - g_probeLeft, stereo_render::CurrentPass(), found);
    }
    g_probePrev.swap(cur);
    --g_probeLeft;
}

// Dev (ff16vr_viewdump.trigger present at start): raw dumps of each main RenderView and its camera node after 300 and
// 1200 updates, to find the camera parameter behind the motion-blur scale (1 after a load, 1000 after a menu).
void DumpBytes(const void* p, size_t want, const wchar_t* name) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return;
    const size_t avail = (size_t)((const uint8_t*)mbi.BaseAddress + mbi.RegionSize - (const uint8_t*)p);
    const size_t n = want < avail ? want : avail;
    if (FILE* f = _wfopen((vr::g_gameDir + L"\\" + name).c_str(), L"wb")) {
        fwrite(p, 1, n, f);
        fclose(f);
    }
}

void ViewDump(uint8_t* view) {
    static int s_enabled = -1;
    if (s_enabled < 0) {
        s_enabled = GetFileAttributesW((vr::g_gameDir + L"\\ff16vr_viewdump.trigger").c_str()) != INVALID_FILE_ATTRIBUTES;
        if (s_enabled) LOG("View dump: enabled");
    }
    if (!s_enabled) return;
    static void* s_view[8] = {};
    static int s_count[8] = {};
    int i = 0;
    for (; i < 8 && s_view[i] && s_view[i] != view; ++i) {}
    if (i == 8) return;
    s_view[i] = view;
    const int c = ++s_count[i];
    if (c != 300 && c != 1200) return;
    void* node = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_seenMutex);
        for (int k = 0; k < g_nSeen; ++k)
            if (g_seen[k] == view) node = g_seenNode[k];
    }
    wchar_t name[96];
    swprintf(name, 96, L"ff16vr_dump_view%d_%d.bin", i, c);
    DumpBytes(view, 0x2000, name);
    if (node) {
        swprintf(name, 96, L"ff16vr_dump_node%d_%d.bin", i, c);
        DumpBytes(node, 0x1000, name);
    }
    LOG("View dump: view %d (%p, node %p) after %d updates", i, view, node, c);
}

// Dev (ff16vr_viewseq.trigger): the main RenderView over 8 consecutive frame passes in native stereo, dumped at the
// start of each pass (final state of the previous pass) and in the view hook (mid-update, before the head transform),
// to find which fields hold the previous frame's camera (per-eye previous camera / motion-vector fix).
std::atomic<uint8_t*> g_mainView{nullptr};
int g_seqLeft = 0, g_seqIndex = 0;
DWORD g_seqPoll = 0;

void SeqDump(uint8_t* view, const char* when) {
    wchar_t name[96];
    swprintf(name, 96, L"ff16vr_seq_%d_%hs_p%d.bin", g_seqIndex, when, stereo_render::CurrentPass());
    DumpBytes(view, 0x2000, name);
}

// ---- Per-eye camera history (PerEyeHistory) ----
// In native stereo the RenderView is updated once per eye pass, so the game's "previous frame" camera (copied from the
// current one during RenderView::Update, after the view hook) is the other eye's: motion vectors then contain the eye
// offset (ghosting with TAA / DLSS, smear with motion blur). Run 161 (view dumps over 8 passes): the camera state
// lives in +0x030..+0x640 and +0xF00..+0x1000; +0x4D0 and +0xF00 receive the previous camera, and nothing in these
// ranges is written before the view hook. So: at the start of each pass the final view state of the previous pass is
// saved for that eye, and in the view hook (before the game copies current -> previous and recomputes) the eye's own
// saved state is put back. The game then derives "previous" from the same eye's last frame.
struct EyeSnap {
    uint8_t bytes[0x1000];
    bool valid = false;
};
EyeSnap g_eyeSnap[2];
bool g_prevPassStereo = false;
std::atomic<long> g_historyRestores{0}, g_ringRestores{0};
constexpr size_t kHistRanges[2][2] = {{0x030, 0x640}, {0xF40, 0x1000}};



void SaveEyeSnap(int pass) {
    uint8_t* v = g_mainView.load();
    const bool stereo = stereo_render::NativeActive();
    if (!stereo) {
        for (auto& e : g_eyeSnap) e.valid = false;
    } else if (v && g_prevPassStereo) {
        const int eye = (pass + 1) & 1;  // the view holds the final state of the previous pass (previous view)
        memcpy(g_eyeSnap[eye].bytes, v, sizeof(g_eyeSnap[eye].bytes));
        g_eyeSnap[eye].valid = true;
    }
    g_prevPassStereo = stereo;
}

void RestoreEyeHistory(uint8_t* view) {
    if (!vr::g_config.perEyeHistory || !stereo_render::NativeActive()) return;
    const int eye = stereo_render::CurrentPass();
    if (eye < 0 || eye > 3 || !g_eyeSnap[eye].valid || view != g_mainView.load()) return;
    for (const auto& r : kHistRanges) memcpy(view + r[0], g_eyeSnap[eye].bytes + r[0], r[1] - r[0]);
    // Per-eye motion vectors (cam_prev): also the CameraParam ring (+0x610, 2 x 0x400) and its toggle / slot indices
    // (+0xE74..+0xE76). RenderView::Update then writes this eye's camera into its own ring and takes "previous"
    // (camera constants +0x400, the reprojection +0xE80 / +0xF00, the jitter delta) from the same eye's last frame.
    if (cam_prev::Active()) {
        memcpy(view + 0x640, g_eyeSnap[eye].bytes + 0x640, 0xE10 - 0x640);
        memcpy(view + 0xE74, g_eyeSnap[eye].bytes + 0xE74, 3);
        if (g_ringRestores++ == 0) LOG("Per-eye motion vectors: first ring restore (eye %d)", eye);
    }
    if (g_historyRestores.fetch_add(1) == 0) LOG("Per-eye history: first restore (eye %d, view %p)", eye, view);
}


// ---- Same camera jitter for both eyes of a tick (StereoJitterSync) ----
// TAA / DLSS / FSR shift the projection by a sub-pixel jitter that follows a fixed sequence. The jitter generator
// (RenderView+0x1000, used at ffxvi.exe+0xAA2F9D) advances its index (+0x10) on every RenderView::Update, so in native
// stereo the left eye only ever gets the even samples and the right eye the odd ones: with per-eye upscaler history
// each eye reconstructs from half the pattern (pixelated with PerEyeDlss). Before the right eye's Update the index is
// put back to the left eye's value, so both eyes of a tick share a sample and each eye sees the whole sequence.
uint64_t g_jitPassSerial = 0, g_jitDone = ~0ull;
int32_t g_jitIndexLeft = 0;
bool g_jitHave = false;
long g_jitSyncs = 0, g_jitLogged = 0;

void SyncJitter(uint8_t* view) {
    if (!vr::g_config.stereoJitterSync || !stereo_render::NativeActive()) {
        g_jitHave = false;
        return;
    }
    if (g_jitDone == g_jitPassSerial) return;  // first main-view Update of this pass only
    g_jitDone = g_jitPassSerial;
    uint8_t* gen = *reinterpret_cast<uint8_t**>(view + 0x1000);
    if (!gen) return;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(gen + 0x10, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & 0xFF) != PAGE_READWRITE) return;
    int32_t* idx = reinterpret_cast<int32_t*>(gen + 0x10);
    if (stereo_render::CurrentPass() == 0) {
        g_jitIndexLeft = *idx;
        g_jitHave = true;
    } else if (g_jitHave) {
        if (g_jitSyncs < 4 || g_jitSyncs % 2000 == 0 || (*idx != g_jitIndexLeft && g_jitLogged < 6)) {
            if (*idx != g_jitIndexLeft) ++g_jitLogged;
            LOG("Jitter sync: right eye index %d -> %d (left eye's); %ld syncs", *idx, g_jitIndexLeft, g_jitSyncs);
        }
        *idx = g_jitIndexLeft;
        ++g_jitSyncs;
    }
}

}  // namespace

long HistoryRestores() { return g_historyRestores.load(); }
void* MainView() { return g_mainView.load(); }
unsigned long long PassSerial() { return g_jitPassSerial; }

void OnFramePass() {
    ++g_jitPassSerial;
    SaveEyeSnap(stereo_render::CurrentPass());
    const DWORD now = GetTickCount();
    if (!g_seqLeft && now - g_seqPoll > 1000) {
        g_seqPoll = now;
        const std::wstring trig = vr::g_gameDir + L"\\ff16vr_viewseq.trigger";
        if (GetFileAttributesW(trig.c_str()) != INVALID_FILE_ATTRIBUTES && stereo_render::NativeActive()) {
            DeleteFileW(trig.c_str());
            g_seqLeft = 8;
            g_seqIndex = 0;
            LOG("View seq: dumping 8 passes of view %p", (void*)g_mainView.load());
        }
    }
    if (!g_seqLeft) return;
    if (uint8_t* v = g_mainView.load()) {
        ++g_seqIndex;
        SeqDump(v, "start");
    }
    --g_seqLeft;
}

namespace {

extern "C" void ViewPostEntry(float* v, uint8_t* view) {
    if (view && IsMainView(view)) {
        if (g_mainView.load() != view) {
            for (auto& e : g_eyeSnap) e.valid = false;
        }
        g_mainView = view;
        if (g_seqLeft) SeqDump(view, "mid");
        RestoreEyeHistory(view);
        SyncJitter(view);
    }
    if (view && IsMainView(view)) ViewDump(view);
    if (view) LogViewParams(view);
    if (view) frame_probe::OnViewUpdate(IsMainView(view));
    if (view && IsMainView(view)) stereo_render::NoteMainView();
    ViewPost(v, view);
    if (view && IsMainView(view)) ViewProbe(v, view);
}

bool InstallViewHook() {
    int count = 0;
    uint8_t* sig = patches::FindCode(kViewCallSite, &count);
    if (!sig) {
        LOG("Camera hook: view call site not found (%d matches)", count);
        return false;
    }
    uint8_t* call = sig + kViewCallOffset;
    int32_t rel = 0;
    memcpy(&rel, call + 1, 4);
    void* orig = call + 5 + rel;
    uint8_t* stub = patches::AllocCodeNear(call, 128);
    if (!stub) return false;
    void* post = reinterpret_cast<void*>(&ViewPostEntry);
    uint8_t code[64];
    size_t n = 0;
    auto put = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) code[n++] = x; };
    put({0x51});                          // push rcx (out)
    put({0x48, 0x83, 0xEC, 0x20});        // sub rsp, 20h
    put({0x48, 0xB8}); memcpy(code + n, &orig, 8); n += 8;   // mov rax, original
    put({0xFF, 0xD0});                    // call rax
    put({0x48, 0x8B, 0x4C, 0x24, 0x20});  // mov rcx, [rsp+20h] (out)
    put({0x48, 0x89, 0xDA});              // mov rdx, rbx (RenderView)
    put({0x48, 0xB8}); memcpy(code + n, &post, 8); n += 8;   // mov rax, ViewPostEntry
    put({0xFF, 0xD0});                    // call rax
    put({0x48, 0x83, 0xC4, 0x20});        // add rsp, 20h
    put({0x59});                          // pop rcx
    put({0xC3});                          // ret
    memcpy(stub, code, n);
    FlushInstructionCache(GetCurrentProcess(), stub, n);
    uint8_t patch[5] = {0xE8};
    int32_t newRel = static_cast<int32_t>(stub - (call + 5));
    memcpy(patch + 1, &newRel, 4);
    if (!patches::WriteCodeBytes(call, patch, 5)) return false;
    LOG("Camera hook: view matrix hook at ffxvi.exe+0x%llX (original ffxvi.exe+0x%llX)",
        (unsigned long long)(call - patches::GameBase()), (unsigned long long)((uint8_t*)orig - patches::GameBase()));
    return true;
}

DWORD WINAPI ControlThread(LPVOID) {
    const std::wstring trigger = vr::g_gameDir + L"\\ff16vr_camtest.trigger";
    bool keyWas = false;
    for (;;) {
        Sleep(100);
        bool key = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (key && !keyWas) {
            g_test = !g_test;
            LOG("Camera hook test %s (F9): yaw %.1f pitch %.1f", g_test ? "ON" : "OFF", g_yaw.load(), g_pitch.load());
        }
        keyWas = key;
        if (GetFileAttributesW(trigger.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        char buf[64] = {};
        if (FILE* f = _wfopen(trigger.c_str(), L"r")) {
            fgets(buf, sizeof(buf), f);
            fclose(f);
        }
        DeleteFileW(trigger.c_str());
        float yw = 0, pt = 0;
        int pass = -1;
        const char* args = buf;
        if (buf[0] == 'p' && buf[1] >= '0' && buf[1] <= '1') {  // "p0 yaw pitch": only on frame pass 0 (or p1)
            pass = buf[1] - '0';
            args = buf + 2;
        }
        g_testPass = pass;
        if (sscanf(args, "%f %f", &yw, &pt) >= 1) {
            g_yaw = yw;
            g_pitch = pt;
            g_test = true;
        } else {
            g_test = false;
        }
        LOG("Camera hook test %s (trigger): yaw %.1f pitch %.1f, pass %d", g_test ? "ON" : "OFF", g_yaw.load(), g_pitch.load(),
            g_testPass.load());
    }
}

}  // namespace

void Install() {
    int count = 0;
    uint8_t* sig = patches::FindCode(kCallSite, &count);
    if (!sig) {
        LOG("Camera hook: call site not found (%d matches); camera hook disabled", count);
        return;
    }
    uint8_t* call = sig + kCallOffset;
    int32_t rel = 0;
    memcpy(&rel, call + 1, 4);
    g_orig = reinterpret_cast<GetWorld_t>(call + 5 + rel);

    uint8_t* stub = patches::AllocCodeNear(call, 64);
    if (!stub) {
        LOG("Camera hook: no memory near the game code; camera hook disabled");
        return;
    }
    // mov r8, rbx (the RenderView)  ;  mov rax, imm64  ;  jmp rax
    uint8_t code[16] = {0x49, 0x89, 0xD8, 0x48, 0xB8};
    void* target = reinterpret_cast<void*>(&HookGetWorld);
    memcpy(code + 5, &target, 8);
    code[13] = 0xFF;
    code[14] = 0xE0;
    memcpy(stub, code, 15);
    FlushInstructionCache(GetCurrentProcess(), stub, 15);

    uint8_t patch[5] = {0xE8};
    int32_t newRel = static_cast<int32_t>(stub - (call + 5));
    memcpy(patch + 1, &newRel, 4);
    if (!patches::WriteCodeBytes(call, patch, 5)) {
        LOG("Camera hook: could not patch the call site");
        return;
    }
    LOG("Camera hook installed at ffxvi.exe+0x%llX (original ffxvi.exe+0x%llX); F9 or ff16vr_camtest.trigger toggles the test offset",
        (unsigned long long)(call - patches::GameBase()), (unsigned long long)((uint8_t*)g_orig - patches::GameBase()));
    InstallViewHook();
    HANDLE t = CreateThread(nullptr, 0, ControlThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

}  // namespace camera_hook
