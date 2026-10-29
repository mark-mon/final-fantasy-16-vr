#pragma once

// Dev profiler for the native-stereo frame passes. <game>\ff16vr_prof.trigger containing a duration in seconds
// (default 5) samples the game thread about 1000 times per second for that long. Each sample walks the stack and
// is charged to the frame pass (left / right / single frame), the frame function's call site it is under and the
// two game functions below that. Also logs every thread's CPU time over the window (by thread start address), and
// the wall time and game-thread CPU time of each frame pass. All results go to the log ("Prof: ..." lines).
namespace pass_prof {

void Install();

}  // namespace pass_prof
