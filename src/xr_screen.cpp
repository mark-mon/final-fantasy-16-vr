// Step 1: show the game's flat output on a head-locked-at-recenter virtual
// screen (OpenXR quad layer) in a SteamVR headset.
//
// Everything runs on the game's render thread inside the Present hook:
//   xrWaitFrame -> xrBeginFrame -> copy backbuffer -> blit into XR image
//   -> xrEndFrame(quad layer) -> native Present.
// Our GPU work is submitted to the game's own direct queue, which is also the
// queue bound to the OpenXR session.

#include "xr_screen.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <algorithm>
#include <vector>

#include "config.h"
#include "log.h"
#include "vr_shared.h"
#include "stereo_render.h"
#include "menu_options.h"
#include "hud_layer.h"
#include "vrkit_menu_d3d12.h"
#include "vrkit_menu_placement.h"

namespace xr {
namespace {

// ---------------------------------------------------------------------------
// OpenXR function table
// ---------------------------------------------------------------------------
#define XR_INSTANCE_FUNCS(X)              \
    X(xrDestroyInstance)                  \
    X(xrGetInstanceProperties)            \
    X(xrResultToString)                   \
    X(xrGetSystem)                        \
    X(xrGetSystemProperties)              \
    X(xrCreateSession)                    \
    X(xrDestroySession)                   \
    X(xrPollEvent)                        \
    X(xrBeginSession)                     \
    X(xrEndSession)                       \
    X(xrWaitFrame)                        \
    X(xrBeginFrame)                       \
    X(xrEndFrame)                         \
    X(xrCreateReferenceSpace)             \
    X(xrDestroySpace)                     \
    X(xrLocateSpace)                      \
    X(xrLocateViews)                      \
    X(xrEnumerateSwapchainFormats)        \
    X(xrCreateSwapchain)                  \
    X(xrDestroySwapchain)                 \
    X(xrEnumerateSwapchainImages)         \
    X(xrAcquireSwapchainImage)            \
    X(xrWaitSwapchainImage)               \
    X(xrReleaseSwapchainImage)            \
    X(xrEnumerateViewConfigurationViews)  \
    X(xrStringToPath)                     \
    X(xrCreateActionSet)                  \
    X(xrDestroyActionSet)                 \
    X(xrCreateAction)                     \
    X(xrSuggestInteractionProfileBindings) \
    X(xrAttachSessionActionSets)          \
    X(xrCreateActionSpace)                \
    X(xrSyncActions)                      \
    X(xrGetActionStatePose)               \
    X(xrGetD3D12GraphicsRequirementsKHR)

PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;
PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties = nullptr;
PFN_xrCreateInstance xrCreateInstance = nullptr;
#define XR_DECLARE(name) PFN_##name name = nullptr;
XR_INSTANCE_FUNCS(XR_DECLARE)
#undef XR_DECLARE

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
std::recursive_mutex g_mutex;
bool g_disabled = false;
ULONGLONG g_nextRetry = 0;

// Game objects
IDXGISwapChain* g_gameSwap = nullptr;  // weak; compared by pointer only
ID3D12CommandQueue* g_queue = nullptr;  // owned
ID3D12Device* g_device = nullptr;       // owned

// OpenXR
HMODULE g_loader = nullptr;
XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
volatile long g_xrSwapCreates = 0;  // stereo / mono swapchain (re)creations, for the Perf log
uint32_t g_recW = 0, g_recH = 0;  // runtime's recommended per-eye image size
float g_eyeTanHeadset[2][4] = {};  // per eye: tan up, down, left, right (latest xrLocateViews)
volatile bool g_haveEyeTan = false;
XrSession g_session = XR_NULL_HANDLE;
XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
bool g_running = false;
XrSpace g_localSpace = XR_NULL_HANDLE;
XrSpace g_viewSpace = XR_NULL_HANDLE;
// Eye tracking (XR_EXT_eye_gaze_interaction) for foveated rendering
bool g_hasGazeExt = false;
XrActionSet g_gazeSet = XR_NULL_HANDLE;
XrAction g_gazeAction = XR_NULL_HANDLE;
XrSpace g_gazeSpace = XR_NULL_HANDLE;
uint32_t g_maxW = 4096, g_maxH = 4096;

XrSwapchain g_xrSwap = XR_NULL_HANDLE;
std::vector<XrSwapchainImageD3D12KHR> g_xrImages;
DXGI_FORMAT g_xrFormat = DXGI_FORMAT_UNKNOWN;
uint32_t g_xrW = 0, g_xrH = 0;
uint32_t g_xrLayers = 1;  // 2 = native stereo (array slice per eye)

// Virtual screen placement
XrPosef g_screenPose = {{0, 0, 0, 1}, {0, 0, -2.5f}};
bool g_needRecenter = true;
bool g_keyWasDown = false;
// VRMode 1: head pose at recenter time (yaw only + position), the origin of the game camera's head offset.
float g_recYawQ[4] = {0, 0, 0, 1};
float g_recPos[3] = {0, 0, 0};

// D3D12 blit resources
constexpr int kFrames = 3;
ID3D12RootSignature* g_rootSig = nullptr;
ID3DBlob* g_vs = nullptr;
ID3DBlob* g_ps = nullptr;
ID3D12PipelineState* g_pso = nullptr;
DXGI_FORMAT g_psoFormat = DXGI_FORMAT_UNKNOWN;
ID3D12DescriptorHeap* g_srvHeap = nullptr;
ID3D12DescriptorHeap* g_rtvHeap = nullptr;
UINT g_rtvInc = 0;
ID3D12CommandAllocator* g_alloc[kFrames] = {};
UINT64 g_allocFence[kFrames] = {};
int g_frameIdx = 0;
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12Fence* g_fence = nullptr;
UINT64 g_fenceValue = 0;
HANDLE g_fenceEvent = nullptr;

ID3D12Resource* g_inter = nullptr;  // copy of the game's backbuffer
DXGI_FORMAT g_interFmt = DXGI_FORMAT_UNKNOWN;
UINT64 g_interW = 0;
UINT g_interH = 0;

uint64_t g_frameCount = 0;

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
const char* ResultStr(XrResult r) {
    static thread_local char buf[XR_MAX_RESULT_STRING_SIZE];
    if (xrResultToString && g_instance && XR_SUCCEEDED(xrResultToString(g_instance, r, buf))) return buf;
    snprintf(buf, sizeof(buf), "XrResult(%d)", (int)r);
    return buf;
}

bool Check(XrResult r, const char* what) {
    if (XR_FAILED(r)) {
        LOG("OpenXR: %s failed: %s", what, ResultStr(r));
        return false;
    }
    return true;
}

const char* StateName(XrSessionState s) {
    switch (s) {
        case XR_SESSION_STATE_IDLE: return "IDLE";
        case XR_SESSION_STATE_READY: return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
        case XR_SESSION_STATE_STOPPING: return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING: return "EXITING";
        default: return "UNKNOWN";
    }
}

bool IsLinearFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            return true;
        default:
            return false;
    }
}

DXGI_FORMAT TypedFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return f;
    }
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

void WaitGpuIdle() {
    if (!g_fence || !g_fenceEvent) return;
    if (g_fence->GetCompletedValue() < g_fenceValue) {
        g_fence->SetEventOnCompletion(g_fenceValue, g_fenceEvent);
        if (WaitForSingleObject(g_fenceEvent, 2000) != WAIT_OBJECT_0) LOG("WARNING: GPU idle wait timed out");
    }
}

// Rotate v by unit quaternion q.
XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v) {
    XrVector3f u{q.x, q.y, q.z};
    auto cross = [](const XrVector3f& a, const XrVector3f& b) {
        return XrVector3f{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    };
    XrVector3f t = cross(u, v);
    t = {2 * t.x, 2 * t.y, 2 * t.z};
    XrVector3f c = cross(u, t);
    return {v.x + q.w * t.x + c.x, v.y + q.w * t.y + c.y, v.z + q.w * t.z + c.z};
}

// ---------------------------------------------------------------------------
// OpenXR lifetime
// ---------------------------------------------------------------------------
bool LoadLoader() {
    std::wstring path = vr::g_gameDir + L"\\openxr_loader.dll";
    g_loader = LoadLibraryW(path.c_str());
    if (!g_loader) g_loader = LoadLibraryW(L"openxr_loader.dll");
    if (!g_loader) {
        LOG("ERROR: openxr_loader.dll not found next to ffxvi.exe (err %lu)", GetLastError());
        return false;
    }
    xrGetInstanceProcAddr =
        reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(g_loader, "xrGetInstanceProcAddr"));
    if (!xrGetInstanceProcAddr) {
        LOG("ERROR: xrGetInstanceProcAddr missing from openxr_loader.dll");
        return false;
    }
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                          reinterpret_cast<PFN_xrVoidFunction*>(&xrEnumerateInstanceExtensionProperties));
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateInstance));
    return xrEnumerateInstanceExtensionProperties && xrCreateInstance;
}

