#pragma once
#include <windows.h>

// Render resolution for VR: how big the game renders (the swap-chain / window size it uses for its internal targets).
// The game renders at its window's client size (borderless window; it follows WM_SIZE with ResizeBuffers and resizes
// all its targets), so in Mono / Native the mod resizes the window to the per-eye render size (see config RenderScale).
// Also logs how the game decides that size (window style, client rect, user32 sizing calls with their game
// call sites) and offers a dev trigger to resize the game window: ff16vr_resize.trigger containing "W H" (client size).
struct ID3D12Device;
namespace render_size {

void OnDevice(ID3D12Device* dev);                     // game's D3D12 device: opens its adapter for VRAM logging

void Install();                                        // IAT hooks on the game's user32 sizing imports (logging)
void OnSwapChain(HWND hwnd, UINT w, UINT h, const char* how);  // swap chain created / resized (logs a game stack)
void OnPresent();                                      // keeps the window at the VR render size; polls the dev trigger
const char* Status();                                  // "W x H per eye" / "game resolution W x H"
float VrAspect();                                      // width / height of the VR render size while applied, else 0
long ResizeCount();                                    // ResizeBuffers calls so far

}  // namespace render_size
