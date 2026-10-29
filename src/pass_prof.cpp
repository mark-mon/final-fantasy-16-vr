#include "pass_prof.h"

#include <windows.h>
#include <tlhelp32.h>
#include <intrin.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "config.h"
#include "log.h"
#include "stereo_render.h"

namespace pass_prof {
namespace {

// One sample key: pass slot (0 left, 1 right, 2 single frame, 3 outside the frame function), the frame function's
// call site (return address RVA; 0 = not under it), the two game functions below (begin RVAs) and the leaf's module.
struct Key {
    uint32_t slot, site, f1, f2;
    uint64_t leafBase;
};
struct Entry {
    Key k;
    uint32_t n;
    bool used;
};
constexpr size_t kTable = 1 << 14;
Entry* g_table = nullptr;
uint32_t g_dropped = 0;

bool Same(const Key& a, const Key& b) {
    return a.slot == b.slot && a.site == b.site && a.f1 == b.f1 && a.f2 == b.f2 && a.leafBase == b.leafBase;
}

void Add(const Key& k) {  // no allocation: runs while the game thread may be suspended
    uint64_t h = k.slot * 0x9E3779B97F4A7C15ull ^ (uint64_t)k.site * 0xC2B2AE3D27D4EB4Full ^ (uint64_t)k.f1 * 0x165667B19E3779F9ull ^
                 (uint64_t)k.f2 * 0x27D4EB2F165667C5ull ^ k.leafBase;
    for (size_t i = 0; i < kTable; ++i) {
        Entry& e = g_table[(h + i) & (kTable - 1)];
        if (!e.used) { e.used = true; e.k = k; e.n = 1; return; }
        if (Same(e.k, k)) { ++e.n; return; }
    }
    ++g_dropped;
}

std::string ModName(uint64_t addr, uint64_t* base) {
    HMODULE m = nullptr;
    *base = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &m) || !m)
        return "?";
    *base = reinterpret_cast<uint64_t>(m);
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(m, p, MAX_PATH);
    std::wstring w(p);
    w = w.substr(w.find_last_of(L"\\/") + 1);
    return std::string(w.begin(), w.end());
}

std::string AddrName(uint64_t addr) {
    if (!addr) return "-";
    uint64_t base = 0;
    std::string n = ModName(addr, &base);
    char b[64];
    snprintf(b, sizeof(b), "+0x%llX", (unsigned long long)(addr - base));
    return n + b;
}

using NtQit_t = LONG(WINAPI*)(HANDLE, int, PVOID, ULONG, PULONG);

struct ThreadTimes {
    DWORD tid;
    uint64_t start;
    uint64_t cpu100ns;
};

std::vector<ThreadTimes> SnapThreads() {
    std::vector<ThreadTimes> out;
    static NtQit_t qit = reinterpret_cast<NtQit_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    THREADENTRY32 te{sizeof(te)};
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        FILETIME c, e, k, u;
        ThreadTimes t{te.th32ThreadID, 0, 0};
        if (GetThreadTimes(h, &c, &e, &k, &u))
            t.cpu100ns = (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);
        if (qit) qit(h, 9 /*ThreadQuerySetWin32StartAddress*/, &t.start, sizeof(t.start), nullptr);
        CloseHandle(h);
        out.push_back(t);
    }
    CloseHandle(snap);
    return out;
}

uint64_t ProcessCpu100ns() {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    return (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime);
}

