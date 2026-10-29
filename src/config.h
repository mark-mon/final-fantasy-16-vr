#pragma once
#include <string>
#include <vector>

namespace vr {

struct Config {
    bool enabled = true;

    // 0 = keep the game patches but don't hook rendering or start OpenXR
    // (use for RenderDoc / PIX captures of the plain game).
    bool vrOutput = true;

    // VRMode: 0 = Flat (game on a virtual screen, quad layer); 1 = Mono (head-tracked: the game camera follows the
    // headset and renders its field of view; both eyes see the same image on a projection layer); 2 = Native
    // (per-eye stereo rendering in gameplay). config.txt accepts the names or numbers. Default: Native.
    int vrMode = 2;
    bool positionTracking = true;
    float worldScale = 1.0f;       // game units per meter of head movement
    int poseLatencyFrames = 1;     // frames between the game's camera update and the Present that shows it
    float fovScale = 1.0f;         // >1 renders a wider field of view than the headset (reprojection margin)

    // Render resolution in Mono / Native (the game renders at its window's client size, so the mod sizes the window):
    // per-eye size = headset's recommended eye image (re-shaped to the rendered field of view) x renderScale.
    // renderWidth / renderHeight > 0 set the size directly. renderScale = 0: leave the window alone.
    float renderScale = 1.0f;
    int renderWidth = 0;
    int renderHeight = 0;

    // Native: DLSS keeps a separate history for each eye (right eye on its own Streamline viewport)
    bool perEyeDlss = true;

    // Fixed foveated rendering (VRS Tier 2) in Mono / Native: full rate inside foveatedInner, 2x2 to foveatedMiddle,
    // foveatedOuterRate beyond (0 = 2x2, 1 = 4x4). Radii as a fraction of the image half height.
    bool foveatedRendering = true;
    float foveatedInner = 0.5f;
    float foveatedMiddle = 0.8f;
    int foveatedOuterRate = 1;
    bool foveatedGaze = false;  // follow the eyes when the headset has eye tracking (XR_EXT_eye_gaze_interaction)

    // Virtual screen (meters, relative to the head pose at recenter time)
    float screenDistance = 2.5f;
    float screenWidth = 3.2f;
    float screenVerticalOffset = 0.0f;

    // HUD on its own layer (HudLayer = 1): head-locked panel in Mono / Native, on top of the screen in Flat / menus
    bool hudLayer = true;
    float hudDistance = 1.5f;      // meters
    float hudWidth = 1.6f;         // meters
    float hudHeight = 0.0f;        // vertical offset, meters
    int hudAnchor = 1;             // 0 = follows the head, 1 = fixed in the room (placed at recenter); default World

    // In-headset settings menu (Insert, or hold L3 + R3): head-locked panel distance and width in meters
    float menuDistance = 1.0f;
    float menuWidth = 0.9f;
    float menuOffsetY = -0.05f;  // meters up (+) / down (-) from eye level
    int menuAnchor = 0;          // 0 = World (stays where it opened), 1 = Head (follows the headset)

    // Keyboard virtual-key code used to recenter the screen (default F8 = 0x77)
    int recenterKey = 0x77;

    // Game patches (replace the equivalent FFXVIFix options)
    bool uncapCutsceneFps = true;
    bool disableDepthOfField = true;
    bool disableMotionBlur = true;
    int blurMenuFixHoldMs = 1000;    // live: how long the fix keeps the pause menu open (10..1000 ms)
    bool disableTaa = false;
    bool perEyeHistory = true;       // live: each eye's previous camera comes from its own last frame (motion vectors)         // live: replace the game's TAA pass with a plain copy (no effect with DLSS/FSR/XeSS)   // live: keep the game's motion blur off (also after loads)
    // Characters / animated objects: remove the other eye\'s offset from their motion vectors (taa.cpp, GPU pass).
    bool perEyeObjectMotion = true;
    // Both eyes of a tick use the same TAA / DLSS / FSR jitter sample (camera_hook.cpp).
    bool stereoJitterSync = true;
    // Per-eye DLSS: each eye\'s motion vectors against its own previous camera (cam_prev.cpp).
    bool perEyeMotionVectors = true;
    // DlssGhostingFix: the one config / menu switch for the five per-eye fixes above (PerEyeDlss, PerEyeHistory,
    // PerEyeObjectMotion, StereoJitterSync, PerEyeMotionVectors); see ApplyDlssGhostingFix.
    bool dlssGhostingFix = false;  // default off (user, Oct 29: slightly sharper image without it)
    bool disableCinematicEffects = true;
    bool disableGraphicsDebuggerCheck = false;  // development only (RenderDoc / PIX); off for players

    // Load *.asi plugins (e.g. FFXVIFix) from the game folder, since this DLL
    // replaces the dinput8.dll that ASI loaders normally use.
    bool loadAsiPlugins = false;

    // Extra DLLs to load, separated by ';' (relative to the game folder)
    std::vector<std::wstring> chainLoad;

    bool verboseLog = false;

    // Development: load RenderDoc in-process for frame captures (see rdoc.h)
    bool renderDoc = false;
    // Pipeline-creation guard: always on under RenderDoc; PsoGuard = 1 also installs it (log only) without RenderDoc
    bool psoGuard = false;
    // Development: log frame flow (stacks, threads, per-second counts) for native stereo work
    bool frameProbe = false;
    // Let the game's NVAPI calls through RenderDoc (RenderDoc option AllowUnsupportedVendorExtensions = 0x10DE).
    // Without it the game doesn't recognise the NVIDIA GPU and picks wave-64 shaders the driver rejects.
    bool renderDocNvApi = true;
    int renderDocDelayMs = -1;  // <0: load at D3D12 device creation (Streamline bypassed); 0: at start; >0: after N ms
    std::wstring renderDocDir = L"D:\\vrconversions\\sharedtools\\renderdoc\\RenderDoc_1.46_64";
    std::wstring captureDir = L"D:\\vrconversions\\ff16\\captures";
};

extern Config g_config;
void ApplyDlssGhostingFix();  // copy dlssGhostingFix to the five per-eye fields
extern std::wstring g_gameDir;

// Loads <gameDir>\config.txt, writing a commented default file if missing.
void LoadConfig();

}  // namespace vr
