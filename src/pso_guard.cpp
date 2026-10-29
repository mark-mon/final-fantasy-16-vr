#include "pso_guard.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "config.h"
#include "log.h"

namespace pso_guard {
namespace {

// ID3D12Device vtable slots: IUnknown 0-2, ID3D12Object 3-6, ID3D12Device 7.., ID3D12Device1 44-46, ID3D12Device2 47.
constexpr int kSlotCreateGraphics = 10;
constexpr int kSlotCreateCompute = 11;
constexpr int kSlotCreateStream = 47;
constexpr int kSlotCreatePipelineLibrary = 44;  // ID3D12Device1

using CreatePipelineLibrary_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device1*, const void*, SIZE_T, REFIID, void**);
CreatePipelineLibrary_t o_library = nullptr;
std::atomic<int> g_libraryCalls{0};

// RenderDoc refuses pipeline libraries: a saved blob gets D3D12_ERROR_DRIVER_VERSION_MISMATCH with no object
// and a dry run gets DXGI_ERROR_UNSUPPORTED. FF16 then calls LoadGraphicsPipeline on a null library and
// crashes. Always hand it RenderDoc's empty library instead: every Load* reports "not found", so the game
// compiles each pipeline normally (RenderDoc needs the bytecode anyway).
HRESULT STDMETHODCALLTYPE Hook_CreatePipelineLibrary(ID3D12Device1* self, const void* blob, SIZE_T len, REFIID riid,
                                                     void** out) {
    int n = ++g_libraryCalls;
    if (!out) {
        if (n <= 20) LOG("PSO guard: CreatePipelineLibrary dry run (blob %zu bytes) -> reporting S_OK", (size_t)len);
        return S_OK;
    }
    HRESULT hr = o_library(self, nullptr, 0, riid, out);
    if (n <= 20)
        LOG("PSO guard: CreatePipelineLibrary (game blob %zu bytes ignored) -> empty library 0x%08lX %p", (size_t)len, hr,
            out ? *out : nullptr);
    return hr;
}

using CreateGraphics_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID,
                                                     void**);
using CreateCompute_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID,
                                                    void**);
using CreateStream_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID,
                                                   void**);

CreateGraphics_t o_graphics = nullptr;
CreateCompute_t o_compute = nullptr;
CreateStream_t o_stream = nullptr;

std::atomic<int> g_computeOk{0}, g_computeRetryOk{0}, g_computeDummy{0}, g_computeFail{0};
std::atomic<int> g_graphicsFail{0}, g_streamFail{0}, g_dumped{0};
std::mutex g_dumpMutex;
bool g_replace = true;
// Compute shaders created, by required wave size (from the PSV0 part): [0] none, [1] 32, [2] 64, [3] other/unknown
std::atomic<int> g_wave[4];

int WaveClass(const D3D12_SHADER_BYTECODE& bc) {
    auto b = static_cast<const uint8_t*>(bc.pShaderBytecode);
    if (!b || bc.BytecodeLength < 32 || memcmp(b, "DXBC", 4) != 0) return 3;
    uint32_t n = *reinterpret_cast<const uint32_t*>(b + 28);
    for (uint32_t i = 0; i < n && 32 + 4 * (i + 1) <= bc.BytecodeLength; ++i) {
        uint32_t o = *reinterpret_cast<const uint32_t*>(b + 32 + 4 * i);
        if (o + 8 + 4 + 24 > bc.BytecodeLength || memcmp(b + o, "PSV0", 4) != 0) continue;
        const uint8_t* p = b + o + 8;
        uint32_t rs = *reinterpret_cast<const uint32_t*>(p);
        if (rs < 24) return 3;
        uint32_t mn = *reinterpret_cast<const uint32_t*>(p + 4 + 16);
        uint32_t mx = *reinterpret_cast<const uint32_t*>(p + 4 + 20);
        if (mn == 0 && mx == 0) return 0;
        if (mn == 32 && mx == 32) return 1;
        if (mn == 64 && mx == 64) return 2;
        return 3;
    }
    return 3;
}

