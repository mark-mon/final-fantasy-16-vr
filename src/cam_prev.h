#pragma once

// Per-eye previous camera in the GPU camera constants, for per-eye DLSS history (cam_prev.cpp).
namespace cam_prev {

void Install();
void SetMode(int m);  // dev: 0 off, 1 auto (per-eye DLSS evaluating), 2 always
const char* Status();
bool Active();  // per-eye motion vectors wanted now (camera_hook also keeps the CameraParam ring per eye)

}  // namespace cam_prev
