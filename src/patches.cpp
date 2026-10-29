// Game-code patches for VR comfort and performance.
//
// Several signatures and patch ideas come from FFXVIFix by Lyall
// (https://github.com/Lyall/FFXVIFix, MIT License, Copyright (c) 2024 Lyall).
// Each one has been re-checked against the current ffxvi.exe. Every patch checks
// the original bytes before writing, so a game update or another mod that already
// patched the same spot is logged and skipped instead of corrupting code.

#include "patches.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "config.h"
#include "log.h"

namespace patches {
namespace {

struct Module {
    uint8_t* base = nullptr;
    uint8_t* text = nullptr;
    size_t textSize = 0;
};

Module GetGameModule() {
    Module m;
    m.base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(m.base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(m.base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 5) == 0) {
            m.text = m.base + sec->VirtualAddress;
            m.textSize = sec->Misc.VirtualSize;
        }
    }
    return m;
}

// "48 8B ?? ??" -> bytes + mask
bool ParsePattern(const char* pat, std::vector<uint8_t>& bytes, std::vector<bool>& mask) {
    const char* p = pat;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0] == '?') {
            bytes.push_back(0);
            mask.push_back(false);
            p += (p[1] == '?') ? 2 : 1;
        } else {
            char hex[3] = {p[0], p[1], 0};
            bytes.push_back((uint8_t)strtoul(hex, nullptr, 16));
            mask.push_back(true);
            p += 2;
        }
    }
    return !bytes.empty();
}

// Returns the unique match, or nullptr if there are zero or several matches.
uint8_t* FindUnique(const Module& m, const char* pattern, int* outCount) {
    std::vector<uint8_t> bytes;
    std::vector<bool> mask;
    ParsePattern(pattern, bytes, mask);
    size_t n = bytes.size();
    uint8_t* found = nullptr;
    int count = 0;
    for (size_t i = 0; i + n <= m.textSize; ++i) {
        const uint8_t* p = m.text + i;
        if (p[0] != bytes[0] && mask[0]) continue;
        size_t j = 0;
        for (; j < n; ++j)
            if (mask[j] && p[j] != bytes[j]) break;
        if (j == n) {
            if (++count == 1) found = m.text + i;
        }
    }
    if (outCount) *outCount = count;
    return count == 1 ? found : nullptr;
}

bool WriteCode(uint8_t* dst, const void* src, size_t len) {
    DWORD old;
    if (!VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(dst, src, len);
    VirtualProtect(dst, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
    return true;
}

// Allocate executable memory within +-2 GB of `near` so a rel32 jmp can reach it.
uint8_t* AllocNear(uint8_t* nearAddr, size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t target = reinterpret_cast<uintptr_t>(nearAddr);
    const uintptr_t range = 0x7FF00000ull;
    uintptr_t lo = target > range ? target - range : gran;
    uintptr_t hi = target + range;
    // Search downward first (below the image is usually free), then upward.
    for (uintptr_t a = (target & ~(gran - 1)) - gran; a > lo; a -= gran) {
        void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) return static_cast<uint8_t*>(p);
    }
    for (uintptr_t a = (target & ~(gran - 1)) + gran; a < hi; a += gran) {
        void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) return static_cast<uint8_t*>(p);
    }
    return nullptr;
}

std::vector<uint8_t> Hex(const char* s) {
    std::vector<uint8_t> b;
    std::vector<bool> m;
    ParsePattern(s, b, m);
    return b;
}

struct BytePatch {
    const char* name;
    const bool* enabled;
    const char* pattern;
    int offset;            // from signature start
    const char* expected;  // original bytes at offset
    const char* replacement;
};

bool ApplyBytePatch(const Module& m, const BytePatch& p) {
    int count = 0;
    uint8_t* sig = FindUnique(m, p.pattern, &count);
    if (!sig) {
        LOG("Patch %-28s SKIPPED: signature matched %d times (game updated, or already patched by another mod?)",
            p.name, count);
        return false;
    }
    uint8_t* at = sig + p.offset;
    auto expected = Hex(p.expected);
    auto replacement = Hex(p.replacement);
    if (memcmp(at, replacement.data(), replacement.size()) == 0) {
        LOG("Patch %-28s already applied at ffxvi.exe+0x%llX", p.name, (unsigned long long)(at - m.base));
        return true;
    }
    if (memcmp(at, expected.data(), expected.size()) != 0) {
        LOG("Patch %-28s SKIPPED: unexpected bytes at ffxvi.exe+0x%llX", p.name, (unsigned long long)(at - m.base));
        return false;
    }
    if (!WriteCode(at, replacement.data(), replacement.size())) {
        LOG("Patch %-28s FAILED: could not write code", p.name);
        return false;
    }
    LOG("Patch %-28s applied at ffxvi.exe+0x%llX", p.name, (unsigned long long)(at - m.base));
    return true;
}

