#include "sl_hooks.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include <d3d12.h>

#include "config.h"
#include "log.h"
#include "fovdlss.h"
#include "stereo_edge.h"
#include "mvfix.h"
#include "eye_history.h"
#include "warp.h"
#include "vrs.h"
#include "stereo_render.h"

namespace sl_hooks {
namespace {

// ---- minimal Streamline ABI (sl_struct.h / sl_core_types.h, SL 2.x)
struct SlBase {
    SlBase* next;
    GUID type;
    size_t version;
};
struct SlViewport : SlBase {
    uint32_t value;
};
// {171B6435-9B3C-4FC8-9994-FBE52569AAA4}
const GUID kViewportType = {0x171b6435, 0x9b3c, 0x4fc8, {0x99, 0x94, 0xfb, 0xe5, 0x25, 0x69, 0xaa, 0xa4}};
constexpr uint32_t kFeatureDLSS = 0;
using SlResult = int;
using Feature = uint32_t;

using GetNewFrameToken_t = SlResult (*)(void*& token, const uint32_t* frameIndex);
using SetConstants_t = SlResult (*)(const void* values, const void* frame, const SlViewport& vp);
using SetTag_t = SlResult (*)(const SlViewport& vp, const void* tags, uint32_t n, void* cmd);
using Evaluate_t = SlResult (*)(Feature f, const void* frame, const SlBase** inputs, uint32_t n, void* cmd);
using Allocate_t = SlResult (*)(void* cmd, Feature f, const SlViewport& vp);
using Free_t = SlResult (*)(Feature f, const SlViewport& vp);
using GetFeatureFunction_t = SlResult (*)(Feature f, const char* name, void*& fn);
using DLSSSetOptions_t = SlResult (*)(const SlViewport& vp, const void* options);
using IsFeatureSupported_t = SlResult (*)(Feature f, const void* adapterInfo);
using Init_t = SlResult (*)(const void* pref, uint64_t sdkVersion);
using IsFeatureLoaded_t = SlResult (*)(Feature f, bool& loaded);
using SetFeatureLoaded_t = SlResult (*)(Feature f, bool loaded);
using GetFeatureVersion_t = SlResult (*)(Feature f, void* version);
using GetFeatureRequirements_t = SlResult (*)(Feature f, void* req);

GetNewFrameToken_t o_GetNewFrameToken = nullptr;
SetConstants_t o_SetConstants = nullptr;
SetTag_t o_SetTag = nullptr;
Evaluate_t o_Evaluate = nullptr;
Allocate_t o_Allocate = nullptr;
Free_t o_Free = nullptr;
GetFeatureFunction_t o_GetFeatureFunction = nullptr;
IsFeatureSupported_t o_IsFeatureSupported = nullptr;
Init_t o_Init = nullptr;
IsFeatureLoaded_t o_IsFeatureLoaded = nullptr;
SetFeatureLoaded_t o_SetFeatureLoaded = nullptr;
GetFeatureVersion_t o_GetFeatureVersion = nullptr;
GetFeatureRequirements_t o_GetFeatureRequirements = nullptr;
DLSSSetOptions_t o_DLSSSetOptions = nullptr;

using GetProcAddress_t = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddress_t o_GetProcAddress = nullptr;

// frame token -> eye (tokens are obtained at the start of each rendered frame)
struct TokenEye { const void* token; uint32_t index; int eye; };
TokenEye g_tokens[16] = {};
int g_tokenPos = 0;
std::mutex g_mutex;
std::atomic<long> g_logs{0}, g_evals[2] = {}, g_eyeDisagree{0};

bool Enabled() { return vr::g_config.perEyeDlss; }
std::atomic<ULONGLONG> g_lastEval{0};

void Log1(const char* what, int eye, uint32_t vp, uint32_t mapped) {
    if (g_logs.fetch_add(1) < 40)
        LOG("SL: %s eye %d viewport %u -> %u (thread %lu)", what, eye, vp, mapped, GetCurrentThreadId());
}

// game call sites (ffxvi.exe offsets) of the current call, for finding what re-applies the upscaler settings
std::string GameStack() {
    static uint8_t* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    static size_t size = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew)
                             ->OptionalHeader.SizeOfImage;
    void* frames[24];
    const USHORT n = RtlCaptureStackBackTrace(1, 24, frames, nullptr);
    std::string out;
    int shown = 0;
    for (USHORT i = 0; i < n && shown < 8; ++i) {
        auto* p = static_cast<uint8_t*>(frames[i]);
        if (p < base || p >= base + size) continue;
        char b[24];
        snprintf(b, sizeof(b), " +0x%zX", (size_t)(p - base));
        out += b;
        ++shown;
    }
    return out;
}

int EyeOfToken(const void* token) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto& t : g_tokens)
        if (t.token == token) return t.eye;
    return -2;  // unknown
}

