# Final Fantasy XVI VR (FF16VR)

A VR mod for the PC version of **Final Fantasy XVI**. It renders the game in full stereo with head tracking through
OpenXR (SteamVR), puts the HUD and menus on panels in front of you, and fixes the image problems that come from
rendering a game twice per frame.

**Version 0.9.2 (beta).**

## Features

- **Native stereo:** in gameplay each eye is rendered from its own position, with head tracking (rotation and
  position).
- **Flat screen for everything else:** menus, loading screens and cutscenes without a gameplay camera are shown on a
  large virtual screen. The opening cinematic of a new game also plays on the screen.
- **HUD on its own panel**, fixed in the room or following your head, in place of being painted into the 3D view.
- **Resolution control:** the game renders at the headset's per-eye size, scaled by a render-resolution setting.
- **Foveated rendering** (variable rate shading): full detail in the middle, coarser shading towards the edges.
  Eye-tracked on headsets that support it.
- **Motion blur kept off.** After every load the game turns its motion blur back on, which smears the whole VR image.
  The mod patches the game's blur strength so it stays off.
- **DLSS / FSR 3 ghosting fix (on by default):** per-eye upscaler history, camera history and jitter, and the motion
  vectors the upscaler receives are rebuilt from depth and each eye's own cameras. Removes the halo around characters
  and the soft textures of rendering a game twice per frame, with DLSS and with FSR 3.
- **Foveated DLSS:** DLSS only on the centre of each eye (eye-tracked on headsets that support it), the rest upscaled
  with a light TAA. A large performance gain with DLSS.
- **Image options:** DLSS model (preset) choice, sharpening, texture LOD bias, field-of-view scale.
- **In-headset settings menu** for all of the above.
- **Game patches:** uncapped cutscene frame rate, no depth of field, no cinematic vignette / film grain / chromatic
  aberration.

## Requirements

- Final Fantasy XVI (Steam, PC).
- A PC VR headset with **SteamVR set as the OpenXR runtime** (SteamVR → Settings → OpenXR). Tested with SteamVR 2.17.
- A fast GPU and CPU. Native stereo renders the game twice per frame: about twice the GPU work and noticeably more CPU
  work. Tested on NVIDIA RTX 5090 and RTX 5070; AMD and Intel GPUs are untested.

## Install

