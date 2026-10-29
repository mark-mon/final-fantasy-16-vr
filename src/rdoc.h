#pragma once

namespace rdoc {

// Development aid (config: RenderDoc = 1). Loads renderdoc.dll into the game early, before the game
// creates its D3D12 device, so frames can be captured without launching the game from RenderDoc
// (the game quits when started by renderdoccmd). A capture is requested by creating
// <game>\ff16vr_capture.trigger (optional content: number of frames); the mod then writes the .rdc path
// into <game>\ff16vr_capture.done.
void Init();

// Capture mode: called for every GetProcAddress the game makes. When the game asks Streamline
// (sl.interposer.dll) for D3D12CreateDevice / CreateDXGIFactory2, returns wrappers that load RenderDoc
// at that moment (after the game's early NVAPI check, before any D3D12 object exists) and call the real,
// RenderDoc-hooked d3d12.dll / dxgi.dll functions, bypassing Streamline. Otherwise returns `proc`.
void* WrapProc(void* module, const char* name, void* proc);

}  // namespace rdoc
