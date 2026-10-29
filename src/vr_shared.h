#pragma once
#include <cstdint>

// State shared between the OpenXR side (render thread, xr_screen.cpp) and the camera hook (game thread).
namespace vrs {

struct Pose {
    float q[4] = {0, 0, 0, 1};  // x y z w
    float p[3] = {0, 0, 0};
};

// What the render thread publishes every frame (VRMode 1).
struct HeadSample {
    bool valid = false;
    Pose rel;          // head relative to the recenter pose (yaw removed), meters: applied to the game camera
    Pose abs;          // head in OpenXR LOCAL space: submitted with the frame rendered from `rel`
    float tanUp = 1, tanDown = 1, tanLeft = 1, tanRight = 1;  // union of both eyes' field of view (positive tangents)
    // Native stereo: each eye's pose relative to the head (OpenXR VIEW space) and its own field of view.
    Pose eye[2];
    float eyeTan[2][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}};  // up, down, left, right (positive tangents)
    // Eye tracking (XR_EXT_eye_gaze_interaction): gaze direction in head (VIEW) space, -z forward.
    bool gazeValid = false;
    float gazeDir[3] = {0, 0, -1};
};

// What the camera hook used for one game camera update.
struct UsedSample {
    bool valid = false;
    Pose abs;
    float tanHalfV = 1, tanHalfH = 1;  // symmetric field of view the game rendered with
    uint64_t presentTag = 0;           // present count when the camera was updated
    int eye = -1;                      // -1 = mono (both eyes), 0 = left, 1 = right
};

void PublishHead(const HeadSample& s);
HeadSample LatestHead();

// Camera hook: remember what a camera update used. Present: pick the update `lag` presents back.
void RecordUsed(const UsedSample& s);
bool UsedForPresent(uint64_t presentCount, int lag, UsedSample* out);
uint64_t PresentCount();
void CountPresent();

// Small helpers (quaternion / vector math, OpenXR conventions: x right, y up, -z forward).
void QuatToMat3(const float q[4], float m[3][3]);  // column-vector rotation matrix
void QuatMul(const float a[4], const float b[4], float out[4]);
void QuatConj(const float q[4], float out[4]);
void QuatRotate(const float q[4], const float v[3], float out[3]);
void PoseMul(const Pose& a, const Pose& b, Pose* out);  // a * b: b expressed in a's frame

}  // namespace vrs
