#pragma once
#include <d3d12.h>

// Fixed foveated rendering with D3D12 Variable Rate Shading (Tier 2 shading-rate image).
// Each eye is rendered with a symmetric frustum, so the eye's optical centre is the image centre: one shading-rate
// image per render-target size, full rate inside FoveatedInner, 2x2 up to FoveatedOuter, FoveatedOuterRate beyond
// (radii as a fraction of the half height; ellipse follows the image aspect).
// Applied in Mono / Native to the game's scene passes: OMSetRenderTargets with a colour target whose size matches the
// game's render size (or its DLSS / upscaler input size, same aspect, at least 1/4 of the width). Shadow passes (no
// colour target), post-processing (compute), the UI and the final copy to the swap chain keep full rate.
namespace vrs {

void OnDevice(ID3D12Device* device, ID3D12CommandQueue* queue);  // hooks CreateRenderTargetView + OMSetRenderTargets
void OnSwapChainSize(unsigned w, unsigned h);
void SetBackBuffer(ID3D12Resource* const* buffers, unsigned count);  // never foveate the swap chain
const char* Status();

}  // namespace vrs