uint64_t Fnv1a(const void* data, size_t n) {
    uint64_t h = 1469598103934665603ull;
    auto p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// Writes a shader to <CaptureDir>\failed_pso\<kind>_<hash>.bin (first 200 distinct shaders only).
void Dump(const char* kind, const D3D12_SHADER_BYTECODE& bc, uint64_t hash) {
    if (!bc.pShaderBytecode || !bc.BytecodeLength) return;
    std::lock_guard<std::mutex> lock(g_dumpMutex);
    if (g_dumped >= 200) return;
    std::wstring dir = vr::g_config.captureDir + L"\\failed_pso";
    CreateDirectoryW(vr::g_config.captureDir.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t name[64];
    swprintf(name, 64, L"\\%hs_%016llx.bin", kind, (unsigned long long)hash);
    std::wstring path = dir + name;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    if (FILE* f = _wfopen(path.c_str(), L"wb")) {
        fwrite(bc.pShaderBytecode, 1, bc.BytecodeLength, f);
        fclose(f);
        ++g_dumped;
    }
}

// A do-nothing compute shader (DXBC cs_5_0, no resources, so it fits any root signature).
ID3DBlob* g_dummyCs = nullptr;
std::once_flag g_dummyOnce;

ID3DBlob* DummyCs() {
    std::call_once(g_dummyOnce, [] {
        HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = dc ? reinterpret_cast<pD3DCompile>(GetProcAddress(dc, "D3DCompile")) : nullptr;
        if (!compile) { LOG("PSO guard: d3dcompiler_47.dll not available; no dummy pipelines"); return; }
        static const char src[] = "[numthreads(1, 1, 1)] void main() {}";
        ID3DBlob* err = nullptr;
        HRESULT hr = compile(src, sizeof(src) - 1, "ff16vr_dummy_cs", nullptr, nullptr, "main", "cs_5_0", 0, 0, &g_dummyCs, &err);
        if (FAILED(hr)) {
            LOG("PSO guard: dummy compute shader failed to compile (0x%08lX) %s", hr,
                err ? static_cast<const char*>(err->GetBufferPointer()) : "");
            g_dummyCs = nullptr;
        }
        if (err) err->Release();
    });
    return g_dummyCs;
}

void Summary(bool force = false) {
    int total = g_computeRetryOk + g_computeDummy + g_computeFail + g_graphicsFail + g_streamFail;
    if (force || total <= 5 || total % 25 == 0)
        LOG("PSO guard totals: wave none/32/64/other=%d/%d/%d/%d; compute ok=%d retry-ok=%d dummy=%d failed=%d; graphics failed=%d; stream failed=%d; dumped=%d",
            g_wave[0].load(), g_wave[1].load(), g_wave[2].load(), g_wave[3].load(), g_computeOk.load(), g_computeRetryOk.load(), g_computeDummy.load(), g_computeFail.load(),
            g_graphicsFail.load(), g_streamFail.load(), g_dumped.load());
}

HRESULT STDMETHODCALLTYPE Hook_CreateCompute(ID3D12Device* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                             REFIID riid, void** out) {
    HRESULT hr = o_compute(self, desc, riid, out);
    if (desc && out) {
        ++g_wave[WaveClass(desc->CS)];
        if (SUCCEEDED(hr) && (++g_computeOk % 1000) == 0) Summary(true);
    }
    if (SUCCEEDED(hr) || !desc || !out) return hr;
    const uint64_t hash = Fnv1a(desc->CS.pShaderBytecode, desc->CS.BytecodeLength);
    const size_t cached = desc->CachedPSO.CachedBlobSizeInBytes;

    // 1) Retry without the game's cached pipeline blob.
    if (cached) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC d = *desc;
        d.CachedPSO = {};
        HRESULT hr2 = o_compute(self, &d, riid, out);
        if (SUCCEEDED(hr2)) {
            ++g_computeRetryOk;
            LOG("PSO guard: compute %016llx failed 0x%08lX with CachedPSO (%zu bytes); OK without it", (unsigned long long)hash,
                hr, cached);
            Summary();
            return hr2;
        }
    }

    // 2) Keep the shader for offline inspection, hand the game a do-nothing pipeline.
    Dump("cs", desc->CS, hash);
    LOG("PSO guard: compute %016llx FAILED 0x%08lX (CS %zu bytes, CachedPSO %zu bytes, flags 0x%X, node 0x%X, rootsig %p)",
        (unsigned long long)hash, hr, desc->CS.BytecodeLength, cached, (unsigned)desc->Flags, desc->NodeMask,
        (void*)desc->pRootSignature);
    if (!g_replace) { ++g_computeFail; Summary(); return hr; }
    if (ID3DBlob* cs = DummyCs()) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
        d.pRootSignature = desc->pRootSignature;
        d.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        d.NodeMask = desc->NodeMask;
        HRESULT hr3 = o_compute(self, &d, riid, out);
        if (SUCCEEDED(hr3)) {
            ++g_computeDummy;
            LOG("PSO guard: compute %016llx replaced by a do-nothing pipeline", (unsigned long long)hash);
            Summary();
            return hr3;
        }
        LOG("PSO guard: dummy pipeline for %016llx also failed 0x%08lX", (unsigned long long)hash, hr3);
    }
    ++g_computeFail;
    Summary();
    return hr;
}

