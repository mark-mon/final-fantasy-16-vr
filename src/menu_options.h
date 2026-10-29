#pragma once
#include <d3d12.h>
#include "vrkit_menu_placement.h"

// FF16VR adapter for the vrkit in-headset settings menu (D3D12 backend). Registers the config.txt options and
// applies changes live where the mod can; the rest are marked "restart". Saved to config.txt when the menu closes.
namespace menu_options {


bool Init(ID3D12Device* dev, int framesInFlight);  // once, on the render thread, after the D3D12 device exists
const vrkit::menu::Placement& Placement();

}  // namespace menu_options