bool CreateInstance() {
    uint32_t count = 0;
    XrResult r = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    if (XR_FAILED(r)) {
        LOG("OpenXR: no runtime available yet (is SteamVR installed and set as the OpenXR runtime?) result %d", (int)r);
        return false;
    }
    std::vector<XrExtensionProperties> exts(count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, exts.data());
    bool hasD3D12 = false;
    for (auto& e : exts)
        if (strcmp(e.extensionName, XR_KHR_D3D12_ENABLE_EXTENSION_NAME) == 0) hasD3D12 = true;
    if (!hasD3D12) {
        LOG("ERROR: OpenXR runtime does not support %s", XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
        g_disabled = true;
        return false;
    }

    for (auto& e : exts)
        if (strcmp(e.extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0) g_hasGazeExt = true;
    LOG("OpenXR: eye gaze extension %s", g_hasGazeExt ? "available (enabled for foveated rendering)" : "not offered by the runtime");
    const char* enabled[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ci.applicationInfo.applicationName, "FF16VR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy_s(ci.applicationInfo.engineName, "FF16VR");
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = g_hasGazeExt ? 2 : 1;
    ci.enabledExtensionNames = enabled;
    r = xrCreateInstance(&ci, &g_instance);
    if (XR_FAILED(r)) {
        LOG("OpenXR: xrCreateInstance failed (%d); will retry", (int)r);
        g_instance = XR_NULL_HANDLE;
        return false;
    }

#define XR_LOAD(name) \
    xrGetInstanceProcAddr(g_instance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&name));
    XR_INSTANCE_FUNCS(XR_LOAD)
#undef XR_LOAD
#define XR_VERIFY(name)                                    \
    if (!name) {                                           \
        LOG("ERROR: OpenXR function %s missing", #name);   \
        g_disabled = true;                                 \
        return false;                                      \
    }
    XR_INSTANCE_FUNCS(XR_VERIFY)
#undef XR_VERIFY

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(g_instance, &props)))
        LOG("OpenXR runtime: %s %u.%u.%u", props.runtimeName, XR_VERSION_MAJOR(props.runtimeVersion),
            XR_VERSION_MINOR(props.runtimeVersion), XR_VERSION_PATCH(props.runtimeVersion));
    return true;
}

bool GetSystem() {
    XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = xrGetSystem(g_instance, &gi, &g_system);
    if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        LOG("OpenXR: headset not available yet; will retry");
        g_system = XR_NULL_SYSTEM_ID;
        return false;
    }
    if (!Check(r, "xrGetSystem")) {
        g_system = XR_NULL_SYSTEM_ID;
        return false;
    }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(g_instance, g_system, &sp))) {
        g_maxW = sp.graphicsProperties.maxSwapchainImageWidth;
        g_maxH = sp.graphicsProperties.maxSwapchainImageHeight;
        LOG("OpenXR system: %s (max swapchain %u x %u)", sp.systemName, g_maxW, g_maxH);
    }
    XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t nv = 0;
    if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                                       &nv, vcv)) && nv >= 1) {
        g_recW = vcv[0].recommendedImageRectWidth;
        g_recH = vcv[0].recommendedImageRectHeight;
        LOG("OpenXR recommended eye image: %u x %u (max %u x %u)", g_recW, g_recH, vcv[0].maxImageRectWidth,
            vcv[0].maxImageRectHeight);
    }
    return true;
}

void DestroyMenu();
void DestroyHudSwapchain();
void CloseNativeFrameEmpty();

void DestroySwapchain() {
    WaitGpuIdle();
    if (g_xrSwap) xrDestroySwapchain(g_xrSwap);
    g_xrSwap = XR_NULL_HANDLE;
    g_xrImages.clear();
    g_xrW = g_xrH = 0;
    SafeRelease(g_rtvHeap);
}

// Eye gaze action (only when the runtime and the headset support it). Failure just leaves foveation fixed.
void SetupGaze() {
    if (!g_hasGazeExt || !xrCreateActionSet || !xrSyncActions) return;
    XrSystemEyeGazeInteractionPropertiesEXT eg{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    sp.next = &eg;
    if (XR_FAILED(xrGetSystemProperties(g_instance, g_system, &sp)) || !eg.supportsEyeGazeInteraction) {
        LOG("OpenXR: this headset reports no eye tracking; foveated rendering stays fixed");
        return;
    }
    XrActionSetCreateInfo asi{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(asi.actionSetName, "ff16vr_gaze");
    strcpy_s(asi.localizedActionSetName, "FF16VR eye gaze");
    if (XR_FAILED(xrCreateActionSet(g_instance, &asi, &g_gazeSet))) return;
    XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
    aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strcpy_s(aci.actionName, "gaze");
    strcpy_s(aci.localizedActionName, "Eye gaze");
    XrPath profile = XR_NULL_PATH, gazePath = XR_NULL_PATH;
    bool ok = XR_SUCCEEDED(xrCreateAction(g_gazeSet, &aci, &g_gazeAction)) &&
              XR_SUCCEEDED(xrStringToPath(g_instance, "/interaction_profiles/ext/eye_gaze_interaction", &profile)) &&
              XR_SUCCEEDED(xrStringToPath(g_instance, "/user/eyes_ext/input/gaze_ext/pose", &gazePath));
    if (ok) {
        XrActionSuggestedBinding b{g_gazeAction, gazePath};
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = profile;
        sb.countSuggestedBindings = 1;
        sb.suggestedBindings = &b;
        ok = Check(xrSuggestInteractionProfileBindings(g_instance, &sb), "xrSuggestInteractionProfileBindings(eye gaze)");
    }
    if (ok) {
        XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        ai.countActionSets = 1;
        ai.actionSets = &g_gazeSet;
        ok = Check(xrAttachSessionActionSets(g_session, &ai), "xrAttachSessionActionSets(eye gaze)");
    }
    if (ok) {
        XrActionSpaceCreateInfo si{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        si.action = g_gazeAction;
        si.poseInActionSpace = {{0, 0, 0, 1}, {0, 0, 0}};
        ok = Check(xrCreateActionSpace(g_session, &si, &g_gazeSpace), "xrCreateActionSpace(eye gaze)");
    }
    if (!ok) {
        if (g_gazeSet) xrDestroyActionSet(g_gazeSet);
        g_gazeSet = XR_NULL_HANDLE;
        g_gazeAction = XR_NULL_HANDLE;
        g_gazeSpace = XR_NULL_HANDLE;
        return;
    }
    LOG("OpenXR: eye tracking available; foveated rendering can follow the gaze (FoveatedGaze)");
}

// Gaze direction in head (VIEW) space at `time`; false when not tracked.
bool LocateGaze(XrTime time, float dir[3]) {
    if (!g_gazeSpace || !vr::g_config.foveatedGaze) return false;
    XrActiveActionSet aas{g_gazeSet, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &aas;
    if (XR_FAILED(xrSyncActions(g_session, &si))) return false;
    XrActionStatePose ps{XR_TYPE_ACTION_STATE_POSE};
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = g_gazeAction;
    if (XR_FAILED(xrGetActionStatePose(g_session, &gi, &ps)) || !ps.isActive) return false;
    XrSpaceLocation gl{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(g_gazeSpace, g_viewSpace, time, &gl))) return false;
    if (!(gl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return false;
    const XrVector3f f = Rotate(gl.pose.orientation, {0, 0, -1});
    dir[0] = f.x;
    dir[1] = f.y;
    dir[2] = f.z;
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        LOG("OpenXR: first eye gaze sample (%.2f, %.2f, %.2f)", f.x, f.y, f.z);
    }
    return true;
}

void DestroySession() {
    DestroySwapchain();
    DestroyMenu();
    DestroyHudSwapchain();
    if (g_gazeSpace) xrDestroySpace(g_gazeSpace);
    if (g_gazeSet) xrDestroyActionSet(g_gazeSet);
    g_gazeSpace = XR_NULL_HANDLE;
    g_gazeSet = XR_NULL_HANDLE;
    g_gazeAction = XR_NULL_HANDLE;
    if (g_localSpace) xrDestroySpace(g_localSpace);
    if (g_viewSpace) xrDestroySpace(g_viewSpace);
    g_localSpace = g_viewSpace = XR_NULL_HANDLE;
    if (g_session) {
        xrDestroySession(g_session);
        LOG("OpenXR session destroyed");
    }
    g_session = XR_NULL_HANDLE;
    g_running = false;
    g_state = XR_SESSION_STATE_UNKNOWN;
}

void DestroyInstance() {
    DestroySession();
    if (g_instance) xrDestroyInstance(g_instance);
    g_instance = XR_NULL_HANDLE;
    g_system = XR_NULL_SYSTEM_ID;
}

bool CreateSession() {
    XrGraphicsRequirementsD3D12KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    if (!Check(xrGetD3D12GraphicsRequirementsKHR(g_instance, g_system, &req), "xrGetD3D12GraphicsRequirementsKHR"))
        return false;
    LUID luid = g_device->GetAdapterLuid();
    if (memcmp(&luid, &req.adapterLuid, sizeof(LUID)) != 0) {
        // Real headsets: usually means the game runs on another GPU (laptops / multi-GPU) and session
        // creation will fail. SteamVR's null driver reports some other adapter, so only warn and try.
        LOG("WARNING: OpenXR wants adapter LUID %08lX:%08lX, the game uses %08lX:%08lX. "
            "If session creation fails, set the game to use the headset's GPU.",
            req.adapterLuid.HighPart, req.adapterLuid.LowPart, luid.HighPart, luid.LowPart);
    }

    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = g_device;
    binding.queue = g_queue;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = g_system;
    if (!Check(xrCreateSession(g_instance, &sci, &g_session), "xrCreateSession")) {
        g_session = XR_NULL_HANDLE;
        return false;
    }

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!Check(xrCreateReferenceSpace(g_session, &rs, &g_localSpace), "xrCreateReferenceSpace(LOCAL)")) {
        DestroySession();
        return false;
    }
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!Check(xrCreateReferenceSpace(g_session, &rs, &g_viewSpace), "xrCreateReferenceSpace(VIEW)")) {
        DestroySession();
        return false;
    }
    SetupGaze();
    LOG("OpenXR session created");
    return true;
}

void PollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (g_instance && xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                g_state = e->state;
                LOG("OpenXR session state -> %s", StateName(g_state));
                if (g_state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (Check(xrBeginSession(g_session, &bi), "xrBeginSession")) {
                        g_running = true;
                        g_needRecenter = true;
                    }
                } else if (g_state == XR_SESSION_STATE_STOPPING) {
                    Check(xrEndSession(g_session), "xrEndSession");
                    g_running = false;
                } else if (g_state == XR_SESSION_STATE_EXITING || g_state == XR_SESSION_STATE_LOSS_PENDING) {
                    DestroySession();
                    g_nextRetry = GetTickCount64() + 3000;
                    return;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                LOG("OpenXR instance loss pending (SteamVR closed?)");
                DestroyInstance();
                g_nextRetry = GetTickCount64() + 5000;
                return;
            default:
                break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// ---------------------------------------------------------------------------
// D3D12 resources
// ---------------------------------------------------------------------------
const char* kBlitHlsl = R"(
Texture2D src : register(t0);
SamplerState samp : register(s0);
cbuffer Params : register(b0) { uint mode; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float3 SrgbToLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 LinearToSrgb(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
float4 PSMain(VSOut i) : SV_Target {
    float3 c = saturate(src.SampleLevel(samp, i.uv, 0).rgb);
    if (mode == 1) c = SrgbToLinear(c);
    if (mode == 2) c = LinearToSrgb(c);
    return float4(c, 1);
}
)";

bool CompileShaders() {
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!dc) {
        LOG("ERROR: d3dcompiler_47.dll not found");
        return false;
    }
    auto compile = reinterpret_cast<pD3DCompile>(GetProcAddress(dc, "D3DCompile"));
    if (!compile) return false;
    ID3DBlob* err = nullptr;
    auto build = [&](const char* entry, const char* target, ID3DBlob** out) {
        HRESULT hr = compile(kBlitHlsl, strlen(kBlitHlsl), "ff16vr_blit", nullptr, nullptr, entry, target,
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
        if (FAILED(hr)) {
            LOG("ERROR: shader %s failed: %s", entry, err ? (const char*)err->GetBufferPointer() : "?");
            SafeRelease(err);
            return false;
        }
        SafeRelease(err);
        return true;
    };
    return build("VSMain", "vs_5_0", &g_vs) && build("PSMain", "ps_5_0", &g_ps);
}

bool EnsureGpuResources() {
    if (g_list) return true;
    HRESULT hr;

    if (!CompileShaders()) return false;

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 1;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC ss{};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    ss.MaxLOD = D3D12_FLOAT32_MAX;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd{2, params, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) {
        LOG("ERROR: root signature: %s", err ? (const char*)err->GetBufferPointer() : "?");
        SafeRelease(err);
        return false;
    }
    hr = g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_rootSig));
    SafeRelease(blob);
    if (FAILED(hr)) return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srvHeap)))) return false;
    g_rtvInc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (int i = 0; i < kFrames; ++i)
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[i]))))
            return false;
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr,
                                           IID_PPV_ARGS(&g_list))))
        return false;
    g_list->Close();
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    LOG("D3D12 blit resources created");
    return true;
}

