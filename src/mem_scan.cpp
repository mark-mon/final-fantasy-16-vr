#include "mem_scan.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "config.h"
#include "log.h"

// Dev tool (ff16vr_memscan.trigger): finds CPU copies of the game's per-frame constant block in process memory.
// Pattern: frame constants row 13 = (0.43, 2.2, 0.066667, 3000) as raw floats (from the RenderDoc constant dumps);
// row 11 .w (0x14 bytes before) is the parameter that scales motion blur (1 after a load, 1000 after the pause menu).
// Logs every hit: address, region type, row 11 and rows 12-15. "force" in the trigger file also writes 1000.0 into
// row 11 .w of every hit that holds 1.0 (once; repeated by OnPresent while forcing is on).
namespace mem_scan {
namespace {

const uint32_t kPattern[4] = {1054615798u, 1074580685u, 1032358025u, 1161527296u};
std::vector<float*> g_force;
bool g_forcing = false;
DWORD g_lastPoll = 0;

int ScanRegion(uint8_t* base, size_t size, const MEMORY_BASIC_INFORMATION& mbi, bool force, int hits);

int SafeScanRegion(uint8_t* base, size_t size, const MEMORY_BASIC_INFORMATION& mbi, bool force, int hits) {
    __try {
        return ScanRegion(base, size, mbi, force, hits);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return hits;
    }
}

void Scan(bool force) {
    g_force.clear();
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uint8_t* p = static_cast<uint8_t*>(si.lpMinimumApplicationAddress);
    uint8_t* end = static_cast<uint8_t*>(si.lpMaximumApplicationAddress);
    int hits = 0;
    size_t scanned = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (p < end && VirtualQuery(p, &mbi, sizeof(mbi))) {
        uint8_t* base = static_cast<uint8_t*>(mbi.BaseAddress);
        const size_t size = mbi.RegionSize;
        const DWORD prot = mbi.Protect & 0xFF;
        const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                              (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE);
        if (readable && size >= 64 && size < (1ull << 31)) {
            scanned += size;
            hits = SafeScanRegion(base, size, mbi, force, hits);
        }
        p = base + size;
    }
    LOG("Mem scan: %d hits in %zu MB; forcing %zu", hits, scanned >> 20, g_force.size());
    g_forcing = force && !g_force.empty();
}

int ScanRegion(uint8_t* base, size_t size, const MEMORY_BASIC_INFORMATION& mbi, bool force, int hits) {
    const uint32_t* w = reinterpret_cast<const uint32_t*>(base);
    const size_t n = size / 4;
    for (size_t i = 5; i + 4 <= n; ++i) {
        if (w[i] != kPattern[0] || w[i + 1] != kPattern[1] || w[i + 2] != kPattern[2] || w[i + 3] != kPattern[3]) continue;
        const float* row13 = reinterpret_cast<const float*>(w + i);
        float* r11w = const_cast<float*>(row13 - 5);  // row 11 .w = 0x14 bytes before row 13
        const float* r12 = row13 - 4;
        if (hits < 40)
            LOG("Mem scan: hit %p (%s %s) row11.w %.4g | row12 %.4g %.4g %u %.4g | row14 %.4g %u %u", (void*)row13,
                mbi.Type == MEM_PRIVATE ? "private" : mbi.Type == MEM_MAPPED ? "mapped" : "image",
                (mbi.Protect & PAGE_WRITECOMBINE) ? "wc" : "", *r11w, r12[0], r12[1],
                reinterpret_cast<const uint32_t*>(r12)[2], r12[3], row13[4],
                reinterpret_cast<const uint32_t*>(row13)[5], reinterpret_cast<const uint32_t*>(row13)[6]);
        ++hits;
        if (force && *r11w == 1.0f) g_force.push_back(r11w);
    }
    return hits;
}

// "find a b [c d]": up to 4 adjacent floats (relative tolerance 1e-4), in non-write-combined memory only (CPU-side data).
float g_find[4];
int g_findN = 0;

bool Near(float v, float want) {
    const float d = v - want;
    const float m = (want < 0 ? -want : want) * 1e-4f + 1e-12f;
    return d <= m && d >= -m;
}

int FindRegion(uint8_t* base, size_t size, const MEMORY_BASIC_INFORMATION& mbi, int hits) {
    const float* f = reinterpret_cast<const float*>(base);
    const size_t n = size / 4;
    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    for (size_t i = 8; i + g_findN + 8 <= n; ++i) {
        if (!Near(f[i], g_find[0])) continue;
        bool ok = true;
        for (int k = 1; k < g_findN && ok; ++k) ok = Near(f[i + k], g_find[k]);
        if (!ok) continue;
        if (hits < 60) {
            char ctx[400];
            int o = 0;
            for (int k = -8; k < 12; ++k) o += snprintf(ctx + o, sizeof(ctx) - o, k == 0 ? "[%.6g] " : "%.6g ", f[i + k]);
            const uintptr_t a = reinterpret_cast<uintptr_t>(f + i);
            LOG("Mem find: %p %s prot %lX region %p+%zX%s rva %llX | %s", (void*)(f + i),
                mbi.Type == MEM_PRIVATE ? "private" : mbi.Type == MEM_MAPPED ? "mapped" : "image", mbi.Protect, mbi.BaseAddress,
                mbi.RegionSize, mbi.AllocationBase == (void*)exe ? " (exe)" : "", (unsigned long long)(a - exe), ctx);
        }
        ++hits;
    }
    return hits;
}

int SafeFindRegion(uint8_t* base, size_t size, const MEMORY_BASIC_INFORMATION& mbi, int hits) {
    __try {
        return FindRegion(base, size, mbi, hits);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return hits;
    }
}

void Find(const char* args) {
    g_findN = sscanf(args, "%f %f %f %f", &g_find[0], &g_find[1], &g_find[2], &g_find[3]);
    if (g_findN <= 0) return;
    const DWORD t0 = GetTickCount();
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uint8_t* p = static_cast<uint8_t*>(si.lpMinimumApplicationAddress);
    uint8_t* end = static_cast<uint8_t*>(si.lpMaximumApplicationAddress);
    int hits = 0;
    size_t scanned = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (p < end && VirtualQuery(p, &mbi, sizeof(mbi))) {
        uint8_t* base = static_cast<uint8_t*>(mbi.BaseAddress);
        const size_t size = mbi.RegionSize;
        const DWORD prot = mbi.Protect & 0xFF;
        const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_WRITECOMBINE | PAGE_NOCACHE)) &&
                              (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE ||
                               prot == PAGE_READONLY || prot == PAGE_EXECUTE_READ);
        if (readable && size >= 128 && size < (64ull << 30)) {
            scanned += size;
            hits = SafeFindRegion(base, size, mbi, hits);
        }
        p = base + size;
    }
    LOG("Mem find (%s): %d hits in %zu MB, %lu ms", args, hits, scanned >> 20, GetTickCount() - t0);
}