void LogGraphicsShaders(const char* what, const D3D12_SHADER_BYTECODE* stages[5], uint64_t hashes[5]) {
    static const char* names[5] = {"vs", "ps", "ds", "hs", "gs"};
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "%s:", what);
    for (int i = 0; i < 5 && n < (int)sizeof(buf); ++i)
        if (stages[i] && stages[i]->BytecodeLength)
            n += snprintf(buf + n, sizeof(buf) - n, " %s=%016llx(%zu)", names[i], (unsigned long long)hashes[i],
                          stages[i]->BytecodeLength);
    LOG("%s", buf);
}

HRESULT STDMETHODCALLTYPE Hook_CreateGraphics(ID3D12Device* self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                              REFIID riid, void** out) {
    HRESULT hr = o_graphics(self, desc, riid, out);
    if (SUCCEEDED(hr) || !desc || !out) return hr;
    const size_t cached = desc->CachedPSO.CachedBlobSizeInBytes;
    if (cached) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = *desc;
        d.CachedPSO = {};
        HRESULT hr2 = o_graphics(self, &d, riid, out);
        if (SUCCEEDED(hr2)) {
            LOG("PSO guard: graphics pipeline failed 0x%08lX with CachedPSO (%zu bytes); OK without it", hr, cached);
            return hr2;
        }
    }
    ++g_graphicsFail;
    const D3D12_SHADER_BYTECODE* st[5] = {&desc->VS, &desc->PS, &desc->DS, &desc->HS, &desc->GS};
    uint64_t h[5] = {};
    static const char* kinds[5] = {"vs", "ps", "ds", "hs", "gs"};
    for (int i = 0; i < 5; ++i) {
        h[i] = Fnv1a(st[i]->pShaderBytecode, st[i]->BytecodeLength);
        Dump(kinds[i], *st[i], h[i]);
    }
    char what[96];
    snprintf(what, sizeof(what), "PSO guard: graphics pipeline FAILED 0x%08lX (CachedPSO %zu bytes)", hr, cached);
    LogGraphicsShaders(what, st, h);
    Summary();
    return hr;
}

// ---- pipeline state streams (ID3D12Device2::CreatePipelineState)
// Each subobject is { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE; Inner } aligned to sizeof(void*), as in d3dx12.h.
template <typename Inner>
struct alignas(void*) Sub {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    Inner inner;
};

