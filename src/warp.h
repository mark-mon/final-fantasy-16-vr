#pragma once
// Frame warp (VRMode Warp): the eye not drawn this tick is rebuilt from depth. See warp.cpp.
#include <d3d12.h>
#include <cstdint>
#include "stereo_edge.h"

namespace warp {
void Init(ID3D12Device* dev, ID3D12CommandQueue* queue);  // xr_screen: the device and the game's queue
bool Wanted();  // frame warp running, or the lab in native stereo
// At the upscaler (DLSS / FSR), on the game's command list: keep this eye's depth and motion vectors.
void OnUpscalerInputs(ID3D12GraphicsCommandList* cl, const stereo_edge::Inputs& in, int eye);
// At the present of `eye`'s image: its depth, motion vectors and camera become its held state.
// Before Hold / Rebuild at the present of an image of `eye`: selects the capture (depth, camera) that image was rendered
// with. The engine presents one or more frames after rendering.
void BeginPresent(int eye, uint32_t serial);  // serial: the UsedSample of this present (0 = pair by counting)
void Hold(int eye);
// Rebuild the other eye from the drawn one (`source`, image srcColor = the game's image) and the other eye's held image
// (histColor). eyeOffset: the other eye relative to the drawn one, in camera axes (game units). useRealDest (lab): the
// other eye's own camera of this tick. Returns the rebuilt image (PIXEL_SHADER_RESOURCE, RGBA16F) or nullptr.
ID3D12Resource* Rebuild(int source, ID3D12Resource* srcColor, ID3D12Resource* histColor, bool haveHist, DXGI_FORMAT colorFmt,
                        const float eyeOffset[3], bool useRealDest);
void SetBackSign(int s);  // dev
void SetMotion(bool on);  // dev
void RequestDump(int eye);
void DumpNow(int eye);  // dev: write that eye's current capture now (plain Native with DevCapture=1)  // dev: write that eye's depth, motion vectors and camera at the next rebuild
void Poll();  // dev trigger file (xr_screen, each present)
const char* Status();
}  // namespace warp
