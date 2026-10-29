// Logs who ends the process: hooks ExitProcess / TerminateProcess in ffxvi.exe's import table and writes
// a stack trace (module+offset) to ff16vr.log. Used to find out why the game quits under RenderDoc, and
// generally useful when the game closes without a crash report.

#include "exit_trace.h"

#include <windows.h>
#include <intrin.h>

#include <cstdint>
#include <cstring>

#include "config.h"
#include "log.h"
#include "rdoc.h"

namespace exittrace {
namespace {

using ExitProcess_t = void(WINAPI*)(UINT);
using TerminateProcess_t = BOOL(WINAPI*)(HANDLE, UINT);
ExitProcess_t o_ExitProcess = nullptr;
TerminateProcess_t o_TerminateProcess = nullptr;

void LogStack(const char* what, UINT code) {
    void* frames[40];
    USHORT n = RtlCaptureStackBackTrace(1, 40, frames, nullptr);
    LOG("EXIT TRACE: %s(code 0x%X) on thread %lu, %u frames:", what, code, GetCurrentThreadId(), n);
    for (USHORT i = 0; i < n; ++i) {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &mod) && mod) {
            GetModuleFileNameA(mod, name, MAX_PATH);
            const char* base = strrchr(name, '\\');
            LOG("    #%02u %s+0x%llX", i, base ? base + 1 : name,
                (unsigned long long)(static_cast<char*>(frames[i]) - reinterpret_cast<char*>(mod)));
        } else {
            LOG("    #%02u %p", i, frames[i]);
        }
    }
}

void WINAPI Hook_ExitProcess(UINT code) {
    LogStack("ExitProcess", code);
    o_ExitProcess(code);
}

BOOL WINAPI Hook_TerminateProcess(HANDLE h, UINT code) {
    if (h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId()) LogStack("TerminateProcess(self)", code);
    return o_TerminateProcess(h, code);
}

// Patch one import of `module` (by function name, from any DLL). Returns the old pointer.
void* PatchImport(HMODULE module, const char* func, void* hook) {
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk
                                                                                             : imp->FirstThunk));
        auto* addrs = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addrs) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(ibn->Name), func) != 0) continue;
            void* old = reinterpret_cast<void*>(addrs->u1.Function);
            DWORD prot;
            VirtualProtect(&addrs->u1.Function, sizeof(void*), PAGE_READWRITE, &prot);
            addrs->u1.Function = reinterpret_cast<ULONGLONG>(hook);
            VirtualProtect(&addrs->u1.Function, sizeof(void*), prot, &prot);
            return old;
        }
    }
    return nullptr;
}

// Logged early-exit decision points.
using BoolNoArg_t = bool(__cdecl*)();
using RestartApp_t = bool(__cdecl*)(uint32_t);
using IsDbg_t = BOOL(WINAPI*)();
using CheckRemoteDbg_t = BOOL(WINAPI*)(HANDLE, PBOOL);
RestartApp_t o_RestartApp = nullptr;
BoolNoArg_t o_SteamInit = nullptr;
IsDbg_t o_IsDbg = nullptr;
CheckRemoteDbg_t o_CheckRemote = nullptr;

bool __cdecl Hook_RestartApp(uint32_t appId) {
    bool r = o_RestartApp(appId);
    LOG("TRACE: SteamAPI_RestartAppIfNecessary(%u) -> %d", appId, r);
    return r;
}
bool __cdecl Hook_SteamInit() {
    bool r = o_SteamInit();
    LOG("TRACE: SteamAPI_Init() -> %d", r);
    return r;
}
BOOL WINAPI Hook_IsDbg() {
    BOOL r = o_IsDbg();
    static int n = 0;
    if (n++ < 5) LOG("TRACE: IsDebuggerPresent() -> %d", r);
    return r;
}
BOOL WINAPI Hook_CheckRemote(HANDLE h, PBOOL p) {
    BOOL r = o_CheckRemote(h, p);
    static int n = 0;
    if (n++ < 5) LOG("TRACE: CheckRemoteDebuggerPresent() -> %d (present=%d)", r, p ? *p : -1);
    return r;
}

