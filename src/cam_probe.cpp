#include "cam_probe.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "config.h"
#include "log.h"

namespace cam_probe {
namespace {

struct Hit {
    uintptr_t matrix;   // address of row 0 of the 4x4 projection
    uintptr_t watch;    // element written every frame (jitter term)
    int layout;         // 0 = column-vector (as in the camera cbuffer), 1 = row-vector (transposed)
    float fx, fy, jx, jy, nearZ;
    MEMORY_BASIC_INFORMATION mbi;
};

bool ReadSafe(const void* p, void* out, size_t n) {
    __try {
        memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Plausible(float f) { return std::isfinite(f) && f > 0.05f && f < 50.f; }
bool Small(float f) { return std::isfinite(f) && std::fabs(f) < 0.05f; }

// Checks a 16-byte aligned 4x4 at m (row-major storage of 16 floats).
int Classify(const float* m, Hit& h) {
    // layout 0 (cbuffer): [fx 0 -jx 0; 0 fy -jy 0; 0 0 0 n; 0 0 -1 0]
    if (m[12] == 0 && m[13] == 0 && m[14] == -1.f && m[15] == 0 && m[8] == 0 && m[9] == 0 && m[10] == 0 &&
        m[11] > 0.001f && m[11] < 100.f && Plausible(m[0]) && Plausible(m[5]) && m[1] == 0 && m[3] == 0 && m[4] == 0 &&
        m[7] == 0 && Small(m[2]) && Small(m[6])) {
        h.layout = 0; h.fx = m[0]; h.fy = m[5]; h.jx = m[2]; h.jy = m[6]; h.nearZ = m[11];
        return 1;
    }
    // layout 1 (transposed): [fx 0 0 0; 0 fy 0 0; -jx -jy 0 -1; 0 0 n 0]
    if (m[11] == -1.f && m[10] == 0 && m[12] == 0 && m[13] == 0 && m[15] == 0 && m[14] > 0.001f && m[14] < 100.f &&
        Plausible(m[0]) && Plausible(m[5]) && m[1] == 0 && m[2] == 0 && m[3] == 0 && m[4] == 0 && m[6] == 0 &&
        m[7] == 0 && Small(m[8]) && Small(m[9])) {
        h.layout = 1; h.fx = m[0]; h.fy = m[5]; h.jx = m[8]; h.jy = m[9]; h.nearZ = m[14];
        return 1;
    }
    return 0;
}

std::vector<Hit> Scan(size_t maxHits) {
    std::vector<Hit> hits;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t a = (uintptr_t)si.lpMinimumApplicationAddress, end = (uintptr_t)si.lpMaximumApplicationAddress;
    const size_t chunk = 1 << 20;
    std::vector<float> buf((chunk + 64) / 4);  // allocated once; its own region is skipped below
    const uintptr_t self = (uintptr_t)buf.data();
    size_t regions = 0, bytes = 0;
    while (a < end && hits.size() < maxHits) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)a, &mbi, sizeof(mbi))) break;
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        DWORD prot = mbi.Protect & 0xFF;
        bool rw = prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_WRITECOPY;
        bool own = self >= (uintptr_t)mbi.BaseAddress && self < next;
        if (mbi.State == MEM_COMMIT && rw && !own && !(mbi.Protect & PAGE_GUARD) && mbi.RegionSize <= (512ull << 20)) {
            ++regions;
            bytes += mbi.RegionSize;
            for (uintptr_t p = (uintptr_t)mbi.BaseAddress; p < next && hits.size() < maxHits; p += chunk) {
                size_t n = (size_t)std::min<uintptr_t>(chunk + 64, next - p);
                if (!ReadSafe((void*)p, buf.data(), n)) continue;
                size_t nf = n / 4;
                for (size_t i = 0; i + 16 <= nf && i * 4 < chunk; i += 4) {
                    // cheap pre-filter: a -1.0f at index 14 (layout 0) or 11 (layout 1)
                    if (buf[i + 14] != -1.f && buf[i + 11] != -1.f) continue;
                    Hit h{};
                    if (!Classify(&buf[i], h)) continue;
                    h.matrix = p + i * 4;
                    h.watch = h.matrix + (h.layout == 0 ? 2 * 4 : 8 * 4);  // jx element
                    h.mbi = mbi;
                    hits.push_back(h);
                    if (hits.size() >= maxHits) break;
                }
            }
        }
        a = next;
    }
    LOG("CAMPROBE scan: %zu writable regions, %zu MB, %zu hits", regions, bytes >> 20, hits.size());
    return hits;
}

std::string ModOff(uintptr_t a) {
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)a, &m) && m) {
        GetModuleFileNameA(m, name, MAX_PATH);
        const char* s = strrchr(name, '\\');
        char out[MAX_PATH + 32];
        snprintf(out, sizeof(out), "%s+0x%llX", s ? s + 1 : name, (unsigned long long)(a - (uintptr_t)m));
        return out;
    }
    char out[32];
    snprintf(out, sizeof(out), "%p", (void*)a);
    return out;
}

