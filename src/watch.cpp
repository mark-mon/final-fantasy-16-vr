#include "watch.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "config.h"
#include "log.h"

// Dev tool: hardware write watchpoint (debug registers DR0, 4 bytes) on one motion-blur parameter block, to find the
// game code that writes the blocks (and where its values come from). Trigger: ff16vr_watch.trigger ("mb" = first
// full-strength block found in the heap; "clean" = first block already at 1/1000; or a hex address).
// Each hit logs the instruction address (exe RVA), registers, xmm0-5, a call stack (RtlVirtualUnwind) and writes the
// code bytes around RIP and each caller to ff16vr_watch_<n>.bin (offset table in the log). 12 hits, then disarmed.
namespace watch {
namespace {

std::atomic<int> g_hits{0};
std::atomic<bool> g_armed{false};
uintptr_t g_addr = 0;
bool g_rw = false;        // "vcfg": break on reads too (RW = 11)
DWORD g_windowMs = 10000;
uintptr_t g_exe = 0;
size_t g_exeSize = 0;
PVOID g_veh = nullptr;
constexpr int kMaxHits = 24;
DWORD g_lastPoll = 0;

bool InExe(uintptr_t a) { return a >= g_exe && a < g_exe + g_exeSize; }

void DumpCode(FILE* f, uintptr_t at, const char* label, int hit) {
    uint8_t buf[384] = {};
    const uintptr_t from = at - 256;
    SIZE_T got = 0;
    ReadProcessMemory(GetCurrentProcess(), (void*)from, buf, sizeof(buf), &got);
    const long off = ftell(f);
    fwrite(buf, 1, sizeof(buf), f);
    LOG("Watch hit %d: %s rva %llX: code [%llX..%llX) at file offset %ld", hit, label, (unsigned long long)(at - g_exe),
        (unsigned long long)(from - g_exe), (unsigned long long)(from - g_exe + sizeof(buf)), off);
}

LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if (!(c->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    const int hit = ++g_hits;
    if (hit > kMaxHits) {
        c->Dr7 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    const float* v = reinterpret_cast<const float*>(g_addr);
    LOG("Watch hit %d: thread %lu rip %llX (%s) value now %.6g %.6g | rax %llX rbx %llX rcx %llX rdx %llX rsi %llX rdi %llX "
        "rbp %llX rsp %llX r8 %llX r9 %llX r10 %llX r11 %llX r12 %llX r13 %llX r14 %llX r15 %llX",
        hit, GetCurrentThreadId(), (unsigned long long)(InExe(c->Rip) ? c->Rip - g_exe : c->Rip), InExe(c->Rip) ? "exe rva" : "abs",
        v[0], v[1], c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp, c->R8, c->R9, c->R10, c->R11, c->R12,
        c->R13, c->R14, c->R15);
    const M128A* x = &c->Xmm0;
    for (int i = 0; i < 6; ++i) {
        float fx[4];
        memcpy(fx, &x[i], 16);
        LOG("Watch hit %d: xmm%d %.6g %.6g %.6g %.6g", hit, i, fx[0], fx[1], fx[2], fx[3]);
    }
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\ff16vr_watch_%d.bin", vr::g_gameDir.c_str(), hit);
    FILE* f = _wfopen(path, L"wb");
    // Unwind a copy of the context.
    CONTEXT u = *c;
    for (int depth = 0; depth < 10 && u.Rip; ++depth) {
        char label[32];
        snprintf(label, sizeof(label), depth ? "caller %d" : "rip", depth);
        if (f && InExe(u.Rip) && depth < 6) DumpCode(f, u.Rip, label, hit);
        else LOG("Watch hit %d: %s %s %llX", hit, label, InExe(u.Rip) ? "rva" : "abs",
                 (unsigned long long)(InExe(u.Rip) ? u.Rip - g_exe : u.Rip));
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(u.Rip, &imageBase, nullptr);
        if (!fn) {
            u.Rip = *reinterpret_cast<DWORD64*>(u.Rsp);
            u.Rsp += 8;
        } else {
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, u.Rip, fn, &u, &handlerData, &establisher, nullptr);
        }
    }
    if (f) fclose(f);
    if (hit == kMaxHits) c->Dr7 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Sets DR0/DR7 on every thread of the process except this helper thread.
void SetAllThreads(uintptr_t addr, bool on) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof(te)};
    int n = 0;
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != (DWORD)-1) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                c.Dr0 = on ? addr : 0;
                c.Dr6 = 0;
                // L0 enable, RW0 = 01 (write), LEN0 = 11 (4 bytes)
                c.Dr7 = on ? ((c.Dr7 & ~0xF0003ull) | 1ull | ((g_rw ? 3ull : 1ull) << 16) | (3ull << 18)) : (c.Dr7 & ~0xF0003ull);
                if (SetThreadContext(t, &c)) ++n;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    LOG("Watch: DR0 %s %p on %d threads", on ? "armed at" : "cleared,", (void*)addr, n);
}

// "vcfg": the game's VideoConfigData (config.xml <VideoConfigData>; serializer at +0x7B43xx: brightness_ +0x18,
// contrast_ +0x1C, brightnessUI_ +0x20, performanceMode_ +0x24, motionBlurIntensity_ +0x28). Values on the test PC:
// 0, 0.5, 1, 0, 0. Returns the address of motionBlurIntensity_ (first match; all matches logged).
uintptr_t FindVideoConfig() {
    uintptr_t first = 0;
    int n = 0;
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t* p = nullptr;
    while (VirtualQuery(p, &mbi, sizeof(mbi))) {
        uint8_t* b = static_cast<uint8_t*>(mbi.BaseAddress);
        const DWORD prot = mbi.Protect & 0xFF;
        if (mbi.State == MEM_COMMIT && (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY) &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_WRITECOMBINE | PAGE_NOCACHE)) && mbi.RegionSize < (1ull << 30)) {
            __try {
                const uint32_t* w = reinterpret_cast<const uint32_t*>(b);
                for (size_t i = 0; i + 8 <= mbi.RegionSize / 4; ++i) {
                    if (w[i + 1] != 0x3F000000u || w[i + 2] != 0x3F800000u || w[i] != 0 || w[i + 3] != 0 || w[i + 4] != 0) continue;
                    const uintptr_t a = reinterpret_cast<uintptr_t>(w + i + 4);
                    if (n++ < 20) LOG("Watch: VideoConfigData candidate, motionBlurIntensity_ at %p (%s)", (void*)a,
                                      mbi.Type == MEM_IMAGE ? "image" : "heap");
                    if (!first) first = a;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        p = b + mbi.RegionSize;
        if (reinterpret_cast<uintptr_t>(p) >= 0x7FFFFFFF0000ull) break;
    }
    LOG("Watch: %d VideoConfigData candidates", n);
    return first;
}

// GraphicsConfigData (serializer +0x26903C): ... sharpnessDLSS_ / FSR_ / XESS_ / Custom_ floats, frameRateNumerator_,
// frameRateDenominator_ ints. Test PC: 1, 0.5, 1, 1, 24000, 100. The VideoConfigData of the same settings object is
// looked for within 0x600 bytes before it; its motionBlurIntensity_ is returned. Logs the bytes around it.
uintptr_t FindVideoConfigNearGraphics() {
    uintptr_t pick = 0;
    int n = 0;
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t* p = nullptr;
    while (VirtualQuery(p, &mbi, sizeof(mbi))) {
        uint8_t* b = static_cast<uint8_t*>(mbi.BaseAddress);
        const DWORD prot = mbi.Protect & 0xFF;
        if (mbi.State == MEM_COMMIT && (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY) &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_WRITECOMBINE | PAGE_NOCACHE)) && mbi.RegionSize < (1ull << 30)) {
            __try {
                const uint32_t* w = reinterpret_cast<const uint32_t*>(b);
                const size_t nw = mbi.RegionSize / 4;
                for (size_t i = 0x200; i + 8 <= nw; ++i) {
                    if (w[i + 4] != 24000u || w[i + 5] != 100u || w[i + 1] != 0x3F000000u || w[i] != 0x3F800000u) continue;
                    const uint8_t* g = reinterpret_cast<const uint8_t*>(w + i);
                    ++n;
                    LOG("Watch: GraphicsConfigData candidate %d: sharpness at %p (%s)", n, (const void*)g,
                        mbi.Type == MEM_IMAGE ? "image" : "heap");
                    // dump 0x200 bytes before it, 32 per line
                    for (int o = -0x200; o < 0x20; o += 32) {
                        char line[160];
                        int k = snprintf(line, sizeof(line), "Watch:   %+05X:", o);
                        for (int j = 0; j < 32; j += 4) k += snprintf(line + k, sizeof(line) - k, " %08X", *reinterpret_cast<const uint32_t*>(g + o + j));
                        LOG("%s", line);
                    }
                    // nearest VideoConfigData-like run before it: 0, 0.5, 1 then two zero dwords
                    for (int o = -0x30; o > -0x600; o -= 4) {
                        const uint32_t* v = reinterpret_cast<const uint32_t*>(g + o);
                        if (v[0] == 0 && v[1] == 0x3F000000u && v[2] == 0x3F800000u && v[3] == 0 && v[4] == 0) {
                            LOG("Watch:   VideoConfigData-like run at %+X (motionBlurIntensity_ at %p)", o, (const void*)(v + 4));
                            if (!pick) pick = reinterpret_cast<uintptr_t>(v + 4);
                            break;
                        }
                    }
                    if (n >= 6) break;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        if (n >= 6) break;
        p = b + mbi.RegionSize;
        if (reinterpret_cast<uintptr_t>(p) >= 0x7FFFFFFF0000ull) break;
    }
    LOG("Watch: %d GraphicsConfigData candidates", n);
    return pick;
}

// "fc": a CPU copy of the per-frame constants (row 13 = 0.43, 2.2, 0.066667, 3000; see mem_scan.cpp); the watch goes
// on row 11 .w (the motion-blur divisor: 1 after a load, 1000 with motion blur Off). Logs up to 8 copies.
uintptr_t FindFrameConst() {
    // row 13 (0.43, 2.2, 0.066667, 3000) on the test PC; also searched in write-combined (upload heap) memory, where the
    // GPU copy lives (run 219: no CPU-side copy)
    static const uint32_t kPat[4] = {1054615798u, 1074580685u, 1032358025u, 1161527296u};
    uintptr_t first = 0;
    int n = 0;
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t* p = nullptr;
    while (VirtualQuery(p, &mbi, sizeof(mbi))) {
        uint8_t* b = static_cast<uint8_t*>(mbi.BaseAddress);
        const DWORD prot = mbi.Protect & 0xFF;
        if (mbi.State == MEM_COMMIT && (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY) &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOCACHE)) && mbi.RegionSize < (1ull << 30)) {
            __try {
                const uint32_t* w = reinterpret_cast<const uint32_t*>(b);
                for (size_t i = 8; i + 4 <= mbi.RegionSize / 4; ++i) {
                    if (w[i] != kPat[0] || w[i + 1] != kPat[1] || w[i + 2] != kPat[2] || w[i + 3] != kPat[3]) continue;
                    const float* f = reinterpret_cast<const float*>(w + i) - 5;  // row 11 .w
                    if (n++ < 8) LOG("Watch: frame constants copy %p (%s%s): row 11 .w = %g", (const void*)f,
                                     mbi.Type == MEM_IMAGE ? "image" : mbi.Type == MEM_MAPPED ? "mapped" : "private",
                                     (mbi.Protect & PAGE_WRITECOMBINE) ? ", write-combined" : "", *f);
                    if (!first) first = reinterpret_cast<uintptr_t>(f);
                    if (n >= 2) break;  // reading write-combined memory is slow (run 220 stalled the game)
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        if (n >= 2) break;
        p = b + mbi.RegionSize;
        if (reinterpret_cast<uintptr_t>(p) >= 0x7FFFFFFF0000ull) break;
    }
    LOG("Watch: %d frame constant copies", n);
    return first;
}

uintptr_t FindBlock(bool clean) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uint8_t* p = static_cast<uint8_t*>(si.lpMinimumApplicationAddress);
    uint8_t* end = static_cast<uint8_t*>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi;
    while (p < end && VirtualQuery(p, &mbi, sizeof(mbi))) {
        uint8_t* base = static_cast<uint8_t*>(mbi.BaseAddress);
        const DWORD prot = mbi.Protect & 0xFF;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && prot == PAGE_READWRITE &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_WRITECOMBINE | PAGE_NOCACHE)) && mbi.RegionSize < (1ull << 30)) {
            __try {
                const uint32_t* w = reinterpret_cast<const uint32_t*>(base);
                const float* f = reinterpret_cast<const float*>(base);
                for (size_t i = 0; i + 6 <= mbi.RegionSize / 4; ++i) {
                    if (w[i + 2] != 0x40400000u || w[i + 3] != 0x41200000u || w[i + 4] != 0x40800000u || w[i + 5] != 0x41800000u) continue;
                    if (!(f[i] > 0.0f && f[i + 1] > f[i] && f[i] < 100.0f)) continue;
                    if ((f[i] >= 0.05f) != clean) return reinterpret_cast<uintptr_t>(f + i);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        p = base + mbi.RegionSize;
    }
    return 0;
}

DWORD WINAPI ArmThread(void* param) {
    const uintptr_t addr = reinterpret_cast<uintptr_t>(param);
    g_addr = addr;
    g_hits = 0;
    SetAllThreads(addr, true);
    for (DWORD i = 0; i < g_windowMs / 100 && g_hits < kMaxHits; ++i) Sleep(100);
    SetAllThreads(0, false);
    LOG("Watch: done, %d hits", g_hits.load());
    g_armed = false;
    return 0;
}

// "cam": an upload-heap camera cbuffer (cCamera, 0x900 bytes, 256-aligned, write-combined memory) whose previous
// half holds the other eye (eye positions row 38 differ by an IPD); the watch goes on its previous eye position.
bool CamLooks(const uint8_t* p) {
    const float* n0 = reinterpret_cast<const float*>(p + 37 * 16);
    const float* e0 = reinterpret_cast<const float*>(p + 38 * 16);
    const float* n1 = reinterpret_cast<const float*>(p + 0x400 + 37 * 16);
    const float* e1 = reinterpret_cast<const float*>(p + 0x400 + 38 * 16);
    if (n0[3] != 1.0f || n1[3] != 1.0f || e0[3] < 0.1999f || e0[3] > 0.2001f || e1[3] < 0.1999f || e1[3] > 0.2001f) return false;
    float d2 = 0;
    for (int i = 0; i < 3; ++i) {
        if (!(e0[i] == e0[i]) || e0[i] > 1e6f || e0[i] < -1e6f) return false;
        d2 += (e0[i] - e1[i]) * (e0[i] - e1[i]);
    }
    auto off = [](const float* e, const float* n) {
        return std::sqrt((e[0] - n[0]) * (e[0] - n[0]) + (e[1] - n[1]) * (e[1] - n[1]) + (e[2] - n[2]) * (e[2] - n[2]));
    };
    const float o0 = off(e0, n0), o1 = off(e1, n1);  // eye beside the camera node (half IPD x WorldScale, + head)
    static int logged = 0;
    if (logged < 16 && o0 > 0.01f && o0 < 0.3f) {
        ++logged;
        LOG("Watch cam candidate %p: node %.3f %.3f %.3f eye %.3f %.3f %.3f prev eye %.3f %.3f %.3f off %.4f / %.4f d %.4f",
            (const void*)p, n0[0], n0[1], n0[2], e0[0], e0[1], e0[2], e1[0], e1[1], e1[2], o0, o1, std::sqrt(d2));
    }
    if (!(o0 > 0.01f && o0 < 0.3f && o1 > 0.01f && o1 < 0.3f)) return false;
    if (std::fabs(n0[0]) + std::fabs(n0[1]) + std::fabs(n0[2]) < 1.0f) return false;  // a world position, not a local one
    return d2 > 0.02f * 0.02f && d2 < 0.5f * 0.5f;
}
uintptr_t ScanCamRegion(uint8_t* b, size_t size) {
    __try {
        for (size_t o = (0x100 - (reinterpret_cast<uintptr_t>(b) & 0xFF)) & 0xFF; o + 0x900 <= size; o += 0x100)
            if (CamLooks(b + o)) return reinterpret_cast<uintptr_t>(b + o);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}
uintptr_t FindCam() {
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t* p = nullptr;
    while (VirtualQuery(p, &mbi, sizeof(mbi))) {
        uint8_t* b = static_cast<uint8_t*>(mbi.BaseAddress);
        if (mbi.State == MEM_COMMIT && (mbi.Protect & 0xFF) == PAGE_READWRITE && !(mbi.Protect & PAGE_GUARD))
            if (uintptr_t r = ScanCamRegion(b, mbi.RegionSize)) {
                LOG("Watch: camera cbuffer slot %p (protect %lX)", (void*)r, mbi.Protect);
                return r + 0x400 + 38 * 16;
            }
        p = b + mbi.RegionSize;
        if (reinterpret_cast<uintptr_t>(p) >= 0x7FFFFFFF0000ull) break;
    }
    return 0;
}

}  // namespace

void OnPresent() {
    const DWORD now = GetTickCount();
    if (now - g_lastPoll < 1000) return;
    g_lastPoll = now;
    const std::wstring trig = vr::g_gameDir + L"\\ff16vr_watch.trigger";
    if (GetFileAttributesW(trig.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    char line[64] = {};
    if (FILE* f = _wfopen(trig.c_str(), L"r")) {
        fgets(line, sizeof(line), f);
        fclose(f);
    }
    DeleteFileW(trig.c_str());
    if (g_armed.exchange(true)) return;
    if (!g_exe) {
        g_exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_exe);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_exe + dos->e_lfanew);
        g_exeSize = nt->OptionalHeader.SizeOfImage;
    }
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, &Veh);
    uintptr_t addr = 0;
    g_rw = false;
    g_windowMs = 10000;
    if (strncmp(line, "fc", 2) == 0) {
        // search on its own thread (slow), then arm
        g_windowMs = 15000;
        auto th = [](void*) -> DWORD {
            const uintptr_t a = FindFrameConst();
            if (!a) {
                g_armed = false;
                return 0;
            }
            return ArmThread(reinterpret_cast<void*>(a));
        };
        if (HANDLE h = CreateThread(nullptr, 0, th, nullptr, 0, nullptr)) CloseHandle(h);
        return;
    } else if (strncmp(line, "gcfg", 4) == 0) {
        g_rw = true;
        g_windowMs = 30000;
        addr = FindVideoConfigNearGraphics();
    } else if (strncmp(line, "vcfg", 4) == 0) {
        // "vcfg [n]": read/write watch on candidate n (default 0) for 30 s
        int pick = atoi(line + 4);
        g_rw = true;
        g_windowMs = 30000;
        addr = FindVideoConfig();
        (void)pick;
    } else if (strncmp(line, "rw ", 3) == 0) {
        g_rw = true;
        g_windowMs = 30000;
        addr = (uintptr_t)strtoull(line + 3, nullptr, 16);
    } else if (strncmp(line, "mb", 2) == 0) addr = FindBlock(false);
    else if (strncmp(line, "clean", 5) == 0) addr = FindBlock(true);
    else if (strncmp(line, "cam", 3) == 0) addr = FindCam();
    else addr = (uintptr_t)strtoull(line, nullptr, 16);
    const float* v = reinterpret_cast<const float*>(addr);
    if (!addr) {
        LOG("Watch: no target (%s)", line);
        g_armed = false;
        return;
    }
    LOG("Watch: target %p = %.6g %.6g (exe base %p, size %zX)", (void*)addr, v[0], v[1], (void*)g_exe, g_exeSize);
    if (HANDLE h = CreateThread(nullptr, 0, &ArmThread, reinterpret_cast<void*>(addr), 0, nullptr)) CloseHandle(h);
}

}  // namespace watch