// One stack sample of the game thread (suspended by the caller). Fills the key; no allocation, no locks.
bool Sample(HANDLE th, uint64_t exeBase, uint64_t exeEnd, uint64_t frameLo, uint64_t frameHi, Key* k) {
    CONTEXT c{};
    c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(th, &c)) return false;
    uint64_t pcs[64];
    int n = 0;
    __try {
        for (; n < 64 && c.Rip; ++n) {
            pcs[n] = c.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
            if (!rf) {
                c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
                c.Rsp += 8;
            } else {
                PVOID hd = nullptr;
                DWORD64 ef = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, rf, &c, &hd, &ef, nullptr);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (!n) return false;
    k->leafBase = pcs[0];  // resolved to a module later (outside the suspension)
    int site = -1;
    for (int i = 1; i < n; ++i)
        if (pcs[i] >= frameLo && pcs[i] < frameHi) { site = i; break; }
    if (site < 0) return true;  // slot 3, site 0
    k->site = (uint32_t)(pcs[site] - exeBase);
    // the game functions called from that site: first two exe frames below it (towards the leaf)
    int got = 0;
    for (int i = site - 1; i >= 0 && got < 2; --i) {
        if (pcs[i] < exeBase || pcs[i] >= exeEnd) continue;
        DWORD64 ib = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(pcs[i], &ib, nullptr);
        const uint32_t f = rf ? rf->BeginAddress : (uint32_t)(pcs[i] - exeBase);
        (got == 0 ? k->f1 : k->f2) = f;
        ++got;
    }
    return true;
}

// Worker samples: pass slot, the six outermost game functions of the stack (thread start first), the innermost
// game function, and the leaf's module (ntdll = waiting / idle).
struct WKey {
    uint32_t slot;
    uint32_t f[6];
    uint32_t inner;
    uint64_t leafBase;
};
struct WEntry {
    WKey k;
    uint32_t n;
    bool used;
};
WEntry* g_wtable = nullptr;

void WAdd(const WKey& k) {
    uint64_t h = k.slot * 0x9E3779B97F4A7C15ull ^ k.leafBase ^ (uint64_t)k.inner * 0x165667B19E3779F9ull;
    for (int i = 0; i < 6; ++i) h = h * 0x100000001B3ull ^ k.f[i];
    for (size_t i = 0; i < kTable; ++i) {
        WEntry& e = g_wtable[(h + i) & (kTable - 1)];
        if (!e.used) { e.used = true; e.k = k; e.n = 1; return; }
        if (!memcmp(&e.k, &k, sizeof(k))) { ++e.n; return; }
    }
    ++g_dropped;
}

bool SampleWorker(HANDLE th, uint64_t exeBase, uint64_t exeEnd, WKey* k) {
    CONTEXT c{};
    c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(th, &c)) return false;
    uint64_t pcs[64];
    int n = 0;
    __try {
        for (; n < 64 && c.Rip; ++n) {
            pcs[n] = c.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
            if (!rf) {
                c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
                c.Rsp += 8;
            } else {
                PVOID hd = nullptr;
                DWORD64 ef = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, rf, &c, &hd, &ef, nullptr);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (!n) return false;
    k->leafBase = pcs[0];
    int got = 0;
    for (int i = n - 1; i >= 0; --i) {  // outermost first
        if (pcs[i] < exeBase || pcs[i] >= exeEnd) continue;
        DWORD64 ib = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(pcs[i], &ib, nullptr);
        const uint32_t fn = rf ? rf->BeginAddress : (uint32_t)(pcs[i] - exeBase);
        if (got < 6) k->f[got++] = fn;
        k->inner = fn;
    }
    return true;
}

void Run(int seconds) {
    const DWORD tid = stereo_render::GameThreadId();
    void* fn = stereo_render::FrameFnAddr();
    if (!tid || !fn) { LOG("Prof: game thread or frame function unknown"); return; }
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!th) { LOG("Prof: OpenThread failed (%lu)", GetLastError()); return; }
    if (!g_table) g_table = static_cast<Entry*>(VirtualAlloc(nullptr, sizeof(Entry) * kTable, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    ZeroMemory(g_table, sizeof(Entry) * kTable);
    if (!g_wtable) g_wtable = static_cast<WEntry*>(VirtualAlloc(nullptr, sizeof(WEntry) * kTable, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    ZeroMemory(g_wtable, sizeof(WEntry) * kTable);
    g_dropped = 0;
    const uint64_t exeBase = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exeBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(exeBase)->e_lfanew);
    const uint64_t exeEnd = exeBase + nt->OptionalHeader.SizeOfImage;
    const uint64_t frameLo = reinterpret_cast<uint64_t>(fn), frameHi = frameLo + 0x600;

    using TP = UINT(WINAPI*)(UINT);
    HMODULE winmm = LoadLibraryW(L"winmm.dll");
    TP tbp = winmm ? reinterpret_cast<TP>(GetProcAddress(winmm, "timeBeginPeriod")) : nullptr;
    TP tep = winmm ? reinterpret_cast<TP>(GetProcAddress(winmm, "timeEndPeriod")) : nullptr;
    if (tbp) tbp(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    std::vector<ThreadTimes> t0 = SnapThreads();
    // worker pool: the most common thread start address inside the game exe (8 or more threads)
    std::vector<HANDLE> workers;
    {
        std::vector<std::pair<uint64_t, int>> starts;
        for (const ThreadTimes& t : t0) {
            if (t.start < exeBase || t.start >= exeEnd || t.tid == tid) continue;
            auto it = std::find_if(starts.begin(), starts.end(), [&](const auto& p) { return p.first == t.start; });
            if (it == starts.end()) starts.push_back({t.start, 1});
            else ++it->second;
        }
        std::sort(starts.begin(), starts.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
        if (!starts.empty() && starts[0].second >= 8)
            for (const ThreadTimes& t : t0)
                if (t.start == starts[0].first)
                    if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, t.tid))
                        workers.push_back(h);
    }
    size_t rr = 0;
    uint32_t wSamples = 0, wSlotN[4] = {0, 0, 0, 0};
    const uint64_t p0 = ProcessCpu100ns();
    stereo_render::TakePassTotals();
    LARGE_INTEGER q0, q1, qf;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q0);
    uint32_t samples = 0, slotN[4] = {0, 0, 0, 0};
    const ULONGLONG end = GetTickCount64() + (ULONGLONG)seconds * 1000;
    while (GetTickCount64() < end) {
        Sleep(1);
        if (SuspendThread(th) == (DWORD)-1) break;
        Key k{};
        const int eye = stereo_render::FrameEye();
        const bool ok = Sample(th, exeBase, exeEnd, frameLo, frameHi, &k);
        ResumeThread(th);
        if (!ok) continue;
        k.slot = k.site == 0 ? 3 : eye == 0 ? 0 : eye == 1 ? 1 : 2;
        Add(k);
        ++samples;
        ++slotN[k.slot];
        for (int j = 0; j < 6 && !workers.empty(); ++j) {
            HANDLE w = workers[rr++ % workers.size()];
            if (SuspendThread(w) == (DWORD)-1) continue;
            WKey wk{};
            const int we = stereo_render::FrameEye();
            const bool wok = SampleWorker(w, exeBase, exeEnd, &wk);
            ResumeThread(w);
            if (!wok) continue;
            wk.slot = we == 0 ? 0 : we == 1 ? 1 : 2;
            WAdd(wk);
            ++wSamples;
            ++wSlotN[wk.slot];
        }
    }
    for (HANDLE w : workers) CloseHandle(w);
    QueryPerformanceCounter(&q1);
    const stereo_render::PassTotals pt = stereo_render::TakePassTotals();
    const uint64_t p1 = ProcessCpu100ns();
    std::vector<ThreadTimes> t1 = SnapThreads();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    if (tep) tep(1);
    CloseHandle(th);

    const double winMs = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart;
    const long ticks = pt.calls[0] + pt.calls[2];
    const double msPerSample = samples ? winMs / samples : 0;
    // TSC rate for thread cycle counts
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    const uint64_t r0 = __rdtsc();
    Sleep(50);
    QueryPerformanceCounter(&b);
    const double cyclesPerMs = (double)(__rdtsc() - r0) / ((double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)qf.QuadPart);

    LOG("Prof: %.0f ms window, %u samples (%u dropped), ticks %ld (stereo %ld, single %ld); samples left %u right %u "
        "single %u outside %u",
        winMs, samples, g_dropped, ticks, pt.calls[0], pt.calls[2], slotN[0], slotN[1], slotN[2], slotN[3]);
    const char* slotName[4] = {"left", "right", "single", "outside"};
    for (int s = 0; s < 3; ++s)
        if (pt.calls[s])
            LOG("Prof: pass %s: %ld calls, wall %.2f ms, game-thread CPU %.2f ms per call", slotName[s], pt.calls[s],
                pt.wallMs[s] / pt.calls[s], pt.cycles[s] / cyclesPerMs / pt.calls[s]);
    LOG("Prof: process CPU %.1f ms per tick (%.0f%% of one core)", ticks ? (double)(p1 - p0) / 1e4 / ticks : 0.0,
        (double)(p1 - p0) / 1e4 / winMs * 100.0);

    // resolve leaf modules, then aggregate
    std::vector<Entry> all;
    for (size_t i = 0; i < kTable; ++i)
        if (g_table[i].used) all.push_back(g_table[i]);
    for (Entry& e : all) {
        uint64_t base = 0;
        ModName(e.k.leafBase, &base);
        e.k.leafBase = base;
    }
    auto ms = [&](uint32_t n, int slot) {
        const long calls = slot == 0 ? pt.calls[0] : slot == 1 ? pt.calls[1] : slot == 2 ? pt.calls[2] : ticks;
        return calls ? n * msPerSample / calls : 0.0;
    };
    struct Agg { uint32_t slot, site, f1, f2; uint64_t leaf; uint32_t n; };
    auto aggregate = [&](bool useF1, bool useF2, bool useLeaf) {
        std::vector<Agg> v;
        for (const Entry& e : all) {
            Agg g{e.k.slot, e.k.site, useF1 ? e.k.f1 : 0, useF2 ? e.k.f2 : 0, useLeaf ? e.k.leafBase : 0, e.n};
            auto it = std::find_if(v.begin(), v.end(), [&](const Agg& x) {
                return x.slot == g.slot && x.site == g.site && x.f1 == g.f1 && x.f2 == g.f2 && x.leaf == g.leaf; });
            if (it == v.end()) v.push_back(g);
            else it->n += g.n;
        }
        std::sort(v.begin(), v.end(), [](const Agg& x, const Agg& y) { return x.n > y.n; });
        return v;
    };
    auto rva = [&](uint32_t r) { char b[24]; snprintf(b, sizeof(b), "+0x%X", r); return std::string(r ? b : "-"); };
    {
        auto v = aggregate(false, false, false);
        for (size_t i = 0; i < v.size() && i < 40; ++i)
            LOG("Prof site: %-7s site %-10s %5u samples  %.2f ms/call", slotName[v[i].slot], rva(v[i].site).c_str(), v[i].n,
                ms(v[i].n, v[i].slot));
    }
    {
        auto v = aggregate(true, false, false);
        for (size_t i = 0; i < v.size() && i < 60; ++i)
            LOG("Prof fn: %-7s site %-10s f1 %-10s %5u samples  %.2f ms/call", slotName[v[i].slot], rva(v[i].site).c_str(),
                rva(v[i].f1).c_str(), v[i].n, ms(v[i].n, v[i].slot));
    }
    {
        auto v = aggregate(true, true, false);
        for (size_t i = 0; i < v.size() && i < 80; ++i)
            LOG("Prof fn2: %-7s site %-10s f1 %-10s f2 %-10s %5u samples  %.2f ms/call", slotName[v[i].slot],
                rva(v[i].site).c_str(), rva(v[i].f1).c_str(), rva(v[i].f2).c_str(), v[i].n, ms(v[i].n, v[i].slot));
    }
    {
        auto v = aggregate(false, false, true);
        for (size_t i = 0; i < v.size() && i < 50; ++i) {
            uint64_t base = 0;
            std::string mod = v[i].leaf ? ModName(v[i].leaf, &base) : "?";
            LOG("Prof leaf: %-7s site %-10s leaf %-24s %5u samples  %.2f ms/call", slotName[v[i].slot],
                rva(v[i].site).c_str(), mod.c_str(), v[i].n, ms(v[i].n, v[i].slot));
        }
    }
    // threads by CPU over the window
    struct TD { DWORD tid; uint64_t start; double ms; };
    std::vector<TD> td;
    for (const ThreadTimes& b1 : t1) {
        uint64_t before = 0;
        for (const ThreadTimes& b0 : t0)
            if (b0.tid == b1.tid) { before = b0.cpu100ns; break; }
        td.push_back({b1.tid, b1.start, (double)(b1.cpu100ns - before) / 1e4});
    }
    std::sort(td.begin(), td.end(), [](const TD& x, const TD& y) { return x.ms > y.ms; });
    // group by start address too
    std::vector<std::pair<std::string, std::pair<double, int>>> groups;
    for (const TD& t : td) {
        std::string s = AddrName(t.start);
        auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == s; });
        if (it == groups.end()) groups.push_back({s, {t.ms, 1}});
        else it->second.first += t.ms, ++it->second.second;
    }
    std::sort(groups.begin(), groups.end(), [](const auto& x, const auto& y) { return x.second.first > y.second.first; });
    for (size_t i = 0; i < groups.size() && i < 20; ++i)
        LOG("Prof threads: start %-28s x%-3d CPU %.1f ms over the window, %.2f ms per tick", groups[i].first.c_str(),
            groups[i].second.second, groups[i].second.first, ticks ? groups[i].second.first / ticks : 0.0);
    for (size_t i = 0; i < td.size() && i < 12; ++i)
        LOG("Prof thread: tid %lu%s start %s CPU %.1f ms (%.0f%% of a core)", td[i].tid, td[i].tid == tid ? " (game)" : "",
            AddrName(td[i].start).c_str(), td[i].ms, td[i].ms / winMs * 100.0);

    // workers: what the job threads run in each pass
    if (!wSamples) return;
    const size_t nW = (size_t)std::max<uint32_t>(1, (uint32_t)std::count_if(t0.begin(), t0.end(), [&](const ThreadTimes&) { return true; }));
    (void)nW;
    std::vector<WEntry> wall;
    uint64_t ntdllBase = reinterpret_cast<uint64_t>(GetModuleHandleW(L"ntdll.dll"));
    uint32_t busy[3] = {0, 0, 0};
    for (size_t i = 0; i < kTable; ++i)
        if (g_wtable[i].used) {
            WEntry e = g_wtable[i];
            uint64_t base = 0;
            ModName(e.k.leafBase, &base);
            e.k.leafBase = base;
            wall.push_back(e);
            if (base != ntdllBase && e.k.slot < 3) busy[e.k.slot] += e.n;
        }
    const double wWorkers = (double)rr / std::max(1.0, (double)wSamples);  // ~1 (failed samples aside)
    (void)wWorkers;
    // each worker sample stands for (window * workers / worker samples) ms of one worker thread
    const double wMs = winMs * (double)std::max<size_t>(1, workers.size()) / (double)wSamples;
    auto perCall = [&](uint32_t n, uint32_t slot) {
        const long calls = slot < 3 ? pt.calls[slot] : 0;
        return calls ? n * wMs / calls : 0.0;
    };
    LOG("Prof workers: %zu threads, %u samples (left %u right %u single %u); busy (not in ntdll) per call: left %.1f ms, "
        "right %.1f ms, single %.1f ms of worker time",
        workers.size(), wSamples, wSlotN[0], wSlotN[1], wSlotN[2], perCall(busy[0], 0), perCall(busy[1], 1), perCall(busy[2], 2));
    struct WAgg { uint32_t slot; uint32_t f[6]; uint32_t inner; uint32_t n; };
    auto wagg = [&](int depth, bool inner) {
        std::vector<WAgg> v;
        for (const WEntry& e : wall) {
            if (e.k.leafBase == ntdllBase || e.k.slot > 2) continue;  // busy samples only
            WAgg g{e.k.slot, {0, 0, 0, 0, 0, 0}, inner ? e.k.inner : 0, e.n};
            for (int i = 0; i < depth; ++i) g.f[i] = e.k.f[i];
            auto it = std::find_if(v.begin(), v.end(), [&](const WAgg& x) {
                return x.slot == g.slot && x.inner == g.inner && !memcmp(x.f, g.f, sizeof(g.f)); });
            if (it == v.end()) v.push_back(g);
            else it->n += g.n;
        }
        std::sort(v.begin(), v.end(), [](const WAgg& x, const WAgg& y) { return x.n > y.n; });
        return v;
    };
    for (int depth = 3; depth <= 6; ++depth) {
        auto v = wagg(depth, false);
        for (size_t i = 0; i < v.size() && i < 30; ++i) {
            std::string chain;
            for (int j = 1; j < depth; ++j) chain += rva(v[i].f[j]) + " ";
            LOG("Prof wk%d: %-6s %s %5u samples  %.2f ms/call", depth, slotName[v[i].slot], chain.c_str(), v[i].n,
                perCall(v[i].n, v[i].slot));
        }
    }
    {
        auto v = wagg(0, true);
        for (size_t i = 0; i < v.size() && i < 50; ++i)
            LOG("Prof wkinner: %-6s %-10s %5u samples  %.2f ms/call", slotName[v[i].slot], rva(v[i].inner).c_str(), v[i].n,
                perCall(v[i].n, v[i].slot));
    }
    {   // depth-3 chain + inner, to tie inner functions to their job
        auto v = wagg(4, true);
        for (size_t i = 0; i < v.size() && i < 60; ++i)
            LOG("Prof wkjob: %-6s %s%s%s-> %-10s %5u samples  %.2f ms/call", slotName[v[i].slot], (rva(v[i].f[1]) + " ").c_str(),
                (rva(v[i].f[2]) + " ").c_str(), (rva(v[i].f[3]) + " ").c_str(), rva(v[i].inner).c_str(), v[i].n,
                perCall(v[i].n, v[i].slot));
    }
}

DWORD WINAPI Watch(LPVOID) {
    const std::wstring trigger = vr::g_gameDir + L"\\ff16vr_prof.trigger";
    for (;;) {
        Sleep(250);
        if (GetFileAttributesW(trigger.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        char buf[32] = {};
        if (FILE* f = _wfopen(trigger.c_str(), L"r")) {
            fgets(buf, sizeof(buf), f);
            fclose(f);
        }
        DeleteFileW(trigger.c_str());
        int s = atoi(buf);
        if (s <= 0 || s > 60) s = 5;
        LOG("Prof: sampling the game thread for %d s", s);
        Run(s);
    }
}

}  // namespace

void Install() {
    HANDLE t = CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

}  // namespace pass_prof
