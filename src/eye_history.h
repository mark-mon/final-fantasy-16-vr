#pragma once
// Per-eye history for the game's temporal effects (TAA, fog, half-res effects): see eye_history.cpp.
namespace eye_history {
void Install();  // before the D3D12 device is created (registers the command-list copy hooks)
const char* Status();
// Copies recorded while a Streamline / DLSS call runs on this thread are not the game's history copies (Streamline copies
// an eOnlyValidNow tag - the rebuilt motion vectors - into one pool texture shared by both eyes; swapping that per eye
// gave the left eye the right eye's previous vectors: noise while the head moved, user log Nov 9).
struct Suspend {
    Suspend();
    ~Suspend();
};
long Ignored();
void SetMask(int mask);  // dev: categories swapped (1 froxel x64, 2 froxel x4, 4 2D small, 8 half-res, 16 full-res)
}
namespace eye_history { void SetMultiMode(int mode); }  // dev "peehx"
