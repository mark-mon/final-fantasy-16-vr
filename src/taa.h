#pragma once
#include <d3d12.h>
// TAA off (DisableTAA): swaps the game's TAA compute pass for a pass-through copy. See taa.cpp.
namespace taa {
void OnDeviceCreated(ID3D12Device* dev);  // from the D3D12CreateDevice wrapper, before the game creates pipelines
const char* Status();
// Shared command-list vtable hooks (used by eye_history.cpp): hook slot `slot` of every command-list vtable the
// device hooks see; OrigListFn returns the game's function for `self`.
void HookListSlot(int slot, void* fn);
void* OrigListFn(void* self, int slot);
// Device vtable hooks (ID3D12Device slots; register before the device is created). OrigListFn works for these too.
void HookDeviceSlot(int slot, void* fn);
ID3D12PipelineState* CurrentPso(void* cl);  // last pipeline set on this command list (this thread)
}
