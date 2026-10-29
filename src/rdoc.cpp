#include "rdoc.h"

#include <windows.h>
#include <unknwn.h>
#include <dxgi1_6.h>
#include <d3d12.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../third_party/renderdoc/renderdoc_app.h"
#include "config.h"
#include "log.h"
#include "pso_guard.h"
#include "taa.h"

namespace rdoc {
namespace {

RENDERDOC_API_1_6_0* g_api = nullptr;

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

DWORD WINAPI TriggerThread(LPVOID) {
    const std::wstring trigger = vr::g_gameDir + L"\\ff16vr_capture.trigger";
    const std::wstring done = vr::g_gameDir + L"\\ff16vr_capture.done";
    bool keyWasDown = true;  // ignore a key held at start
    for (;;) {
        Sleep(50);
        // F7 (any window in front): one-frame capture; every capture gets its own file name (..._frameN.rdc)
        const bool keyDown = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        const bool keyPressed = keyDown && !keyWasDown;
        keyWasDown = keyDown;
        const bool fileTrigger = GetFileAttributesW(trigger.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (!keyPressed && !fileTrigger) continue;

        uint32_t frames = 1;
        if (fileTrigger) {
            if (FILE* f = _wfopen(trigger.c_str(), L"r")) {
                unsigned n = 0;
                if (fscanf(f, "%u", &n) == 1 && n >= 1 && n <= 10) frames = n;
                fclose(f);
            }
            DeleteFileW(trigger.c_str());
        } else {
            // two frames: in Native stereo that is one frame per eye, so captures of two states can be compared
            // eye for eye (the right eye's frame runs with the game clock frozen)
            frames = 2;
            LOG("RenderDoc: F7 pressed (capturing 2 frames: left and right eye in Native)");
            MessageBeep(MB_OK);
        }
        DeleteFileW(done.c_str());

        uint32_t before = g_api->GetNumCaptures();
        if (frames == 1) g_api->TriggerCapture();
        else g_api->TriggerMultiFrameCapture(frames);
        LOG("RenderDoc: capture of %u frame(s) requested (captures so far: %u)", frames, before);

        // Wait for RenderDoc to finish writing.
        std::string path;
        for (int i = 0; i < 720 && path.empty(); ++i) {  // up to 3 min (two large captures)
            Sleep(250);
            uint32_t now = g_api->GetNumCaptures();
            if (now >= before + frames) {
                uint32_t len = 0;
                uint64_t ts = 0;
                g_api->GetCapture(now - 1, nullptr, &len, &ts);
                path.resize(len);
                g_api->GetCapture(now - 1, path.data(), &len, &ts);
                if (!path.empty() && path.back() == '\0') path.pop_back();
            }
        }
        for (uint32_t k = before; !path.empty() && k + 1 < g_api->GetNumCaptures(); ++k) {  // log the earlier ones too
            uint32_t len = 0;
            uint64_t ts = 0;
            g_api->GetCapture(k, nullptr, &len, &ts);
            std::string p(len, '\0');
            g_api->GetCapture(k, p.data(), &len, &ts);
            LOG("RenderDoc: capture written: %s", p.c_str());
        }
        if (path.empty()) {
            LOG("RenderDoc: no capture appeared within 3 min");
            path = "FAILED";
        } else {
            LOG("RenderDoc: capture written: %s", path.c_str());
            MessageBeep(MB_ICONASTERISK);
        }
        if (FILE* f = _wfopen(done.c_str(), L"w")) {
            fputs(path.c_str(), f);
            fclose(f);
        }
    }
}

}  // namespace

void Load();

using CreateDevice_t = HRESULT(WINAPI*)(IUnknown*, int, REFIID, void**);
using CreateFactory2_t = HRESULT(WINAPI*)(UINT, REFIID, void**);

static void* RealExport(const wchar_t* dll, const char* name) {
    HMODULE m = GetModuleHandleW(dll);
    if (!m) m = LoadLibraryW(dll);
    return m ? reinterpret_cast<void*>(GetProcAddress(m, name)) : nullptr;
}

static HRESULT WINAPI Wrap_D3D12CreateDevice(IUnknown* adapter, int level, REFIID riid, void** out) {
    if (!g_api) Load();
    auto fn = reinterpret_cast<CreateDevice_t>(RealExport(L"d3d12.dll", "D3D12CreateDevice"));
    HRESULT hr = fn ? fn(adapter, level, riid, out) : E_FAIL;
    LOG("RenderDoc capture mode: D3D12CreateDevice (direct, Streamline bypassed) -> 0x%08lX", hr);
    if (SUCCEEDED(hr) && out && *out && g_api) {
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&dev))) && dev) {
            pso_guard::Install(dev);
            dev->Release();
        }
        taa::OnDeviceCreated(static_cast<ID3D12Device*>(*out));  // TAA option / character motion fix in captures too
    }
    return hr;
}

