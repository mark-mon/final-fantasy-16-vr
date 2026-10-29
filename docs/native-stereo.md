# FF16 native stereo: recon and feasibility (2026-10-25)

## Frame flow (all on the main thread, entry ffxvi.exe+0xC4D48A)
- Frame function +0x9B0EB0 (`rdi` = frame object):
  - `[rdi+0x5B0]` = renderer
  - +0x9B117D `renderer->vtable[0x38]`, then +0x9B1187 `+0xA2F858(gfxDevice @0x142743670)`
  - **+0x9B119B `renderer->vtable[0x40]` = scene Render()**: view updates and render-job building, about 2.7 ms CPU in gameplay
  - task-list loop, `+0xA2FAE8(dev)`, `+0x9A2E80`, then +0x9B1202 `+0xA2FE00(dev)` -> Present (+0xA2FE88 -> +0xA2BAF0 -> Streamline -> our hook)
- Gameplay at the default settings: 86 fps (11.6 ms per frame). Per frame:
  - Render() 2.7 ms;
  - Render() end to Present 1.5 ms;
  - Present 0.14 ms;
  - Present to next Render() 7.2 ms (game update plus the frame-start fence wait).
- Main-view updates match presents 1:1. Two RenderView updates per frame in gameplay; one of them is the main view.

## Hook: src/stereo_render.cpp
- Patches the call at +0x9B1198 (`mov rax,[rcx]; call [rax+40h]`) with `call stub; nop` -> `RenderHook(renderer)`.
- Dev trigger: `<game>\ff16vr_render2.trigger` containing `1` or `0` turns render-twice on or off live. `stereo_render::CurrentPass()` returns 0 or 1.
- Logs phase timings (lines starting `STEREO Render():`) every 10 s, or every 2 s while render-twice is on.

## Result: calling Render() twice per frame with the same camera (commands 075 and 076)
- The game survives and recovers fully when switched back to once.
- The second call costs the same CPU time as the first (2.9 ms).
- **Frame rate drops from 86 to 16-25 fps.** Present to next Render() goes from 7 ms to 33-57 ms. The GPU frame-start fence wait balloons, so the GPU frame now costs about 4x, not 2x.
- **The image washes out**: blown-out white or grey, with overexposed fog (screenshots r2_twice_a and r2_twice_b). Likely causes: per-frame state advancing twice (auto-exposure, TAA/temporal history, frame index), and/or per-frame upload buffers being overwritten while the GPU still reads them.
- Conclusion: a naive whole-scene double call is not viable as it is. Options:
  1. Make the second pass light. Identify the per-frame "advance" state (frame counters, exposure, temporal history, ring indices +0xE75/+0xE76) and run it once per frame, keeping per-eye history.
  2. Render the second eye as an extra view inside the scene renderer (viewports `[this+0x38+8i]` count +0x1AF7, view slots `[this+0x58+8i]` count +0x1D9C). The engine already handles several views per frame.
  3. Find out why the GPU cost is 4x. A RenderDoc capture with render-twice on (two scene passes in one capture) would show whether pass 1's work is redone, whether the shadow and other caches are invalidated, or whether it waits on something.

## Agent notes
- If the Claude desktop window is in front of the game, Send-FF16Key refuses keys and the boot stalls before the title (074). Commands 075 and 076 minimize the Claude window for the run and restore it afterwards.
- Run test builds with `VROutput=0` and `FrameProbe=1`, so SteamVR is not started (null-HMD Room Setup kills the game).

## Update (04:40): what does work for native stereo
- Calling Render() twice per frame does NOT give a second frame. The second call appends a second copy of every draw
  to the same frame (G-buffer pass 1948 + 1020 draws, forward/transparent and fog drawn twice), which is the
  washed-out image. The capture analysis is in outbox\rdc_ff16_frame9420_all.zip.
- **Calling the whole frame function twice per main-loop iteration works** (patched call at ffxvi.exe+0x9B2C4F ->
  FrameHook). The game clock is frozen for the second call by patching the timer call at +0x9B0F8A (TimeHook
  returns the previous value, so dt = 0).
  - Result (command 082): a clean image, the game is stable, and the GPU cost is exactly 2x (43 stereo pairs/s
    against 86 flat frames/s).
  - stereo_render::CurrentPass() is 0 for the left eye and 1 for the right.
- Bug found: the Render() timing wrapper reset the pass variable to 0 inside every frame (fixed in 5D61D0CC517F).
- Gating:
  - Both eyes are rendered only once the gameplay camera (IsMainView) has updated on 120 consecutive frames.
  - Logos, title and menus stay single-frame (the logo sequence stalls if every tick is rendered twice).
- OpenXR (xr_screen.cpp NativePresent):
  - One double-wide swapchain image per OpenXR frame.
  - The eye of each present comes from the camera sample it was rendered with, or else from the pass parity
    (pass + PoseLatencyFrames) & 1. With lag 1, the present on pass 1 carries the left eye and the next pass-0
    present carries the right eye.
  - An OpenXR frame opens on a left eye and closes on the right.
  - Each view is submitted with its own eye pose (head pose * eye-in-head pose) and a symmetric per-eye FOV.