// Eye of the frame being rendered (set by the frame hook before each pass). The eye recorded with the frame token is
// only compared for the log: if the game took one token per tick for both passes, the token's eye would be wrong.
int EyeFor(const void* frame) {
    const int now = stereo_render::FrameEye();
    const int tok = frame ? EyeOfToken(frame) : -2;
    if (tok != -2 && tok != now) ++g_eyeDisagree;
    return now;
}

SlViewport Mapped(const SlViewport& vp, int eye) {
    SlViewport m = vp;
    m.next = nullptr;
    if (eye >= 1 && eye <= 3) m.value = vp.value + kRightEyeViewportOffset * (uint32_t)eye;  // view v: + v * 0x100
    return m;
}

SlResult Hk_GetNewFrameToken(void*& token, const uint32_t* frameIndex) {
    SlResult r = o_GetNewFrameToken(token, frameIndex);
    if (r == 0 && token) {
        const int eye = stereo_render::FrameEye();
        std::lock_guard<std::mutex> lock(g_mutex);
        g_tokens[g_tokenPos] = {token, frameIndex ? *frameIndex : 0u, eye};
        g_tokenPos = (g_tokenPos + 1) % 16;
    }
    return r;
}

// Extra DLSS viewports mirrored from the game's: view 1 (right eye).
int ExtraViews() { return 1; }

// Constants (sl_consts.h, version 2): base 32, cameraViewToClip 32, clipToCameraView 96, clipToLensClip 160,
// clipToPrevClip 224, prevClipToClip 288, jitterOffset 352, mvecScale 360, cameraPinholeOffset 368, cameraPos 376,
// up 388, right 400, fwd 412, near 424, far 428, fov 432, aspect 436, mvInvalid 440, depthInverted 444,
// cameraMotionIncluded 445, motionVectors3D 446, reset 447, ortho 448, mvDilated 449, mvJittered 450.
// VerboseLog: one line per eye every ~2 s (DLSS sharpness investigation).
void LogConstants(const void* values, int eye, uint32_t vp) {
    if (!vr::g_config.verboseLog || !values) return;
    static ULONGLONG s_next[2] = {};
    const int e = eye == 1 ? 1 : 0;
    const ULONGLONG now = GetTickCount64();
    if (now < s_next[e]) return;
    s_next[e] = now + 2000;
    const auto* b = static_cast<const uint8_t*>(values);
    auto f = [&](int off) { return *reinterpret_cast<const float*>(b + off); };
    LOG("SL constants eye %d vp %u: jitter %.4f %.4f mvecScale %.5f %.5f pos %.3f %.3f %.3f near %.3f far %.1f fov %.4f "
        "aspect %.4f | depthInv %d camMotion %d mv3D %d reset %d dilated %d jittered %d | clipToPrevClip row3 %.5f %.5f %.5f "
        "%.5f diag %.5f %.5f",
        eye, vp, f(352), f(356), f(360), f(364), f(376), f(380), f(384), f(424), f(428), f(432), f(436), b[444], b[445],
        b[446], b[447], b[449], b[450], f(224 + 48), f(224 + 52), f(224 + 56), f(224 + 60), f(224), f(224 + 20));
}

// StereoEdgeFix inputs of the game's viewport (shared history: the game's own viewport only)
struct EdgeRec {
    float mvScale[2] = {};
    bool depthInverted = true;
    bool haveConst = false;
    uint8_t mvTag[64] = {};
    uint8_t mvRes[128] = {};
    ID3D12Resource* depth = nullptr;
    uint32_t depthState = 0;
    uint32_t extent[4] = {};  // top, left, width, height (motion vectors)
    bool haveMv = false;
};
EdgeRec g_edge;
std::mutex g_edgeMutex;

