#include "config.h"
#include "log.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace vr {

Config g_config;
std::wstring g_gameDir;

static const char* kDefaultConfig =
    "# FF16VR configuration\n"
    "# Lines starting with # or ; are comments. Edit and restart the game to apply.\n"
    "\n"
    "# Master switch (1 = on, 0 = mod loads but does nothing)\n"
    "Enabled = 1\n"
    "\n"
    "# VR output (0 = game patches only, no OpenXR; use this for RenderDoc captures)\n"
    "VROutput = 1\n"
    "\n"
    "# Flat   = the game on a virtual screen in front of you\n"
    "# Mono   = head-tracked: the game camera follows the headset and fills your view (same image in both eyes)\n"
    "# Native = full stereo: in gameplay each eye is rendered from its own position (about twice the GPU cost)\n"
    "# F8 recenters.\n"
    "VRMode = Native\n"
    "# Head position moves the camera too (1) or rotation only (0)\n"
    "PositionTracking = 1\n"
    "# Game units per meter of head movement\n"
    "WorldScale = 1.0\n"
    "# Frames between the game's camera update and the frame shown (raise if the image wobbles when turning)\n"
    "PoseLatencyFrames = 1\n"
    "# Render a slightly wider field of view than the headset (e.g. 1.1) to hide edges during reprojection\n"
    "FovScale = 1.0\n"
    "# Render resolution per eye in Mono / Native, relative to the headset's recommended size (1.0 = 100 %;\n"
    "# lower = faster, higher = sharper). The game window is resized to that (the desktop view is cropped).\n"
    "# 0 = leave the game's own resolution. RenderWidth / RenderHeight > 0 set the per-eye size directly.\n"
    "RenderScale = 1.0\n"
    "RenderWidth = 0\n"
    "RenderHeight = 0\n"
    "# Native: DLSS ghosting fix (per-eye DLSS history, per-eye camera history and motion vectors, same jitter for\n"
    "# both eyes, character motion fix). Removes the halo around characters and keeps DLSS sharp. 1 = on\n"
    "DlssGhostingFix = 0\n"
    "# Fixed foveated rendering (variable rate shading) in Mono / Native: full detail in the middle, coarser shading\n"
    "# towards the edges. Radii as a fraction of the half height; outer rate 2x2 or 4x4.\n"
    "FoveatedRendering = 1\n"
    "FoveatedInner = 0.5\n"
    "FoveatedMiddle = 0.8\n"
    "FoveatedOuterRate = 4x4\n"
    "# Move the full-detail centre with your eyes when the headset has eye tracking (otherwise fixed in the middle)\n"
    "FoveatedGaze = 0\n"
    "\n"
    "# Virtual screen placement, in meters, relative to your head when recentered\n"
    "ScreenDistance = 2.5\n"
    "ScreenWidth = 3.2\n"
    "ScreenVerticalOffset = 0.0\n"
    "\n"
    "# HUD on its own layer (1) or baked into the 3D image (0). In Mono / Native the HUD floats on a panel in front of\n"
    "# you (distance / width / height offset in meters); in Flat and in menus it is drawn on the screen as before.\n"
    "HudLayer = 1\n"
    "HudDistance = 1.5\n"
    "HudWidth = 1.6\n"
    "HudHeight = 0.0\n"
    "# HUD panel: Head = follows your view, World = stays in place in the room (placed in front of you at recenter)\n"
    "HudAnchor = World\n"
    "\n"
    "# In-headset settings menu: Insert key, or hold L3 + R3 (both stick clicks). Distance and width in meters.\n"
    "MenuDistance = 1.0\n"
    "MenuWidth = 0.9\n"
    "# Menu height offset (m) and anchor: World = stays where it opened (turn your head to read it), Head = follows\n"
    "MenuOffsetY = -0.05\n"
    "MenuAnchor = World\n"
    "\n"
    "# Keyboard key to recenter the screen in front of you (hex virtual-key code, 0x77 = F8)\n"
    "RecenterKey = 0x77\n"
    "\n"
    "# ---- Game patches (1 = on, 0 = off) ----\n"
    "# Remove the 30 fps cap in real-time cutscenes and photo mode\n"
    "UncapCutsceneFPS = 1\n"
    "# Turn off depth of field everywhere (blur is uncomfortable in VR)\n"
    "DisableDepthOfField = 1\n"
    "# Keep motion blur off: after loads the game turns its motion blur back on (the eye offset then smears the VR image);\n"
    "# the mod keeps the game's blur strength at its Off value\n"
    "DisableMotionBlur = 1\n"
    "# Turn the game's TAA off (anti-aliasing setting TAA only; DLSS / FSR / XeSS are not affected). Sharper, but aliased\n"
    "DisableTAA = 0\n"
    "# Turn off cutscene / photo mode vignette, film grain and chromatic aberration\n"
    "DisableCinematicEffects = 1\n"
    "\n"
    "# Load *.asi plugins from the game folder. Not needed any more for the patches above;\n"
    "# if you turn this on together with FFXVIFix, disable the same features in one of them.\n"
    "LoadAsiPlugins = 0\n"
    "\n"
    "# Extra DLLs to load, separated by ';' (relative to the game folder)\n"
    "ChainLoad =\n"
    "\n"
    "# Extra logging to ff16vr.log\n"
    "VerboseLog = 0\n"
    "\n"
    "# ---- Development ----\n"
    "# Load RenderDoc into the game for frame captures (use with VROutput = 0)\n"
    "RenderDoc = 0\n"
    "# When to load RenderDoc: -1 = when the game creates its D3D12 device (bypasses Streamline/DLSS; recommended),\n"
    "# 0 = at start (the game quits), N = after N ms\n"
    "RenderDocDelayMs = -1\n"
    "# Let the game's NVAPI calls through RenderDoc (without it the game picks AMD wave-64 shaders and quits)\n"
    "RenderDocNvAPI = 1\n"
    "# Guard pipeline creation (always on under RenderDoc; 1 = also log pipeline failures without RenderDoc)\n"
    "PsoGuard = 0\n";

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static bool ParseBool(const std::string& v) {
    std::string l = Lower(v);
    return l == "1" || l == "true" || l == "yes" || l == "on";
}