bool EnsurePso(DXGI_FORMAT fmt) {
    if (g_pso && g_psoFormat == fmt) return true;
    SafeRelease(g_pso);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_rootSig;
    pd.VS = {g_vs->GetBufferPointer(), g_vs->GetBufferSize()};
    pd.PS = {g_ps->GetBufferPointer(), g_ps->GetBufferSize()};
    auto& rt = pd.BlendState.RenderTarget[0];
    rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    D3D12_DEPTH_STENCILOP_DESC so{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                  D3D12_COMPARISON_FUNC_ALWAYS};
    pd.DepthStencilState.FrontFace = pd.DepthStencilState.BackFace = so;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = fmt;
    pd.SampleDesc.Count = 1;
    HRESULT hr = g_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_pso));
    if (FAILED(hr)) {
        LOG("ERROR: CreateGraphicsPipelineState(fmt %d) failed 0x%08lX", (int)fmt, hr);
        return false;
    }
    g_psoFormat = fmt;
    return true;
}

bool EnsureIntermediate(const D3D12_RESOURCE_DESC& bb) {
    if (g_inter && g_interFmt == bb.Format && g_interW == bb.Width && g_interH == bb.Height) return true;
    WaitGpuIdle();
    SafeRelease(g_inter);

    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = bb.Width;
    rd.Height = bb.Height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = bb.Format;
    rd.SampleDesc.Count = 1;
    HRESULT hr = g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                   IID_PPV_ARGS(&g_inter));
    if (FAILED(hr)) {
        LOG("ERROR: intermediate texture creation failed 0x%08lX", hr);
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = TypedFormat(bb.Format);
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(g_inter, &sd, g_srvHeap->GetCPUDescriptorHandleForHeapStart());
    g_interFmt = bb.Format;
    g_interW = bb.Width;
    g_interH = bb.Height;
    LOG("Game backbuffer: %llu x %u, format %d", bb.Width, bb.Height, (int)bb.Format);
    if (bb.Format == DXGI_FORMAT_R10G10B10A2_UNORM)
        LOG("Note: 10-bit backbuffer. If colours look washed out, turn HDR off in the game.");
    return true;
}

DXGI_FORMAT ChooseXrFormat() {
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &n, nullptr);
    std::vector<int64_t> fmts(n);
    xrEnumerateSwapchainFormats(g_session, n, &n, fmts.data());
    const DXGI_FORMAT prefs[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                 DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM,
                                 DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM};
    for (DXGI_FORMAT p : prefs)
        for (int64_t f : fmts)
            if (f == p) return p;
    return n ? (DXGI_FORMAT)fmts[0] : DXGI_FORMAT_UNKNOWN;
}

// layers = 2 for native stereo: one array slice per eye (each eye can use the full max swapchain width; a
// double-wide image limited each eye to half of it).
bool EnsureXrSwapchain(uint32_t w, uint32_t h, uint32_t layers = 1) {
    w = (std::min)(w, g_maxW);
    h = (std::min)(h, g_maxH);
    if (g_xrSwap && g_xrW == w && g_xrH == h && g_xrLayers == layers) return true;
    CloseNativeFrameEmpty();
    DestroySwapchain();

    if (g_xrFormat == DXGI_FORMAT_UNKNOWN) {
        g_xrFormat = ChooseXrFormat();
        LOG("OpenXR swapchain format: %d", (int)g_xrFormat);
        if (g_xrFormat == DXGI_FORMAT_UNKNOWN) return false;
    }
    if (!EnsurePso(g_xrFormat)) return false;

    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = g_xrFormat;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = layers;
    ci.mipCount = 1;
    if (!Check(xrCreateSwapchain(g_session, &ci, &g_xrSwap), "xrCreateSwapchain")) {
        g_xrSwap = XR_NULL_HANDLE;
        return false;
    }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(g_xrSwap, 0, &n, nullptr);
    g_xrImages.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    xrEnumerateSwapchainImages(g_xrSwap, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(g_xrImages.data()));

    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, n * layers, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap)))) return false;
    D3D12_CPU_DESCRIPTOR_HANDLE h0 = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < n; ++i) {
        for (uint32_t l = 0; l < layers; ++l) {
            D3D12_RENDER_TARGET_VIEW_DESC rv{};
            rv.Format = g_xrFormat;  // runtimes may create typeless images, so be explicit
            if (layers > 1) {
                rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rv.Texture2DArray.FirstArraySlice = l;
                rv.Texture2DArray.ArraySize = 1;
            } else {
                rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE hnd{h0.ptr + (SIZE_T)(i * layers + l) * g_rtvInc};
            g_device->CreateRenderTargetView(g_xrImages[i].texture, &rv, hnd);
        }
    }
    g_xrW = w;
    g_xrH = h;
    g_xrLayers = layers;
    ++g_xrSwapCreates;
    LOG("OpenXR swapchain created: %u x %u x %u layer(s), %u images", w, h, layers, n);
    return true;
}

void ReleaseGpuResources() {
    WaitGpuIdle();
    DestroySwapchain();
    SafeRelease(g_inter);
    SafeRelease(g_pso);
    SafeRelease(g_list);
    for (auto& a : g_alloc) SafeRelease(a);
    SafeRelease(g_fence);
    SafeRelease(g_srvHeap);
    SafeRelease(g_rootSig);
    SafeRelease(g_vs);
    SafeRelease(g_ps);
    if (g_fenceEvent) CloseHandle(g_fenceEvent);
    g_fenceEvent = nullptr;
    g_fenceValue = 0;
    for (auto& v : g_allocFence) v = 0;
    g_psoFormat = g_interFmt = DXGI_FORMAT_UNKNOWN;
}

