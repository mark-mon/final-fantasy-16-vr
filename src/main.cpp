// FF16VR - dinput8.dll proxy entry point.
// The game statically imports DINPUT8.dll, so placing this DLL in the game
// folder gets us loaded at startup. All real DirectInput calls are forwarded
// to the system dinput8.dll.

#include <windows.h>
#include <unknwn.h>
#include <string>

#include "version.h"
#include "config.h"
#include "dxgi_hooks.h"
#include "log.h"
#include "patches.h"
#include "rdoc.h"
#include "exit_trace.h"
#include "cam_probe.h"
#include "cam_prev.h"
#include "camera_hook.h"
#include "stereo_render.h"
#include "render_size.h"
#include "motion_blur.h"
#include "sl_hooks.h"
#include "dev_pad.h"
#include "pass_prof.h"
#include "dinput_hook.h"

static HMODULE g_self = nullptr;
static HMODULE g_realDinput = nullptr;

static FARPROC RealProc(const char* name) {
    if (!g_realDinput) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\dinput8.dll");
        g_realDinput = LoadLibraryW(path);
        if (!g_realDinput) {
            LOG("ERROR: could not load system dinput8.dll (err %lu)", GetLastError());
            return nullptr;
        }
    }
    return GetProcAddress(g_realDinput, name);
}

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID* out, IUnknown* outer) {
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, IUnknown*);
    auto fn = reinterpret_cast<Fn>(RealProc("DirectInput8Create"));
    HRESULT hr = fn ? fn(hinst, ver, riid, out, outer) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out) dinput_hook::OnCreated(*out);
    return hr;
}

HRESULT WINAPI Proxy_DllCanUnloadNow() {
    using Fn = HRESULT(WINAPI*)();
    auto fn = reinterpret_cast<Fn>(RealProc("DllCanUnloadNow"));
    return fn ? fn() : S_FALSE;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    auto fn = reinterpret_cast<Fn>(RealProc("DllGetClassObject"));
    return fn ? fn(clsid, riid, out) : E_FAIL;
}

HRESULT WINAPI Proxy_DllRegisterServer() {
    using Fn = HRESULT(WINAPI*)();
    auto fn = reinterpret_cast<Fn>(RealProc("DllRegisterServer"));
    return fn ? fn() : E_FAIL;
}

HRESULT WINAPI Proxy_DllUnregisterServer() {
    using Fn = HRESULT(WINAPI*)();
    auto fn = reinterpret_cast<Fn>(RealProc("DllUnregisterServer"));
    return fn ? fn() : E_FAIL;
}

}  // extern "C"

static void LoadPlugins() {
    using namespace vr;
    if (g_config.loadAsiPlugins) {
        const wchar_t* dirs[] = {L"", L"\\scripts", L"\\plugins"};
        for (const wchar_t* sub : dirs) {
            std::wstring dir = g_gameDir + sub;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((dir + L"\\*.asi").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                std::wstring p = dir + L"\\" + fd.cFileName;
                HMODULE m = LoadLibraryW(p.c_str());
                LOG("ASI plugin %ls: %s", fd.cFileName, m ? "loaded" : "FAILED");
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    for (const std::wstring& name : g_config.chainLoad) {
        std::wstring p = g_gameDir + L"\\" + name;
        HMODULE m = LoadLibraryW(p.c_str());
        LOG("ChainLoad %ls: %s (err %lu)", name.c_str(), m ? "loaded" : "FAILED", m ? 0 : GetLastError());
    }
}

static DWORD WINAPI InitThread(LPVOID) {
    using namespace vr;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_gameDir = exe;
    g_gameDir = g_gameDir.substr(0, g_gameDir.find_last_of(L"\\/"));

    LogInit((g_gameDir + L"\\ff16vr.log").c_str());
    LOG("FF16VR %s loaded (build %s %s). Game: %ls", FF16VR_VERSION, __DATE__, __TIME__, exe);

    LoadConfig();
    exittrace::Install();

    if (!g_config.enabled) {
        LOG("Disabled in config.txt");
        return 0;
    }

    // Patch game code before any plugin gets a chance to touch the same spots.
    patches::ApplyGamePatches();
    camera_hook::Install();  // camera source hook (test offset via F9 / ff16vr_camtest.trigger)
    stereo_render::Install();  // native stereo (frame twice per tick) + Render() timing
    cam_prev::Install();       // per-eye previous camera in the GPU camera constants (per-eye DLSS)
    sl_hooks::Install();       // Streamline (DLSS): right eye on its own viewport (per-eye history)
    motion_blur::Start();      // DisableMotionBlur: menu round trip after loads when the game left motion blur on
    render_size::Install();    // user32 sizing import logging + ff16vr_resize.trigger
    pass_prof::Install();      // dev: ff16vr_prof.trigger samples the game thread per frame pass
    dev_pad::Install();        // XInput import hook (menu pad blocking) + ff16vr_pad.trigger for automated tests
    rdoc::Init();  // must happen before the game creates its D3D12 device
    cam_probe::Init();  // idle until <game>\ff16vr_camprobe.trigger appears
    LoadPlugins();

    if (!g_config.vrOutput && !g_config.frameProbe) {
        LOG("VROutput=0: game patches applied, VR output off (capture mode)");
        return 0;
    }
    if (!g_config.vrOutput) LOG("VROutput=0 + FrameProbe=1: swap chain hooks for the frame probe only, no OpenXR");

    if (!hooks::InstallDxgiHooks()) {
        LOG("ERROR: DXGI hooks failed to install; VR disabled");
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        // Do the real work outside the loader lock.
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