// Early-startup call log (first 3 s): which DLLs/functions the game asks for, and from where.
using GetProcAddress_t = FARPROC(WINAPI*)(HMODULE, LPCSTR);
using LoadLibraryExW_t = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using LoadLibraryW_t = HMODULE(WINAPI*)(LPCWSTR);
using LoadLibraryA_t = HMODULE(WINAPI*)(LPCSTR);
GetProcAddress_t o_GetProcAddress = nullptr;
LoadLibraryExW_t o_LoadLibraryExW = nullptr;
LoadLibraryW_t o_LoadLibraryW = nullptr;
LoadLibraryA_t o_LoadLibraryA = nullptr;
ULONGLONG g_t0 = 0;
bool Early() { return GetTickCount64() - g_t0 < 3000; }
unsigned long long CallerOff(void* ret) {
    return (unsigned long long)(static_cast<char*>(ret) - reinterpret_cast<char*>(GetModuleHandleW(nullptr)));
}

// Steam startup wrappers (handed out through GetProcAddress: the game resolves steam_api64 lazily).
RestartApp_t g_realRestart = nullptr;
BoolNoArg_t g_realSteamInit = nullptr;
bool __cdecl Wrap_RestartApp(uint32_t appId) {
    bool r = g_realRestart(appId);
    LOG("TRACE: SteamAPI_RestartAppIfNecessary(%u) -> %d%s", appId, r,
        (r && vr::g_config.renderDoc) ? " (overridden to 0 because RenderDoc = 1)" : "");
    return (r && vr::g_config.renderDoc) ? false : r;
}
bool __cdecl Wrap_SteamInit() {
    bool r = g_realSteamInit();
    LOG("TRACE: SteamAPI_Init() -> %d", r);
    return r;
}

FARPROC WINAPI Hook_GetProcAddress(HMODULE m, LPCSTR name) {
    FARPROC r = o_GetProcAddress(m, name);
    if (r && (ULONG_PTR)name > 0xFFFF) {
        if (!strcmp(name, "SteamAPI_RestartAppIfNecessary")) {
            g_realRestart = reinterpret_cast<RestartApp_t>(r);
            return reinterpret_cast<FARPROC>(&Wrap_RestartApp);
        }
        if (!strcmp(name, "SteamAPI_Init")) {
            g_realSteamInit = reinterpret_cast<BoolNoArg_t>(r);
            return reinterpret_cast<FARPROC>(&Wrap_SteamInit);
        }
    }
    if ((ULONG_PTR)name > 0xFFFF) r = reinterpret_cast<FARPROC>(rdoc::WrapProc(m, name, reinterpret_cast<void*>(r)));
    if (Early()) {
        char mod[MAX_PATH] = "?";
        GetModuleFileNameA(m, mod, MAX_PATH);
        const char* b = strrchr(mod, '\\');
        if ((ULONG_PTR)name > 0xFFFF)
            LOG("TRACE: GetProcAddress(%s, %s) -> %p  [exe+0x%llX]", b ? b + 1 : mod, name, r, CallerOff(_ReturnAddress()));
    }
    return r;
}
HMODULE WINAPI Hook_LoadLibraryExW(LPCWSTR n, HANDLE f, DWORD fl) {
    HMODULE r = o_LoadLibraryExW(n, f, fl);
    if (Early()) LOG("TRACE: LoadLibraryExW(%ls, 0x%lX) -> %p  [exe+0x%llX]", n ? n : L"", fl, r, CallerOff(_ReturnAddress()));
    return r;
}
HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR n) {
    HMODULE r = o_LoadLibraryW(n);
    if (Early()) LOG("TRACE: LoadLibraryW(%ls) -> %p  [exe+0x%llX]", n ? n : L"", r, CallerOff(_ReturnAddress()));
    return r;
}
HMODULE WINAPI Hook_LoadLibraryA(LPCSTR n) {
    HMODULE r = o_LoadLibraryA(n);
    if (Early()) LOG("TRACE: LoadLibraryA(%s) -> %p  [exe+0x%llX]", n ? n : "", r, CallerOff(_ReturnAddress()));
    return r;
}