// ---------------------------------------------------------------------------
// Per-frame work
// ---------------------------------------------------------------------------
void RecordAndSubmit(ID3D12Resource* backbuffer, uint32_t imageIndex, uint32_t layer = 0) {
    const uint32_t x0 = 0, w = g_xrW;
    int f = g_frameIdx;
    g_frameIdx = (g_frameIdx + 1) % kFrames;
    if (g_fence->GetCompletedValue() < g_allocFence[f]) {
        g_fence->SetEventOnCompletion(g_allocFence[f], g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, 1000);
    }
    g_alloc[f]->Reset();
    g_list->Reset(g_alloc[f], g_pso);

    D3D12_RESOURCE_BARRIER pre[2] = {
        Transition(backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(g_inter, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
    g_list->ResourceBarrier(2, pre);
    g_list->CopyResource(g_inter, backbuffer);
    D3D12_RESOURCE_BARRIER post[2] = {
        Transition(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT),
        Transition(g_inter, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
    g_list->ResourceBarrier(2, post);

    // OpenXR D3D12 colour images are in RENDER_TARGET state while acquired.
    UINT mode = 0;
    bool srcLinear = IsLinearFormat(TypedFormat(g_interFmt));
    bool dstLinear = IsLinearFormat(g_xrFormat);
    if (!srcLinear && dstLinear) mode = 1;
    if (srcLinear && !dstLinear) mode = 2;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv{g_rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
                                    (SIZE_T)(imageIndex * g_xrLayers + (std::min)(layer, g_xrLayers - 1)) * g_rtvInc};
    g_list->SetGraphicsRootSignature(g_rootSig);
    g_list->SetDescriptorHeaps(1, &g_srvHeap);
    g_list->SetGraphicsRootDescriptorTable(0, g_srvHeap->GetGPUDescriptorHandleForHeapStart());
    g_list->SetGraphicsRoot32BitConstant(1, mode, 0);
    g_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{(float)x0, 0, (float)w, (float)g_xrH, 0, 1};
    D3D12_RECT sc{(LONG)x0, 0, (LONG)(x0 + w), (LONG)g_xrH};
    g_list->RSSetViewports(1, &vp);
    g_list->RSSetScissorRects(1, &sc);
    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_list->DrawInstanced(3, 1, 0, 0);
    g_list->Close();

    ID3D12CommandList* lists[] = {g_list};
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_fence, ++g_fenceValue);
    g_allocFence[f] = g_fenceValue;
}

constexpr ULONGLONG kTrackedAfterMs = 1000;  // live gameplay camera this long before leaving the theater view
// While paging through menus (a short live stretch ended within the last 8 s) the bar is higher: page changes on the
// 5090 show the gameplay camera for 0.8-0.9 s, sometimes 1.25-1.7 s (user log, Oct 28).
constexpr ULONGLONG kTrackedAfterPagingMs = 2200, kPagingWindowMs = 8000;
XrTime g_frameTime = 0;  // predicted display time of the OpenXR frame being built
vrkit::menu::QuadPlacer g_menuPlacer;

void UpdateRecenter(XrTime time) {
    g_frameTime = time;
    int key = vr::g_config.recenterKey;
    bool down = key > 0 && (GetAsyncKeyState(key) & 0x8000) != 0;
    if (down && !g_keyWasDown) g_needRecenter = true;
    g_keyWasDown = down;
    if (!g_needRecenter) return;

    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(g_viewSpace, g_localSpace, time, &loc))) return;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((loc.locationFlags & need) != need) return;

    XrVector3f fwd = Rotate(loc.pose.orientation, {0, 0, -1});
    float yaw = atan2f(-fwd.x, -fwd.z);
    float d = vr::g_config.screenDistance;
    const XrVector3f& p = loc.pose.position;
    g_screenPose.position = {p.x - d * sinf(yaw), p.y + vr::g_config.screenVerticalOffset, p.z - d * cosf(yaw)};
    g_screenPose.orientation = {0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
    g_recYawQ[0] = 0; g_recYawQ[1] = sinf(yaw * 0.5f); g_recYawQ[2] = 0; g_recYawQ[3] = cosf(yaw * 0.5f);
    g_recPos[0] = p.x; g_recPos[1] = p.y; g_recPos[2] = p.z;
    g_needRecenter = false;
    LOG("Screen recentered: pos (%.2f, %.2f, %.2f) yaw %.1f deg", g_screenPose.position.x, g_screenPose.position.y,
        g_screenPose.position.z, yaw * 57.2958f);
}

// VRMode 1: head pose + both eyes' field of view for the game camera (published every frame).
void PublishHeadSample(XrTime time) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(g_viewSpace, g_localSpace, time, &loc))) return;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((loc.locationFlags & need) != need) return;

    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = time;
    vli.space = g_viewSpace;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t n = 0;
    vrs::HeadSample h;
    h.tanUp = h.tanDown = h.tanLeft = h.tanRight = 0;
    if (XR_SUCCEEDED(xrLocateViews(g_session, &vli, &vs, 2, &n, views)) && n == 2) {
        for (int e = 0; e < 2; ++e) {
            const XrView& v = views[e];
            h.eye[e].q[0] = v.pose.orientation.x; h.eye[e].q[1] = v.pose.orientation.y;
            h.eye[e].q[2] = v.pose.orientation.z; h.eye[e].q[3] = v.pose.orientation.w;
            h.eye[e].p[0] = v.pose.position.x; h.eye[e].p[1] = v.pose.position.y; h.eye[e].p[2] = v.pose.position.z;
            h.eyeTan[e][0] = tanf(v.fov.angleUp);
            h.eyeTan[e][1] = tanf(-v.fov.angleDown);
            h.eyeTan[e][2] = tanf(-v.fov.angleLeft);
            h.eyeTan[e][3] = tanf(v.fov.angleRight);
        }
        static bool s_tanLogged = false;
        memcpy(g_eyeTanHeadset, h.eyeTan, sizeof(g_eyeTanHeadset));
        g_haveEyeTan = true;
        if (!s_tanLogged) {
            s_tanLogged = true;
            for (int e = 0; e < 2; ++e)
                LOG("OpenXR eye %d FOV tangents: up %.3f down %.3f left %.3f right %.3f", e, h.eyeTan[e][0], h.eyeTan[e][1],
                    h.eyeTan[e][2], h.eyeTan[e][3]);
        }
        for (auto& v : views) {
            h.tanLeft = std::max(h.tanLeft, tanf(-v.fov.angleLeft));
            h.tanRight = std::max(h.tanRight, tanf(v.fov.angleRight));
            h.tanUp = std::max(h.tanUp, tanf(v.fov.angleUp));
            h.tanDown = std::max(h.tanDown, tanf(-v.fov.angleDown));
        }
    } else {
        h.tanLeft = h.tanRight = h.tanUp = h.tanDown = 1.0f;  // 90 degrees if the runtime gives no views
    }
    const XrPosef& P = loc.pose;
    h.abs.q[0] = P.orientation.x; h.abs.q[1] = P.orientation.y; h.abs.q[2] = P.orientation.z; h.abs.q[3] = P.orientation.w;
    h.abs.p[0] = P.position.x; h.abs.p[1] = P.position.y; h.abs.p[2] = P.position.z;
    float inv[4];
    vrs::QuatConj(g_recYawQ, inv);
    vrs::QuatMul(inv, h.abs.q, h.rel.q);
    float d[3] = {h.abs.p[0] - g_recPos[0], h.abs.p[1] - g_recPos[1], h.abs.p[2] - g_recPos[2]};
    vrs::QuatRotate(inv, d, h.rel.p);
    h.gazeValid = LocateGaze(time, h.gazeDir);
    h.valid = true;
    vrs::PublishHead(h);
}


// ---------------------------------------------------------------------------
// In-headset settings menu (vrkit, D3D12 backend): a head-locked quad layer drawn on top while open.
// ---------------------------------------------------------------------------
XrSwapchain g_menuSwap = XR_NULL_HANDLE;
std::vector<XrSwapchainImageD3D12KHR> g_menuImages;
ID3D12CommandAllocator* g_menuAlloc[3] = {};
ID3D12GraphicsCommandList* g_menuList = nullptr;
ID3D12Fence* g_menuFence = nullptr;
HANDLE g_menuEvent = nullptr;
uint64_t g_menuFenceValue = 0;
uint64_t g_menuAllocFence[3] = {};
int g_menuFrame = 0;
bool g_menuReady = false;
bool g_menuFailed = false;
bool g_menuVisible = false;
bool g_menuShot = false;      // xrshot taken this frame: also save the menu image
void SaveXrImage(ID3D12Resource* img, UINT slice = 0);
XrCompositionLayerQuad g_menuQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
uint64_t g_xrFramesThisSecond = 0, g_xrFps = 0;
ULONGLONG g_xrFpsTick = 0;

void DestroyMenu() {
    if (g_menuSwap) xrDestroySwapchain(g_menuSwap);
    g_menuSwap = XR_NULL_HANDLE;
    g_menuImages.clear();
}

bool EnsureMenu() {
    if (g_menuFailed) return false;
    if (!g_menuReady) {
        if (!menu_options::Init(g_device, 3)) { g_menuFailed = true; return false; }
        for (auto& a : g_menuAlloc)
            if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) { g_menuFailed = true; return false; }
        if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_menuAlloc[0], nullptr, IID_PPV_ARGS(&g_menuList))) ||
            FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_menuFence)))) { g_menuFailed = true; return false; }
        g_menuList->Close();
        g_menuEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_menuReady = true;
    }
    return true;
}

bool EnsureMenuSwapchain() {
    if (g_menuSwap) return true;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = g_xrFormat == DXGI_FORMAT_R8G8B8A8_UNORM ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    ci.sampleCount = 1;
    ci.width = vrkit::menu12::Width();
    ci.height = vrkit::menu12::Height();
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (!Check(xrCreateSwapchain(g_session, &ci, &g_menuSwap), "xrCreateSwapchain (menu)")) {
        g_menuSwap = XR_NULL_HANDLE;
        return false;
    }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(g_menuSwap, 0, &n, nullptr);
    g_menuImages.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    xrEnumerateSwapchainImages(g_menuSwap, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(g_menuImages.data()));
    LOG("Menu swapchain created: %u x %u", ci.width, ci.height);
    return true;
}

