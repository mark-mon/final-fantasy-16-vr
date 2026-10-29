#pragma once

// Per-eye DLSS history (NVIDIA Streamline).
// In Native stereo the game renders left, right, left, right... through one DLSS viewport, so DLSS blends each eye
// with the other eye's history (soft image when the head is still). The game loads sl.interposer.dll and finds the
// Streamline functions with GetProcAddress; the mod wraps them (import of GetProcAddress in ffxvi.exe) and sends the
// right eye's frames to a second viewport (game viewport + kRightEyeViewportOffset): constants, evaluate, options,
// resource allocation. Resource tags are set on both viewports (the eyes share the game's render targets).
// Config: PerEyeDlss (default 1).
namespace sl_hooks {

constexpr unsigned kRightEyeViewportOffset = 0x100;

void Install();
bool DlssEvaluatingRecently();  // per-eye DLSS evaluated within the last 0.5 s  // patch ffxvi.exe's GetProcAddress import (before the game initialises Streamline)

}  // namespace sl_hooks