1. Download `FF16VR-0.9.2.zip` from the [Releases](../../releases) page.
2. Extract `dinput8.dll` and `openxr_loader.dll` into the game folder, next to `ffxvi.exe`
   (for example `...\steamapps\common\FINAL FANTASY XVI\`). The mod writes `config.txt` on first start.
3. Start SteamVR, then start the game from Steam as usual.

To uninstall, delete those two files, `config.txt` and `ff16vr.log`.

Upgrading from 0.9.0: replace the two DLLs; your `config.txt` and menu settings are kept. The ghosting fix is on
by default in 0.9.2 (the setting is now `DlssGhostingFixV2`; the old `DlssGhostingFix` line is ignored).

If you use other mods that ship their own `dinput8.dll` (ASI loaders), see `LoadAsiPlugins` / `ChainLoad` in
`config.txt`.

## Recommended game settings

- **HDR:** off.
- **Frame generation** (DLSS-G / FSR FG): off.
- **Upscaling:** DLSS or FSR 3. With the ghosting fix (default) DLSS Performance and Ultra Performance are both
  usable; FSR 3 Performance works well too.
- **Motion blur:** Off. The mod keeps it off after loads.
- **Frame rate limit:** unlimited or the highest setting. The headset paces the game.

## Controls

| Action | Key / button |
|---|---|
| Recenter the view, screen and HUD | **F8** (or "Recenter view" in the mod menu) |
| Open / close the mod's settings menu | **Insert**, or hold **L3 + R3** (both stick clicks) |

## Settings

All settings are in the in-headset menu and in `config.txt` (the mod writes a commented default file if it is
missing). The main ones:

| Setting | Default | What it does |
|---|---|---|
| VR mode | Native | Flat (game on a screen), Mono (head-tracked, same image in both eyes) or Native (stereo). |
| Render resolution | 1.00 | Per-eye resolution relative to the headset's recommended size. Lower is faster. |
| DLSS ghosting fix (`DlssGhostingFixV2`) | on | Per-eye DLSS / FSR history, camera history, jitter and rebuilt motion vectors. Removes the halo around characters. |
| DLSS model | Default | DLSS preset: K (quality), M (fast), L (Ultra Performance), J. Needs DLSS 310+. |
| Sharpening | 0 | LumaSharpen on the headset image. 0 = off; any value above 0 sharpens (never blurs). 0.65 is the LumaSharpen shader's original default, 1–1.5 is a good range for DLSS Performance; up to 5. |
| Texture sharpness | 0 | Texture LOD bias (negative = sharper textures). |
| Foveated DLSS | on | DLSS on the centre of each eye only (size 0.6), the rest with a light TAA. Big DLSS speed-up. |
| Foveated rendering | off | Coarser shading towards the edges (variable rate shading). Gains little on top of Foveated DLSS. "Follow eye tracking" is off by default. |
| HUD anchor | World | World: the HUD panel stays in place in the room. Head: it follows your view. |
| HUD distance / width / height | 1.5 / 1.6 / 0 m | HUD panel placement. |
| Screen distance / width | 2.5 / 3.2 m | Virtual screen for menus and non-gameplay scenes. |
| World scale | 1.0 | Game units per meter of head movement. |

`config.txt` only (no menu entry): `PositionTracking` (1), `PoseLatencyFrames` (1), `DisableMotionBlur` (1) and the
start-up patches' details.

## What's new in 0.9.2

- The DLSS / FSR 3 ghosting fix is complete and on by default: each eye's upscaler now gets motion vectors rebuilt
  from depth and that eye's own cameras, so the halo around characters is gone and textures stay sharp (also at DLSS
  Ultra Performance). The two "one eye noisy while moving the head" bugs are fixed (per-eye history swap of the
  upscaler's own copy; eye classification of camera buffers while the head moves).
- Foveated DLSS (Performance page): DLSS on the centre of each eye, light TAA outside. Default on for new installs.
- DLSS model choice, sharpening, texture LOD bias and field-of-view scale (VR page).
- Menu: "Recenter view" is the first entry; position tracking, pose latency and motion blur moved to `config.txt`
  (always-on settings); the debug frame capture is on the Game page. New installs default to Foveated DLSS on and
  foveated rendering off (the faster combination); an existing `config.txt` keeps its values.
- The frame-warp mode is not offered in this release.

## Known issues (beta)

- Paging quickly through some pause-menu pages can still briefly switch the HUD between the screen and the panel.
- The new-game opening cinematic plays on the flat screen; any subtitles in that stretch are hidden.
- Native stereo roughly doubles the rendering cost; expect lower frame rates than flat play.
- With the DLSS ghosting fix off, a faint halo is visible around characters with DLSS / FSR.
- Frame generation (DLSS-G / FSR FG) is not supported; keep it off.

## Troubleshooting

The mod writes `ff16vr.log` in the game folder. If something goes wrong, turn on "Verbose log" (Game page of the mod
menu, or `VerboseLog = 1` in `config.txt`), reproduce the problem, and include the log with your report. The log is
replaced each time the game starts.

## Building from source

The mod is a `dinput8.dll` proxy written in C++20. `build.sh` cross-compiles it on Linux with clang-cl and lld-link
(LLVM 22+) against an MSVC / Windows SDK sysroot:

```
TOOLCHAIN=<msvc + windows sdk sysroot> LLVM=<llvm root> ./build.sh
```

The output is `build/dinput8.dll`. `openxr_loader.dll` is the Khronos OpenXR loader (x64).

Source layout:

- `src/` — the mod (OpenXR output, stereo rendering, camera hooks, HUD layer, game patches, DLSS / TAA fixes).
- `vrkit/` — the in-headset menu and input helpers (uses Dear ImGui).
- `third_party/` — OpenXR headers, RenderDoc in-app API header, MinHook.
- `docs/` — development notes: how the game's renderer works and how each fix was found.

## Third-party code

- [Dear ImGui](https://github.com/ocornut/imgui) — MIT license (`vrkit/third_party/imgui/LICENSE.txt`).
- [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) headers and loader — Apache 2.0.
- [RenderDoc](https://renderdoc.org) in-app API header — MIT license.
- [MinHook](https://github.com/TsudaKageyu/minhook) — BSD 2-clause (`third_party/minhook/LICENSE.txt`).
- Frame-warp shader (development mode only) derived from Rift Apart VR by scubasteve2365 — MIT (`third_party/riftapartvr/LICENSE`).

Final Fantasy XVI is a trademark of Square Enix. This project is not affiliated with or endorsed by Square Enix.
