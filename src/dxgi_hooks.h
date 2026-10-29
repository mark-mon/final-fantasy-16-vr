#pragma once

namespace hooks {

// Patches the native DXGI factory vtable (CreateSwapChain / CreateSwapChainForHwnd)
// so we can capture the game's D3D12 command queue and then hook the native
// swap chain's Present / Present1 / ResizeBuffers / ResizeBuffers1.
bool InstallDxgiHooks();

}  // namespace hooks