static std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

void ApplyDlssGhostingFix() {
    const bool on = g_config.dlssGhostingFix;
    g_config.perEyeDlss = g_config.perEyeHistory = g_config.perEyeObjectMotion = g_config.stereoJitterSync =
        g_config.perEyeMotionVectors = on;
}

void LoadConfig() {
    std::wstring path = g_gameDir + L"\\config.txt";
    std::ifstream in(path);
    if (!in) {
        LOG("config.txt not found, writing defaults");
        std::ofstream out(path);
        if (out) out << kDefaultConfig;
        ApplyDlssGhostingFix();  // the five per-eye fixes follow DlssGhostingFix (default off)
        return;
    }

    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            LOG("config.txt:%d: ignored (no '=')", lineNo);
            continue;
        }
        std::string key = Lower(Trim(line.substr(0, eq)));
        std::string val = Trim(line.substr(eq + 1));

        Config& c = g_config;
        if (key == "enabled") c.enabled = ParseBool(val);
        else if (key == "vroutput") c.vrOutput = ParseBool(val);
        else if (key == "screendistance") c.screenDistance = std::clamp((float)atof(val.c_str()), 0.3f, 50.0f);
        else if (key == "screenwidth") c.screenWidth = std::clamp((float)atof(val.c_str()), 0.2f, 100.0f);
        else if (key == "screenverticaloffset") c.screenVerticalOffset = (float)atof(val.c_str());
        else if (key == "recenterkey") c.recenterKey = (int)strtol(val.c_str(), nullptr, 0);
        else if (key == "loadasiplugins") c.loadAsiPlugins = ParseBool(val);
        else if (key == "uncapcutscenefps") c.uncapCutsceneFps = ParseBool(val);
        else if (key == "disabledepthoffield") c.disableDepthOfField = ParseBool(val);
        else if (key == "disablemotionblur" || key == "blurrendermenufix") c.disableMotionBlur = ParseBool(val);
        else if (key == "blurmenufixholdms") c.blurMenuFixHoldMs = std::clamp(atoi(val.c_str()) / 10 * 10, 10, 1000);
        else if (key == "disabletaa") c.disableTaa = ParseBool(val);
        else if (key == "dlssghostingfix") c.dlssGhostingFix = ParseBool(val);
        // the five parts of DlssGhostingFix (older config files): accepted, set from DlssGhostingFix
        else if (key == "pereyehistory" || key == "pereyeobjectmotion" || key == "stereojittersync" ||
                 key == "pereyemotionvectors" || key == "pereyedlss") {}
        else if (key == "disablecinematiceffects") c.disableCinematicEffects = ParseBool(val);
        else if (key == "disablegraphicsdebuggercheck") c.disableGraphicsDebuggerCheck = ParseBool(val);
        else if (key == "verboselog") c.verboseLog = ParseBool(val);
        else if (key == "renderdoc") c.renderDoc = ParseBool(val);
        else if (key == "renderdocdir") c.renderDocDir = Widen(val);
        else if (key == "renderdocnvapi") c.renderDocNvApi = ParseBool(val);
        else if (key == "psoguard") c.psoGuard = ParseBool(val);
        else if (key == "frameprobe") c.frameProbe = ParseBool(val);
        else if (key == "vrmode") {
            std::string m = Lower(val);
            if (m == "flat" || m == "0") c.vrMode = 0;
            else if (m == "mono" || m == "1") c.vrMode = 1;
            else if (m == "native" || m == "2") c.vrMode = 2;
            else LOG("config.txt:%d: VRMode '%s' unknown (Flat, Mono, Native); using Flat", lineNo, val.c_str());
        }
        else if (key == "positiontracking") c.positionTracking = ParseBool(val);
        else if (key == "worldscale") c.worldScale = std::clamp((float)atof(val.c_str()), 0.01f, 100.0f);
        else if (key == "poselatencyframes") c.poseLatencyFrames = std::clamp(atoi(val.c_str()), 0, 4);
        else if (key == "fovscale") c.fovScale = std::clamp((float)atof(val.c_str()), 0.5f, 1.5f);
        else if (key == "renderscale") c.renderScale = std::clamp((float)atof(val.c_str()), 0.0f, 3.0f);
        else if (key == "renderwidth") c.renderWidth = std::clamp(atoi(val.c_str()), 0, 8192);
        else if (key == "foveatedrendering") c.foveatedRendering = ParseBool(val);
        else if (key == "foveatedgaze") c.foveatedGaze = ParseBool(val);
        else if (key == "foveatedinner") c.foveatedInner = std::clamp((float)atof(val.c_str()), 0.05f, 2.0f);
        else if (key == "foveatedmiddle") c.foveatedMiddle = std::clamp((float)atof(val.c_str()), 0.05f, 3.0f);
        else if (key == "foveatedouterrate") { std::string m = Lower(val); c.foveatedOuterRate = (m == "4x4" || m == "1") ? 1 : 0; }
        else if (key == "renderheight") c.renderHeight = std::clamp(atoi(val.c_str()), 0, 8192);
        else if (key == "hudlayer") c.hudLayer = ParseBool(val);
        else if (key == "huddistance") c.hudDistance = std::clamp((float)atof(val.c_str()), 0.3f, 10.0f);
        else if (key == "hudwidth") c.hudWidth = std::clamp((float)atof(val.c_str()), 0.2f, 10.0f);
        else if (key == "hudheight") c.hudHeight = std::clamp((float)atof(val.c_str()), -3.0f, 3.0f);
        else if (key == "hudanchor") c.hudAnchor = (val == "World" || val == "world" || val == "1") ? 1 : 0;
        else if (key == "menudistance") c.menuDistance = std::clamp((float)atof(val.c_str()), 0.3f, 5.0f);
        else if (key == "menuwidth") c.menuWidth = std::clamp((float)atof(val.c_str()), 0.2f, 5.0f);
        else if (key == "menuoffsety") c.menuOffsetY = std::clamp((float)atof(val.c_str()), -1.0f, 1.0f);
        else if (key == "menuanchor") { std::string m = Lower(val); c.menuAnchor = (m == "head" || m == "1") ? 1 : 0; }
        else if (key == "renderdocdelayms") c.renderDocDelayMs = atoi(val.c_str());
        else if (key == "capturedir") c.captureDir = Widen(val);
        else if (key == "chainload") {
            std::stringstream ss(val);
            std::string item;
            while (std::getline(ss, item, ';')) {
                item = Trim(item);
                if (!item.empty()) c.chainLoad.push_back(Widen(item));
            }
        } else {
            LOG("config.txt:%d: unknown key '%s'", lineNo, key.c_str());
        }
    }

    ApplyDlssGhostingFix();
    LOG("Config: DlssGhostingFix=%d", g_config.dlssGhostingFix);
    LOG("Config: Enabled=%d ScreenDistance=%.2f ScreenWidth=%.2f ScreenVerticalOffset=%.2f RecenterKey=0x%X "
        "LoadAsiPlugins=%d ChainLoad=%zu VerboseLog=%d",
        g_config.enabled, g_config.screenDistance, g_config.screenWidth, g_config.screenVerticalOffset,
        g_config.recenterKey, g_config.loadAsiPlugins, g_config.chainLoad.size(), g_config.verboseLog);
    static const char* kModes[] = {"Flat", "Mono", "Native"};
    LOG("Config: VROutput=%d VRMode=%s PositionTracking=%d WorldScale=%.2f PoseLatencyFrames=%d FovScale=%.2f",
        g_config.vrOutput, kModes[g_config.vrMode], g_config.positionTracking, g_config.worldScale, g_config.poseLatencyFrames,
        g_config.fovScale);
    LOG("Config: RenderScale=%.2f RenderWidth=%d RenderHeight=%d PerEyeDlss=%d", g_config.renderScale, g_config.renderWidth,
        g_config.renderHeight, g_config.perEyeDlss);
    LOG("Config: FoveatedRendering=%d FoveatedInner=%.2f FoveatedMiddle=%.2f FoveatedOuterRate=%s FoveatedGaze=%d",
        g_config.foveatedRendering, g_config.foveatedInner, g_config.foveatedMiddle, g_config.foveatedOuterRate ? "4x4" : "2x2",
        g_config.foveatedGaze);
    LOG("Config: DisableMotionBlur=%d BlurMenuFixHoldMs=%d DisableTAA=%d PerEyeHistory=%d PerEyeObjectMotion=%d StereoJitterSync=%d PerEyeMotionVectors=%d", g_config.disableMotionBlur,
        g_config.blurMenuFixHoldMs, g_config.disableTaa, g_config.perEyeHistory, g_config.perEyeObjectMotion, g_config.stereoJitterSync,
        g_config.perEyeMotionVectors);
    LOG("Config: UncapCutsceneFPS=%d DisableDepthOfField=%d DisableCinematicEffects=%d DisableGraphicsDebuggerCheck=%d",
        g_config.uncapCutsceneFps, g_config.disableDepthOfField, g_config.disableCinematicEffects,
        g_config.disableGraphicsDebuggerCheck);
}

}  // namespace vr
