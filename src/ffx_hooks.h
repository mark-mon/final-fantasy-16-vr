#pragma once
// Per-eye FSR 3 upscaler history (the right eye gets its own FidelityFX upscale context). See ffx_hooks.cpp.
namespace ffx_hooks {
void Install();  // patch ffxvi.exe's amd_fidelityfx_dx12.dll imports (at load, before the game creates contexts)
const char* Status();
bool FsrEvaluatingRecently();  // a per-eye FSR upscale ran in the last 0.5 s
}