- Camera: ApplyHmd uses the per-eye pose and FOV when native is active. HeadSample carries eye[2] and eyeTan[2][4].
- Dev tools:
  - ff16vr_render2.trigger: 0 = normal, 1 = Render twice, 2 = frame twice, 3 = frame twice without presenting pass 0.
  - ff16vr_camtest.trigger "p0 40 0": test offset on pass 0 only.
  - ff16vr_xrshot.trigger: saves the submitted OpenXR image to xrshot_N.bmp.
- Known issues and to do:
  - The game's camera motion blur and TAA see the eye change between passes as camera motion. Turn Motion Blur off
    in the game settings. TAA/DLSS history still alternates between eyes (later: per-eye history).
  - Native without a gameplay camera falls back to mono / the theater screen.
- Automated tests:
  - The SteamVR null headset no longer triggers Room Setup: C:\Program Files (x86)\Steam\config\
    chaperone_info.vrchap with universes 0..20 is permanent (user request; SteamVR found universe 2 in it).
  - But commands 089-091 never got past the title on the null headset (also in Mono). SteamVR shows "Standing by"
    and Continue does not load. Untested in the real headset so far.

## Update (06:35): in-headset menu, theater fallback, automated boot
- **In-headset menu (vrkit, D3D12):**
  - New files: vrkit/vrkit_menu_d3d12.* (ImGui 1.91.5 imgui_impl_dx12) and src/menu_options.* (FF16 options: VR, Screen and Game pages).
  - xr_screen.cpp MenuFrame() draws a head-locked quad layer, in VIEW space, in every path.
  - Opens with Insert, or by holding L3 + R3. Saves to config.txt on close.
  - VRMode switches live (stereo_render::SetNativeMode).
- **Pad blocking** goes through the ffxvi.exe import of XInputGetState (XINPUT9_1_0), patched in vrkit_input.cpp with VRKIT_IAT_HOOKS (no MinHook).
- **Theater fallback:** when the gameplay camera sample is stale (more than PoseLatencyFrames+3 presents old), the frame goes to the theater quad instead of the head-tracked projection. This covers the pause menu, loading screens and the title. The log shows "View: ..." lines.
- **Input paths:**
  - The game reads the pad through XInput.
  - DirectInput is only used for the mouse (GUID_SysMouse, GetDeviceState with 20 bytes).
  - The keyboard is probably window messages or raw input.
- **Automated boot:** ff16tools Start-FF16GameplayPad.
  - It presses `A Enter` through ff16vr_pad.trigger (dev_pad.cpp, via XInput and DirectInput) every few seconds, starting 15 s after launch.
  - Earlier presses stalled the game at the logos; presses only after the title appeared were ignored.
  - Run 101 reached Native gameplay, opened the pause menu with Start (theater screen), and closed it again.
- **SteamVR on this PC (permanent):** chaperone file, pauseCompositorOnStandby=false, turnOffScreensTimeout=86400. SteamVR is the default OpenXR runtime (set by the user).

## Update (08:15): HUD on its own layer
- hud_layer.cpp: the UI composite (~30 draws through a UAV, premultiplied "over") goes into a swap-chain-sized
  R10G10B10A2 image. The mod hooks CreateUnorderedAccessView and points that UAV at its own transparent RGBA8 texture.
- Each Present: copy the HUD out for the VR HUD layer, clear it, and after the VR side has taken the scene, blend the HUD
  back onto the backbuffer so the desktop still shows it.
- VR: a head-locked quad (HudDistance / HudWidth / HudHeight); in theater / Flat it sits on the screen quad.
- Verified in run 110. Saved as release\dinput8-hudonly.dll (sha256 BAD9DC283589).

## Update (09:20): render resolution and aspect (runs 111-116)
- **How the game sizes its rendering:** it renders at its window's client size. The resolution setting (3840x2160 in
  the save; shared through Steam cloud) only sets the initial window. In borderless mode it follows WM_SIZE with
  ResizeBuffers and resizes all its targets (run 113: 1600x1200, 2400x1800, 1200x1600 all worked, also larger than
  the monitor). Settings live in the save (system.png), not in a text file.
