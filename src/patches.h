#pragma once
#include <cstddef>
#include <cstdint>

namespace patches {

// Applies the game-code patches enabled in config.txt (frame cap, DOF, etc.).
// Each patch is located by byte signature and verified before writing.
void ApplyGamePatches();

// Helpers for other hooks.
uint8_t* FindCode(const char* pattern, int* count);  // unique match in ffxvi.exe .text, else nullptr
// The game's post-process flags word (cinematic look bits 0x1B) as last written, before DisableCinematicEffects; ~0u unknown.
uint32_t PostModeWord();
uint8_t* GameBase();
bool WriteCodeBytes(uint8_t* dst, const void* src, size_t len);
uint8_t* AllocCodeNear(uint8_t* nearAddr, size_t size);  // executable memory within +-2 GB

}  // namespace patches
