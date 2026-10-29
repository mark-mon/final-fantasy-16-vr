#include "dxgi_hooks.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <atomic>
#include <mutex>

#include "log.h"
#include "xr_screen.h"
#include "frame_probe.h"
#include "stereo_render.h"
#include "hud_layer.h"
#include "render_size.h"
#include "mem_scan.h"
#include "motion_blur.h"
#include "watch.h"
#include "vrs.h"
#include "config.h"

namespace hooks {

// ---- vtable slot indices (native DXGI COM layout) ----
// IDXGIFactory
constexpr int kFactory_CreateSwapChain = 10;
// IDXGIFactory2
constexpr int kFactory_CreateSwapChainForHwnd = 15;
// IDXGISwapChain
constexpr int kSwap_Present = 8;
constexpr int kSwap_ResizeBuffers = 13;
// IDXGISwapChain1
constexpr int kSwap_Present1 = 22;
// IDXGISwapChain3
constexpr int kSwap_ResizeBuffers1 = 39;

using CreateSwapChain_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwnd_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                             IDXGISwapChain1**);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffers_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeBuffers1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*,
                                                     IUnknown* const*);

static CreateSwapChain_t o_CreateSwapChain = nullptr;
static CreateSwapChainForHwnd_t o_CreateSwapChainForHwnd = nullptr;
static Present_t o_Present = nullptr;
static Present1_t o_Present1 = nullptr;
static ResizeBuffers_t o_ResizeBuffers = nullptr;
static ResizeBuffers1_t o_ResizeBuffers1 = nullptr;

static std::once_flag g_swapHooksOnce;
static thread_local int t_presentDepth = 0;

// Replace one vtable entry, returning the previous function.
static void* PatchVtable(void** vtable, int index, void* hook) {
    void* original = vtable[index];
    if (original == hook) return nullptr;
    DWORD oldProtect;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LOG("ERROR: VirtualProtect failed on vtable slot %d (err %lu)", index, GetLastError());
        return nullptr;
    }
    vtable[index] = hook;
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), &vtable[index], sizeof(void*));
    return original;
}

// NVIDIA Streamline wraps D3D12 objects in proxies. If the queue handed to DXGI
// is a proxy, unwrap it so our command lists go straight to the native queue.
template <typename T>
static T* UnwrapStreamline(T* obj) {
    HMODULE sl = GetModuleHandleW(L"sl.interposer.dll");
    if (!sl || !obj) return obj;
    using GetNative_t = int(__cdecl*)(void*, void**);
    auto getNative = reinterpret_cast<GetNative_t>(GetProcAddress(sl, "slGetNativeInterface"));
    if (!getNative) return obj;
    void* base = nullptr;
    int res = getNative(obj, &base);
    if (res == 0 && base && base != obj) {
        LOG("Streamline: unwrapped proxy %p -> native %p", obj, base);
        return static_cast<T*>(base);
    }
    return obj;
}

static HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (t_presentDepth++ == 0 && !(flags & DXGI_PRESENT_TEST)) { frame_probe::OnPresent(); render_size::OnPresent(); mem_scan::OnPresent(); watch::OnPresent(); hud::BeforeVr(); if (vr::g_config.vrOutput) xr::OnPresent(sc); hud::AfterVr(sc); }
    const bool outer = t_presentDepth == 1 && !(flags & DXGI_PRESENT_TEST);
    if (outer) stereo_render::OnPresentBegin();
    if (outer && stereo_render::SkipThisPresent()) {
        --t_presentDepth;
        return S_OK;
    }
    HRESULT hr = o_Present(sc, sync, flags);
    if (outer) stereo_render::OnPresentEnd();
    --t_presentDepth;
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                               const DXGI_PRESENT_PARAMETERS* params) {
    if (t_presentDepth++ == 0 && !(flags & DXGI_PRESENT_TEST)) { frame_probe::OnPresent(); render_size::OnPresent(); mem_scan::OnPresent(); watch::OnPresent(); hud::BeforeVr(); if (vr::g_config.vrOutput) xr::OnPresent(sc); hud::AfterVr(sc); }
    const bool outer = t_presentDepth == 1 && !(flags & DXGI_PRESENT_TEST);
    if (outer) stereo_render::OnPresentBegin();
    if (outer && stereo_render::SkipThisPresent()) {
        --t_presentDepth;
        return S_OK;
    }
    HRESULT hr = o_Present1(sc, sync, flags, params);
    if (outer) stereo_render::OnPresentEnd();
    --t_presentDepth;
    return hr;
}

// VRS: render size and the swap chain's buffers (never foveated)
static void NoteSwapChain(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC d{};
    if (FAILED(sc->GetDesc(&d))) return;
    vrs::OnSwapChainSize(d.BufferDesc.Width, d.BufferDesc.Height);
    ID3D12Resource* bufs[4] = {};
    unsigned n = 0;
    for (UINT i = 0; i < d.BufferCount && i < 4; ++i) {
        ID3D12Resource* r = nullptr;
        if (SUCCEEDED(sc->GetBuffer(i, IID_PPV_ARGS(&r)))) {
            bufs[n++] = r;
            r->Release();  // the swap chain keeps it alive; only compared by address
        }
    }
    vrs::SetBackBuffer(bufs, n);
}

static HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt,
                                                    UINT flags) {
    xr::OnResizeBuffers(sc);
    render_size::OnSwapChain(nullptr, w, h, "ResizeBuffers");
    HRESULT hr = o_ResizeBuffers(sc, count, w, h, fmt, flags);
    if (SUCCEEDED(hr)) {
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(sc->GetDesc(&d))) hud::OnSwapChainSize(d.BufferDesc.Width, d.BufferDesc.Height);
        NoteSwapChain(sc);
    }
    LOG("ResizeBuffers(%u x %u, fmt %d, count %u) -> 0x%08lX", w, h, (int)fmt, count, hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers1(IDXGISwapChain3* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt,
                                                     UINT flags, const UINT* nodeMask, IUnknown* const* queues) {
    xr::OnResizeBuffers(sc);
    render_size::OnSwapChain(nullptr, w, h, "ResizeBuffers1");
    HRESULT hr = o_ResizeBuffers1(sc, count, w, h, fmt, flags, nodeMask, queues);
    if (SUCCEEDED(hr)) {
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(sc->GetDesc(&d))) hud::OnSwapChainSize(d.BufferDesc.Width, d.BufferDesc.Height);
        NoteSwapChain(sc);
    }
    LOG("ResizeBuffers1(%u x %u, fmt %d, count %u) -> 0x%08lX", w, h, (int)fmt, count, hr);
    return hr;
}

static void HookSwapChain(IDXGISwapChain* sc) {
    std::call_once(g_swapHooksOnce, [sc] {
        void** vt = *reinterpret_cast<void***>(sc);
        o_Present = reinterpret_cast<Present_t>(PatchVtable(vt, kSwap_Present, (void*)&Hook_Present));
        o_ResizeBuffers = reinterpret_cast<ResizeBuffers_t>(PatchVtable(vt, kSwap_ResizeBuffers, (void*)&Hook_ResizeBuffers));

        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) {
            o_Present1 = reinterpret_cast<Present1_t>(PatchVtable(vt, kSwap_Present1, (void*)&Hook_Present1));
            o_ResizeBuffers1 =
                reinterpret_cast<ResizeBuffers1_t>(PatchVtable(vt, kSwap_ResizeBuffers1, (void*)&Hook_ResizeBuffers1));
            sc3->Release();
        }
        LOG("Swap chain hooks installed (vtable %p): Present=%p Present1=%p ResizeBuffers=%p ResizeBuffers1=%p", vt,
            o_Present, o_Present1, o_ResizeBuffers, o_ResizeBuffers1);
    });
}

static void OnSwapChainCreated(IUnknown* device, IDXGISwapChain* sc, UINT w, UINT h, DXGI_FORMAT fmt) {
    ID3D12CommandQueue* queue = nullptr;
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) {
        LOG("Swap chain %p created for a non-D3D12 device; ignoring", sc);
        return;
    }
    ID3D12CommandQueue* native = UnwrapStreamline(queue);
    if (native != queue) {
        native->AddRef();
        queue->Release();
        queue = native;
    }
    D3D12_COMMAND_QUEUE_DESC qd = queue->GetDesc();
    LOG("Swap chain %p created: %u x %u fmt %d, queue %p (type %d)", sc, w, h, (int)fmt, queue, (int)qd.Type);

    HookSwapChain(sc);
    {
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&dev)))) {
            hud::OnDevice(dev, queue);
            render_size::OnDevice(dev);
            vrs::OnDevice(dev, queue);
            dev->Release();
        }
        hud::OnSwapChainSize(w, h);
        NoteSwapChain(sc);
    }
    xr::OnSwapChainCreated(sc, queue);  // takes ownership of the queue reference
}

static HRESULT STDMETHODCALLTYPE Hook_CreateSwapChain(IDXGIFactory* f, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                                      IDXGISwapChain** out) {
    HRESULT hr = o_CreateSwapChain(f, device, desc, out);
    if (desc) render_size::OnSwapChain(desc->OutputWindow, desc->BufferDesc.Width, desc->BufferDesc.Height, "CreateSwapChain");
    if (SUCCEEDED(hr) && out && *out && desc)
        OnSwapChainCreated(device, *out, desc->BufferDesc.Width, desc->BufferDesc.Height, desc->BufferDesc.Format);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* device, HWND hwnd,
                                                             const DXGI_SWAP_CHAIN_DESC1* desc,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                                             IDXGIOutput* output, IDXGISwapChain1** out) {
    HRESULT hr = o_CreateSwapChainForHwnd(f, device, hwnd, desc, fs, output, out);
    if (desc) render_size::OnSwapChain(hwnd, desc->Width, desc->Height, "CreateSwapChainForHwnd");
    if (SUCCEEDED(hr) && out && *out && desc) OnSwapChainCreated(device, *out, desc->Width, desc->Height, desc->Format);
    return hr;
}

bool InstallDxgiHooks() {
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (!dxgi) dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) {
        LOG("ERROR: dxgi.dll not available");
        return false;
    }
    using CreateFactory1_t = HRESULT(WINAPI*)(REFIID, void**);
    auto createFactory = reinterpret_cast<CreateFactory1_t>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (!createFactory) {
        LOG("ERROR: CreateDXGIFactory1 not found");
        return false;
    }

    IDXGIFactory2* factory = nullptr;
    HRESULT hr = createFactory(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        LOG("ERROR: CreateDXGIFactory1 failed 0x%08lX", hr);
        return false;
    }

    void** vt = *reinterpret_cast<void***>(factory);
    o_CreateSwapChain =
        reinterpret_cast<CreateSwapChain_t>(PatchVtable(vt, kFactory_CreateSwapChain, (void*)&Hook_CreateSwapChain));
    o_CreateSwapChainForHwnd = reinterpret_cast<CreateSwapChainForHwnd_t>(
        PatchVtable(vt, kFactory_CreateSwapChainForHwnd, (void*)&Hook_CreateSwapChainForHwnd));
    factory->Release();

    LOG("DXGI factory hooks installed (vtable %p): CreateSwapChain=%p CreateSwapChainForHwnd=%p", vt, o_CreateSwapChain,
        o_CreateSwapChainForHwnd);
    return o_CreateSwapChain && o_CreateSwapChainForHwnd;
}

}  // namespace hooks
