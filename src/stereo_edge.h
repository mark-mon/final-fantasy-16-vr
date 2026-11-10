#pragma once
// Stereo edge fix (StereoEdgeFix): shared upscaler history without the halo at depth edges. See stereo_edge.cpp.
#include <d3d12.h>
#include <cstdint>
namespace stereo_edge {
struct Inputs {
    ID3D12Resource* depth = nullptr;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* mv = nullptr;
    D3D12_RESOURCE_STATES mvState = D3D12_RESOURCE_STATE_COMMON;
    int x = 0, y = 0, w = 0, h = 0;              // rendered extent in the textures
    float pixPerMvX = 0.f, pixPerMvY = 0.f;      // pixels of motion per motion-vector unit
    bool depthInverted = true;
    int eye = 0;
    float camPos[3] = {};  // the camera position Streamline was given for this evaluate (sl::Constants cameraPos)
    bool haveCamPos = false;
    uint8_t sl[464] = {};  // the sl::Constants of this evaluate (matrices at 32.., jitter 352, mvec scale 360, camera 376..)
    bool haveSl = false;
    float jitter[2] = {0, 0};  // the upscaler's jitter offset for this pass (render pixels): identifies the pass's camera
    bool haveJitter = false;
};
void Install();  // command-list state hooks (before the device is created)
bool Wanted();   // StereoEdgeFix on
// Records the pass into the game's command list; returns the fixed motion vectors (state NON_PIXEL | PIXEL shader
// resource) or nullptr (then the game's own are used).
ID3D12Resource* Run(ID3D12GraphicsCommandList* cl, const Inputs& in);
long Passes();
// Puts back the compute state (heaps, root signature, root arguments, pipeline) the game had set on this list before a
// pass of ours (tracked by the hooks Install sets up).
void RestoreListState(ID3D12GraphicsCommandList* cl);
// Frame warp: copies the rendered extent of the depth / motion vectors into depthOut (R32_FLOAT) / mvOut (R16G16_FLOAT),
// both kept in NON_PIXEL_SHADER_RESOURCE; recorded into the game's command list.
bool Capture(ID3D12GraphicsCommandList* cl, const Inputs& in, ID3D12Resource* depthOut, ID3D12Resource* mvOut);
}