using MessageBoxW_t = int(WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT);
MessageBoxW_t o_MessageBoxW = nullptr;
int WINAPI Hook_MessageBoxW(HWND h, LPCWSTR text, LPCWSTR caption, UINT type) {
    LOG("MESSAGE BOX [%ls]: %ls  [exe+0x%llX]", caption ? caption : L"", text ? text : L"", CallerOff(_ReturnAddress()));
    return o_MessageBoxW(h, text, caption, type);
}

}  // namespace

void Install() {
    g_t0 = GetTickCount64();
    HMODULE exe = GetModuleHandleW(nullptr);
    o_ExitProcess = reinterpret_cast<ExitProcess_t>(PatchImport(exe, "ExitProcess", (void*)&Hook_ExitProcess));
    o_TerminateProcess =
        reinterpret_cast<TerminateProcess_t>(PatchImport(exe, "TerminateProcess", (void*)&Hook_TerminateProcess));
    o_RestartApp = reinterpret_cast<RestartApp_t>(PatchImport(exe, "SteamAPI_RestartAppIfNecessary", (void*)&Hook_RestartApp));
    o_SteamInit = reinterpret_cast<BoolNoArg_t>(PatchImport(exe, "SteamAPI_Init", (void*)&Hook_SteamInit));
    o_IsDbg = reinterpret_cast<IsDbg_t>(PatchImport(exe, "IsDebuggerPresent", (void*)&Hook_IsDbg));
    o_CheckRemote = reinterpret_cast<CheckRemoteDbg_t>(PatchImport(exe, "CheckRemoteDebuggerPresent", (void*)&Hook_CheckRemote));
    o_GetProcAddress = reinterpret_cast<GetProcAddress_t>(PatchImport(exe, "GetProcAddress", (void*)&Hook_GetProcAddress));
    o_LoadLibraryExW = reinterpret_cast<LoadLibraryExW_t>(PatchImport(exe, "LoadLibraryExW", (void*)&Hook_LoadLibraryExW));
    o_LoadLibraryW = reinterpret_cast<LoadLibraryW_t>(PatchImport(exe, "LoadLibraryW", (void*)&Hook_LoadLibraryW));
    o_LoadLibraryA = reinterpret_cast<LoadLibraryA_t>(PatchImport(exe, "LoadLibraryA", (void*)&Hook_LoadLibraryA));
    o_MessageBoxW = reinterpret_cast<MessageBoxW_t>(PatchImport(exe, "MessageBoxW", (void*)&Hook_MessageBoxW));
    LOG("Startup trace hooks: GetProcAddress=%d LoadLibraryExW=%d LoadLibraryW=%d LoadLibraryA=%d", o_GetProcAddress != nullptr,
        o_LoadLibraryExW != nullptr, o_LoadLibraryW != nullptr, o_LoadLibraryA != nullptr);
    LOG("Startup trace hooks: RestartAppIfNecessary=%d SteamAPI_Init=%d IsDebuggerPresent=%d CheckRemoteDebuggerPresent=%d",
        o_RestartApp != nullptr, o_SteamInit != nullptr, o_IsDbg != nullptr, o_CheckRemote != nullptr);
    bool exitHooked = o_ExitProcess != nullptr, termHooked = o_TerminateProcess != nullptr;
    if (!o_ExitProcess) o_ExitProcess = &ExitProcess;
    if (!o_TerminateProcess) o_TerminateProcess = &TerminateProcess;
    LOG("Exit trace installed (ExitProcess %s, TerminateProcess %s)", exitHooked ? "hooked" : "not imported",
        termHooked ? "hooked" : "not imported");
}

}  // namespace exittrace