// Cinematic effects (vignette, film grain, chromatic aberration in cutscenes /
// photo mode). The post-process mode is chosen from a flags word written by
//   89 4F 18   mov [rdi+0x18], ecx
//   C1 E8 0A   shr eax, 0x0A
// We divert those 6 bytes to a stub that clears the cinematic mode bits first:
//   if (cl & 0x1B) cl = 3;
bool ApplyCinematicEffectsPatch(const Module& m) {
    const char* name = "DisableCinematicEffects";
    int count = 0;
    uint8_t* site = FindUnique(m, "89 ?? ?? C1 ?? ?? 41 ?? ?? 74 ?? C6 ?? ?? ?? EB ?? 8B ??", &count);
    if (!site) {
        LOG("Patch %-28s SKIPPED: signature matched %d times", name, count);
        return false;
    }
    const uint8_t stolen[] = {0x89, 0x4F, 0x18, 0xC1, 0xE8, 0x0A};
    if (memcmp(site, stolen, sizeof(stolen)) != 0) {
        LOG("Patch %-28s SKIPPED: unexpected bytes at ffxvi.exe+0x%llX (already hooked?)", name,
            (unsigned long long)(site - m.base));
        return false;
    }
    uint8_t* cave = AllocNear(site, 64);
    if (!cave) {
        LOG("Patch %-28s FAILED: no memory near module", name);
        return false;
    }
    uint8_t* back = site + sizeof(stolen);
    uint8_t stub[] = {
        0xF6, 0xC1, 0x1B,                    // test cl, 0x1B
        0x74, 0x02,                          // jz +2
        0xB1, 0x03,                          // mov cl, 3
        0x89, 0x4F, 0x18,                    // mov [rdi+0x18], ecx   (original)
        0xC1, 0xE8, 0x0A,                    // shr eax, 0x0A         (original)
        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,  // jmp [rip+0]
        0, 0, 0, 0, 0, 0, 0, 0               // absolute return address
    };
    memcpy(stub + sizeof(stub) - 8, &back, 8);
    memcpy(cave, stub, sizeof(stub));
    FlushInstructionCache(GetCurrentProcess(), cave, sizeof(stub));

    uint8_t jmp[6] = {0xE9, 0, 0, 0, 0, 0x90};
    int32_t rel = (int32_t)(cave - (site + 5));
    memcpy(jmp + 1, &rel, 4);
    if (!WriteCode(site, jmp, sizeof(jmp))) {
        LOG("Patch %-28s FAILED: could not write code", name);
        return false;
    }
    LOG("Patch %-28s applied at ffxvi.exe+0x%llX (stub %p)", name, (unsigned long long)(site - m.base), cave);
    return true;
}

}  // namespace

void ApplyGamePatches() {
    const vr::Config& c = vr::g_config;
    Module m = GetGameModule();
    if (!m.text) {
        LOG("Patches: .text section not found");
        return;
    }

    const BytePatch bytePatches[] = {
        // Cutscene / photo mode 30 fps cap: the "cap active" flag is set by
        // `mov dil, 1`; make it `mov dil, 0`.
        {"UncapCutsceneFPS", &c.uncapCutsceneFps, "75 ?? 85 ?? 74 ?? 40 ?? 01 41 ?? ?? ?? ?? ?? ?? ??", 6,
         "40 B7 01", "40 B7 00"},
        // Depth of field: the "DOF enabled?" query always returns false.
        {"DisableDepthOfField", &c.disableDepthOfField, "74 ?? 80 ?? ?? 08 73 ?? 44 ?? ?? ?? 41 ?? ?? C3", 0, "74",
         "EB"},
        // Near depth of field path.
        {"DisableDepthOfField(near)", &c.disableDepthOfField,
         "75 ?? 48 ?? ?? ?? 44 ?? ?? ?? ?? ?? ?? 75 ?? 0F ?? ?? ?? 48 ?? ?? ?? ?? ?? ??", 0, "75", "EB"},
        // Graphics-debugger check, so RenderDoc / PIX captures work.
        {"DisableGraphicsDebuggerCheck", &c.disableGraphicsDebuggerCheck,
         "74 ?? E8 ?? ?? ?? ?? 84 ?? 0F 85 ?? ?? ?? ?? 48 8B ?? ?? ?? ?? ?? 48 85 ?? 74 ??", 0, "74", "EB"},
    };

    for (const BytePatch& p : bytePatches) {
        if (*p.enabled) ApplyBytePatch(m, p);
        else LOG("Patch %-28s disabled in config.txt", p.name);
    }

    if (c.disableCinematicEffects) ApplyCinematicEffectsPatch(m);
    else LOG("Patch %-28s disabled in config.txt", "DisableCinematicEffects");
}

}  // namespace patches

// ---- shared helpers for other hooks (camera_hook.cpp) ----
namespace patches {
uint8_t* FindCode(const char* pattern, int* count) {
    static Module m = GetGameModule();
    return FindUnique(m, pattern, count);
}
uint8_t* GameBase() { return GetGameModule().base; }
bool WriteCodeBytes(uint8_t* dst, const void* src, size_t len) { return WriteCode(dst, src, len); }
uint8_t* AllocCodeNear(uint8_t* nearAddr, size_t size) { return AllocNear(nearAddr, size); }
}  // namespace patches
