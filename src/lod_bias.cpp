// Texture sharpness: extra mip LOD bias on the game's texture samplers (TextureLodBias).
//
// An upscaler renders at a lower resolution than it outputs, so textures should be sampled with a negative mip bias
// (log2(render / output), -1 at DLSS Performance) or they stay at the blur level of the render resolution. This adds
// the configured bias to the game's samplers:
//   - descriptor samplers (ID3D12Device::CreateSampler, slot 22): the game rewrites them every frame, so a changed
//     bias applies at once;
//   - static samplers in root signatures (CreateRootSignature, slot 16): patched when the root signature is created
//     (the bias at that time; a changed value applies after a restart).
// Only samplers that blend mips (linear mip filter or anisotropic), have mips (MaxLOD > 0) and are not comparison
// samplers (shadow maps) are changed. Streamline's proxy device and the runtime device both reach the hooks; a
// thread-local depth keeps the bias from being added twice.
#include "lod_bias.h"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "log.h"
#include "taa.h"

namespace lod_bias {
namespace {

using CreateSampler_t = void(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_SAMPLER_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateRootSig_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);

std::mutex g_mutex;
std::atomic<long> g_created{0}, g_eligible{0}, g_biased{0}, g_rootSigs{0}, g_staticBiased{0};
float g_gameMin = 1e9f, g_gameMax = -1e9f;   // the game's own MipLODBias values on eligible samplers
thread_local int t_depth = 0;

bool Eligible(D3D12_FILTER f, D3D12_COMPARISON_FUNC, float maxLod) {
    if (maxLod <= 0.f) return false;
    if (D3D12_DECODE_IS_COMPARISON_FILTER(f)) return false;
    if (D3D12_DECODE_FILTER_REDUCTION(f) != D3D12_FILTER_REDUCTION_TYPE_STANDARD) return false;  // min / max filters
    return D3D12_DECODE_IS_ANISOTROPIC_FILTER(f) || D3D12_DECODE_MIP_FILTER(f) == D3D12_FILTER_TYPE_LINEAR;
}

float Biased(float game, float extra) { return std::clamp(game + extra, -16.f, 15.99f); }

void STDMETHODCALLTYPE Hk_CreateSampler(ID3D12Device* dev, const D3D12_SAMPLER_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    auto orig = reinterpret_cast<CreateSampler_t>(taa::OrigListFn(dev, 22));
    if (t_depth > 0 || !d) {
        orig(dev, d, h);
        return;
    }
    // Run 243: the game writes its sampler descriptors every frame (~24 000 per second), so the bias set at creation
    // follows the option live and nothing needs recording. Keep this path cheap: no lock.
    const float extra = vr::g_config.textureLodBias;
    const long n = ++g_created;
    if (!Eligible(d->Filter, d->ComparisonFunc, d->MaxLOD)) {
        orig(dev, d, h);
        return;
    }
    if ((n & 1023) == 1) {  // sampled statistics of the game's own bias
        std::lock_guard<std::mutex> lock(g_mutex);
        g_gameMin = (std::min)(g_gameMin, d->MipLODBias);
        g_gameMax = (std::max)(g_gameMax, d->MipLODBias);
    }
    ++g_eligible;
    if (extra == 0.f) {
        orig(dev, d, h);
        return;
    }
    D3D12_SAMPLER_DESC s = *d;
    s.MipLODBias = Biased(d->MipLODBias, extra);
    ++g_biased;
    ++t_depth;
    orig(dev, &s, h);
    --t_depth;
}

HRESULT STDMETHODCALLTYPE Hk_CreateRootSignature(ID3D12Device* dev, UINT mask, const void* blob, SIZE_T len, REFIID riid,
                                                 void** out) {
    auto orig = reinterpret_cast<CreateRootSig_t>(taa::OrigListFn(dev, 16));
    const float extra = vr::g_config.textureLodBias;
    if (t_depth > 0 || !blob || !len || extra == 0.f) return orig(dev, mask, blob, len, riid, out);
    ++t_depth;
    HRESULT hr = E_FAIL;
    ID3D12VersionedRootSignatureDeserializer* des = nullptr;
    ID3DBlob* ser = nullptr;
    if (SUCCEEDED(D3D12CreateVersionedRootSignatureDeserializer(blob, len, IID_PPV_ARGS(&des))) && des) {
        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* vd = nullptr;
        if (SUCCEEDED(des->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &vd)) && vd &&
            vd->Version == D3D_ROOT_SIGNATURE_VERSION_1_1 && vd->Desc_1_1.NumStaticSamplers) {
            const auto& rs = vd->Desc_1_1;
            std::vector<D3D12_STATIC_SAMPLER_DESC> ss(rs.pStaticSamplers, rs.pStaticSamplers + rs.NumStaticSamplers);
            int changed = 0;
            for (auto& s : ss)
                if (Eligible(s.Filter, s.ComparisonFunc, s.MaxLOD)) {
                    {
                        std::lock_guard<std::mutex> lock(g_mutex);
                        g_gameMin = (std::min)(g_gameMin, s.MipLODBias);
                        g_gameMax = (std::max)(g_gameMax, s.MipLODBias);
                    }
                    s.MipLODBias = Biased(s.MipLODBias, extra);
                    ++changed;
                }
            if (changed) {
                D3D12_VERSIONED_ROOT_SIGNATURE_DESC nd = *vd;
                nd.Desc_1_1.pStaticSamplers = ss.data();
                ID3DBlob* err = nullptr;
                if (SUCCEEDED(D3D12SerializeVersionedRootSignature(&nd, &ser, &err)) && ser) {
                    hr = orig(dev, mask, ser->GetBufferPointer(), ser->GetBufferSize(), riid, out);
                    if (SUCCEEDED(hr)) {
                        g_staticBiased += changed;
                        if (++g_rootSigs <= 3) LOG("Texture LOD bias: root signature with %d of %u static samplers biased %+.2f",
                                                   changed, rs.NumStaticSamplers, extra);
                    }
                }
                if (err) err->Release();
            }
        }
    }
    if (ser) ser->Release();
    if (des) des->Release();
    if (FAILED(hr)) hr = orig(dev, mask, blob, len, riid, out);  // anything unexpected: the game's own blob
    --t_depth;
    return hr;
}

}  // namespace

void Install() {
    taa::HookDeviceSlot(22, (void*)&Hk_CreateSampler);
    taa::HookDeviceSlot(16, (void*)&Hk_CreateRootSignature);
    LOG("Texture LOD bias: sampler hooks registered (TextureLodBias=%.2f)", vr::g_config.textureLodBias);
}

void Apply() {
    LOG("Texture LOD bias %+.2f (descriptor samplers follow at once; static samplers in root signatures after a restart)",
        vr::g_config.textureLodBias);
}

const char* Status() {
    static char buf[160];
    std::lock_guard<std::mutex> lock(g_mutex);
    snprintf(buf, sizeof(buf), "texture LOD bias %+.2f: %ld samplers created, %ld eligible (game bias %.2f..%.2f), %ld static biased",
             vr::g_config.textureLodBias, g_created.load(), g_eligible.load(), g_gameMin < 1e8f ? g_gameMin : 0.f,
             g_gameMax > -1e8f ? g_gameMax : 0.f, g_staticBiased.load());
    return buf;
}

}  // namespace lod_bias
