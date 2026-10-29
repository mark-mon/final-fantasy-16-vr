#include "vrkit_menu_d3d12.h"
#include "vrkit_options.h"
#include <windows.h>
#include "imgui.h"
#include "imgui_impl_dx12.h"

namespace vrkit { namespace menu12 {

namespace {
ID3D12Device* g_dev = nullptr;
ID3D12Resource* g_tex = nullptr;
ID3D12DescriptorHeap* g_rtvHeap = nullptr;
ID3D12DescriptorHeap* g_srvHeap = nullptr;   // shader visible: the ImGui font texture
bool g_ok = false;
bool g_built = false;   // a frame was built by Update() and not drawn yet
}  // namespace

bool Init(ID3D12Device* dev, int framesInFlight, const menu::Settings& s) {
    if (g_ok) return true;
    g_dev = dev;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = s.texW;
    d.Height = s.texH;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {};
    cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_SOURCE, &cv,
                                            IID_PPV_ARGS(&g_tex)))) {
        Log("vrkit: menu texture failed");
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC rh = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC sh = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_rtvHeap))) ||
        FAILED(dev->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g_srvHeap)))) {
        Log("vrkit: menu descriptor heaps failed");
        return false;
    }
    D3D12_RENDER_TARGET_VIEW_DESC rv = {};
    rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    dev->CreateRenderTargetView(g_tex, &rv, g_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    if (!menu::core::InitImGui(s)) return false;
    if (!ImGui_ImplDX12_Init(dev, framesInFlight, DXGI_FORMAT_R8G8B8A8_UNORM, g_srvHeap,
                             g_srvHeap->GetCPUDescriptorHandleForHeapStart(),
                             g_srvHeap->GetGPUDescriptorHandleForHeapStart())) {
        Log("vrkit: ImGui D3D12 init failed");
        return false;
    }
    g_ok = true;
    Log("vrkit: menu ready (%zu options on %zu pages, D3D12)", Options::Get().All().size(),
        Options::Get().Pages().size());
    return true;
}

void Shutdown() {
    if (g_ok) ImGui_ImplDX12_Shutdown();
    menu::core::ShutdownImGui();
    if (g_tex) { g_tex->Release(); g_tex = nullptr; }
    if (g_rtvHeap) { g_rtvHeap->Release(); g_rtvHeap = nullptr; }
    if (g_srvHeap) { g_srvHeap->Release(); g_srvHeap = nullptr; }
    g_ok = false;
}

bool Update() {
    g_built = false;
    if (!g_ok || !menu::core::UpdateInput()) return false;
    ImGui_ImplDX12_NewFrame();
    menu::core::BuildFrame();
    g_built = true;
    return true;
}

void Record(ID3D12GraphicsCommandList* cl) {
    if (!g_ok || !g_built) return;
    g_built = false;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cl->ResourceBarrier(1, &b);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const float clear[4] = {0, 0, 0, 0};
    cl->ClearRenderTargetView(rtv, clear, 0, nullptr);
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const menu::Settings& s = menu::core::Get();
    D3D12_VIEWPORT vp = {0, 0, (float)s.texW, (float)s.texH, 0, 1};
    D3D12_RECT sc = {0, 0, (LONG)s.texW, (LONG)s.texH};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    cl->SetDescriptorHeaps(1, &g_srvHeap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);
}

bool IsOpen() { return menu::core::IsOpen(); }
void SetOpen(bool open) { menu::core::SetOpen(open); }
ID3D12Resource* Texture() { return g_tex; }
UINT Width() { return menu::core::Get().texW; }
UINT Height() { return menu::core::Get().texH; }

} } // namespace vrkit::menu12