// ---- hardware write breakpoints -------------------------------------------------------------------
constexpr int kMaxWriters = 64;
constexpr int kFrames = 10;
struct Writer {
    std::atomic<uintptr_t> rip{0};
    std::atomic<int> count{0};
    int slot = 0;
    uintptr_t frames[kFrames] = {};
};
Writer g_writers[kMaxWriters];
std::atomic<int> g_nWriters{0};
std::atomic<bool> g_armed{false};
PVOID g_veh = nullptr;

void Unwind(CONTEXT c, uintptr_t* frames) {
    for (int i = 0; i < kFrames; ++i) {
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        if (fe) {
            PVOID hd = nullptr;
            DWORD64 ef = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fe, &c, &hd, &ef, nullptr);
        } else {
            DWORD64 ret = 0;
            if (!ReadSafe((void*)c.Rsp, &ret, 8)) break;
            c.Rip = ret;
            c.Rsp += 8;
        }
        if (!c.Rip) break;
        frames[i] = (uintptr_t)c.Rip;
    }
}

LONG CALLBACK Veh(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_armed) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    DWORD64 dr6 = c->Dr6;
    if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    int slot = (dr6 & 1) ? 0 : (dr6 & 2) ? 1 : (dr6 & 4) ? 2 : 3;
    uintptr_t rip = (uintptr_t)c->Rip;
    int n = g_nWriters.load();
    int found = -1;
    for (int i = 0; i < n; ++i)
        if (g_writers[i].rip.load() == rip && g_writers[i].slot == slot) { found = i; break; }
    if (found < 0 && n < kMaxWriters) {
        int idx = g_nWriters.fetch_add(1);
        if (idx < kMaxWriters) {
            g_writers[idx].slot = slot;
            Unwind(*c, g_writers[idx].frames);
            g_writers[idx].rip.store(rip);
            found = idx;
        }
    }
    if (found >= 0) g_writers[found].count.fetch_add(1);
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void SetBreakpoints(const uintptr_t* addrs, int n) {
    // n == 0 clears them. Applied to every thread of the process except this one.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof(te)};
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    int threads = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != (DWORD)-1) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                DWORD64 dr7 = 0;
                DWORD64* dr[4] = {&c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3};
                for (int i = 0; i < 4; ++i) {
                    *dr[i] = i < n ? addrs[i] : 0;
                    if (i < n) dr7 |= (1ull << (i * 2)) | (1ull << (16 + i * 4)) | (3ull << (18 + i * 4));  // local enable, RW=01 write, LEN=11 (4 bytes)
                }
                c.Dr7 = dr7;
                c.Dr6 = 0;
                SetThreadContext(t, &c);
                ++threads;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    if (n) LOG("CAMPROBE breakpoints on %d address(es) set in %d threads", n, threads);
}

void Watch(const std::vector<Hit>& hits, DWORD ms) {
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, Veh);
    for (size_t base = 0; base < hits.size(); base += 4) {
        int n = (int)std::min<size_t>(4, hits.size() - base);
        uintptr_t addrs[4] = {};
        for (int i = 0; i < n; ++i) addrs[i] = hits[base + i].watch;
        for (auto& w : g_writers) { w.rip = 0; w.count = 0; memset(w.frames, 0, sizeof(w.frames)); }
        g_nWriters = 0;
        g_armed = true;
        SetBreakpoints(addrs, n);
        Sleep(ms);
        SetBreakpoints(addrs, 0);
        Sleep(50);
        g_armed = false;
        int nw = std::min(g_nWriters.load(), kMaxWriters);
        for (int i = 0; i < n; ++i) {
            int writes = 0;
            for (int w = 0; w < nw; ++w) if (g_writers[w].slot == i) writes += g_writers[w].count;
            LOG("CAMPROBE hit %zu (%p): %d writes in %lu ms", base + i, (void*)addrs[i], writes, ms);
            for (int w = 0; w < nw; ++w) {
                if (g_writers[w].slot != i) continue;
                std::string st;
                for (int f = 0; f < kFrames && g_writers[w].frames[f]; ++f) st += " <- " + ModOff(g_writers[w].frames[f]);
                LOG("CAMPROBE   writer %s x%d  stack:%s", ModOff(g_writers[w].rip).c_str(), g_writers[w].count.load(), st.c_str());
            }
        }
    }
}

