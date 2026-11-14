#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <mutex>
#include <string>

namespace vr {

static FILE* g_file = nullptr;
static std::mutex g_mutex;
static ULONGLONG g_start = 0;

void LogInit(const wchar_t* path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) return;
    g_start = GetTickCount64();
    // Keep the previous run's log (a run that had to be killed is otherwise lost on the next launch).
    std::wstring prev = path;
    const size_t dot = prev.rfind(L".log");
    prev = (dot != std::wstring::npos ? prev.substr(0, dot) : prev) + L".prev.log";
    MoveFileExW(path, prev.c_str(), MOVEFILE_REPLACE_EXISTING);
    // Shared so tools (test agent, editors) can read the log while the game runs.
    g_file = _wfsopen(path, L"w", _SH_DENYNO);
    if (g_file) {   // wall-clock time, so a log can be matched to a crash dump / Event Viewer entry
        SYSTEMTIME lt{}, ut{}; GetLocalTime(&lt); GetSystemTime(&ut);
        fprintf(g_file, "[%7llu.%03llu] Log started %04u-%02u-%02u %02u:%02u:%02u local (%04u-%02u-%02u %02u:%02u:%02u UTC)\n", 0ull, 0ull,
                lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond, ut.wYear, ut.wMonth, ut.wDay, ut.wHour, ut.wMinute, ut.wSecond);
        fflush(g_file);
    }
}

const char* WallClock() {   // "HH:MM:SS" local time (for periodic lines)
    static thread_local char buf[16];
    SYSTEMTIME lt{}; GetLocalTime(&lt);
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u", lt.wHour, lt.wMinute, lt.wSecond);
    return buf;
}

void Log(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(g_mutex);
    ULONGLONG t = GetTickCount64() - g_start;
    if (g_file) {
        fprintf(g_file, "[%7llu.%03llu] [T%05lu] %s\n", t / 1000, t % 1000, GetCurrentThreadId(), buf);
        fflush(g_file);
    }
    OutputDebugStringA("[FF16VR] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

}  // namespace vr
