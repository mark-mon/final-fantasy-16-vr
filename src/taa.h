#pragma once
#include <d3d12.h>
// TAA off (DisableTAA): swaps the game's TAA compute pass for a pass-through copy. See taa.cpp.
namespace taa {
void OnDeviceCreated(ID3D12Device* dev);  // from the D3D12CreateDevice wrapper, before the game creates pipelines
const char* Status();
}
