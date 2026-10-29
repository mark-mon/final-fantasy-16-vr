#pragma once

namespace camera_hook {

// Hooks the camera source of RenderView::Update (ffxvi.exe+0xAA23C4 in 1.03): the call that computes the camera
// node's world matrix (scene-graph helper, +0x9AEB7C). The hook runs the original, then applies a head transform
// H to the result (M' = H * M, row-vector convention), so the view, projection products, inverses and frustum
// planes the game derives from it all follow.
//
// Step 5a (test): a fixed yaw/pitch offset, toggled with F9 or <game>\ff16vr_camtest.trigger
// ("<yaw> <pitch>" in degrees, or "off"). Later this takes the HMD pose from OpenXR.
void Install();
void OnFramePass();
void* MainView();               // the gameplay RenderView (null until seen)
unsigned long long PassSerial();  // frame passes so far
long HistoryRestores();  // per-eye camera history restores so far (PerEyeHistory)  // stereo_render: before each frame pass (dev: ff16vr_viewseq.trigger dumps)

}  // namespace camera_hook
