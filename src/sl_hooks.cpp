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

GetNewFrameToken_t o_GetNewFrameToken = nullptr;
SetConstants_t o_SetConstants = nullptr;
SetTag_t o_SetTag = nullptr;
Evaluate_t o_Evaluate = nullptr;
Allocate_t o_Allocate = nullptr;
Free_t o_Free = nullptr;
GetFeatureFunction_t o_GetFeatureFunction = nullptr;
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

SlResult Hk_SetConstants(const void* values, const void* frame, const SlViewport& vp) {
    const int eye = Enabled() ? EyeFor(frame) : -1;
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

SlResult Hk_SetTag(const SlViewport& vp, const void* tags, uint32_t n, void* cmd) {
    LogTagSizes(vp, tags, n);
    SlResult r = o_SetTag(vp, tags, n, cmd);
    if (Enabled() && vp.value < kRightEyeViewportOffset)
        for (int v = 1; v <= ExtraViews(); ++v) {
            SlViewport m = Mapped(vp, v);
            o_SetTag(m, tags, n, cmd);
        }
    return r;
}

SlResult Hk_Evaluate(Feature f, const void* frame, const SlBase** inputs, uint32_t n, void* cmd) {
    if (f != kFeatureDLSS || !Enabled() || !inputs || n == 0 || n > 16) return o_Evaluate(f, frame, inputs, n, cmd);
    const int eye = EyeFor(frame);
    g_lastEval = GetTickCount64();
    if (eye >= 0) ++g_evals[eye & 1];
    if (eye < 1 || eye > 3) return o_Evaluate(f, frame, inputs, n, cmd);
    const SlBase* copy[16];
    SlViewport m{};
    bool found = false;
    for (uint32_t i = 0; i < n; ++i) {
        copy[i] = inputs[i];
        if (!found && inputs[i] && IsEqualGUID(inputs[i]->type, kViewportType)) {
            m = Mapped(*static_cast<const SlViewport*>(inputs[i]), eye);
            copy[i] = &m;
            found = true;
            Log1("evaluate DLSS", eye, static_cast<const SlViewport*>(inputs[i])->value, m.value);
        }
    }
    const long a = g_evals[0], b = g_evals[1];
    if ((a + b) % 1800 == 0)
        LOG("SL: DLSS evaluations left %ld right %ld, token/frame eye disagreements %ld", a, b, (long)g_eyeDisagree);
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
SlResult Hk_DLSSSetOptions(const SlViewport& vp, const void* options) {
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
    if (r != p) LOG("SL: %s wrapped", name);
    return r;
}

}  // namespace

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