// The sl::Constants of recent slSetConstants calls, by frame token: the game may set them on another thread than the
// one that evaluates (job threads), so they are not kept per thread. The evaluate takes the newest entry with its frame
// token (both eyes may share one), else the newest of all.
struct ConstRec {
    const void* frame = nullptr;
    unsigned long long seq = 0;
    uint8_t sl[464] = {};
};
ConstRec g_const[8];
unsigned long long g_constSeq = 0;
std::mutex g_constMutex;
std::atomic<long> g_setConstCalls{0}, g_setConstThread{0}, g_constOtherThread{0}, g_constTokenMiss{0};
void StoreConstants(const uint8_t* b, const void* frame) {
    std::lock_guard<std::mutex> lock(g_constMutex);
    ConstRec& r = g_const[g_constSeq % 8];
    r.frame = frame;
    r.seq = ++g_constSeq;
    memcpy(r.sl, b, sizeof(r.sl));
}
bool LoadConstants(const void* frame, uint8_t out[464]) {
    std::lock_guard<std::mutex> lock(g_constMutex);
    const ConstRec* best = nullptr;
    for (const ConstRec& r : g_const)
        if (r.seq && r.frame == frame && (!best || r.seq > best->seq)) best = &r;
    if (!best) {
        for (const ConstRec& r : g_const)
            if (r.seq && (!best || r.seq > best->seq)) best = &r;
        if (best && frame) ++g_constTokenMiss;
    }
    if (!best) return false;
    memcpy(out, best->sl, 464);
    return true;
}
thread_local const void* t_evalFrame = nullptr;  // the frame token of the evaluate running on this thread
SlResult Hk_SetConstants(const void* values, const void* frame, const SlViewport& vp) {
    if (values && vp.value < kRightEyeViewportOffset) {
        const auto* b = static_cast<const uint8_t*>(values);
        std::lock_guard<std::mutex> lock(g_edgeMutex);
        memcpy(g_edge.mvScale, b + 360, 8);
        g_edge.depthInverted = b[444] != 0;
        g_edge.haveConst = true;
        StoreConstants(b, frame);
        ++g_setConstCalls;
        g_setConstThread = (long)GetCurrentThreadId();
    }
    const int eye = Enabled() ? EyeFor(frame) : -1;
    LogConstants(values, Enabled() ? eye : stereo_render::FrameEye(), vp.value);
    if (eye < 1 || eye > 3) return o_SetConstants(values, frame, vp);
    SlViewport m = Mapped(vp, eye);
    Log1("constants", eye, vp.value, m.value);
    return o_SetConstants(values, frame, m);
}

// The eyes share the game's render targets: tag them on both viewports.
// ResourceTag (64 bytes): base 32, Resource* 32, type 40, lifecycle 44, extent {top,left,width,height} 48.
// Resource: base 32, type 32, native (ID3D12Resource*) 40.
void LogTagSizes(const SlViewport& vp, const void* tags, uint32_t n) {
    static uint32_t s_last[2][4] = {};
    const auto* t = static_cast<const uint8_t*>(tags);
    for (uint32_t i = 0; t && i < n && i < 16; ++i, t += 64) {
        const uint32_t type = *reinterpret_cast<const uint32_t*>(t + 40);
        if (type != 3 && type != 4) continue;  // scaling input / output colour
        const uint8_t* res = *reinterpret_cast<uint8_t* const*>(t + 32);
        auto* native = res ? *reinterpret_cast<ID3D12Resource* const*>(res + 40) : nullptr;
        const uint32_t* ext = reinterpret_cast<const uint32_t*>(t + 48);
        uint32_t w = 0, h = 0;
        if (native) {
            D3D12_RESOURCE_DESC d = native->GetDesc();
            w = (uint32_t)d.Width;
            h = d.Height;
        }
        uint32_t* last = s_last[type - 3];
        if (last[0] != w || last[1] != h || last[2] != ext[2] || last[3] != ext[3]) {
            last[0] = w; last[1] = h; last[2] = ext[2]; last[3] = ext[3];
            LOG("SL: %s colour for viewport %u: texture %u x %u, extent %u x %u at %u,%u%s", type == 3 ? "DLSS input" : "DLSS output",
                vp.value, w, h, ext[2], ext[3], ext[1], ext[0], (type == 3 ? GameStack() : std::string()).c_str());
        }
    }
}

void NoteEdgeTags(const SlViewport& vp, const void* tags, uint32_t n) {
    if (vp.value >= kRightEyeViewportOffset || !tags) return;
    const auto* t = static_cast<const uint8_t*>(tags);
    std::lock_guard<std::mutex> lock(g_edgeMutex);
    for (uint32_t i = 0; i < n && i < 32; ++i, t += 64) {
        const uint32_t type = *reinterpret_cast<const uint32_t*>(t + 40);
        const uint8_t* res = *reinterpret_cast<uint8_t* const*>(t + 32);
        if (!res) continue;
        if (type == 0) {  // depth
            g_edge.depth = *reinterpret_cast<ID3D12Resource* const*>(res + 40);
            g_edge.depthState = *reinterpret_cast<const uint32_t*>(res + 64);
        } else if (type == 1) {  // motion vectors
            memcpy(g_edge.mvTag, t, 64);
            memcpy(g_edge.mvRes, res, 128);
            memcpy(g_edge.extent, t + 48, 16);
            g_edge.haveMv = true;
            static int s_logs = 0;
            if (s_logs++ < 2) {
                auto* native = *reinterpret_cast<ID3D12Resource* const*>(res + 40);
                const D3D12_RESOURCE_DESC d = native ? native->GetDesc() : D3D12_RESOURCE_DESC{};
                LOG("SL: motion vector tag: texture %llu x %u format %d flags 0x%X state 0x%X, extent %u,%u %u x %u, lifecycle %u",
                    (unsigned long long)d.Width, d.Height, (int)d.Format, (unsigned)d.Flags,
                    *reinterpret_cast<const uint32_t*>(res + 64), g_edge.extent[1], g_edge.extent[0], g_edge.extent[2],
                    g_edge.extent[3], *reinterpret_cast<const uint32_t*>(t + 44));
            }
        }
    }
}

