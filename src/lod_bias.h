#pragma once
// Texture sharpness: extra mip LOD bias on the game's texture samplers (TextureLodBias). See lod_bias.cpp.
namespace lod_bias {
void Install();  // before the D3D12 device is created
void Apply();    // after TextureLodBias changed: rewrite the recorded samplers
const char* Status();
}
