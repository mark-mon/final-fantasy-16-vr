#pragma once

struct ID3D12Device;

namespace pso_guard {

// RenderDoc capture mode only. Under RenderDoc the driver rejects some of the game's compute pipelines
// (E_INVALIDARG) and the game then uses the null pipeline and crashes during its shader compile.
// Install() patches the (RenderDoc-wrapped) device's CreateComputePipelineState so that a failed creation:
//   1. is retried without the game's CachedPSO blob, if it passed one;
//   2. otherwise has its shader dumped to <CaptureDir>\failed_pso\ and is replaced by a do-nothing
//      compute pipeline on the same root signature, so the game keeps running.
// Graphics / stream pipeline failures are logged (and retried without CachedPSO) but not replaced.
// replace = false: log and count only (used without RenderDoc to see what the game does natively).
void Install(ID3D12Device* device, bool replace = true);

}  // namespace pso_guard
