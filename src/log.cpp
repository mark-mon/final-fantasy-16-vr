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
