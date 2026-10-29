#pragma once

namespace cam_probe {

// Development aid: finds the game's camera data and the code that writes it.
// Create <game>\ff16vr_camprobe.trigger to run it (in normal play, RenderDoc not needed). Then:
//   1. scans the game's writable memory for a reversed-Z perspective projection laid out as in the camera
//      constant buffer (rows (0,0,0,near) and (0,0,-1,0)), logs each hit and dumps the memory around it to
//      <CaptureDir>\camprobe\hit_NN.bin;
//   2. puts hardware write breakpoints (debug registers, 4 at a time, all threads) on the jittered projection
//      element of each hit for a few seconds and logs the code that writes it, with call stacks.
// Results go to ff16vr.log ("CAMPROBE" lines); <game>\ff16vr_camprobe.done is written when finished.
void Init();

}  // namespace cam_probe