size_t SubobjectSize(int type) {
    switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: return sizeof(Sub<ID3D12RootSignature*>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: return sizeof(Sub<D3D12_SHADER_BYTECODE>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: return sizeof(Sub<D3D12_STREAM_OUTPUT_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: return sizeof(Sub<D3D12_BLEND_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: return sizeof(Sub<UINT>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: return sizeof(Sub<D3D12_RASTERIZER_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: return sizeof(Sub<D3D12_DEPTH_STENCIL_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: return sizeof(Sub<D3D12_INPUT_LAYOUT_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: return sizeof(Sub<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: return sizeof(Sub<D3D12_PRIMITIVE_TOPOLOGY_TYPE>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: return sizeof(Sub<D3D12_RT_FORMAT_ARRAY>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: return sizeof(Sub<DXGI_FORMAT>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: return sizeof(Sub<DXGI_SAMPLE_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: return sizeof(Sub<UINT>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: return sizeof(Sub<D3D12_CACHED_PIPELINE_STATE>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: return sizeof(Sub<D3D12_PIPELINE_STATE_FLAGS>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: return sizeof(Sub<D3D12_DEPTH_STENCIL_DESC1>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: return sizeof(Sub<D3D12_VIEW_INSTANCING_DESC>);
        default: return 0;  // unknown / newer subobject: stop parsing
    }
}

HRESULT STDMETHODCALLTYPE Hook_CreateStream(ID3D12Device2* self, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid,
                                            void** out) {
    HRESULT hr = o_stream(self, desc, riid, out);
    if (SUCCEEDED(hr) || !desc || !out || !desc->pPipelineStateSubobjectStream) return hr;

    // Find the CACHED_PSO subobject and the shaders in a private copy of the stream.
    std::string copy(static_cast<const char*>(desc->pPipelineStateSubobjectStream), desc->SizeInBytes);
    size_t off = 0, cachedOff = SIZE_MAX, cached = 0;
    bool parsed = true;
    const D3D12_SHADER_BYTECODE* cs = nullptr;
    while (off + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= copy.size()) {
        int type = *reinterpret_cast<const int*>(copy.data() + off);
        size_t sz = SubobjectSize(type);
        if (!sz || off + sz > copy.size()) { parsed = (off == copy.size()); break; }
        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO) {
            cachedOff = off + offsetof(Sub<D3D12_CACHED_PIPELINE_STATE>, inner);
            cached = reinterpret_cast<const D3D12_CACHED_PIPELINE_STATE*>(copy.data() + cachedOff)->CachedBlobSizeInBytes;
        }
        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS)
            cs = reinterpret_cast<const D3D12_SHADER_BYTECODE*>(copy.data() + off + offsetof(Sub<D3D12_SHADER_BYTECODE>, inner));
        off += sz;
    }
    if (cached && cachedOff != SIZE_MAX) {
        *reinterpret_cast<D3D12_CACHED_PIPELINE_STATE*>(copy.data() + cachedOff) = {};
        D3D12_PIPELINE_STATE_STREAM_DESC d = {copy.size(), copy.data()};
        HRESULT hr2 = o_stream(self, &d, riid, out);
        if (SUCCEEDED(hr2)) {
            LOG("PSO guard: stream pipeline failed 0x%08lX with CachedPSO (%zu bytes); OK without it", hr, cached);
            return hr2;
        }
    }
    ++g_streamFail;
    uint64_t h = cs ? Fnv1a(cs->pShaderBytecode, cs->BytecodeLength) : 0;
    if (cs) Dump("stream_cs", *cs, h);
    LOG("PSO guard: stream pipeline FAILED 0x%08lX (%zu bytes, parsed=%d, CachedPSO %zu bytes, CS %016llx)", hr,
        (size_t)desc->SizeInBytes, parsed ? 1 : 0, cached, (unsigned long long)h);
    Summary();
    return hr;
}

void Patch(void** vt, int slot, void* hook, void** orig) {
    if (vt[slot] == hook) return;
    DWORD old;
    VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    *orig = vt[slot];
    vt[slot] = hook;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
}

}  // namespace

void Install(ID3D12Device* device, bool replace) {
    if (!device || o_compute) return;
    g_replace = replace;
    void** vt = *reinterpret_cast<void***>(device);
    Patch(vt, kSlotCreateGraphics, reinterpret_cast<void*>(&Hook_CreateGraphics), reinterpret_cast<void**>(&o_graphics));
    Patch(vt, kSlotCreateCompute, reinterpret_cast<void*>(&Hook_CreateCompute), reinterpret_cast<void**>(&o_compute));
    if (replace) {
        ID3D12Device1* d1 = nullptr;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d1))) && d1) {
            Patch(*reinterpret_cast<void***>(d1), kSlotCreatePipelineLibrary, reinterpret_cast<void*>(&Hook_CreatePipelineLibrary),
                  reinterpret_cast<void**>(&o_library));
            d1->Release();
            LOG("PSO guard: pipeline libraries redirected to RenderDoc's empty library");
        }
    }
    ID3D12Device2* d2 = nullptr;
    bool stream = false;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d2))) && d2) {
        void** vt2 = *reinterpret_cast<void***>(d2);
        Patch(vt2, kSlotCreateStream, reinterpret_cast<void*>(&Hook_CreateStream), reinterpret_cast<void**>(&o_stream));
        stream = true;
        d2->Release();
    }
    LOG("PSO guard installed on device %p (compute + graphics%s, %s): failed pipelines are retried without CachedPSO, "
        "failed shaders are dumped to %ls\\failed_pso%s",
        (void*)device, stream ? " + stream" : "", replace ? "replace mode" : "log-only mode", vr::g_config.captureDir.c_str(),
        replace ? ", failed compute pipelines are replaced by a do-nothing pipeline" : "");
}

}  // namespace pso_guard
