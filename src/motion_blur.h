#pragma once
// Keeps the game's motion blur Off in VR (DisableMotionBlur): after a load the game builds its motion-blur parameters
// at full strength and only re-applies its Off setting when the pause menu closes. See motion_blur.cpp.
namespace motion_blur {
void Start();          // background watcher thread
const char* Status();  // for the perf line
}