// Once per OpenXR frame, before xrEndFrame. Returns the menu layer while the menu is open, else nullptr.
const XrCompositionLayerBaseHeader* MenuFrame() {
    ULONGLONG now = GetTickCount64();
    ++g_xrFramesThisSecond;
    if (now - g_xrFpsTick >= 1000) { g_xrFps = g_xrFramesThisSecond; g_xrFramesThisSecond = 0; g_xrFpsTick = now; }
    g_menuVisible = false;
    if (!EnsureMenu() || !vrkit::menu12::Update()) {
        g_menuPlacer.Update(false, false, XrPosef{{0, 0, 0, 1}, {0, 0, 0}});  // re-anchor next time it opens
        return nullptr;
    }
    if (!EnsureMenuSwapchain()) return nullptr;
    int f = g_menuFrame;
    g_menuFrame = (g_menuFrame + 1) % 3;
    if (g_menuFence->GetCompletedValue() < g_menuAllocFence[f]) {
        g_menuFence->SetEventOnCompletion(g_menuAllocFence[f], g_menuEvent);
        WaitForSingleObject(g_menuEvent, 1000);
    }
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!Check(xrAcquireSwapchainImage(g_menuSwap, &ai, &idx), "xrAcquireSwapchainImage (menu)")) return nullptr;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    bool ok = Check(xrWaitSwapchainImage(g_menuSwap, &wi), "xrWaitSwapchainImage (menu)");
    if (ok) {
        g_menuAlloc[f]->Reset();
        g_menuList->Reset(g_menuAlloc[f], nullptr);
        vrkit::menu12::Record(g_menuList);
        ID3D12Resource* img = g_menuImages[idx].texture;
        auto b0 = Transition(img, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        g_menuList->ResourceBarrier(1, &b0);
        g_menuList->CopyResource(img, vrkit::menu12::Texture());
        auto b1 = Transition(img, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        g_menuList->ResourceBarrier(1, &b1);
        g_menuList->Close();
        ID3D12CommandList* lists[] = {g_menuList};
        g_queue->ExecuteCommandLists(1, lists);
        g_queue->Signal(g_menuFence, ++g_menuFenceValue);
        g_menuAllocFence[f] = g_menuFenceValue;
        if (g_menuShot) SaveXrImage(img);
    }
    g_menuShot = false;
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_menuSwap, &ri);
    if (!ok) return nullptr;
    // World anchor: the panel stays where it opened (placed in front of the head at that moment); Head: follows.
    XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION};
    const bool headOk = g_frameTime && XR_SUCCEEDED(xrLocateSpace(g_viewSpace, g_localSpace, g_frameTime, &hl)) &&
                        (hl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    g_menuPlacer.Update(true, headOk, hl.pose);
    g_menuQuad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    g_menuQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    g_menuQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    g_menuQuad.subImage.swapchain = g_menuSwap;
    g_menuQuad.subImage.imageRect = {{0, 0}, {(int32_t)vrkit::menu12::Width(), (int32_t)vrkit::menu12::Height()}};
    g_menuPlacer.Apply(menu_options::Placement(), g_localSpace, g_viewSpace, vrkit::menu12::Width(),
                       vrkit::menu12::Height(), g_menuQuad);
    g_menuVisible = true;
    return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&g_menuQuad);
}

// ---------------------------------------------------------------------------
// HUD layer (hud_layer.cpp redirects the game's UI composite into its own texture): copied into an OpenXR swapchain
// once per OpenXR frame and shown as a quad, head-locked in the head-tracked views, on the screen otherwise.
// ---------------------------------------------------------------------------
XrSwapchain g_hudSwap = XR_NULL_HANDLE;
std::vector<XrSwapchainImageD3D12KHR> g_hudImages;
uint32_t g_hudSwapW = 0, g_hudSwapH = 0;
ID3D12CommandAllocator* g_hudAlloc[3] = {};
ID3D12GraphicsCommandList* g_hudList = nullptr;
ID3D12Fence* g_hudFence = nullptr;
HANDLE g_hudEvent = nullptr;
uint64_t g_hudFenceValue = 0;
uint64_t g_hudAllocFence[3] = {};
int g_hudFrame = 0;
XrCompositionLayerQuad g_hudQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
bool g_hudLogged = false;

void DestroyHudSwapchain() {
    if (g_hudSwap) xrDestroySwapchain(g_hudSwap);
    g_hudSwap = XR_NULL_HANDLE;
    g_hudImages.clear();
    g_hudSwapW = g_hudSwapH = 0;
}

// onScreen: draw the HUD over the theater screen (same pose and size) instead of head-locked.
// ---- Full-screen fades in the HUD (user, Oct 28: at the start of a new game the opening 3D scene was covered by a
// HUD-sized black box). The game fades with a uniform overlay in its UI image; flat that covers the screen, in VR the
// head-locked HUD panel turns into a black rectangle. A 16 x 9 grid of HUD pixels is read back each frame (3 frames
// late); while nearly all samples are the same dark, non-transparent colour, the HUD layer is not shown.
constexpr int kProbeX = 16, kProbeY = 9, kProbeN = kProbeX * kProbeY, kProbeStride = 512;
ID3D12Resource* g_probeBuf = nullptr;
uint8_t* g_probeMap = nullptr;
bool g_probeValid[3] = {};
int g_fadeRun = 0, g_clearRun = 0, g_fadeLogs = 0;
bool g_hudFade = false;
std::atomic<bool> g_hudMovie{false};
int g_movieRun = 0, g_movieClear = 0;

// Called with slot f's previous readback complete (its fence waited); returns whether to show the HUD this frame.
void ProbeResult(int f) {
    if (!g_probeMap || !g_probeValid[f]) return;
    const uint8_t* base = g_probeMap + (size_t)f * kProbeN * kProbeStride;
    int hist[4][32] = {};
    for (int i = 0; i < kProbeN; ++i)
        for (int c = 0; c < 4; ++c) ++hist[c][base[i * kProbeStride + c] >> 3];
    int mode[4];
    for (int c = 0; c < 4; ++c) {
        int best = 0;
        for (int b = 1; b < 32; ++b)
            if (hist[c][b] > hist[c][best]) best = b;
        mode[c] = best;
    }
    int same = 0;
    for (int i = 0; i < kProbeN; ++i) {
        const uint8_t* px = base + i * kProbeStride;
        bool ok = true;
        for (int c = 0; c < 4; ++c) ok = ok && std::abs((px[c] >> 3) - mode[c]) <= 1;
        same += ok;
    }
    if (vr::g_config.verboseLog) {  // dev: what the HUD image holds (opening cinematic investigation)
        static int s_n = 0;
        if (++s_n % 45 == 0) {
            int opaqueDark = 0, clear = 0, opaque = 0;
            for (int i = 0; i < kProbeN; ++i) {
                const uint8_t* px = base + i * kProbeStride;
                if (px[3] < 16) ++clear;
                if (px[3] > 200) ++opaque;
                if (px[3] > 128 && px[0] < 50 && px[1] < 50 && px[2] < 50) ++opaqueDark;
            }
            char rows[kProbeY][kProbeX + 1];
            for (int y = 0; y < kProbeY; ++y) {
                for (int x = 0; x < kProbeX; ++x) {
                    const uint8_t* px = base + (y * kProbeX + x) * kProbeStride;
                    const int lum = (px[0] + px[1] + px[2]) / 3;
                    rows[y][x] = px[3] < 16 ? '.' : px[3] < 128 ? (lum < 50 ? ':' : '+') : (lum < 50 ? '#' : lum < 160 ? 'o' : '@');
                }
                rows[y][kProbeX] = 0;
            }
            LOG("HUD probe: clear %d opaque %d opaque-dark %d of %d; mode rgba %d %d %d %d /31 (%d alike) | %s|%s|%s|%s|%s|%s|%s|%s|%s",
                clear, opaque, opaqueDark, kProbeN, mode[0], mode[1], mode[2], mode[3], same, rows[0], rows[1], rows[2], rows[3],
                rows[4], rows[5], rows[6], rows[7], rows[8]);
        }
    }
    // Letterbox mask of the opening cinematic (user log Oct 29, 42-102 s): rows 0 and 8 clear, rows 1-7 opaque black.
    {
        int edgeClear = 0, midDark = 0;
        for (int i = 0; i < kProbeN; ++i) {
            const uint8_t* px = base + i * kProbeStride;
            const int row = i / kProbeX;
            if (row == 0 || row == kProbeY - 1) edgeClear += px[3] < 16;
            else midDark += px[3] > 200 && px[0] < 40 && px[1] < 40 && px[2] < 40;
        }
        const bool movie = edgeClear >= 2 * kProbeX * 9 / 10 && midDark >= (kProbeY - 2) * kProbeX * 85 / 100;
        g_movieRun = movie ? g_movieRun + 1 : 0;
        g_movieClear = movie ? 0 : g_movieClear + 1;
        const bool was = g_hudMovie;
        if (!was && g_movieRun >= 10) g_hudMovie = true;   // ~0.2 s at 45+ fps
        if (was && g_movieClear >= 10) g_hudMovie = false;
        if (was != g_hudMovie.load())
            LOG("HUD: letterboxed cinematic mask %s (edges clear %d / %d, middle dark %d / %d)",
                g_hudMovie ? "seen: flat screen, no HUD panel" : "gone: back to normal", edgeClear, 2 * kProbeX, midDark,
                (kProbeY - 2) * kProbeX);
    }
    const bool dark = mode[0] <= 5 && mode[1] <= 5 && mode[2] <= 5;  // < ~0.2
    const bool fade = same >= kProbeN * 9 / 10 && mode[3] >= 4 && dark;  // alpha > ~0.12
    g_fadeRun = fade ? g_fadeRun + 1 : 0;
    g_clearRun = fade ? 0 : g_clearRun + 1;
    const bool was = g_hudFade;
    if (!g_hudFade && g_fadeRun >= 3) g_hudFade = true;
    if (g_hudFade && g_clearRun >= 2) g_hudFade = false;
    if (was != g_hudFade && g_fadeLogs++ < 200)
        LOG("HUD: %s (%d of %d samples alike, alpha ~%d / 31)", g_hudFade ? "full-screen fade overlay, panel hidden"
                                                                    : "fade overlay gone, panel shown", same, kProbeN, mode[3]);
}

