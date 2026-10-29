#pragma once

struct IDXGISwapChain;
struct ID3D12CommandQueue;

namespace xr {

// Called when the game creates its swap chain. Takes ownership of one queue reference.
void OnSwapChainCreated(IDXGISwapChain* sc, ID3D12CommandQueue* queue);

// Called on the game's render thread just before the native Present.
void OnPresent(IDXGISwapChain* sc);

// Called before the swap chain's buffers are resized.
void OnResizeBuffers(IDXGISwapChain* sc);

// Recenter the view / screen at the next frame (menu action; same as the recenter key).
void RequestRecenter();
// The HUD shows the opening cinematic's letterbox mask (opaque black middle, clear top / bottom rows): play that
// stretch on the flat screen without the HUD panel (xr_screen.cpp ProbeResult).
bool HudMovieMode();
// OpenXR frames submitted in the last second.
unsigned VrFramesPerSecond();
void RecommendedEyeSize(unsigned* w, unsigned* h);  // 0 x 0 until the headset is found
bool HeadsetEyeTangents(float out[2][4]);          // per eye: tan up, down, left, right; false until first located
void MaxSwapchainSize(unsigned* w, unsigned* h);
long SwapchainCreates();                           // OpenXR scene swapchain (re)creations so far

}  // namespace xr