// "snap": records every address (private read/write memory, exe image data) holding exactly 1.0f.
// "diff": logs the recorded addresses that now hold 1000, 0.001, 0 or another value != 1 (first 200 of each kind,
// with context), to find the setting behind the motion-blur scale (frame constants row 11 .w: 1 after a load,
// 1000 after the pause menu).
std::vector<uintptr_t> g_snap;

void SnapRegion(uint8_t* base, size_t size) {
    const uint32_t* w = reinterpret_cast<const uint32_t*>(base);
    const size_t n = size / 4;
    for (size_t i = 0; i < n; ++i)
        if (w[i] == 0x3F800000u) {
            if (g_snap.size() >= 60000000) return;
            g_snap.push_back(reinterpret_cast<uintptr_t>(w + i));
        }
}

void SafeSnap(uint8_t* base, size_t size) {
    __try {
        SnapRegion(base, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void Snap() {
    const DWORD t0 = GetTickCount();
    g_snap.clear();
    g_snap.reserve(20000000);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uint8_t* p = static_cast<uint8_t*>(si.lpMinimumApplicationAddress);
    uint8_t* end = static_cast<uint8_t*>(si.lpMaximumApplicationAddress);
    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    MEMORY_BASIC_INFORMATION mbi;
    size_t scanned = 0;
    while (p < end && VirtualQuery(p, &mbi, sizeof(mbi))) {
        uint8_t* base = static_cast<uint8_t*>(mbi.BaseAddress);
        const DWORD prot = mbi.Protect & 0xFF;
        const bool rw = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_WRITECOMBINE | PAGE_NOCACHE)) &&
                        (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY);
        const bool want = rw && (mbi.Type == MEM_PRIVATE || mbi.AllocationBase == (void*)exe);
        if (want && mbi.RegionSize < (64ull << 30)) {
            scanned += mbi.RegionSize;
            SafeSnap(base, mbi.RegionSize);
        }
        p = base + mbi.RegionSize;
    }
    LOG("Mem snap: %zu addresses hold 1.0 (%zu MB scanned, %lu ms)", g_snap.size(), scanned >> 20, GetTickCount() - t0);
}

int SafeRead(uintptr_t a, uint32_t* out) {
    __try {
        *out = *reinterpret_cast<const uint32_t*>(a);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void Diff() {
    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    int n1000 = 0, nMilli = 0, nOther = 0, nZero = 0, gone = 0;
    for (uintptr_t a : g_snap) {
        uint32_t v;
        if (!SafeRead(a, &v)) {
            ++gone;
            continue;
        }
        if (v == 0x3F800000u) continue;
        float f;
        memcpy(&f, &v, 4);
        const bool is1000 = f > 999.0f && f < 1001.0f;
        const bool isMilli = f > 0.00099f && f < 0.00101f;
        int* cnt = is1000 ? &n1000 : isMilli ? &nMilli : v == 0 ? &nZero : &nOther;
        ++*cnt;
        if ((is1000 || isMilli) && *cnt <= 200) {
            const float* c = reinterpret_cast<const float*>(a);
            uint32_t ctxw[12];
            for (int k = 0; k < 12; ++k)
                if (!SafeRead(a - 16 + 4 * k, &ctxw[k])) ctxw[k] = 0;
            float cf[12];
            memcpy(cf, ctxw, sizeof(cf));
            MEMORY_BASIC_INFORMATION mbi{};
            VirtualQuery(c, &mbi, sizeof(mbi));
            LOG("Mem diff: %p now %.6g (%s, alloc %p%s, rva %llX) | %.4g %.4g %.4g %.4g [%.4g] %.4g %.4g %.4g %.4g %.4g %.4g %.4g",
                (void*)a, f, mbi.Type == MEM_PRIVATE ? "private" : "image", mbi.AllocationBase,
                mbi.AllocationBase == (void*)exe ? " exe" : "", (unsigned long long)(a - exe), cf[0], cf[1], cf[2], cf[3],
                cf[4], cf[5], cf[6], cf[7], cf[8], cf[9], cf[10], cf[11]);
        }
    }
    LOG("Mem diff: of %zu: 1000 -> %d, 0.001 -> %d, 0 -> %d, other -> %d, unreadable %d", g_snap.size(), n1000, nMilli,
        nZero, nOther, gone);
}

}  // namespace

void OnPresent() {
    if (g_forcing)
        for (float* f : g_force)
            if (*f == 1.0f) *f = 1000.0f;
    const DWORD now = GetTickCount();
    if (now - g_lastPoll < 1000) return;
    g_lastPoll = now;
    const std::wstring trig = vr::g_gameDir + L"\\ff16vr_memscan.trigger";
    if (GetFileAttributesW(trig.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    char line[128] = {};
    if (FILE* f = _wfopen(trig.c_str(), L"r")) {
        fgets(line, sizeof(line), f);
        fclose(f);
    }
    DeleteFileW(trig.c_str());
    for (char* q = line; *q; ++q)
        if (*q == '\r' || *q == '\n') *q = 0;
    if (strncmp(line, "snap", 4) == 0) {
        Snap();
        return;
    }
    if (strncmp(line, "diff", 4) == 0) {
        Diff();
        return;
    }
    if (strncmp(line, "find ", 5) == 0) {
        Find(line + 5);
        return;
    }
    Scan(strncmp(line, "force", 5) == 0);
}

}  // namespace mem_scan
