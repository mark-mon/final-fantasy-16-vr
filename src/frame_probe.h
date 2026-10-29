#pragma once

// Development aid for native stereo: logs where frames come from (config FrameProbe = 1).
//  - call stacks (module+offset) + thread ids of the first few Presents and main-view camera updates
//  - once per second: presents, main-view updates and all-view updates in that second
namespace frame_probe {

bool Enabled();
void OnPresent();          // render thread, from the swap chain Present hook
void OnViewUpdate(bool mainView);  // game thread, from the camera LookAt hook

}  // namespace frame_probe