void ProbeRecord(ID3D12GraphicsCommandList* cl, ID3D12Resource* tex, uint32_t w, uint32_t h, int f) {
    if (!g_probeBuf) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (UINT64)3 * kProbeN * kProbeStride;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&g_probeBuf))))
            return;
        D3D12_RANGE rr{0, (SIZE_T)rd.Width};
        void* p = nullptr;
        if (FAILED(g_probeBuf->Map(0, &rr, &p))) return;
        g_probeMap = static_cast<uint8_t*>(p);
    }
    if (!g_probeMap) return;
    const DXGI_FORMAT fmt = tex->GetDesc().Format;
    if (fmt != DXGI_FORMAT_R8G8B8A8_TYPELESS && fmt != DXGI_FORMAT_R8G8B8A8_UNORM) return;
    D3D12_TEXTURE_COPY_LOCATION src{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{g_probeBuf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint.Footprint = {fmt, 1, 1, 1, 256};
    for (int y = 0; y < kProbeY; ++y)
        for (int x = 0; x < kProbeX; ++x) {
            const UINT px = (UINT)((x + 0.5f) * w / kProbeX), py = (UINT)((y + 0.5f) * h / kProbeY);
            D3D12_BOX box{px, py, 0, px + 1, py + 1, 1};
            dst.PlacedFootprint.Offset = ((UINT64)f * kProbeN + y * kProbeX + x) * kProbeStride;
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        }
    g_probeValid[f] = true;
}

// Placement changes of the HUD layer (dev log for the menu-page flicker): -1 none, 0 on the theater screen,
// 1 head-locked (mono head-tracked), 2 head-locked (native stereo).
void NoteHudPlacement(int p) {
    static int s_last = -3, s_logs = 0;
    static ULONGLONG s_since = 0;
    if (p == s_last) return;
    const ULONGLONG now = GetTickCount64();
    if (s_logs++ < 400)
        LOG("HUD placement: %s -> %s after %llu ms", s_last == -1 ? "none" : s_last == 0 ? "screen" : s_last == 1 ? "head (mono)" : s_last == 2 ? "head (native)" : "start",
            p == -1 ? "none" : p == 0 ? "screen" : p == 1 ? "head (mono)" : "head (native)", s_since ? now - s_since : 0ull);
    s_last = p;
    s_since = now;
}

const XrCompositionLayerBaseHeader* HudFrame(bool onScreen, float screenW, float screenH) {
    if (!vr::g_config.hudLayer || !hud::Active() || !hud::Texture()) {
        if (g_hudMovie.exchange(false)) LOG("HUD: letterboxed cinematic mode ended (no HUD image)");
        g_movieRun = 0;
        NoteHudPlacement(-1);
        return nullptr;
    }
    NoteHudPlacement(onScreen ? 0 : stereo_render::NativeActive() ? 2 : 1);
    const uint32_t w = hud::Width(), h = hud::Height();
    if (!w || !h) return nullptr;
    if (!g_hudList) {
        for (auto& a : g_hudAlloc)
            if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return nullptr;
        if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_hudAlloc[0], nullptr, IID_PPV_ARGS(&g_hudList))) ||
            FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_hudFence))))
            return nullptr;
        g_hudList->Close();
        g_hudEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    if (g_hudSwap && (g_hudSwapW != w || g_hudSwapH != h)) DestroyHudSwapchain();
    if (!g_hudSwap) {
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        ci.format = g_xrFormat == DXGI_FORMAT_R8G8B8A8_UNORM ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        ci.sampleCount = 1;
        ci.width = w;
        ci.height = h;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        if (!Check(xrCreateSwapchain(g_session, &ci, &g_hudSwap), "xrCreateSwapchain (HUD)")) {
            g_hudSwap = XR_NULL_HANDLE;
            return nullptr;
        }
        uint32_t n = 0;
        xrEnumerateSwapchainImages(g_hudSwap, 0, &n, nullptr);
        g_hudImages.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        xrEnumerateSwapchainImages(g_hudSwap, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(g_hudImages.data()));
        g_hudSwapW = w;
        g_hudSwapH = h;
        LOG("HUD swapchain created: %u x %u", w, h);
    }
    int f = g_hudFrame;
    g_hudFrame = (g_hudFrame + 1) % 3;
    if (g_hudFence->GetCompletedValue() < g_hudAllocFence[f]) {
        g_hudFence->SetEventOnCompletion(g_hudAllocFence[f], g_hudEvent);
        WaitForSingleObject(g_hudEvent, 1000);
    }
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!Check(xrAcquireSwapchainImage(g_hudSwap, &ai, &idx), "xrAcquireSwapchainImage (HUD)")) return nullptr;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    bool ok = Check(xrWaitSwapchainImage(g_hudSwap, &wi), "xrWaitSwapchainImage (HUD)");
    if (ok) {
        ProbeResult(f);
        g_hudAlloc[f]->Reset();
        g_hudList->Reset(g_hudAlloc[f], nullptr);
        ProbeRecord(g_hudList, hud::Texture(), w, h, f);
        ID3D12Resource* img = g_hudImages[idx].texture;
        auto b0 = Transition(img, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        g_hudList->ResourceBarrier(1, &b0);
        g_hudList->CopyResource(img, hud::Texture());
        auto b1 = Transition(img, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        g_hudList->ResourceBarrier(1, &b1);
        g_hudList->Close();
        ID3D12CommandList* lists[] = {g_hudList};
        g_queue->ExecuteCommandLists(1, lists);
        g_queue->Signal(g_hudFence, ++g_hudFenceValue);
        g_hudAllocFence[f] = g_hudFenceValue;
        if (g_menuShot) SaveXrImage(img);
    }
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_hudSwap, &ri);
    if (!ok || (g_hudFade && !onScreen) || g_hudMovie) return nullptr;  // on the theater screen a fade looks as it does flat
    const auto& c = vr::g_config;
    g_hudQuad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    g_hudQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    g_hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    g_hudQuad.subImage.swapchain = g_hudSwap;
    g_hudQuad.subImage.imageRect = {{0, 0}, {(int32_t)w, (int32_t)h}};
    if (onScreen) {
        g_hudQuad.space = g_localSpace;
        g_hudQuad.pose = g_screenPose;
        g_hudQuad.size = {screenW, screenH};
    } else if (c.hudAnchor == 1) {
        // fixed in the room: in front of the recentered head position / yaw
        const float s = g_recYawQ[1], co = g_recYawQ[3];
        const float yawSin = 2 * s * co, yawCos = co * co - s * s;  // yaw from the half-angle quaternion
        g_hudQuad.space = g_localSpace;
        g_hudQuad.pose.orientation = {g_recYawQ[0], g_recYawQ[1], g_recYawQ[2], g_recYawQ[3]};
        g_hudQuad.pose.position = {g_recPos[0] - c.hudDistance * yawSin, g_recPos[1] + c.hudHeight,
                                   g_recPos[2] - c.hudDistance * yawCos};
        g_hudQuad.size = {c.hudWidth, c.hudWidth * (float)h / (float)w};
    } else {
        g_hudQuad.space = g_viewSpace;
        g_hudQuad.pose = {{0, 0, 0, 1}, {0, c.hudHeight, -c.hudDistance}};
        g_hudQuad.size = {c.hudWidth, c.hudWidth * (float)h / (float)w};
    }
    if (!g_hudLogged) {
        g_hudLogged = true;
        LOG("HUD layer shown (%s)", onScreen ? "on the screen" : "head-locked panel");
    }
    return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&g_hudQuad);
}

// Dev: <game>\ff16vr_xrshot.trigger saves the next submitted OpenXR image (both eyes) as xrshot_N.bmp (half size).
int g_xrShotCount = 0;

