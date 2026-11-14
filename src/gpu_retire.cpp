#include "gpu_retire.h"

#include <windows.h>
#include <unknwn.h>

#include <atomic>
#include <mutex>
#include <vector>

#include "log.h"

namespace gpu_retire {
namespace {
struct Dead { IUnknown* obj; ULONGLONG t; uint64_t present; };
std::mutex g_mu;
std::vector<Dead> g_dead;
std::atomic<uint64_t> g_presents{0};
std::atomic<long> g_pending{0};
constexpr ULONGLONG kMinMs = 5000;
constexpr uint64_t kMinPresents = 30;
}  // namespace

void Defer(IUnknown* obj) {
    if (!obj) return;
    std::lock_guard<std::mutex> l(g_mu);
    g_dead.push_back({obj, GetTickCount64(), g_presents.load()});
    g_pending = (long)g_dead.size();
}

void OnPresent() {
    const uint64_t p = ++g_presents;
    const ULONGLONG now = GetTickCount64();
    std::vector<IUnknown*> go;
    {
        std::lock_guard<std::mutex> l(g_mu);
        for (size_t i = 0; i < g_dead.size();) {
            if (now - g_dead[i].t >= kMinMs && p - g_dead[i].present >= kMinPresents) {
                go.push_back(g_dead[i].obj);
                g_dead[i] = g_dead.back();
                g_dead.pop_back();
            } else {
                ++i;
            }
        }
        g_pending = (long)g_dead.size();
    }
    for (IUnknown* o : go) o->Release();
}

long Pending() { return g_pending.load(); }
}  // namespace gpu_retire