// The depth and motion vectors of the evaluate about to run (from the tags and constants of the left viewport).
bool EdgeInputs(stereo_edge::Inputs* out, uint8_t tag[64], uint8_t res[128]) {
    stereo_edge::Inputs in;
    {
        std::lock_guard<std::mutex> lock(g_edgeMutex);
        if (!g_edge.haveMv || !g_edge.haveConst || !g_edge.depth) return false;
        in.depth = g_edge.depth;
        in.depthState = (D3D12_RESOURCE_STATES)g_edge.depthState;
        in.mv = *reinterpret_cast<ID3D12Resource* const*>(g_edge.mvRes + 40);
        in.mvState = (D3D12_RESOURCE_STATES)*reinterpret_cast<const uint32_t*>(g_edge.mvRes + 64);
        in.x = (int)g_edge.extent[1];
        in.y = (int)g_edge.extent[0];
        in.w = (int)g_edge.extent[2];
        in.h = (int)g_edge.extent[3];
        if (!in.w || !in.h) {
            const D3D12_RESOURCE_DESC d = in.mv->GetDesc();
            in.w = (int)d.Width;
            in.h = (int)d.Height;
        }
        in.pixPerMvX = g_edge.mvScale[0] * in.w;
        in.pixPerMvY = g_edge.mvScale[1] * in.h;
        in.depthInverted = g_edge.depthInverted;
        memcpy(tag, g_edge.mvTag, 64);
        memcpy(res, g_edge.mvRes, 128);
    }
    in.eye = stereo_render::FrameEye() & 1;
    in.haveSl = in.haveCamPos = LoadConstants(t_evalFrame, in.sl);
    if (in.haveSl) {
        memcpy(in.jitter, in.sl + 352, 8);  // sl::Constants jitterOffset (pixels)
        in.haveJitter = true;
    }
    memcpy(in.camPos, in.sl + 376, sizeof(in.camPos));
    *out = in;
    return true;
}