bool XrShotRequested() {
    static ULONGLONG next = 0;
    ULONGLONG now = GetTickCount64();
    if (now < next) return false;
    next = now + 250;
    const std::wstring trig = vr::g_gameDir + L"\\ff16vr_xrshot.trigger";
    if (GetFileAttributesW(trig.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    DeleteFileW(trig.c_str());
    return true;
}

void SaveXrImage(ID3D12Resource* img, UINT slice) {
    D3D12_RESOURCE_DESC d = img->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    if (slice >= d.DepthOrArraySize) slice = 0;
    g_device->GetCopyableFootprints(&d, slice, 1, 0, &fp, nullptr, nullptr, &total);
    fp.Offset = 0;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = nullptr;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&rb)))) return;
    int f = g_frameIdx;
    g_frameIdx = (g_frameIdx + 1) % kFrames;
    if (g_fence->GetCompletedValue() < g_allocFence[f]) {
        g_fence->SetEventOnCompletion(g_allocFence[f], g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, 1000);
    }
    g_alloc[f]->Reset();
    g_list->Reset(g_alloc[f], nullptr);
    auto b0 = Transition(img, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g_list->ResourceBarrier(1, &b0);
    D3D12_TEXTURE_COPY_LOCATION dst{rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{img, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    src.SubresourceIndex = slice;  // one mip: subresource = array slice
    g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    auto b1 = Transition(img, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g_list->ResourceBarrier(1, &b1);
    g_list->Close();
    ID3D12CommandList* lists[] = {g_list};
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_fence, ++g_fenceValue);
    g_allocFence[f] = g_fenceValue;
    g_fence->SetEventOnCompletion(g_fenceValue, g_fenceEvent);
    WaitForSingleObject(g_fenceEvent, 2000);

    uint8_t* data = nullptr;
    D3D12_RANGE r{0, (SIZE_T)total};
    if (SUCCEEDED(rb->Map(0, &r, reinterpret_cast<void**>(&data)))) {
        const uint32_t W = (uint32_t)d.Width / 2, H = d.Height / 2;
        const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                          d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
        const uint32_t rowBytes = (W * 3 + 3) & ~3u;
        std::vector<uint8_t> bmp(54 + (size_t)rowBytes * H);
        auto put32 = [&](size_t o, uint32_t v) { memcpy(&bmp[o], &v, 4); };
        bmp[0] = 'B'; bmp[1] = 'M';
        put32(2, (uint32_t)bmp.size()); put32(10, 54); put32(14, 40); put32(18, W); put32(22, H);
        bmp[26] = 1; bmp[28] = 24; put32(34, rowBytes * H);
        for (uint32_t y = 0; y < H; ++y) {
            const uint8_t* srow = data + fp.Offset + (size_t)(y * 2) * fp.Footprint.RowPitch;
            uint8_t* drow = &bmp[54 + (size_t)(H - 1 - y) * rowBytes];
            for (uint32_t x = 0; x < W; ++x) {
                const uint8_t* px = srow + (size_t)x * 2 * 4;
                drow[x * 3 + 0] = bgra ? px[0] : px[2];
                drow[x * 3 + 1] = px[1];
                drow[x * 3 + 2] = bgra ? px[2] : px[0];
            }
        }
        rb->Unmap(0, nullptr);
        const std::wstring path = vr::g_gameDir + L"\\xrshot_" + std::to_wstring(++g_xrShotCount) + L".bmp";
        if (FILE* fo = _wfopen(path.c_str(), L"wb")) {
            fwrite(bmp.data(), 1, bmp.size(), fo);
            fclose(fo);
            LOG("XR shot saved: xrshot_%d.bmp (%u x %u, format %d)", g_xrShotCount, W, H, (int)d.Format);
        }
    }
    rb->Release();
}

// ---------------------------------------------------------------------------
// VRMode Native: the game renders two frames per tick (stereo_render), left eye then right eye, and presents each.
// One OpenXR frame spans both presents: the first opens it (wait/begin, acquire a double-wide image), each present
// is blitted into the half of the eye its camera was set up for, and the second present ends the frame.
// ---------------------------------------------------------------------------
struct NativeFrame {
    bool open = false;
    bool acquired = false;
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    uint32_t idx = 0;
    int filled = 0;    // bit per eye
    int presents = 0;  // presents since the frame was opened
    XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                 {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
};
NativeFrame g_nf;
uint64_t g_nativeFrames = 0;
uint64_t g_eyeMismatch = 0;

// Ends a pending native OpenXR frame without layers (leaving native mode, or the swapchain is about to be recreated
// between the two eyes: the frame's views would reference the destroyed swapchain).
void CloseNativeFrameEmpty() {
    if (!g_nf.open) return;
    if (g_nf.acquired) {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(g_xrSwap, &ri);
    }
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = g_nf.fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    xrEndFrame(g_session, &ei);
    g_nf.open = false;
}

void NativePresent(ID3D12Resource* bb, uint32_t eyeW, uint32_t eyeH) {
    NativeFrame& nf = g_nf;
    // Which eye is in this backbuffer? The camera sample it was rendered with says so. The engine presents with a
    // pipeline delay of PoseLatencyFrames (1: the image presented on pass 1 was rendered by pass 0, the left eye),
    // so without a fresh sample (menus: no main-view camera update) the eye follows from the pass parity.
    const int lag = vr::g_config.poseLatencyFrames;
    const int nViews = 2;
    vrs::UsedSample used;
    const bool haveUsed = vrs::UsedForPresent(vrs::PresentCount(), lag, &used);
    const bool fresh = haveUsed && used.eye >= 0 && used.eye < nViews && used.presentTag + lag + 2 >= vrs::PresentCount();
    const int parityEye = ((stereo_render::CurrentPass() - lag) % nViews + nViews) % nViews;  // view rendered `lag` passes ago
    const int eye = fresh ? used.eye : parityEye;  // view index
    if (fresh && used.eye != parityEye) ++g_eyeMismatch;

    // An OpenXR frame holds one game tick: left eye first, then right. Views without an open frame
    // are dropped.
    if (!nf.open && eye != 0) {
        vrs::CountPresent();
        return;
    }
    if (!nf.open) {
        nf = NativeFrame{};
        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        if (!Check(xrWaitFrame(g_session, &waitInfo, &nf.fs), "xrWaitFrame")) { vrs::CountPresent(); return; }
        XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        if (!Check(xrBeginFrame(g_session, &beginInfo), "xrBeginFrame")) { vrs::CountPresent(); return; }
        nf.open = true;
        UpdateRecenter(nf.fs.predictedDisplayTime);
        PublishHeadSample(nf.fs.predictedDisplayTime);
        if (nf.fs.shouldRender) {
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (Check(xrAcquireSwapchainImage(g_xrSwap, &ai, &nf.idx), "xrAcquireSwapchainImage")) {
                XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wi.timeout = XR_INFINITE_DURATION;
                nf.acquired = Check(xrWaitSwapchainImage(g_xrSwap, &wi), "xrWaitSwapchainImage");
                if (!nf.acquired) {
                    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    xrReleaseSwapchainImage(g_xrSwap, &ri);
                }
            }
        }
    }

    if (nf.acquired && haveUsed) {
        RecordAndSubmit(bb, nf.idx, (uint32_t)eye);
        XrCompositionLayerProjectionView& pv = nf.views[eye];
        pv.pose.orientation = {used.abs.q[0], used.abs.q[1], used.abs.q[2], used.abs.q[3]};
        pv.pose.position = {used.abs.p[0], used.abs.p[1], used.abs.p[2]};
        pv.fov.angleLeft = -atanf(used.tanHalfH);
        pv.fov.angleRight = atanf(used.tanHalfH);
        pv.fov.angleUp = atanf(used.tanHalfV);
        pv.fov.angleDown = -atanf(used.tanHalfV);
        pv.subImage.swapchain = g_xrSwap;
        pv.subImage.imageRect = {{0, 0}, {(int32_t)eyeW, (int32_t)eyeH}};
        pv.subImage.imageArrayIndex = (uint32_t)eye;
        nf.filled |= 1 << eye;
    }
    vrs::CountPresent();
    ++nf.presents;
    const int all = (1 << nViews) - 1;
    if (nf.presents < nViews && nf.filled != all) return;

    // End the OpenXR frame.
    if (nf.acquired && nf.filled && XrShotRequested()) {
        SaveXrImage(g_xrImages[nf.idx].texture);
        SaveXrImage(g_xrImages[nf.idx].texture, 1);  // right eye
        g_menuShot = true;
    }
    if (nf.acquired) {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(g_xrSwap, &ri);
    }
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[4];
    uint32_t layerCount = 0;
    if (nf.acquired && (nf.filled & 3)) {
        if ((nf.filled & 3) == 1) nf.views[1] = nf.views[0];  // only one eye arrived: show it to both
        if ((nf.filled & 3) == 2) nf.views[0] = nf.views[1];
        proj.space = g_localSpace;
        proj.viewCount = 2;
        proj.views = nf.views;
        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
        layerCount = 1;
    }
    if (nf.fs.shouldRender && layerCount)
        if (const XrCompositionLayerBaseHeader* hl = HudFrame(false, 0, 0)) layers[layerCount++] = hl;
    if (nf.fs.shouldRender)
        if (const XrCompositionLayerBaseHeader* m = MenuFrame()) layers[layerCount++] = m;
    g_menuShot = false;
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = nf.fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layerCount;
    ei.layers = layers;
    Check(xrEndFrame(g_session, &ei), "xrEndFrame");
    nf.open = false;
    ++g_nativeFrames;
    if (g_nativeFrames == 1 || g_nativeFrames % 900 == 0) {
        const auto& l = nf.views[0];
        const auto& r = nf.views[1];
        float dx = r.pose.position.x - l.pose.position.x, dy = r.pose.position.y - l.pose.position.y,
              dz = r.pose.position.z - l.pose.position.z;
        LOG("Native stereo: %llu frames, eyes filled %d, eye order mismatches %llu, IPD %.1f mm, FOV %.1f x %.1f deg",
            (unsigned long long)g_nativeFrames, nf.filled, (unsigned long long)g_eyeMismatch,
            sqrtf(dx * dx + dy * dy + dz * dz) * 1000.f, (l.fov.angleRight - l.fov.angleLeft) * 57.2958f,
            (l.fov.angleUp - l.fov.angleDown) * 57.2958f);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------
void OnSwapChainCreated(IDXGISwapChain* sc, ID3D12CommandQueue* queue) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        LOG("Swap chain queue is not a direct queue; ignoring");
        queue->Release();
        return;
    }
    if (g_queue == queue) {
        queue->Release();  // same queue, drop the extra reference
    } else {
        if (g_queue) {
            LOG("Game switched to a new D3D12 queue; restarting VR session");
            ReleaseGpuResources();
            DestroySession();
            SafeRelease(g_queue);
            SafeRelease(g_device);
        }
        g_queue = queue;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&g_device)))) {
            LOG("ERROR: could not get D3D12 device from queue");
            SafeRelease(g_queue);
            return;
        }
    }
    g_gameSwap = sc;
    LOG("Tracking swap chain %p (device %p, queue %p)", sc, g_device, g_queue);
}

