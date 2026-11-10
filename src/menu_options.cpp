#include "menu_options.h"

#include <windows.h>

#include <cstdio>
#include <string>

#include "version.h"
#include "config.h"
#include "log.h"
#include "lod_bias.h"
#include "render_size.h"
#include "sl_hooks.h"
#include "vrs.h"
#include "stereo_render.h"
#include "vrkit_menu_d3d12.h"
#include "vrkit_options.h"
#include "xr_screen.h"

namespace menu_options {
namespace {

vrkit::menu::Placement g_place;  // where the menu panel appears (World / Head anchor)
std::string g_vrMode;  // "Flat" / "Mono" / "Native", mirrors g_config.vrMode

const char* ModeName(int m) { return m == 3 ? "Warp" : m == 2 ? "Native" : m == 1 ? "Mono" : "Flat"; }

void ApplyMode() {
    int m = g_vrMode == "Warp" ? 3 : g_vrMode == "Native" ? 2 : g_vrMode == "Mono" ? 1 : 0;
    vr::g_config.vrMode = m;
    stereo_render::SetStereoMode(m);
    LOG("Menu: VRMode -> %s", ModeName(m));
}

void LogToMod(const char* m) { LOG("%s", m); }

void Register() {
    using vrkit::Options;
    Options& o = Options::Get();
    if (!o.All().empty()) return;
    auto& c = vr::g_config;
    g_vrMode = ModeName(c.vrMode);

    // ---- VR
    // Frame warp (VRMode Warp) is not offered: it ghosts moving characters in the headset (eye pairing over time, Nov 10).
    // The config value is still accepted (DevWarp) for development.
    if (g_vrMode == "Warp") g_vrMode = "Native";
    o.AddAction("VR", "Recenter view", [] { xr::RequestRecenter(); },
                "Make the current head position the centre (same as F8).");
    auto& mode = o.AddChoice("VR", "VRMode", "VR mode", &g_vrMode, {"Flat", "Mono", "Native"},
                             "Flat: the game on a screen. Mono: head-tracked, same image in both eyes. "
                             "Native: each eye rendered separately in gameplay (about 2x GPU).",
                             [] { ApplyMode(); });
    mode.choiceLabels = {"Flat screen", "Mono (head-tracked)", "Native stereo"};
    o.AddFloat("VR", "RenderScale", "Render resolution", &c.renderScale, 0.f, 2.f, 0.05f, 2,
               "Per-eye resolution in Mono / Native, relative to the headset's recommended size (1.00 = 100 %). "
               "Lower = faster, higher = sharper. 0 = keep the game's own resolution. Applied within a second.");
    // PositionTracking, PoseLatencyFrames and DisableMotionBlur are config.txt-only (always 1 / 1 / on in practice).
    o.AddFloat("VR", "WorldScale", "World scale", &c.worldScale, 0.1f, 10.f, 0.05f, 2,
               "Game units per meter of head movement and eye separation. Bigger = you feel smaller.");
    o.AddFloat("VR", "FovScale", "Field of view scale", &c.fovScale, 0.5f, 1.5f, 0.05f, 2,
               "Render a wider (or narrower) view than the headset's.");
    o.AddBool("VR", "DlssGhostingFixV2", "DLSS ghosting fix", &c.dlssGhostingFix,
              "Native: each eye gets its own DLSS history, camera history, motion vectors and jitter, and characters' "
              "motion is corrected. Removes the halo around characters and keeps DLSS sharp (also helps FSR / TAA).",
              [] {
                  vr::ApplyDlssGhostingFix();
                  LOG("Menu: DlssGhostingFix -> %d", (int)vr::g_config.dlssGhostingFix);
              });
    // MotionRebuild (0..3) is part of the DLSS ghosting fix; it stays at 3 (config.txt / dev command mvmode for tests).
    // StereoEdgeFix (stereo_edge.cpp) stays a config-only dev option: the halo is DLSS rejecting history at the
    // edges (user test Nov 4: dropping more history changes nothing), not wrong history it could be told to drop.
    static std::string s_preset;
    s_preset = c.dlssPreset == 10 ? "J" : c.dlssPreset == 11 ? "K" : c.dlssPreset == 12 ? "L" : c.dlssPreset == 13 ? "M" : "Default";
    auto& dp = o.AddChoice("VR", "DlssPreset", "DLSS model", &s_preset, {"Default", "K", "M", "L", "J"},
                           "Which DLSS model to use. Default: the game's / driver's choice. K: transformer, best quality. "
                           "M: transformer, faster. L: transformer for Ultra Performance. J: like K, a little less ghosting, "
                           "more flicker. Needs a recent DLSS (310+); otherwise DLSS falls back to its default.",
                           [] {
                               vr::g_config.dlssPreset = s_preset == "J" ? 10 : s_preset == "K" ? 11 : s_preset == "L" ? 12
                                                       : s_preset == "M" ? 13 : 0;
                               sl_hooks::ReapplyDlssOptions();
                           });
    dp.choiceLabels = {"Game default", "K (transformer, quality)", "M (transformer, fast)", "L (ultra performance)", "J"};
    // WarpEdgeBand (0..16) stays a config.txt / dev key: frame warp is not offered in the menu.
    o.AddFloat("VR", "LumaSharpen", "Sharpening (LumaSharpen)", &c.lumaSharpen, 0.f, 5.f, 0.05f, 2,
               "Sharpens the image in the headset (luma only, clamped so edges don't halo). 0 = off; anything above 0 "
               "sharpens, never blurs. 0.65 is the LumaSharpen shader's original default, 1 to 1.5 suits DLSS Performance, "
               "up to 5 (above 1 the edge limit grows too). Live.");
    o.AddFloat("VR", "TextureLodBias", "Texture sharpness (LOD bias)", &c.textureLodBias, -2.f, 0.f, 0.05f, 2,
               "Makes the game pick sharper texture mip levels (negative = sharper; -0.5 to -1 suits DLSS Performance). "
               "Too low makes textures shimmer. Live for most textures; the rest after a restart.",
               [] { lod_bias::Apply(); });

    // ---- Performance: fixed foveated rendering (VRS)
    o.AddBool("Performance", "FoveatedDlss", "Foveated DLSS", &c.foveatedDlss,
              "DLSS only on the centre of each eye (follows your eyes with eye tracking); the rest is the game's own image "
              "with a light TAA. Large performance gain with DLSS. Live.",
              [] { vr::ApplyFoveatedDlss(); });
    o.AddFloat("Performance", "FoveatedDlssSize", "Foveated DLSS centre size", &c.foveatedDlssSize, 0.2f, 1.f, 0.05f, 2,
               "Size of the DLSS centre as a fraction of each eye's image (smaller = faster).", [] { vr::ApplyFoveatedDlss(); });
    o.AddBool("Performance", "FoveatedDlssTaa", "Foveated DLSS periphery TAA", &c.foveatedDlssTaa,
              "Outside the DLSS centre: on = the render image upscaled and smoothed by a light TAA; off = plain upscale "
              "(cheaper, more shimmer). Live.", [] { vr::ApplyFoveatedDlss(); });
    o.AddBool("Performance", "FoveatedRendering", "Foveated rendering", &c.foveatedRendering,
              "Shade the edges of each eye's image at a lower rate (variable rate shading). Live.");
    o.AddBool("Performance", "FoveatedGaze", "Follow eye tracking", &c.foveatedGaze,
              "Move the full-detail centre with your eyes (headsets with eye tracking; otherwise it stays in the middle).");
    o.AddFloat("Performance", "FoveatedInner", "Full-detail radius", &c.foveatedInner, 0.1f, 1.5f, 0.05f, 2,
               "Radius of the full-detail centre, as a fraction of the half height.");
    o.AddFloat("Performance", "FoveatedMiddle", "Half-rate radius", &c.foveatedMiddle, 0.1f, 2.0f, 0.05f, 2,
               "Out to this radius 2x2 shading; beyond it the outer rate.");
    static std::string s_outer;
    s_outer = c.foveatedOuterRate ? "4x4" : "2x2";
    o.AddChoice("Performance", "FoveatedOuterRate", "Outer rate", &s_outer, {"2x2", "4x4"},
                "Shading rate outside the half-rate radius (4x4 is faster, softer).",
                [] { vr::g_config.foveatedOuterRate = s_outer == "4x4" ? 1 : 0; });

    // ---- Screen (Flat mode, menus, cutscenes without a gameplay camera)
    o.AddFloat("Screen", "ScreenDistance", "Screen distance (m)", &c.screenDistance, 0.5f, 20.f, 0.1f, 1,
               "How far away the virtual screen is (applied at the next recenter).", [] { xr::RequestRecenter(); });
    o.AddFloat("Screen", "ScreenWidth", "Screen width (m)", &c.screenWidth, 0.5f, 30.f, 0.1f, 1,
               "Width of the virtual screen.");
    o.AddFloat("Screen", "ScreenVerticalOffset", "Screen height offset (m)", &c.screenVerticalOffset, -3.f, 3.f, 0.05f, 2,
               "Moves the screen up or down (applied at the next recenter).", [] { xr::RequestRecenter(); });
    o.AddBool("Screen", "HudLayer", "HUD on its own panel", &c.hudLayer,
              "The game's HUD floats on a panel in front of you instead of being painted into the 3D view.").needsRestart = true;
    o.AddFloat("Screen", "HudDistance", "HUD distance (m)", &c.hudDistance, 0.3f, 10.f, 0.1f, 1,
               "How far away the HUD panel floats.");
    o.AddFloat("Screen", "HudWidth", "HUD width (m)", &c.hudWidth, 0.2f, 10.f, 0.05f, 2, "Width of the HUD panel.");
    o.AddFloat("Screen", "HudHeight", "HUD height offset (m)", &c.hudHeight, -3.f, 3.f, 0.05f, 2,
               "Moves the HUD panel up or down.");
    static std::string s_hudAnchor;
    s_hudAnchor = c.hudAnchor ? "World" : "Head";
    auto& ha = o.AddChoice("Screen", "HudAnchor", "HUD anchor", &s_hudAnchor, {"Head", "World"},
                           "Head: the HUD panel follows your view. World: it stays in place in the room, in front of where "
                           "you faced at the last recenter (F8 / Recenter view moves it in front of you again).",
                           [] { vr::g_config.hudAnchor = s_hudAnchor == "World" ? 1 : 0; });
    ha.choiceLabels = {"Follows head", "Fixed in room"};

    // ---- Menu panel placement (vrkit): MenuDistance / MenuWidth / MenuOffsetY / MenuAnchor
    vrkit::menu::AddPlacementOptions(o, g_place, "Menu");

    // ---- Game patches (applied at start-up)
    o.AddBool("Game", "UncapCutsceneFPS", "Uncap cutscene FPS", &c.uncapCutsceneFps,
              "Remove the 30 fps cap in real-time cutscenes and photo mode.").needsRestart = true;
    o.AddBool("Game", "DisableDepthOfField", "Disable depth of field", &c.disableDepthOfField,
              "Depth-of-field blur is uncomfortable in VR.").needsRestart = true;
    o.AddBool("Game", "DisableTAA", "Disable TAA", &c.disableTaa,
              "Replaces the game's TAA with a plain copy: sharper and no ghosting, but jagged edges. Only when the game's "
              "anti-aliasing is TAA (not DLSS / FSR / XeSS).");
    o.AddBool("Game", "DisableCinematicEffects", "Disable cinematic effects", &c.disableCinematicEffects,
              "Cutscene vignette, film grain and chromatic aberration.").needsRestart = true;
    o.AddBool("Game", "VerboseLog", "Verbose log", &c.verboseLog, "More detail in ff16vr.log.");
    o.AddAction("Game", "Debug: capture 4 frames (in 5 s)", [] { xr::RequestShotBurst(5000); },
                "Saves the next 4 frames sent to the headset as xrshot_N.bmp in the game folder, 5 seconds after you "
                "press it: close the menu and hold still. For bug reports.");
}

}  // namespace

bool Init(ID3D12Device* dev, int framesInFlight) {
    vrkit::SetLog(&LogToMod);
    g_place.distance = vr::g_config.menuDistance;
    g_place.width = vr::g_config.menuWidth;
    g_place.offsetY = vr::g_config.menuOffsetY;
    g_place.anchor = vr::g_config.menuAnchor;
    Register();
    vrkit::menu::Settings s;
    s.title = "FF16 VR " FF16VR_VERSION;
    s.configPath = vr::g_gameDir + L"\\config.txt";
    s.statusLine = [] {
        char b[160];
        snprintf(b, sizeof(b), "%s   %u fps   %s   VRS %s", ModeName(vr::g_config.vrMode), xr::VrFramesPerSecond(),
                 render_size::Status(), vrs::Status());
        return std::string(b);
    };
    return vrkit::menu12::Init(dev, framesInFlight, s);
}

const vrkit::menu::Placement& Placement() { return g_place; }

}  // namespace menu_options
