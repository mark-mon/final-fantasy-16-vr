# Final Fantasy XVI VR (FF16VR)

A VR mod for the PC version of **Final Fantasy XVI**. It renders the game in full stereo with head tracking through
OpenXR (SteamVR), puts the HUD and menus on panels in front of you, and fixes the image problems that come from
rendering a game twice per frame.

**Version 0.9.0 (beta).**

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
- **Optional DLSS ghosting fix:** per-eye DLSS history, camera history, motion vectors and jitter. It removes the faint
  halo around characters when using DLSS / FSR.
- **In-headset settings menu** for all of the above.
- **Game patches:** uncapped cutscene frame rate, no depth of field, no cinematic vignette / film grain / chromatic
  aberration.

## Requirements

- Final Fantasy XVI (Steam, PC).
- A PC VR headset with **SteamVR set as the OpenXR runtime** (SteamVR → Settings → OpenXR). Tested with SteamVR 2.17.
- A fast GPU and CPU. Native stereo renders the game twice per frame: about twice the GPU work and noticeably more CPU
  work. Tested on NVIDIA RTX 5090 and RTX 5070; AMD and Intel GPUs are untested.

## Install

1. Download `FF16VR-0.9.0.zip` from the [Releases](../../releases) page.
2. Extract `dinput8.dll`, `openxr_loader.dll` and `config.txt` into the game folder, next to `ffxvi.exe`
   (for example `...\steamapps\common\FINAL FANTASY XVI\`).
3. Start SteamVR, then start the game from Steam as usual.

To uninstall, delete those three files and `ff16vr.log`.

If you use other mods that ship their own `dinput8.dll` (ASI loaders), see `LoadAsiPlugins` / `ChainLoad` in
`config.txt`.

## Recommended game settings

- **HDR:** off.
- **Frame generation** (DLSS-G / FSR FG): off.
- **Upscaling:** DLSS Performance or FSR 3 Performance work well. Ultra Performance looks soft in VR.
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
| DLSS ghosting fix | off | Per-eye DLSS / camera history. Removes the halo around characters; the rest of the image is slightly softer. |
| Keep motion blur off | on | Stops the game's motion blur from coming back after loads. |
| Foveated rendering | on | Coarser shading towards the edges (outer rate 2x2 or 4x4). "Follow eye tracking" is off by default. |
| HUD anchor | World | World: the HUD panel stays in place in the room. Head: it follows your view. |
| HUD distance / width / height | 1.5 / 1.6 / 0 m | HUD panel placement. |
| Screen distance / width | 2.5 / 3.2 m | Virtual screen for menus and non-gameplay scenes. |
| Position tracking | on | Head movement moves the camera (off: rotation only). |
| World scale | 1.0 | Game units per meter of head movement. |
| Pose latency (frames) | 1 | Raise it if the world wobbles when you turn your head. |

## Known issues (beta)

- Paging quickly through some pause-menu pages can still briefly switch the HUD between the screen and the panel.
- The new-game opening cinematic plays on the flat screen; any subtitles in that stretch are hidden.
- Native stereo roughly doubles the rendering cost; expect lower frame rates than flat play.
- With the DLSS ghosting fix off, a faint halo can be visible around characters with DLSS / FSR.

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
- `third_party/` — OpenXR headers, RenderDoc in-app API header.
- `docs/` — development notes: how the game's renderer works and how each fix was found.

## Third-party code

- [Dear ImGui](https://github.com/ocornut/imgui) — MIT license (`vrkit/third_party/imgui/LICENSE.txt`).
- [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) headers and loader — Apache 2.0.
- [RenderDoc](https://renderdoc.org) in-app API header — MIT license.

Final Fantasy XVI is a trademark of Square Enix. This project is not affiliated with or endorsed by Square Enix.