- **render_size.cpp (new):**
  - In Mono / Native, once the gameplay camera has been stable once (stereo_render::GameplayReached, so the title and
    first load keep the normal 16:9 window), the window is resized (SWP_ASYNCWINDOWPOS, at the monitor corner) to the
    per-eye render size.
  - Per-eye size: the symmetric frustum the camera hook renders (max tangents of both eyes x FovScale), at the pixel
    density of the headset's recommended eye image (xrEnumerateViewConfigurationViews) x RenderScale. Width : height =
    tanH : tanV, so the FOV is exact. Clamped to half the max swapchain width (double-wide stereo swapchain).
  - config: RenderScale (default 1.0, 0 = keep the game's size), RenderWidth / RenderHeight (direct override).
    Menu: VR page "Render resolution" (live); the status line shows the per-eye size.
  - Switching to Flat (or RenderScale 0) puts the window back to the monitor size and removes the patches below.
  - The game takes the projection aspect from its resolution setting (1.777), not the window: camera_hook writes the
    render size's aspect to view+0xE40 while the VR size is applied.
  - **16:9 fit patches:** at a square size the game letterboxed the 3D view into a 16:9 band (run 115). Three helpers
    fit rectangles to 16:9 (compare w/h with 16/9 at ffxvi.exe+0x20A041, +0x20A733, +0x69C7C4); while the VR size is
    applied they are skipped (patches by signature, reverted on restore). Run 116: full square stereo image, HUD in
    the centre 16:9 band of its (transparent) square texture. The pause-menu background still renders letterboxed
    on the theater screen (fine for now).
  - Logs: "Render size:", "Window resize:", "Perf:" every 10 s (game presents/s, VR fps, render size),
    "Size probe:" (window / swap-chain calls with game call sites).
  - Dev trigger ff16vr_resize.trigger: "W H" (window client size), "scale X" (RenderScale live), "fit MASK".
- xr_screen: logs the recommended eye size and the per-eye FOV tangents; a pending native frame is ended (empty)
  before the stereo swapchain is recreated (fixes one XR_ERROR_HANDLE_INVALID after a resize).
- **Performance, RTX 5070, null headset (recommended 1852x2056, 90x90 deg per eye), Native, Hideaway kitchen:**
  - 2048x1152 per eye (old behaviour on this PC, FOV 121x90): 37 VR fps.
  - 2056x2056 per eye (RenderScale 1.0, full square): 30 VR fps.
  - 1440x1440 (0.7) and 2880x2880 (1.4) were measured while the view was still letterboxed (46 / 27 fps), so only
    roughly comparable.
  - Native renders every frame twice (CPU and GPU), so expect about half of the flat frame rate at the same pixels.
- Builds: release\res114..res116 (test builds); release\dinput8-resolution.dll = this step.
- Agent: Start-FF16GameplayRetry now prints the log line that counted as gameplay ("rendering both eyes" or
  "(first gameplay)").
- Next: ghosting (per-eye TAA/DLSS history and previous-frame camera), cheaper second eye (shadows), DLSS mode.

## Update (Oct 26, 01:40): HUD layer bugs found in the user's tests (A: machine, 5090, lighthouse, 4K, DLSS)
- **Scene in the HUD panel (no stereo, "flat screen the size of the HUD"):** on the A: machine a second UAV on the
  same swap-chain-sized R10G10B10A2 target is created from another thread (likely DLSS writing the upscaled scene into
  the target the UI then composites onto). The HUD hook redirected that one too, so the whole scene went into the
  head-locked HUD panel. Fix (hud_layer.cpp): only the UI pass's UAV is redirected; UAVs created with an upscaler
  module in the call stack (nvngx / sl.dlss / FidelityFX / XeSS), or from another thread than the first redirected
  one, are left alone and logged ("NOT redirected", with the module chain).
- **Washed-out logos / HUD (HDR on SDR look):** the UI composite image is HDR10 PQ-encoded; the game's final pass
  converts PQ -> SDR (ICtCp tone map, gamma 2.2). The HUD layer now applies the same conversion (pixel shader ported
  1:1 from the capture, on un-premultiplied colour) before the VR HUD layer and the desktop composite.
- Build: release\dinput8-hudfix.dll (includes the resolution work, VRAM logging, log rotation).
- Bisect builds: release\bisect (04:33 "looked great", 06:19 imgui + theater: confirmed good by the user).

## Update (Oct 26, 02:30): per-eye DLSS history (sl_hooks.cpp)
- User test of hudfix: stereo back, HUD separate, colours right. Remaining: image goes soft when the head is still
  (all upscalers and plain TAA: temporal history alternates between eyes), plus fuzzy ghosting around characters
  (like UEVR synced-sequential without its "ghosting fix": motion vectors from the other eye's previous camera).
- The game loads sl.interposer.dll and resolves sl* functions with GetProcAddress. The mod patches ffxvi.exe's
  GetProcAddress import and wraps slGetNewFrameToken, slSetConstants, slSetTag, slEvaluateFeature,
  slAllocateResources, slFreeResources, slGetFeatureFunction (-> slDLSSSetOptions).
- Right-eye frames (stereo_render::FrameEye() == 1, set before each pass) go to viewport + 0x100 for constants and
  DLSS evaluate; options and allocation are mirrored to that viewport; tags are set on both (shared targets).
- Config / menu: PerEyeDlss (default on). Logs "SL: ..." (wrapped functions, first mapped calls, evaluation counts
  per eye, token/frame-eye disagreements).
- Not covered yet: the game's own TAA and FSR3 (need the history buffers from a capture) and per-eye previous
  camera for motion vectors (the ghosting).

## Update (Oct 26, 11:00): blur after loading = motion blur left on (runs 131-157)
- **Cause:** with Motion Blur Off the game still runs its motion-blur passes, but divides the shutter scale by 1000
  (tile-pass constants row 3 = 0.0035, 0.105 instead of 3.5, 105; frame constants row 11 .w 1000 instead of 1).
  After a load it builds the motion-blur parameters at full strength and only re-applies the Off scale when the
  pause menu closes. Flat play hides it; in stereo the eye offset is read as motion (tile max velocity 2.07 instead of
  0.002) and the whole image smears. Matches the user's "blurry until I open and close the menu".
- **What did not work:**
  - Patching the heap copies (blocks of six floats: shutter, max, 3, 10, 4, 16; about 300 per level). They are
    rebuilt from code every frame, about 130 per second, so a background patcher always lags (runs 136-139).
  - Looking for a stored template or setting: no full-strength copy anywhere in memory (139), and the 1.0 -> 1000
    memory diff only found noise (140).
  - Patching the constant buffer on the GPU side (root CBV / CBV / copy / root-constant hooks, Map tracking per
    resource vtable, device hooks behind Streamline's proxy; runs 141-155). Every root CBV address was outside the
    buffers the mod saw mapped. The capture says the tile pass's constants are a root CBV into a 48 MB upload ring,
    but that ring's mapping was never seen. Code kept in scratch for reference only.
- **Fix (motion_blur.cpp, DisableMotionBlur = 1, default on; Game page of the menu):** once the gameplay camera has
  been live for 3 s in a new stretch (after loads, menus, cutscenes), a low-priority thread counts full-strength
  parameter blocks in the heap (about 1.5 s). If there are 80 or more (blurry: 255-340, after the fix 0-50 stale ones),
  it does what the player does: Start, 1 s, Start through the XInput hook. It checks again after 5 s of gameplay, at
  most twice per load.
  - Run 156: 280 -> 48 -> 3 blocks (700 ms gap, threshold 20). All four XR shots after the load were sharp
    (Laplacian variance 271-286, vs 209 blurry and 286 after a manual menu round trip).
  - Run 158 (final settings, with VRS on): 255 -> 5 blocks after one round trip; sharp shots; 40 VR fps.
  - Build: release\dinput8-mbfix-vrs.dll (= res158). Perf line: "motion blur off (...)".
- Side effect: the pause menu flashes once (maybe twice) about 4 s after each load, and only when the game really
  left motion blur on.
- Dev tools added (trigger files, idle otherwise):
  - mem_scan.cpp (ff16vr_memscan.trigger: "find a b c d", "snap" / "diff", "scan", "force").
  - watch.cpp (ff16vr_watch.trigger: hardware write watchpoint on a parameter block, logs the writer and call stack).
- Menu LB/RB "big screen / small screen": not reproduced on the test PC (137). Switching pages in the pause menu with
  RB / LB logged no stereo switches, resizes or view changes. RB also switched the character (Clive -> Torgal).
  Needs a log from the user's machine.
- Foveated rendering (vrs.cpp, Performance page): 36 -> 40-41 VR fps on the 5070 at 2056 x 2056 per eye.

## Update (Oct 26, 16:20): TAA off option (taa.cpp, DisableTAA, default off; Game page, live)
- The game's TAA is one compute pass, `ProcessTAACandidatesFullScreenCS` (RenderDoc hash ae5132ad67). It reads the
  HDR scene t5 (RGBA16F), history t3 (R11G11B10), depth, velocity and the anti-flicker edge mask t0, and writes u0
  (RGBA16F). `copyHistoryCS` then stores u0 as the next history. Post chain from run 159 (capture 3532,
  `agent\outbox\rdcp_ff16_frame3532\post_shaders.txt`):
  - resolveUvSpaceVelocity
  - tileMax / neighborMax (motion blur)
  - uberPrepare / dofLite
  - histogram and illuminant
  - antiFlickerEdgeDetection
  - TAA
  - copyHistory
  - composite (motion blur)
  - bright / bloom ...
- The mod recognises the pipeline at creation by its shader container digest. It hooks CreateComputePipelineState,
  CreatePipelineState and pipeline-library loads, on both Streamline's proxy and the runtime device, wrapped at
  Streamline's D3D12CreateDevice. It then builds a pass-through pipeline on the same root signature: cs_5_1 compiled
  at run time with d3dcompiler_47, copying t5 to u0. Each thread covers the same pixel block as the original; the
  group count comes from the frame constants, row 30 .zw.
- With DisableTAA on, SetPipelineState (command-list slot 25) gets the pass-through pipeline instead. Run 160:
  pipeline found via the pipeline library, about 2 swaps per frame (one per eye). Sharpness went from 213-226 to
  326-398; hair edges alias.
- The camera jitter stays: about 0.5 px, the projection [0][2] / [1][2] and frame constants row 47. Its CPU source
  isn't found yet (no Halton table in the exe), so edges shimmer slightly.
- No effect with DLSS / FSR / XeSS, which replace this pass. Dev trigger: ff16vr_resize.trigger "taaoff 1".
- Build: release\dinput8-taaoption.dll (= res160; includes the motion-blur fix and VRS).

## Update (Oct 26, 17:00): per-eye previous camera (camera_hook.cpp, PerEyeHistory, default on, VR page, live)
- Checkpoint before this work: release\checkpoint-2026-10-26 (the build of run 160 + source zip).
- Run 161: RenderView dumped over 8 stereo passes, at pass start and in the view hook.
  - The camera state is in +0x030..+0x640 and +0xF00..+0x1000: current matrices +0x0B0 / +0x2D0 / +0x490 / +0x590,
    positions +0x120 / +0x340 / +0x380 / +0xF70 / +0xFC0, camera delta +0xFB0.
  - +0x4D0 and +0xF00 hold the previous frame's matrix. In stereo that is the other eye's.
  - Nothing in these ranges is written before the view hook; RenderView::Update copies current -> previous after it.
- Fix:
  - At the start of each frame pass the view's final state belongs to the previous pass's eye, so it is saved for
    that eye.
  - In the view hook the current eye's saved ranges (+0x030..+0x640, +0xF40..+0x1000) are put back before Update
    continues. The game then copies the same eye's last camera into "previous" and computes its deltas from it.
  - The snapshots reset when stereo stops or the main view changes.
- Run 162 (motion blur left on after the load, TAA on): PerEyeHistory off 211-219, on 236-248, Laplacian variance
  in XR shots. After a menu round trip with it on: 255. The motion-blur smear from the eye offset is mostly gone, as
  the eye offset no longer shows up as camera motion.
- Still shared between the eyes: the game's own TAA history texture. Its history is the other eye's image, so some
  blur or ghosting can remain in TAA mode.
  - DLSS with PerEyeDlss has separate per-eye history (viewport + 0x100) and now gets correct motion vectors.
  - DisableTAA removes TAA history entirely.
- Build: release\dinput8-pereyehistory.dll (= res162). Dev trigger "peh 0/1"; dump trigger ff16vr_viewseq.trigger.

## Update (Oct 26, 19:40): user test of res162 on the 5090; fixes (runs 163-165)
- **Blur still after loads / RenderScale changes on the 5090.** The watcher found 0 parameter blocks there: the block
  pattern (shutter, 30 x shutter, 3, 10, 4, 16) depends on settings.
  - It now accepts any four whole numbers with at least 3 distinct values. Uniform tails such as 1, 1, 1, 1 /
    20, 1, 1, 1 were false positives at 30 x shutter. About 45 false hits remain, under the threshold of 80.
  - It checks on each gameplay stretch, after every ResizeBuffers (RenderScale changes rebuild the post chain and
    lose the Off scale) and every 30 s.
  - Run 165: 311 blocks -> menu round trip -> 48; scale 0.6 -> 269 -> round trip -> 45.
- **VRS "whole screen blocky" on the 5090.** The scene is drawn into a viewport sub-rect of a larger target
  (resolution scale / DLSS input in the top-left corner), and the shading-rate image was centred on the texture.
  - Now: RSSetViewports is hooked too, and the rates are laid out around the viewport.
  - Images are keyed by (target, viewport, centre) and built asynchronously on the mod's own queue; until one is
    ready the closest ready one is used.
  - Run 163: the scene viewport at 50 % (tiles 0,0-64,64 of a 2056 target) has its centre at full rate; corners 4x4.
- **Eye-tracked foveation (FoveatedGaze, Performance page, default on).**
  - XR_EXT_eye_gaze_interaction is enabled when offered, with a gaze pose action when the system reports eye
    tracking.
  - The centre follows the gaze, projected into each eye's symmetric frustum and quantised to 2 tiles. Without
    eye tracking it stays fixed.
  - The null headset reports no eye tracking, so the gaze path is untested.
- The game has its own VRS option: the mod overrides the shading-rate image on scene passes and clears it elsewhere.
  Turn the game's option off while using the mod's.
- Build: release\dinput8-vrsfix.dll (= res165; includes PerEyeHistory, DisableTAA).

## Update (Oct 26, 19:50): quad views test (QuadViews / QuadInset, VR page, live) and blur-fix option name
- The user confirmed res165 fixes VRS. They saw the automatic menu round trip fix the blur and asked for it to be a
  menu option: "Blur render menu fix" (config key DisableMotionBlur; BlurRenderMenuFix is accepted too).
- Quad views:
  - stereo_render runs 4 frame passes per tick: 0 / 1 left / right wide, 2 / 3 left / right inset.
  - The inset camera has the eye's tangents x QuadInset (default 0.5), symmetric, same pose.
  - Render size x QuadInset, so the inset gets the RenderScale density and the wide view half of it.
  - OpenXR: a 4-slice array swapchain; projection layer 1 = wide views, projection layer 2 = inset views on top.
    There is no edge blending yet.
  - PerEyeHistory keeps one history per view.
  - PerEyeDlss uses one DLSS viewport per view (+v * 0x100); tags, options and allocation are mirrored to 3 extra
    viewports.
  - VRS is skipped on the inset views.
  - The view parity fallback for the delayed present is now (pass - lag) mod views.
- Run 167 (5070, null headset, RenderScale 1):
  - stereo 2056 per eye: 36 VR fps;
  - quad 1032 per view: 26 VR fps (105 game frames/s).
  - On the 5070 the per-view fixed cost dominates. The 5090 / faster CPU may differ; the user is to A/B test it.
  - The blur fix triggered on both quad on / off (resolution changes) and fixed the blur each time.
- Build: release\dinput8-quadviews.dll (= res167).

## Update (Oct 26, 20:45): quad views removed
- User on the 5090: quad views cost a lot of fps and raised CPU use by about 50%. Dropped.
- The code was removed: the QuadViews / QuadInset config keys and menu items, the 4-pass loop, the inset FOV, the
  inset layer and 4-slice swapchain, the extra DLSS viewports, and the inset VRS skip.
- Kept: parity (pass - lag) mod 2, per-view machinery sized for 2.
- Build res168 = release\dinput8-noquad.dll. Not run yet: the PC was busy with another game.
- Conclusion: the per-view CPU cost is the limit. The next lever is cutting work in the second-eye pass.

## Update (Oct 27, 02:00): character ghosting cause; CPU profile; cleanup (runs 168-187)
- **Character halo, cause found (runs 168 / 170):** in the TAA / DLSS velocity input, characters and other animated
  objects carry ~20 px of false horizontal motion (+x in one eye, -x in the other; background ~0.1 px).
  - G-buffer VS 547b91b6e2 / PS 73de4028fd: current position x cCamera +416 (current view-projection), previous
    skinned position (bone matrices t0) x cCamera +1440 (the *previous* CameraParam, +0x400). Objects without
    a previous pose use the camera-only reprojection at +2048 instead.
  - cCamera (0x900, b0): CameraParam current +0 / previous +0x400; row 37 = camera node position (w 1),
    row 38 = eye position (w = near 0.2), rows 11..13 = camera-to-world 3x4. In stereo the previous CameraParam
    holds the other eye (63 mm away); the reprojection at +2048 is right (PerEyeHistory).
- **Fix attempts that did not work (all removed again):**
  - Memory scan of CPU-side CameraParam copies (runs 171 / 172).
  - The RenderView camera ring (+0x610 + i * 0x400, indices +0xE75 / +0xE76, copied to the cbuffer at
    +0xAA44EE in RenderView::Update): runs on job threads, and patching the "previous" block had no effect on the
    G-buffer constants (runs 173 / 176 / 180 / 181).
  - Patching the upload-heap cCamera slots at ExecuteCommandLists (runs 177 / 179). The slots move (per-frame
    allocator); the live ones were only found under RenderDoc.
  - A hardware write watch on a slot (runs 182-186). Without RenderDoc no live slot was found, only stale ones.
  - Next idea: find the cCamera writer from a RenderDoc-free angle (e.g. hook the upload allocator), or correct
    the velocity buffer on the GPU before TAA / DLSS (re-project dynamic pixels with the same-eye matrix).
- **CPU (runs 168 / 169):** stereo uses ~126-170 ms of CPU per tick vs ~50 single.
  - Game thread per pass: ~6 ms CPU, the rest waiting in end-of-frame (+0xA2FE00).
  - The ~104 job threads (start +0x120140C) redo all their work in the right-eye pass:
    - render recording +0xA33B50 -> +0xA30C68 -> +0xA32E04, ~17 ms per frame;
    - jobs under +0x9D9BE0 -> +0x6A828 (+0xB01878, +0x855FD8, +0x274708, ...), ~21 ms.
  - Skipping the timed update (+0x9B111E) or renderer vt[7] (+0x9B117D) in the right pass gave no gain
    (vt[7]: slower; run 178). Removed.
- Motion-blur watcher: shutter exactly 1 is excluded (run 174: 1487 false hits, 1 / 30 / 20 16 16 1, opened the
  pause menu). Run 187: a real hit (shutter 0.5, tail 2 4 1 5) still caught and fixed.
- Dev tools added:
  - ff16vr_prof.trigger (pass_prof.cpp: game-thread and job-thread sampling per pass).
  - Pad trigger sticks ("lx=.. ly=.. rx=.. ry=..", percent).
  - XR shots save both eyes.
  - Watch "cam".
- Build: release\dinput8-blurfix2.dll (= res187, smoke-tested: stereo, 36-39 VR fps on the 5070).

## Update (Oct 27, 05:10): character ghosting FIXED on the GPU (PerEyeObjectMotion, VR page, default on)
- taa.cpp: the velocity resolve resolveUvSpaceVelocityCS (DXIL digest d978ef28...) is recognised at pipeline
  creation. A fix pipeline is built on its root signature, and the mod's Dispatch hook (command list slot 14) runs
  it right after the resolve on the same bindings:
  - b0: the resolve's camera. It holds this eye only; previous == current there (run 190).
  - t0: G-buffer velocity (NDC, 65504 = static).
  - t1: depth = near / z.
  - u0: uv velocity.
- Per object pixel (G-buffer velocity != 65504):
  - error e = P00 * IPD * depth / near, with IPD = 2 * |row38 - row37| of b0. This is the disparity between
    the eyes at that depth.
  - Skipped if the velocity equals the camera-only reprojection (rows 128 / 129 / 131, + jitter row 137):
    objects without a previous pose write that and are already right.
  - The sign that shrinks |g.x| is chosen per pixel; out.x -= s * e / 2.
- Run 191 (RenderDoc, TAA path), pixels with > 5 px of motion:
  - bad eye: 274,214 off -> 28 on;
  - other eye: 114 -> 0.
- The TAA / fix hooks are now also installed in RenderDoc capture mode (rdoc.cpp).
- DLSS path not verified on the test PC (it runs TAA); the fix is applied to the resolve output, which the TAA
  reads; if DLSS is tagged with that texture it is covered too.
- Blur watcher: 0.5 s live wait (was 3 s); remembers the heap regions that held parameter blocks and checks them
  first (~100 ms instead of 1.5-4.6 s); full scan only the first time in a session. Run 193b: resolution change
  -> round trip within ~2 s. BlurMenuFixHoldMs (Game page, 10-1000, default 1000; user: 600 works on the 5090).
- Release folder cleaned (user request): old builds moved to ff16vr\release_old. Test builds now go to ff16vr\testbuilds.
- Build: release\dinput8-ghostfix.dll.

## Update (Oct 27, 08:30): ghosting with FSR 3 / DLSS fixed (runs 195-201)
- The user reported no change with the first fix, and no halo at all with TAA: the halo is an upscaler-path problem.
- With an upscaler the game uses another resolve variant, resolveUvSpaceVelocityCS 3ec0c2941a (DXIL digest
  4b563737...). Its bindings: t0 depth D32, t1 stencil, t2 G-buffer velocity, u0 uv velocity. Its camera
  cbuffer's previous CameraParam is the other eye: translation rows 75..77 .w vs rows 11..13 .w are 63 mm apart.
- So the resolve reprojects every static pixel with the other eye's camera. The whole image carries the eye
  disparity as motion: +-7 px median, up to 17 px on near surfaces at FSR performance (2056 texture, scene 1028).
  Characters: in one eye their own G-buffer velocity has the error, in the other eye it is clean (run 200).
- Fix (upscaler variant):
  - dl = +-IPD; IPD = 2 * |row38 - row37|; the side is the sign of (cur - prev camera position) . right.
  - e = P00 * dl * depth / near;  out.x -= 0.5 * renderScale * e, with renderScale = frame row 6 .x (0.5 at
    FSR performance).
  - Applied to static pixels always, and to animated pixels when |g.x + e| < |g.x|.
- Run 201 (RenderDoc, FSR 3 performance): pixels with > 5 px of motion went from 844,710 / 779,984 (fix off) to
  0 / 0 (fix on), both eyes.
- Also fixed:
  - pipeline pointers reused after release (a TAA-path resolve and the later upscaler resolve at the same
    address) are now re-mapped;
  - mapped pipelines are tagged with SetPrivateData, so an unrelated pipeline at a reused address is never
    swapped (also protects DisableTAA).
- Test PC game setting is now FSR 3 performance, sharpening 5 (set by the user).
- Build: release\dinput8-ghostfix2.dll (= testbuilds\res201).

## Update (Oct 27, 22:00): per-eye DLSS noise = motion vectors against the other eye (cam_prev.cpp)
- User test of res204 on the 5090 (log ff16vr\ff16vr-res204.log): with PerEyeDlss on, no character halo but a noisy,
  low-res image; "Same jitter for both eyes" made no visible difference. DLSS there is Ultra Performance (mode 4,
  1603 x 1331 per eye for 4808 x 3992), later Performance (mode 1).
- Jitter: the generator +0xAA186C uses index [gen+0x10] mod N (Halton 2 / 3, N from the render / output ratio), so
  the index found is right; the first logged syncs were 0 -> 0 (before gameplay). Logging is now periodic.
- Cause of the noise: RenderView::Update builds cCamera at +0xAA44EE..+0xAA46C4: cb+0 = ring[+0xE75], cb+0x400 =
  ring[+0xE76] (the 2-entry CameraParam ring at +0x610), cb+0x800 / +0x840 reprojection from +0xE80 / +0xF00,
  cb+0x890 jitter delta (CameraParam +0x2F0 cur - prev), then +0x9E08F8 finalises. In stereo the ring's previous entry
  is the other eye, so with per-eye DLSS history every motion vector is off by the eye disparity (left eye) or by the
  disparity minus one tick of camera motion (right eye) and DLSS throws its history away.
- Fix (PerEyeMotionVectors, VR page, default on; active only while per-eye DLSS evaluates): the finalise call is
  hooked; for the main view's eye cameras (eye from row 38 - row 37 along the right axis) the previous half is
  replaced with the same eye's previous CameraParam (kept per eye) and the jitter delta recomputed. FSR 3 / TAA
  (shared history) are left alone. Dev trigger "pmv 0/1/2" (off / auto / always).
- Build res205.
- Run 211 (res206, FSR 3 + RenderDoc, pmv 2): b0 previous == same eye in both eyes, but the left eye's output still had
  ~7 px on static pixels: the reprojection (cb+0x800 from +0xE80, +0x840 from +0xF00) is built from ring[+0xE76]
  rows 3..6 and the slot positions at +0xFE4 + slot * 12. Fix: with per-eye motion vectors active, RestoreEyeHistory
  also puts back the ring (+0x640..+0xE10) and the toggle / slot bytes (+0xE74..+0xE76) of the eye, so the game's
  whole camera history is per eye (needs PerEyeHistory on).
- Then only animated objects in the left eye kept ~8.5 px (their previous pose was stored by the other eye's pass).
  The upscaler fix pass now handles "previous camera = same eye": animated pixels only, sign that shrinks |g.x|.
- Run 212 (res207): resolve output with pixels > 1 px: left 0.01 %, right 0.01 % (was 72-80 % > 5 px with the
  cbuffer-only patch, and the disparity everywhere without the mod's fixes).
- Jitter sync verified (run 207d): right eye index 18 -> 17 each tick, i.e. the even / odd split was real.
- DLSS could not be enabled on the test PC (superResolution_ 0-8 in config.xml: no slDLSSSetOptions in gameplay;
  FSR 3 = 5, config.xml restored). DLSS verification is on the user's 5090.
- Build: release\dinput8-pereyemv.dll (= res207). Needs PerEyeDlss, PerEyeHistory, PerEyeMotionVectors on.

## Update (Oct 28, 08:30): motion blur Off patched in code (no menu round trip); HUD fades; menu paging
- Run 221: hardware write watch on frame constants row 11 .w in the upload heap (write-combined, found by row 13
  pattern) caught the writer: +0xAF4088 builds the frame constants; +0xAF44F3 `vmovss xmm0, [r14+0x68]`,
  row 11 .w = 1 / [r14+0x68]. Motion blur Off = 0.001 there; after a load the game resets it to 1.0 and only the
  pause menu's close writes 0.001 again (the menu does not read VideoConfigData.motionBlurIntensity_: run 218).
- Fix (motion_blur.cpp): detour at +0xAF44F3 calls BlurScaleHook(r14) every frame: if DisableMotionBlur and the value
  is above 0.0011, write 0.001. Run 222: after the load 5 full-strength / 343 off blocks, 0 menu round trips (before:
  387 full -> menu). The menu round trip remains only as a fallback if the signature is not found.
- Menu: "Keep motion blur off" (VR page); "Menu open time" removed from the menu (config key still read).
- Blur watcher: periodic checks only log; no round trip if one did not halve the count (user's new-game action
  sequence had 3000+ blocks the game wants: pauses).
- HUD fade overlays (xr_screen.cpp): 16 x 9 HUD pixels read back per frame; a uniform dark non-transparent HUD is a
  full-screen fade and the head-locked panel is hidden (user: black box over the new-game opening).
- Menu paging (user's 5090 log): page changes show the gameplay camera 0.8-0.9 s, sometimes 1.25-1.7 s. Head tracking
  needs 1 s of live camera, 2.2 s within 8 s of a short burst; native stereo also needs 1 s.
- Builds: release\dinput8-menuhold2.dll, dinput8-fadefix.dll, dinput8-blurpatch.dll (latest, includes all).

## Update (Oct 29): opening cinematic, HUD anchor, beta defaults (0.9.0)
- New Game opening (user log, VerboseLog HUD probe maps): from ~42 s to ~102 s the HUD image is opaque black in the
  middle with clear top / bottom rows (a letterbox mask, drawn inside out). In VR it covered the view as a black box.
  xr_screen.cpp detects that pattern (rows 0 and 8 clear, rows 1-7 >= 85 % opaque black, ~0.2 s): HudMovieMode()
  hides the HUD panel, keeps the theater view, blocks native stereo (stereo_render) and head tracking (camera_hook),
  so the stretch plays flat; it ends when the pattern goes (fade into the cave). No false hits in gameplay / menus
  (run 224).
- VerboseLog adds "HUD probe" lines (a 16 x 9 map of the HUD image every ~45 frames).
- HudAnchor (Screen page): Head (follows the view) / World (fixed in the room, placed at recenter).
- Beta defaults: VRMode Native, FoveatedGaze off, HudAnchor World, DlssGhostingFix off (user prefers the slightly
  sharper image; the halo is small since the motion-vector fixes), DisableGraphicsDebuggerCheck off (run 225: the game
  runs normally without it). BlurMenuFixHoldMs and the RenderDoc paths are no longer in the default config.txt.
- Version 0.9.0 (src/version.h): in the log's first line and the menu title.
