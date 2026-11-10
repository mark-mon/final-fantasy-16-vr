#pragma once

// Per-eye previous camera in the GPU camera constants, for per-eye DLSS history (cam_prev.cpp).
namespace cam_prev {

void Install();
void SetMode(int m);  // dev: 0 off, 1 auto (per-eye DLSS evaluating), 2 always
const char* Status();
void RequestCbDump(int frames);
void RequestTrace(int calls);  // dev: log the next N camera-constant finalise calls  // dev: write ff16vr_ccameraN_eyeE_{before,after}.bin (0x900 bytes) to the game folder
bool Active();  // per-eye motion vectors wanted now (camera_hook also keeps the CameraParam ring per eye)
// Frame warp: an eye's last camera constants (float4 rows; kRows of them) and the previous camera that render used (where
// its motion vectors point: the game's, or this eye's own when per-eye motion vectors are on); false until known.
constexpr int kRows = 64;
bool EyeCamera(int eye, float last[kRows][4], float prev[kRows][4]);
// Also the previous camera the game itself put in the buffer (before this module's substitution): in native stereo
// the other eye (of this tick for the second pass, of the previous tick for the first).
bool EyeCameraEx(int eye, float last[kRows][4], float used[kRows][4], float game[kRows][4]);
// A recent camera (one per frame pass): its constants (rows), the previous camera the game put in the buffer (game),
// the pass serial, the eye (0 left, 1 right) and the pass of its tick (0 first, 1 second).
struct CamEntry {
    float rows[kRows][4];
    float game[kRows][4];
    unsigned long long serial = 0;
    int eye = -1, pass = 0;
    bool valid = false;
};
// The camera of the tick whose camera node sits at `nodePos` (row 37, within 0.1 mm: what Streamline is given as the
// camera position; the same for both eyes of a tick) and pass `pass` (0 first, 1 second), this eye's previous camera
// and the pass before it (the other eye's, normally). Entries not found come back invalid.
// FSR: the camera of the pass whose projection carries this jitter (the FFX dispatch's jitterOffset, render pixels; the
// camera rows carry it in NDC). The game finalises cameras on job threads ahead of the render thread, so the newest
// camera of an eye can belong to the NEXT pass (right-eye noise while the head moves, user Nov 10). Fills `last` (that
// camera) and `used` (the previous camera its motion vectors point to); *back = how many entries back it was found.
bool FindCameraByJitter(int eye, float jx, float jy, int w, int h, float last[kRows][4], float used[kRows][4], int* back);
float EyeSeparation();
const char* JitterMatchStatus();  // right eye - left eye along the camera's right axis (game units; 0.063 until known)
bool FindCamera(const float nodePos[3], int pass, CamEntry* cur, CamEntry* samePrev, CamEntry* prevPass);

}  // namespace cam_prev