// Frame warp (warp.cpp): keep this eye's depth and motion vectors (the rebuilt ones when MotionRebuild made some).
thread_local ID3D12Resource* t_rebuiltMv = nullptr;
void WarpCapture(void* cmd) {
    ID3D12Resource* rebuilt = t_rebuiltMv;
    t_rebuiltMv = nullptr;
    if (!warp::Wanted() || !cmd || stereo_render::FrameEye() < 0) return;
    stereo_edge::Inputs in;
    uint8_t tag[64], res[128];
    if (!EdgeInputs(&in, tag, res)) return;
    if (rebuilt) {
        in.mv = rebuilt;
        in.mvState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    warp::OnUpscalerInputs(static_cast<ID3D12GraphicsCommandList*>(cmd), in, in.eye);
}

// StereoEdgeFix with shared DLSS history: the fixed motion vectors replace the game's for this evaluation.
void EdgeBeforeEvaluate(const SlViewport& vp, void* cmd) {
    if (!stereo_edge::Wanted() || Enabled() || !stereo_render::NativeActive() || !cmd || !o_SetTag) return;
    stereo_edge::Inputs in;
    uint8_t tag[64], res[128];
    if (!EdgeInputs(&in, tag, res)) return;
    ID3D12Resource* out = stereo_edge::Run(static_cast<ID3D12GraphicsCommandList*>(cmd), in);
    if (!out) return;
    *reinterpret_cast<ID3D12Resource**>(res + 40) = out;
    *reinterpret_cast<uint32_t*>(res + 64) = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    *reinterpret_cast<void**>(tag + 0) = nullptr;   // next
    *reinterpret_cast<uint8_t**>(tag + 32) = res;
    *reinterpret_cast<uint32_t*>(tag + 44) = 0;     // eOnlyValidNow: Streamline copies it at once
    SlResult r;
    {
        eye_history::Suspend noHistory;
        r = o_SetTag(vp, tag, 1, cmd);
    }
    static int s_logs = 0;
    if (s_logs++ < 3) LOG("SL: stereo edge fix motion vectors tagged on viewport %u (result %d)", vp.value, r);
}

// MotionRebuild with per-eye DLSS history: this eye's rebuilt motion vectors replace the game's on its viewport.
void RebuildBeforeEvaluate(const SlViewport& vp, void* cmd) {
    if (!mvfix::Wanted() || !cmd || !o_SetTag) return;
    stereo_edge::Inputs in;
    uint8_t tag[64], res[128];
    if (!EdgeInputs(&in, tag, res)) {
        static int s_n = 0;
        if (s_n++ < 3) LOG("SL: motion rebuild: no depth / motion vector tags yet");
        return;
    }
    ID3D12Resource* out = mvfix::Run(static_cast<ID3D12GraphicsCommandList*>(cmd), in);
    if (!out) return;
    t_rebuiltMv = out;
    *reinterpret_cast<ID3D12Resource**>(res + 40) = out;
    *reinterpret_cast<uint32_t*>(res + 64) = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    *reinterpret_cast<void**>(tag + 0) = nullptr;   // next
    *reinterpret_cast<uint8_t**>(tag + 32) = res;
    *reinterpret_cast<uint32_t*>(tag + 44) = 0;     // eOnlyValidNow: Streamline copies it at once
    SlResult r;
    {
        eye_history::Suspend noHistory;  // Streamline copies the tag into its pool: not a history copy of the game
        r = o_SetTag(vp, tag, 1, cmd);
    }
    static int s_logs = 0;
    if (s_logs++ < 6) LOG("SL: rebuilt motion vectors tagged on viewport %u, eye %d (result %d): game %p -> rebuilt %p", vp.value, in.eye, r, (void*)in.mv, (void*)out);
}

SlResult Hk_SetTag(const SlViewport& vp, const void* tags, uint32_t n, void* cmd) {
    LogTagSizes(vp, tags, n);
    NoteEdgeTags(vp, tags, n);
    SlResult r = o_SetTag(vp, tags, n, cmd);
    if (Enabled() && vp.value < kRightEyeViewportOffset)
        for (int v = 1; v <= ExtraViews(); ++v) {
            SlViewport m = Mapped(vp, v);
            o_SetTag(m, tags, n, cmd);
        }
    return r;
}

// Foveated DLSS (fovdlss.cpp): the eye of the evaluate about to run on this thread, and its gaze centre.
void FoveatedDlssEye() {
    const int view = stereo_render::FrameEye();
    const int eye = stereo_render::NativeActive() && view >= 0 ? (view & 1) : 0;
    fovdlss::SetThreadEye(eye);
    if (!vr::g_config.foveatedDlss) return;
    unsigned w = 0, h = 0;
    float gx = 0.f, gy = 0.f;
    if (fovdlss::LastRenderSize(w, h) && vrs::GazeForCurrentEye((float)w / (float)h, &gx, &gy))
        fovdlss::SetEyeCenter(eye, 0.5f + 0.5f * gx, 0.5f - 0.5f * gy);
    else
        fovdlss::SetEyeCenter(eye, 0.5f, 0.5f);
}

SlResult Hk_Evaluate(Feature f, const void* frame, const SlBase** inputs, uint32_t n, void* cmd) {
    if (f != kFeatureDLSS) return o_Evaluate(f, frame, inputs, n, cmd);
    eye_history::Suspend noHistory;  // copies inside the DLSS evaluate (Streamline, NGX, fovdlss) are not the game's history
    t_evalFrame = frame;
    if ((DWORD)g_setConstThread.load() != GetCurrentThreadId()) ++g_constOtherThread;
    FoveatedDlssEye();
    const SlViewport* vp = nullptr;
    if (inputs && n && n <= 16)
        for (uint32_t i = 0; i < n && !vp; ++i)
            if (inputs[i] && IsEqualGUID(inputs[i]->type, kViewportType)) vp = static_cast<const SlViewport*>(inputs[i]);
    if (!Enabled() || !vp) {
        WarpCapture(cmd);  // (the game's motion vectors)
        if (vp && !Enabled()) EdgeBeforeEvaluate(*vp, cmd);
        return o_Evaluate(f, frame, inputs, n, cmd);
    }
    const int eye = EyeFor(frame);
    g_lastEval = GetTickCount64();
    if (eye >= 0) ++g_evals[eye & 1];
    if (eye < 1 || eye > 3) {
        RebuildBeforeEvaluate(*vp, cmd);
        WarpCapture(cmd);  // after the rebuild: the dev dumps hold the vectors DLSS gets
        return o_Evaluate(f, frame, inputs, n, cmd);
    }
    const SlBase* copy[16];
    SlViewport m = Mapped(*vp, eye);
    for (uint32_t i = 0; i < n; ++i) copy[i] = inputs[i] == vp ? &m : inputs[i];
    Log1("evaluate DLSS", eye, vp->value, m.value);
    RebuildBeforeEvaluate(m, cmd);
    WarpCapture(cmd);
    const long a = g_evals[0], b = g_evals[1];
    if ((a + b) % 1800 == 0)
        LOG("SL: DLSS evaluations left %ld right %ld, token/frame eye disagreements %ld; slSetConstants calls %ld (thread %ld, evaluate thread %lu; "
            "%ld evaluates after constants from another thread, %ld frame token misses)",
            a, b, (long)g_eyeDisagree, g_setConstCalls.load(), g_setConstThread.load(), GetCurrentThreadId(), g_constOtherThread.load(),
            g_constTokenMiss.load());
    return o_Evaluate(f, frame, copy, n, cmd);
}

SlResult Hk_Allocate(void* cmd, Feature f, const SlViewport& vp) {
    SlResult r = o_Allocate(cmd, f, vp);
    if (f == kFeatureDLSS && Enabled() && vp.value < kRightEyeViewportOffset)
        for (int v = 1; v <= ExtraViews(); ++v) {
            SlViewport m = Mapped(vp, v);
            SlResult r2 = o_Allocate(cmd, f, m);
            LOG("SL: allocate DLSS viewport %u (result %d) and view %d viewport %u (result %d)", vp.value, r, v, m.value, r2);
        }
    return r;
}

SlResult Hk_Free(Feature f, const SlViewport& vp) {
    SlResult r = o_Free(f, vp);
    if (f == kFeatureDLSS && vp.value < kRightEyeViewportOffset) {
        for (int v = 1; v <= 3; ++v) {
            SlViewport m = Mapped(vp, v);
            o_Free(f, m);  // fine if it was never allocated
        }
        LOG("SL: free DLSS viewport %u and its view viewports", vp.value);
    }
    return r;
}

// DLSSOptions: base 32, mode 32, outputWidth 36, outputHeight 40
// DLSSOptions (sl_dlss.h, struct version 3): base 32, mode 32, outputWidth 36, outputHeight 40, sharpness 44,
// preExposure 48, exposureScale 52, 3 Booleans 56..58, presets dlaa 60 / quality 64 / balanced 68 / performance 72 /
// ultraPerformance 76 / ultraQuality 80, useAutoExposure 84, alphaUpscalingEnabled 85 (size 88).
// DlssPreset != Default: a copy with every preset field set is passed on (both eye viewports).
uint8_t g_optCopy[128];
const void* WithPreset(const void* options) {
    const int preset = vr::g_config.dlssPreset;
    if (!options || !preset) return options;
    const size_t ver = *reinterpret_cast<const size_t*>(static_cast<const uint8_t*>(options) + 24);
    static size_t s_loggedVer = ~0ull;
    if (ver != s_loggedVer) {
        s_loggedVer = ver;
        LOG("SL: DLSSOptions struct version %zu; preset %d forced %s", ver, preset, ver >= 1 ? "" : "(too old: not applied)");
    }
    if (ver < 1) return options;
    memcpy(g_optCopy, options, 88);
    for (int off = 60; off <= 80; off += 4) memcpy(g_optCopy + off, &preset, 4);
    reinterpret_cast<SlBase*>(g_optCopy)->next = nullptr;  // our copy carries no chained structs
    return g_optCopy;
}

// last options the game set (game viewport), to re-apply when DlssPreset changes in the menu
uint8_t g_lastOpt[128];
bool g_haveLastOpt = false;
SlViewport g_lastOptVp{};
std::mutex g_optMutex;

SlResult Hk_DLSSSetOptions(const SlViewport& vp, const void* options_) {
    if (options_ && vp.value < kRightEyeViewportOffset) {
        std::lock_guard<std::mutex> lock(g_optMutex);
        memcpy(g_lastOpt, options_, 88);
        reinterpret_cast<SlBase*>(g_lastOpt)->next = nullptr;
        g_lastOptVp = vp;
        g_lastOptVp.next = nullptr;
        g_haveLastOpt = true;
    }
    const void* options = WithPreset(options_);
    if (options && vp.value < kRightEyeViewportOffset) {
        const auto* o = static_cast<const uint8_t*>(options);
        const uint32_t mode = *reinterpret_cast<const uint32_t*>(o + 32), ow = *reinterpret_cast<const uint32_t*>(o + 36),
                       oh = *reinterpret_cast<const uint32_t*>(o + 40);
        static uint32_t s_m = ~0u, s_w = 0, s_h = 0;
        if (mode != s_m || ow != s_w || oh != s_h) {
            s_m = mode; s_w = ow; s_h = oh;
            LOG("SL: DLSS options viewport %u: mode %u, output %u x %u; from%s", vp.value, mode, ow, oh, GameStack().c_str());
        }
    }
    SlResult r = o_DLSSSetOptions(vp, options);
    if (vp.value < kRightEyeViewportOffset) {  // mirror always (cheap), so switching PerEyeDlss on works live
        for (int v = 1; v <= ExtraViews(); ++v) {
            SlViewport m = Mapped(vp, v);
            SlResult r2 = o_DLSSSetOptions(m, options);
            static std::atomic<int> s_n{0};
            if (s_n.fetch_add(1) < 6)
                LOG("SL: DLSS options for viewport %u (result %d) mirrored to %u (result %d)", vp.value, r, m.value, r2);
        }
    }
    return r;
}

using DLSSGetOptimal_t = SlResult (*)(const void* options, void* settings);
DLSSGetOptimal_t o_DLSSGetOptimal = nullptr;
// DLSSOptimalSettings: base 32, optimalRenderWidth 32, optimalRenderHeight 36
SlResult Hk_DLSSGetOptimal(const void* options, void* settings) {
    SlResult r = o_DLSSGetOptimal(options, settings);
    if (options && settings) {
        const auto* o = static_cast<const uint8_t*>(options);
        const auto* st = static_cast<const uint8_t*>(settings);
        static int s_n = 0;
        if (s_n++ < 40)
            LOG("SL: DLSS optimal settings for mode %u output %u x %u -> render %u x %u (result %d); from%s",
                *reinterpret_cast<const uint32_t*>(o + 32), *reinterpret_cast<const uint32_t*>(o + 36),
                *reinterpret_cast<const uint32_t*>(o + 40), *reinterpret_cast<const uint32_t*>(st + 32),
                *reinterpret_cast<const uint32_t*>(st + 36), r, GameStack().c_str());
    }
    return r;
}

SlResult Hk_GetFeatureFunction(Feature f, const char* name, void*& fn) {
    SlResult r = o_GetFeatureFunction(f, name, fn);
    static int s_n = 0;
    if (s_n++ < 12) LOG("SL: slGetFeatureFunction(feature %u, %s) -> %d", f, name ? name : "?", r);
    if (r == 0 && fn && name && !strcmp(name, "slDLSSGetOptimalSettings")) {
        if (!o_DLSSGetOptimal) o_DLSSGetOptimal = reinterpret_cast<DLSSGetOptimal_t>(fn);
        fn = reinterpret_cast<void*>(&Hk_DLSSGetOptimal);
        LOG("SL: slDLSSGetOptimalSettings wrapped");
    }
    if (r == 0 && fn && name && !strcmp(name, "slDLSSSetOptions")) {
        if (!o_DLSSSetOptions) o_DLSSSetOptions = reinterpret_cast<DLSSSetOptions_t>(fn);
        fn = reinterpret_cast<void*>(&Hk_DLSSSetOptions);
        LOG("SL: slDLSSSetOptions wrapped");
    }
    return r;
}

bool IsStreamline(HMODULE m) {
    char path[MAX_PATH] = {};
    if (!m || !GetModuleFileNameA(m, path, MAX_PATH)) return false;
    const char* n = strrchr(path, '\\');
    n = n ? n + 1 : path;
    return !_stricmp(n, "sl.interposer.dll");
}

template <typename T>
FARPROC Wrap(FARPROC real, T& orig, T hook) {
    if (!orig) orig = reinterpret_cast<T>(real);
    return reinterpret_cast<FARPROC>(hook);
}

// Diagnostics: which features the game asks about, and what Streamline answers (the test PC runs FSR with DLSS
// selected in the game's config, Nov 9)
SlResult Hk_IsFeatureSupported(Feature f, const void* adapterInfo) {
    SlResult r = o_IsFeatureSupported(f, adapterInfo);
    if (f == kFeatureDLSS && vr::g_config.devNoDlss) {
        LOG("SL: slIsFeatureSupported(DLSS) -> %d, reported as unsupported (DevNoDlss)", r);
        r = (SlResult)7;  // eErrorAdapterNotSupported
    }
    LOG("SL: slIsFeatureSupported(feature %u) -> %d", f, r);
    return r;
}
SlResult Hk_Init(const void* pref, uint64_t sdkVersion) {
    const SlResult r = o_Init(pref, sdkVersion);
    const auto* p = static_cast<const uint8_t*>(pref);
    // sl::Preferences: showConsole 32, logLevel 36, pathsToPlugins 40, numPathsToPlugins 48, pathToLogsAndData 56,
    // logMessageCallback 64, applicationId 72, engine 76, engineVersion 80, projectId 88, renderAPI 96, flags 100,
    // featuresToLoad 104, numFeaturesToLoad 112
    uint32_t nf = p ? *reinterpret_cast<const uint32_t*>(p + 112) : 0;
    const uint32_t* feats = p ? *reinterpret_cast<const uint32_t* const*>(p + 104) : nullptr;
    char list[128] = "";
    for (uint32_t i = 0; feats && i < nf && i < 8; ++i) snprintf(list + strlen(list), sizeof(list) - strlen(list), " %u", feats[i]);
    LOG("SL: slInit(sdk 0x%llX, flags 0x%X, %u features:%s) -> %d", (unsigned long long)sdkVersion,
        p ? *reinterpret_cast<const uint32_t*>(p + 100) : 0, nf, list, r);
    return r;
}

SlResult Hk_IsFeatureLoaded(Feature f, bool& loaded) {
    const SlResult r = o_IsFeatureLoaded(f, loaded);
    LOG("SL: slIsFeatureLoaded(feature %u) -> %d, loaded %d", f, r, (int)loaded);
    return r;
}
SlResult Hk_SetFeatureLoaded(Feature f, bool loaded) {
    const SlResult r = o_SetFeatureLoaded(f, loaded);
    LOG("SL: slSetFeatureLoaded(feature %u, %d) -> %d", f, (int)loaded, r);
    return r;
}
SlResult Hk_GetFeatureVersion(Feature f, void* version) {
    const SlResult r = o_GetFeatureVersion(f, version);
    const auto* v = static_cast<const uint32_t*>(version);  // sl::FeatureVersion: base 32 bytes, then versionSL (3 u32), versionNGX (3 u32)
    LOG("SL: slGetFeatureVersion(feature %u) -> %d, SL %u.%u.%u, NGX %u.%u.%u", f, r, v ? v[8] : 0, v ? v[9] : 0, v ? v[10] : 0,
        v ? v[11] : 0, v ? v[12] : 0, v ? v[13] : 0);
    return r;
}
SlResult Hk_GetFeatureRequirements(Feature f, void* req) {
    const SlResult r = o_GetFeatureRequirements(f, req);
    const auto* q = static_cast<const uint32_t*>(req);  // sl::FeatureRequirements: flags at 32, maxNumViewports 36, maxNumCPUThreads 40
    LOG("SL: slGetFeatureRequirements(feature %u) -> %d, flags 0x%X, viewports %u", f, r, q ? q[8] : 0, q ? q[9] : 0);
    return r;
}

FARPROC WINAPI Hk_GetProcAddress(HMODULE m, LPCSTR name) {
    FARPROC p = o_GetProcAddress(m, name);
    if (!p || !HIWORD(reinterpret_cast<ULONG_PTR>(name)) || strncmp(name, "sl", 2) != 0 || !IsStreamline(m)) return p;
    FARPROC r = p;
    if (!strcmp(name, "slGetNewFrameToken")) r = Wrap(p, o_GetNewFrameToken, &Hk_GetNewFrameToken);
    else if (!strcmp(name, "slSetConstants")) r = Wrap(p, o_SetConstants, &Hk_SetConstants);
    else if (!strcmp(name, "slSetTag")) r = Wrap(p, o_SetTag, &Hk_SetTag);
    else if (!strcmp(name, "slEvaluateFeature")) r = Wrap(p, o_Evaluate, &Hk_Evaluate);
    else if (!strcmp(name, "slAllocateResources")) r = Wrap(p, o_Allocate, &Hk_Allocate);
    else if (!strcmp(name, "slFreeResources")) r = Wrap(p, o_Free, &Hk_Free);
    else if (!strcmp(name, "slGetFeatureFunction")) r = Wrap(p, o_GetFeatureFunction, &Hk_GetFeatureFunction);
    else if (!strcmp(name, "slIsFeatureSupported")) r = Wrap(p, o_IsFeatureSupported, &Hk_IsFeatureSupported);
    else if (!strcmp(name, "slInit")) r = Wrap(p, o_Init, &Hk_Init);
    else if (!strcmp(name, "slIsFeatureLoaded")) r = Wrap(p, o_IsFeatureLoaded, &Hk_IsFeatureLoaded);
    else if (!strcmp(name, "slSetFeatureLoaded")) r = Wrap(p, o_SetFeatureLoaded, &Hk_SetFeatureLoaded);
    else if (!strcmp(name, "slGetFeatureVersion")) r = Wrap(p, o_GetFeatureVersion, &Hk_GetFeatureVersion);
    else if (!strcmp(name, "slGetFeatureRequirements")) r = Wrap(p, o_GetFeatureRequirements, &Hk_GetFeatureRequirements);
    if (r != p) LOG("SL: %s wrapped", name);
    return r;
}

}  // namespace

