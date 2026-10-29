#include "frame_probe.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "config.h"
#include "log.h"

namespace frame_probe {
namespace {

std::atomic<int> g_presents{0}, g_main{0}, g_views{0};
std::atomic<int> g_presentStacks{0}, g_mainStacks{0};
std::atomic<ULONGLONG> g_lastReport{0};
std::atomic<int> g_reports{0};

std::string ModOff(void* a) {
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)a, &m) && m) {
        GetModuleFileNameA(m, name, MAX_PATH);
        const char* s = strrchr(name, '\\');
        char out[MAX_PATH + 32];
        snprintf(out, sizeof(out), "%s+0x%llX", s ? s + 1 : name, (unsigned long long)((uintptr_t)a - (uintptr_t)m));
        return out;
    }
    char out[32];
    snprintf(out, sizeof(out), "%p", a);
    return out;
}

void LogStack(const char* what) {
    void* frames[28] = {};
    USHORT n = RtlCaptureStackBackTrace(2, 28, frames, nullptr);
    std::string s;
    for (USHORT i = 0; i < n; ++i) s += " <- " + ModOff(frames[i]);
    LOG("FRAMEPROBE %s on thread %lu:%s", what, GetCurrentThreadId(), s.c_str());
}

void MaybeReport() {
    ULONGLONG now = GetTickCount64();
    ULONGLONG last = g_lastReport.load();
    if (last == 0) { g_lastReport.compare_exchange_strong(last, now); return; }
    if (now - last < 1000 || !g_lastReport.compare_exchange_strong(last, now)) return;
    if (g_reports.fetch_add(1) >= 60) return;  // first minute only
    LOG("FRAMEPROBE per second: presents %d, main-view updates %d, all-view updates %d", g_presents.exchange(0),
        g_main.exchange(0), g_views.exchange(0));
}

}  // namespace

bool Enabled() { return vr::g_config.frameProbe; }

void OnPresent() {
    if (!Enabled()) return;
    ++g_presents;
    if (g_presentStacks.fetch_add(1) < 3) LogStack("Present");
    MaybeReport();
}

void OnViewUpdate(bool mainView) {
    if (!Enabled()) return;
    ++g_views;
    if (mainView) {
        ++g_main;
        if (g_mainStacks.fetch_add(1) < 3) LogStack("main-view Update (LookAt)");
    }
}

}  // namespace frame_probe
