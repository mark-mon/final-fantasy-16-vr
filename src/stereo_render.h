#pragma once

// Native stereo. The game's main loop calls the frame function (ffxvi.exe+0x9B0EB0 in 1.03) once per tick; in
// VRMode Native the mod calls it twice during gameplay: pass 0 renders the left eye, pass 1 the right eye, with the
// game clock frozen for pass 1 (so both eyes show the same moment). Also wraps renderer->Render() for timing and
// the dev experiment "Render() twice" (ff16vr_render2.trigger: 0 normal, 1 Render twice, 2 frame twice,
// 3 frame twice without presenting pass 0).
namespace stereo_render {

void Install();
int CurrentPass();        // view of this frame pass: 0 left, 1 right
int ViewCount();          // frame passes per tick: 1 or 2
int FrameEye();           // view of the frame being rendered, from any thread: -1 single frame, 0..3 (eye = view & 1)
bool NativeActive();      // both eyes are being rendered this tick (gameplay camera active)
void SetNativeMode(bool on);  // VRMode switched at run time (menu)
void NoteMainView();      // camera hook: the gameplay camera was updated this frame
long ModeSwitches();      // switches between one and two frames per tick so far
bool GameplayReached();   // the gameplay camera has been stable once (sticky; set in any VR mode)
bool SkipThisPresent();   // dev (trigger 3)
void OnPresentBegin();    // swap chain Present timing (game thread)
void OnPresentEnd();

// Profiling (pass_prof): the frame function's address, the game thread, and per-pass totals since the last call
// (wall time and game-thread CPU cycles of each frame-function call, by pass 0 / 1; index 2 = single-frame ticks).
void* FrameFnAddr();
unsigned long GameThreadId();
struct PassTotals { double wallMs[3]; double cycles[3]; long calls[3]; };
PassTotals TakePassTotals();

}  // namespace stereo_render
