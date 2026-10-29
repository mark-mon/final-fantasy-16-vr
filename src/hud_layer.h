#pragma once
#include <d3d12.h>
#include <dxgi.h>

// HUD on its own layer.
// FF XVI draws the scene into a swap-chain-sized R10G10B10A2 image, then ~30 UI draws blend the HUD onto it through
// a UAV (premultiplied "over": rgb = dst*(1-a) + src, a = dst.a*(1-a) + a), and the final draw copies that image to
// the swap chain. The mod points that UAV at its own transparent RGBA8 texture instead, so the HUD lands there with
// correct alpha and the swap chain gets the scene without HUD. Each Present the HUD is copied out (for the VR HUD
// layer) and the texture cleared; after the VR side has taken the scene, the HUD is blended back onto the backbuffer
// so the desktop window still shows it.
namespace hud {

void OnDevice(ID3D12Device* device, ID3D12CommandQueue* queue);  // swap chain created (native device / queue)
void OnSwapChainSize(UINT w, UINT h);

// Present hook, in this order: BeforeVr() (copy + clear the HUD texture), then the VR code, then AfterVr().
void BeforeVr();
void AfterVr(IDXGISwapChain* sc);

bool Active();                 // the game's HUD is being redirected (texture exists and was used)
ID3D12Resource* Texture();     // last frame's HUD, R8G8B8A8_TYPELESS, premultiplied alpha, COPY_SOURCE state
UINT Width();
UINT Height();

}  // namespace hud
