#pragma once
// Per-eye motion vectors rebuilt for the upscaler (MotionRebuild). See mvfix.cpp.
#include <d3d12.h>
#include "stereo_edge.h"

namespace mvfix {
bool Wanted();  // per-eye upscaler history on and the option set
// At the upscaler's evaluate, on the game's command list: returns this eye's rebuilt motion vectors (same size and
// format as the game's texture; NON_PIXEL | PIXEL shader resource) or nullptr (then the game's are used).
ID3D12Resource* Run(ID3D12GraphicsCommandList* cl, const stereo_edge::Inputs& in);
// FSR (no Streamline constants): fills in.sl / in.haveSl with the matrices the rebuild needs, built from this eye's camera
// constants (cam_prev: the camera of its last pass and the previous camera that pass used), in Streamline's layout.
bool ConstantsFromCamera(int eye, stereo_edge::Inputs* in, float jitterX, float jitterY, int renderW, int renderH);
// The object part of an eye's last rebuilt vectors (texture-uv units like the game's, RG16F, the rendered extent; state
// NON_PIXEL | PIXEL shader resource), or nullptr.
ID3D12Resource* LastObject(int eye, UINT* w, UINT* h);
bool DevCommand(const char* cmd, int v);  // dev trigger lines: "mvcam +-1", "mvobj +-1", "mvthr tenths-px", "mvmode N"
const char* Status();
}  // namespace mvfix