// The game's factory stays native (it checks its adapter on it and quits if RenderDoc wraps it), but its
// swap chain must come from a RenderDoc-wrapped factory, because the queue it passes is RenderDoc-wrapped.
// So the native CreateSwapChainForHwnd is redirected to a wrapped factory made on the spot. RenderDoc's
// wrapper then calls the native method itself (thread-local guard lets that call through).
using CreateSwapChainForHwnd_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                             IDXGISwapChain1**);
static CreateSwapChainForHwnd_t o_CreateSwapChainForHwnd = nullptr;
static thread_local bool t_inRedirect = false;

static HRESULT STDMETHODCALLTYPE Redirect_CreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* dev, HWND hwnd,
                                                                 const DXGI_SWAP_CHAIN_DESC1* desc,
                                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                                                 IDXGIOutput* output, IDXGISwapChain1** out) {
    if (t_inRedirect || !g_api) return o_CreateSwapChainForHwnd(self, dev, hwnd, desc, fs, output, out);
    t_inRedirect = true;
    HRESULT hr = E_FAIL;
    IDXGIFactory2* wrapped = nullptr;
    auto create = reinterpret_cast<CreateFactory2_t>(RealExport(L"dxgi.dll", "CreateDXGIFactory2"));
    if (create && SUCCEEDED(create(0, IID_PPV_ARGS(&wrapped))) && wrapped) {
        hr = wrapped->CreateSwapChainForHwnd(dev, hwnd, desc, fs, output, out);
        wrapped->Release();
    }
    t_inRedirect = false;
    LOG("RenderDoc capture mode: swap chain created through a RenderDoc factory -> 0x%08lX", hr);
    return hr;
}

static HRESULT WINAPI Wrap_CreateDXGIFactory2(UINT flags, REFIID riid, void** out) {
    // Not loading RenderDoc here: the game checks its adapter on this factory and quits if RenderDoc wraps it.
    auto fn = reinterpret_cast<CreateFactory2_t>(RealExport(L"dxgi.dll", "CreateDXGIFactory2"));
    HRESULT hr = fn ? fn(flags, riid, out) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out && !o_CreateSwapChainForHwnd) {
        IDXGIFactory2* f2 = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&f2)))) {
            void** vt = *reinterpret_cast<void***>(f2);
            DWORD old;
            VirtualProtect(&vt[15], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
            o_CreateSwapChainForHwnd = reinterpret_cast<CreateSwapChainForHwnd_t>(vt[15]);
            vt[15] = reinterpret_cast<void*>(&Redirect_CreateSwapChainForHwnd);
            VirtualProtect(&vt[15], sizeof(void*), old, &old);
            f2->Release();
        }
    }
    LOG("RenderDoc capture mode: CreateDXGIFactory2 (direct, Streamline bypassed) -> 0x%08lX", hr);
    return hr;
}

// Without RenderDoc (config PsoGuard = 1): wrap Streamline's D3D12CreateDevice to put the PSO guard, in
// log-only mode, on the device the game really uses.
static CreateDevice_t o_SlCreateDevice = nullptr;
static HRESULT WINAPI GuardWrap_D3D12CreateDevice(IUnknown* adapter, int level, REFIID riid, void** out) {
    HRESULT hr = o_SlCreateDevice(adapter, level, riid, out);
    LOG("Streamline D3D12CreateDevice -> 0x%08lX", hr);
    if (SUCCEEDED(hr) && out && *out) {
        taa::OnDeviceCreated(static_cast<ID3D12Device*>(*out));  // the object the game holds (Streamline's proxy)
        ID3D12Device* dev = nullptr;
        if (vr::g_config.psoGuard && SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&dev))) && dev) {
            pso_guard::Install(dev, false);
            dev->Release();
        }
    }
    return hr;
}