void ReapplyDlssOptions() {
    uint8_t opt[128];
    SlViewport vp{};
    {
        std::lock_guard<std::mutex> lock(g_optMutex);
        if (!g_haveLastOpt || !o_DLSSSetOptions) {
            LOG("SL: DLSS model change saved; applies when the game next sets its DLSS options");
            return;
        }
        memcpy(opt, g_lastOpt, sizeof(opt));
        vp = g_lastOptVp;
    }
    const SlResult r = Hk_DLSSSetOptions(vp, opt);
    LOG("SL: DLSS options re-applied (preset %d, result %d)", vr::g_config.dlssPreset, r);
}

bool DlssEvaluatingRecently() { return GetTickCount64() - g_lastEval.load() < 500; }

void Install() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;
    int patched = 0;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), "kernel32.dll") != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* addrs = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addrs) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (strcmp(reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name, "GetProcAddress")) continue;
            DWORD old;
            if (!VirtualProtect(&addrs->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) continue;
            o_GetProcAddress = reinterpret_cast<GetProcAddress_t>(addrs->u1.Function);
            addrs->u1.Function = reinterpret_cast<ULONG_PTR>(&Hk_GetProcAddress);
            VirtualProtect(&addrs->u1.Function, sizeof(void*), old, &old);
            ++patched;
        }
    }
    LOG("SL: GetProcAddress import %s (PerEyeDlss=%d)", patched ? "redirected" : "NOT found", (int)vr::g_config.perEyeDlss);
}

}  // namespace sl_hooks