void OnResizeBuffers(IDXGISwapChain* sc) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (sc == g_gameSwap) WaitGpuIdle();
}

void OnPresent(IDXGISwapChain* sc) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_disabled || sc != g_gameSwap || !g_queue || !g_device) return;

    ULONGLONG now = GetTickCount64();
    if (!g_instance) {
        if (now < g_nextRetry) return;
        g_nextRetry = now + 3000;
        if (!g_loader && !LoadLoader()) {
            g_disabled = true;
            return;
        }
        if (!CreateInstance()) return;
    }
    if (g_system == XR_NULL_SYSTEM_ID) {
        if (now < g_nextRetry) return;
        g_nextRetry = now + 3000;
        if (!GetSystem()) return;
    }
    if (!g_session) {
        if (now < g_nextRetry) return;
        if (!CreateSession()) {
            g_nextRetry = now + 3000;
            return;
        }
    }

    PollEvents();
    if (!g_session || !g_running) return;

    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return;
    ID3D12Resource* bb = nullptr;
    HRESULT hr = sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
    sc3->Release();
    if (FAILED(hr) || !bb) return;
    D3D12_RESOURCE_DESC bd = bb->GetDesc();

    if (!EnsureGpuResources()) {
        LOG("ERROR: GPU resource setup failed; VR disabled");
        g_disabled = true;
        bb->Release();
        return;
    }
    const bool native = vr::g_config.vrMode == 2 && stereo_render::NativeActive();
    if (!EnsureIntermediate(bd) ||
        !EnsureXrSwapchain((uint32_t)bd.Width, bd.Height, native ? 2u : 1)) {
        bb->Release();
        return;
    }
    if (native) {
        NativePresent(bb, g_xrW, g_xrH);
        bb->Release();
        return;
    }
    CloseNativeFrameEmpty();  // left native mode mid-frame

    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (!Check(xrWaitFrame(g_session, &waitInfo, &fs), "xrWaitFrame")) {
        bb->Release();
        return;
    }
    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (!Check(xrBeginFrame(g_session, &beginInfo), "xrBeginFrame")) {
        bb->Release();
        return;
    }

    UpdateRecenter(fs.predictedDisplayTime);
    const bool tracked = vr::g_config.vrMode >= 1;
    if (tracked) PublishHeadSample(fs.predictedDisplayTime);

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pviews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                  {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    const XrCompositionLayerBaseHeader* layers[3];
    uint32_t layerCount = 0;

    if (fs.shouldRender) {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (Check(xrAcquireSwapchainImage(g_xrSwap, &ai, &idx), "xrAcquireSwapchainImage")) {
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            if (Check(xrWaitSwapchainImage(g_xrSwap, &wi), "xrWaitSwapchainImage")) {
                RecordAndSubmit(bb, idx);
                if (XrShotRequested()) {
                    SaveXrImage(g_xrImages[idx].texture);
                    g_menuShot = true;
                }
                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(g_xrSwap, &ri);

                vrs::UsedSample used;
                // Head-tracked only while the gameplay camera is live. Menus, loading screens and cutscenes without
                // a gameplay camera stop updating it: the stale sample would stretch the 2D screen over the whole
                // field of view, so those go to the theater screen instead.
                const bool haveUsed = tracked && vrs::UsedForPresent(vrs::PresentCount(), vr::g_config.poseLatencyFrames, &used);
                const bool live = haveUsed && used.presentTag + vr::g_config.poseLatencyFrames + 3 >= vrs::PresentCount();
                // Back to head-tracked only after the gameplay camera has been live for a moment: switching pages in
                // the pause menu (LB / RB) briefly renders the gameplay camera, which made the view jump between
                // the theater screen and head-tracked.
                // Time-based (run 214 / user on the 5090: some menu page changes still flipped the HUD to head-locked
                // for a moment); short live stretches that don't switch are logged.
                static ULONGLONG s_liveSince = 0;
                static int s_shortLogs = 0;
                static ULONGLONG s_lastShort = 0;  // end of the last live stretch that did not switch (menu paging)
                static bool s_tracked = false;
                const ULONGLONG nowMs = GetTickCount64();
                if (live && !s_liveSince) s_liveSince = nowMs;
                if (!live) {
                    if (s_liveSince && !s_tracked && nowMs - s_liveSince < 3000) {  // longer: native stereo ran meanwhile
                        s_lastShort = nowMs;
                        if (s_shortLogs++ < 100)
                            LOG("View: gameplay camera live for %llu ms in the theater view (no switch)", nowMs - s_liveSince);
                    }
                    s_liveSince = 0;
                    s_tracked = false;
                } else if (nowMs - s_liveSince >= (s_lastShort && nowMs - s_lastShort < kPagingWindowMs ? kTrackedAfterPagingMs
                                                                                                     : kTrackedAfterMs)) {
                    s_tracked = true;
                }
                const bool fresh = s_tracked && !g_hudMovie;
                static int s_lastView = -1;
                if (tracked && s_lastView != (int)fresh) {
                    s_lastView = (int)fresh;
                    LOG("View: %s", fresh ? "head-tracked (gameplay camera)" : "theater screen (no gameplay camera)");
                }
                if (fresh) {
                    // Head-tracked mono: the frame was rendered from used.abs with a symmetric FOV; both eyes get
                    // that image and pose, and the runtime reprojects it to each eye's actual pose.
                    XrPosef pose;
                    pose.orientation = {used.abs.q[0], used.abs.q[1], used.abs.q[2], used.abs.q[3]};
                    pose.position = {used.abs.p[0], used.abs.p[1], used.abs.p[2]};
                    XrFovf fov;
                    fov.angleLeft = -atanf(used.tanHalfH);
                    fov.angleRight = atanf(used.tanHalfH);
                    fov.angleUp = atanf(used.tanHalfV);
                    fov.angleDown = -atanf(used.tanHalfV);
                    for (auto& pv : pviews) {
                        pv.pose = pose;
                        pv.fov = fov;
                        pv.subImage.swapchain = g_xrSwap;
                        pv.subImage.imageRect = {{0, 0}, {(int32_t)g_xrW, (int32_t)g_xrH}};
                        pv.subImage.imageArrayIndex = 0;
                    }
                    proj.space = g_localSpace;
                    proj.viewCount = 2;
                    proj.views = pviews;
                    layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
                    layerCount = 1;
                    static bool logged = false;
                    if (!logged) {
                        logged = true;
                        LOG("Head-tracked projection layer: FOV %.1f x %.1f deg", 2 * atanf(used.tanHalfH) * 57.2958f,
                            2 * atanf(used.tanHalfV) * 57.2958f);
                    }
                } else {
                float width = vr::g_config.screenWidth;
                quad.space = g_localSpace;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = g_xrSwap;
                quad.subImage.imageRect = {{0, 0}, {(int32_t)g_xrW, (int32_t)g_xrH}};
                quad.subImage.imageArrayIndex = 0;
                quad.pose = g_screenPose;
                quad.size = {width, width * (float)bd.Height / (float)bd.Width};
                layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
                layerCount = 1;
                }
            }
        }
    }

    if (fs.shouldRender && layerCount) {
        const bool onScreen = layers[0] == reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
        if (const XrCompositionLayerBaseHeader* hl = HudFrame(onScreen, quad.size.width, quad.size.height))
            layers[layerCount++] = hl;
    }
    if (fs.shouldRender)
        if (const XrCompositionLayerBaseHeader* m = MenuFrame()) layers[layerCount++] = m;
    g_menuShot = false;
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layerCount;
    ei.layers = layers;
    Check(xrEndFrame(g_session, &ei), "xrEndFrame");
    bb->Release();
    vrs::CountPresent();

    if (++g_frameCount == 1) LOG("First VR frame submitted");
    if (vr::g_config.verboseLog && g_frameCount % 600 == 0) LOG("VR frames submitted: %llu", g_frameCount);
}

bool HudMovieMode() { return g_hudMovie.load(); }

void RequestRecenter() { g_needRecenter = true; }
unsigned VrFramesPerSecond() { return (unsigned)g_xrFps; }
void RecommendedEyeSize(unsigned* w, unsigned* h) { *w = g_recW; *h = g_recH; }
bool HeadsetEyeTangents(float out[2][4]) {
    if (!g_haveEyeTan) return false;
    memcpy(out, g_eyeTanHeadset, sizeof(g_eyeTanHeadset));
    return true;
}
void MaxSwapchainSize(unsigned* w, unsigned* h) { *w = g_maxW; *h = g_maxH; }
long SwapchainCreates() { return g_xrSwapCreates; }

}  // namespace xr
