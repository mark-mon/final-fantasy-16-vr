#include "dinput_hook.h"

#include <unknwn.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "log.h"

namespace dinput_hook {
namespace {

// IDirectInput8 vtable: 0 QI, 1 AddRef, 2 Release, 3 CreateDevice, ...
// IDirectInputDevice8 vtable: ... 9 GetDeviceState, 10 GetDeviceData, ...
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(void* self, REFGUID guid, void** dev, IUnknown* outer);
using GetDeviceState_t = HRESULT(STDMETHODCALLTYPE*)(void* self, DWORD cb, void* data);
using GetDeviceData_t = HRESULT(STDMETHODCALLTYPE*)(void* self, DWORD cbObj, void* rgdod, DWORD* inOut, DWORD flags);

std::mutex g_mutex;
CreateDevice_t g_origCreate[2] = {};   // per distinct IDirectInput8 vtable (A / W)
void* g_createVtbl[2] = {};
struct DevVtbl {
    void* vtbl;
    GetDeviceState_t getState;
    GetDeviceData_t getData;
};
DevVtbl g_dev[4] = {};
int g_nDev = 0;

std::atomic<DWORD> g_injUntil{0};
unsigned char g_injKeys[4] = {};
int g_injNKeys = 0;
std::atomic<unsigned> g_injButtons{0};

std::atomic<int> g_logState{0}, g_logData{0};

bool Patch(void** slot, void* fn) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    *slot = fn;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

DevVtbl* Find(void* self) {
    void* vt = *reinterpret_cast<void**>(self);
    for (int i = 0; i < g_nDev; ++i)
        if (g_dev[i].vtbl == vt) return &g_dev[i];
    return nullptr;
}

bool Injecting() { return g_injUntil.load() && (int)(g_injUntil.load() - GetTickCount()) > 0; }

HRESULT STDMETHODCALLTYPE HkGetDeviceState(void* self, DWORD cb, void* data) {
    DevVtbl* d = Find(self);
    if (!d) return E_FAIL;
    HRESULT hr = d->getState(self, cb, data);
    if (g_logState.fetch_add(1) < 6) LOG("DInput: GetDeviceState(device %p, %lu bytes) -> 0x%08lX", self, cb, hr);
    if (Injecting() && data) {
        if (cb == 256) {  // keyboard
            for (int i = 0; i < g_injNKeys; ++i) static_cast<unsigned char*>(data)[g_injKeys[i]] = 0x80;
            hr = S_OK;
        } else if (cb == 80 || cb == 272) {  // DIJOYSTATE / DIJOYSTATE2: rgbButtons at offset 48
            unsigned char* buttons = static_cast<unsigned char*>(data) + 48;
            unsigned bits = g_injButtons.load();
            for (int i = 0; i < 32; ++i)
                if (bits & (1u << i)) buttons[i] = 0x80;
            hr = S_OK;
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HkGetDeviceData(void* self, DWORD cbObj, void* rgdod, DWORD* inOut, DWORD flags) {
    DevVtbl* d = Find(self);
    if (!d) return E_FAIL;
    HRESULT hr = d->getData(self, cbObj, rgdod, inOut, flags);
    if (g_logData.fetch_add(1) < 6)
        LOG("DInput: GetDeviceData(device %p, obj %lu bytes, %lu items) -> 0x%08lX", self, cbObj, inOut ? *inOut : 0, hr);
    return hr;
}

void HookDevice(void* dev) {
    std::lock_guard<std::mutex> lock(g_mutex);
    void** vt = *reinterpret_cast<void***>(dev);
    for (int i = 0; i < g_nDev; ++i)
        if (g_dev[i].vtbl == vt) return;
    if (g_nDev >= 4) return;
    DevVtbl& d = g_dev[g_nDev];
    d.vtbl = vt;
    d.getState = reinterpret_cast<GetDeviceState_t>(vt[9]);
    d.getData = reinterpret_cast<GetDeviceData_t>(vt[10]);
    if (vt[9] == reinterpret_cast<void*>(&HkGetDeviceState)) return;
    Patch(&vt[9], reinterpret_cast<void*>(&HkGetDeviceState));
    Patch(&vt[10], reinterpret_cast<void*>(&HkGetDeviceData));
    ++g_nDev;
    LOG("DInput: device vtable %p hooked (GetDeviceState / GetDeviceData)", vt);
}

template <int N>
HRESULT STDMETHODCALLTYPE HkCreateDevice(void* self, REFGUID guid, void** dev, IUnknown* outer) {
    HRESULT hr = g_origCreate[N](self, guid, dev, outer);
    const unsigned long* g = reinterpret_cast<const unsigned long*>(&guid);
    LOG("DInput: CreateDevice({%08lX-...}) -> 0x%08lX", g[0], hr);
    if (SUCCEEDED(hr) && dev && *dev) HookDevice(*dev);
    return hr;
}
void* const kCreate[2] = {reinterpret_cast<void*>(&HkCreateDevice<0>), reinterpret_cast<void*>(&HkCreateDevice<1>)};

}  // namespace

void OnCreated(void* di) {
    if (!di) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    void** vt = *reinterpret_cast<void***>(di);
    for (int i = 0; i < 2; ++i)
        if (g_createVtbl[i] == vt) return;
    for (int i = 0; i < 2; ++i) {
        if (g_createVtbl[i]) continue;
        g_createVtbl[i] = vt;
        g_origCreate[i] = reinterpret_cast<CreateDevice_t>(vt[3]);
        Patch(&vt[3], kCreate[i]);
        LOG("DInput: IDirectInput8 %p created; CreateDevice hooked", di);
        return;
    }
}

void Inject(const unsigned char* dik, int n, unsigned buttonBits, int holdMs) {
    if (n > 4) n = 4;
    for (int i = 0; i < n; ++i) g_injKeys[i] = dik[i];
    g_injNKeys = n;
    g_injButtons = buttonBits;
    g_injUntil = GetTickCount() + (DWORD)holdMs;
}

}  // namespace dinput_hook
