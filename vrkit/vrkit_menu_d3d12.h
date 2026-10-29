// vrkit - in-game settings menu, D3D12 backend (for D3D12 games, e.g. the FF XVI VR mod).
// Same menu, options and controls as the D3D11 version (vrkit_menu.h). The menu is drawn with Dear ImGui into an
// off-screen R8G8B8A8_TYPELESS texture (rendered as UNORM, premultiplied alpha) on a command list the host
// supplies. The host then copies the texture into its OpenXR swapchain image (R8G8B8A8_UNORM_SRGB or UNORM:
// CopyResource works within the R8G8B8A8 family) and shows it as a quad layer.
//
// Per frame, on the thread that submits to the queue:
//   if (menu12::Update()) {            // true while open (input polled, menu built)
//       menu12::Record(cmdList);       // draws into Texture(); leaves it in COPY_SOURCE state
//       cmdList->CopyResource(swapImage, menu12::Texture());   // (swap image in COPY_DEST)
//       ... execute; keep the list's allocator alive until the GPU is done
//   }
// The ImGui DX12 backend keeps `framesInFlight` vertex / index buffers in a ring: the host must not have more
// than that many Record() calls in flight on the GPU.
#pragma once
#include <d3d12.h>
#include "vrkit_menu_core.h"

namespace vrkit { namespace menu12 {

bool Init(ID3D12Device* dev, int framesInFlight, const menu::Settings& s);
void Shutdown();
// Poll input, handle open/close and navigation, and build the menu while open. Returns true while open.
bool Update();
// Record the menu drawing into Texture() (cleared first). Texture() ends in D3D12_RESOURCE_STATE_COPY_SOURCE.
void Record(ID3D12GraphicsCommandList* cl);
bool IsOpen();
void SetOpen(bool open);
ID3D12Resource* Texture();
UINT Width();
UINT Height();

} } // namespace vrkit::menu12