void* WrapProc(void* module, const char* name, void* proc) {
    if (!vr::g_config.renderDoc && proc && module == GetModuleHandleW(L"sl.interposer.dll") &&
        !strcmp(name, "D3D12CreateDevice")) {
        o_SlCreateDevice = reinterpret_cast<CreateDevice_t>(proc);
        return reinterpret_cast<void*>(&GuardWrap_D3D12CreateDevice);
    }
    if (!vr::g_config.renderDoc || vr::g_config.renderDocDelayMs >= 0 || !proc) return proc;
    if (module != GetModuleHandleW(L"sl.interposer.dll")) return proc;
    if (!strcmp(name, "D3D12CreateDevice")) return reinterpret_cast<void*>(&Wrap_D3D12CreateDevice);
    if (!strcmp(name, "CreateDXGIFactory2")) return reinterpret_cast<void*>(&Wrap_CreateDXGIFactory2);
    return proc;
}

DWORD WINAPI DelayedLoad(LPVOID) {
    Sleep((DWORD)vr::g_config.renderDocDelayMs);
    Load();
    return 0;
}

void Init() {
    if (!vr::g_config.renderDoc) return;
    if (vr::g_config.renderDocDelayMs < 0) {
        LOG("RenderDoc: will load when the game creates its D3D12 device (Streamline bypassed)");
        return;
    }
    if (vr::g_config.renderDocDelayMs == 0) { Load(); return; }
    LOG("RenderDoc: loading in %d ms", vr::g_config.renderDocDelayMs);
    HANDLE t = CreateThread(nullptr, 0, DelayedLoad, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

void Load() {
    const vr::Config& c = vr::g_config;

    std::wstring dll = c.renderDocDir + L"\\renderdoc.dll";
    HMODULE mod = GetModuleHandleW(L"renderdoc.dll");
    if (!mod) mod = LoadLibraryW(dll.c_str());
    if (!mod) {
        LOG("RenderDoc: could not load %ls (err %lu)", dll.c_str(), GetLastError());
        return;
    }
    auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"));
    if (!getApi || !getApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&g_api)) || !g_api) {
        LOG("RenderDoc: RENDERDOC_GetAPI failed");
        g_api = nullptr;
        return;
    }
    int major = 0, minor = 0, patch = 0;
    g_api->GetAPIVersion(&major, &minor, &patch);

    CreateDirectoryW(c.captureDir.c_str(), nullptr);
    std::string tmpl = Narrow(c.captureDir + L"\\ff16");
    g_api->SetCaptureFilePathTemplate(tmpl.c_str());
    g_api->SetCaptureOptionU32(eRENDERDOC_Option_RefAllResources, 0);
    g_api->SetCaptureOptionU32(eRENDERDOC_Option_CaptureCallstacks, 0);
    g_api->SetCaptureOptionU32(eRENDERDOC_Option_APIValidation, 0);
    if (c.renderDocNvApi) {
        int ok = g_api->SetCaptureOptionU32(eRENDERDOC_Option_AllowUnsupportedVendorExtensions, 0x10DE);
        LOG("RenderDoc: NvAPI passthrough (AllowUnsupportedVendorExtensions=0x10DE) -> %d", ok);
    }
    RENDERDOC_InputButton keys[] = {eRENDERDOC_Key_F12};
    g_api->SetCaptureKeys(keys, 1);

    LOG("RenderDoc %d.%d.%d loaded from %ls; captures -> %s*.rdc (F7, F12 or trigger file)", major, minor, patch,
        dll.c_str(), tmpl.c_str());

    HANDLE t = CreateThread(nullptr, 0, TriggerThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

}  // namespace rdoc