void Dump(const std::vector<Hit>& hits) {
    std::wstring dir = vr::g_config.captureDir + L"\\camprobe";
    CreateDirectoryW(vr::g_config.captureDir.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    for (size_t i = 0; i < hits.size(); ++i) {
        const Hit& h = hits[i];
        uintptr_t from = std::max<uintptr_t>((uintptr_t)h.mbi.BaseAddress, h.matrix - 1024);
        uintptr_t to = std::min<uintptr_t>((uintptr_t)h.mbi.BaseAddress + h.mbi.RegionSize, h.matrix + 2048);
        std::vector<char> b(to - from);
        if (!ReadSafe((void*)from, b.data(), b.size())) continue;
        wchar_t name[64];
        swprintf(name, 64, L"\\hit_%02zu_off%zu.bin", i, (size_t)(h.matrix - from));
        if (FILE* f = _wfopen((dir + name).c_str(), L"wb")) {
            fwrite(b.data(), 1, b.size(), f);
            fclose(f);
        }
    }
}

// Groups hits into camera classes (layout, focal lengths, near, jittered or not) and keeps at most `perClass`
// of each, jittered classes first, so the watch phase covers every distinct camera instead of the first N copies.
std::vector<Hit> PickRepresentatives(const std::vector<Hit>& all, size_t perClass, size_t maxTotal) {
    struct Cls { int layout; float fx, fy, nearZ; bool jit; size_t count; std::vector<size_t> idx; };
    std::vector<Cls> cls;
    for (size_t i = 0; i < all.size(); ++i) {
        const Hit& h = all[i];
        bool jit = h.jx != 0 || h.jy != 0;
        Cls* c = nullptr;
        for (auto& k : cls)
            if (k.layout == h.layout && std::fabs(k.fx - h.fx) < 1e-4f && std::fabs(k.fy - h.fy) < 1e-4f &&
                std::fabs(k.nearZ - h.nearZ) < 1e-6f && k.jit == jit) { c = &k; break; }
        if (!c) { cls.push_back({h.layout, h.fx, h.fy, h.nearZ, jit, 0, {}}); c = &cls.back(); }
        ++c->count;
        if (c->idx.size() < perClass) c->idx.push_back(i);
    }
    LOG("CAMPROBE %zu camera classes:", cls.size());
    for (auto& k : cls)
        LOG("CAMPROBE   class %s fx=%.5f fy=%.5f (fovy %.2f) near=%.4g %s: %zu copies", k.layout ? "row-vector" : "column-vector",
            k.fx, k.fy, 2 * atan(1 / k.fy) * 57.29578, k.nearZ, k.jit ? "JITTERED" : "no jitter", k.count);
    std::vector<Hit> out;
    for (int pass = 0; pass < 2; ++pass)  // jittered classes first
        for (auto& k : cls)
            if (k.jit == (pass == 0))
                for (size_t i : k.idx)
                    if (out.size() < maxTotal) out.push_back(all[i]);
    return out;
}

void Run(DWORD watchMs) {
    LOG("CAMPROBE start (watch %lu ms per group of 4)", watchMs);
    std::vector<Hit> hits = PickRepresentatives(Scan(4000), 2, 32);
    for (size_t i = 0; i < hits.size(); ++i) {
        const Hit& h = hits[i];
        const char* type = h.mbi.Type == MEM_PRIVATE ? "private" : h.mbi.Type == MEM_MAPPED ? "mapped" : "image";
        LOG("CAMPROBE hit %zu: %p layout=%s fx=%.5f fy=%.5f (fovy %.2f) jitter=(%.6g,%.6g) near=%.4g region %p+0x%zX %s prot 0x%lX%s",
            i, (void*)h.matrix, h.layout ? "row-vector" : "column-vector", h.fx, h.fy,
            2 * atan(1 / h.fy) * 57.29578, h.jx, h.jy, h.nearZ, h.mbi.BaseAddress, (size_t)h.mbi.RegionSize, type,
            h.mbi.Protect, (h.mbi.Protect & PAGE_WRITECOMBINE) ? " (write-combined: GPU upload memory)" : "");
        if (h.mbi.Type == MEM_IMAGE) LOG("CAMPROBE   in module %s", ModOff(h.matrix).c_str());
    }
    Dump(hits);
    if (!hits.empty()) Watch(hits, watchMs);
    LOG("CAMPROBE done");
}

DWORD WINAPI TriggerThread(LPVOID) {
    const std::wstring trigger = vr::g_gameDir + L"\\ff16vr_camprobe.trigger";
    const std::wstring done = vr::g_gameDir + L"\\ff16vr_camprobe.done";
    for (;;) {
        Sleep(500);
        if (GetFileAttributesW(trigger.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        DWORD ms = 2000;
        if (FILE* f = _wfopen(trigger.c_str(), L"r")) {
            unsigned v = 0;
            if (fscanf(f, "%u", &v) == 1 && v >= 100 && v <= 30000) ms = v;
            fclose(f);
        }
        DeleteFileW(trigger.c_str());
        DeleteFileW(done.c_str());
        Run(ms);
        if (FILE* f = _wfopen(done.c_str(), L"w")) {
            fputs("done", f);
            fclose(f);
        }
    }
}

}  // namespace

void Init() {
    HANDLE t = CreateThread(nullptr, 0, TriggerThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

}  // namespace cam_probe
