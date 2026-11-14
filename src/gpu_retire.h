#pragma once
// Deferred release of GPU objects (textures, buffers) the mod replaces at run time.
// Command lists the game has recorded but the GPU has not run yet can still reference a texture we drop on a
// resolution change; releasing it at once frees memory the GPU still uses. On NVIDIA that ended in 0x139 bluescreens
// (user 2026-11-05 with Foveated DLSS; a player's 0.9.2 dumps 2026-11-12, nvlddmkm in a DPC). Everything replaced at
// run time goes through Defer and is released only after 5 s and 30 presents.
struct IUnknown;
namespace gpu_retire {
void Defer(IUnknown* obj);   // takes over the caller's reference (null is ignored)
void OnPresent();            // called once per game present: releases what is old enough
long Pending();
}
