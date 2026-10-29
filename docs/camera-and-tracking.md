# FF16 camera: where it lives and how the mod drives it (ffxvi.exe 1.03, 2026-10-24)

## How the camera was found

`cam_probe` (src\cam_probe.cpp, trigger `ff16vr_camprobe.trigger`, agent `Invoke-FF16CamProbe`): scans writable
memory for a reversed-Z perspective projection in either layout, groups hits into camera classes, then puts hardware
write breakpoints (DR0-3, all threads) on the jittered element and logs the writing code + call stacks.
Classes in gameplay: main camera (40 deg vertical, near 0.2), two near-0.01 views (40 deg and 45 deg, far 1000),
and 90 deg / near 0.005 probe views (cube maps). All verified harmless to the game (150+ threads, no crash).

## RenderView (one per camera; vtable ffxvi.exe data 0x7FF7..3848 at runtime)

Row-vector (DirectXMath) matrices, 16-byte rows:

| Offset | Content |
|---|---|
| +0x10 | camera scene-graph node (pointer) |
| +0x20 | camera position |
| +0x30 | view (world->view) 4x4 |
| +0x70 | projection (reversed-Z infinite) |
| +0xB0 | view-projection |
| +0xF0 | inverse view (camera world matrix) |
| +0x130 / +0x170 | inverse projection / inverse view-projection |
| +0x1B0..0x28F | frustum planes / corners |
| +0x290..0x5CF | copies + camera-relative variants; +0x550 = projection copy |
| +0x610, +0xA10 | 2-entry ring of 1,024-byte GPU camera blocks (column-vector, see frame-map.md) |
| +0xE20 | pointer to the look-at target object (target position at [+0xE20]+0x40) |
| +0xE3C | vertical FOV (radians; 0.698 = 40 deg) |
| +0xE40 | aspect (1.778) |
| +0xE44 | near (0.2 main view, 0.01 others) |
| +0xE48 | far / flag (1 main view, 1000 others) |
| +0xE75 / +0xE76 | ring index of current / previous GPU block |

## Who writes it

- **`RenderView::Update(this)` = ffxvi.exe+0xAA23C4** (game thread, ~2x per frame for the main view). Call chain
  +0xA9EAF2 <- +0xACCF39 <- +0xC076AD <- +0x9B119E <- +0x9B2C54 <- +0x6FE834 <- +0xC4D48A (thread entry).
  - +0xAA2412: `call +0x9AEB7C` = scene-graph node world matrix of the camera node; **only the position is used**.
  - **+0xAA262A: `call +0x29D090` = LookAt(out, eye = node+0x40, target = [view+0xE20]+0x40, up = r9)** -> local
    4x4, copied to +0x30. This is where the view orientation comes from.
  - +0xAA2818..+0xAA29B2: projection from +0xE3C (fov) / +0xE40 (aspect) / +0xE44 (near) / +0xE48, then copied
    to +0x70 / +0x550 (+0xAA2A44 / +0xAA29EC write them).
- **GPU block fill = ffxvi.exe+0xAA33D8** (render-task execute, vtable slot 8, run as a job from +0xAA0CF8):
  copies ring blocks `view+0x610 + idx*0x400` (idx = +0xE75 current, +0xE76 previous) into a new upload constant
  buffer (current at +0, previous at +0x400), i.e. the 2,304-byte camera cbuffer seen in RenderDoc.

## Mod hooks (src\camera_hook.cpp)

- Call-site redirects (5-byte E8 patched to a stub within +-2 GB, signatures checked):
  - `48 8B D9 48 8D 55 30 48 8B 49 10 E8` (+0xAA2407): node world matrix; logging only (passes rbx = RenderView).
  - `48 83 C2 40 C5 FA 11 45 E4 C5 FA 11 4D E8 E8` (+0xAA261C): **LookAt**; the stub calls the original with all
    argument registers intact, then `ViewPostEntry(out, rbx)`.
- Test mode: F9 or `ff16vr_camtest.trigger` ("yaw pitch" / "off") applies V' = V * R^-1. Verified 2026-10-24:
  yaw +-35, pitch -20 turn the rendered view correctly, HUD unaffected, off restores the original.
- **VRMode = 1 (head-tracked mono)**, main view only (near 0.2, far < 10):
  - render thread (xr_screen.cpp, after xrWaitFrame): locate VIEW in LOCAL + xrLocateViews -> head pose relative
    to the recenter pose (yaw + position, F8) and the union of both eyes' FOV tangents -> `vrs::PublishHead`.
  - game thread (LookAt hook): V' = V * (E^-1)^T with E = head pose (rotation M, position t * WorldScale):
    rows a -> a*M, row 3 += -(M^T t); sets +0xE3C so the render covers the headset FOV (FovScale margin);
    records {absolute head pose, tanHalfV, tanHalfH, present count} with `vrs::RecordUsed`.
  - Present: projection layer, both eyes = the whole game image with the pose/FOV it was rendered with
    (`PoseLatencyFrames` presents back, default 1), so runtime reprojection matches. No stereo yet.
- config: VRMode, PositionTracking, WorldScale, PoseLatencyFrames, FovScale (see config.txt comments).

## Test status (2026-10-24)

- VRMode 1 with SteamVR's null headset (run 062): OpenXR session, head tracking active on the main view (vertical FOV
  90 deg from the null HMD), projection layer submitted, game stable at gameplay. Not yet tried in a real headset.
- The null HMD session drops to SYNCHRONIZED after ~10 s (dashboard / vrmonitor error box); expected to stay
  FOCUSED with a real headset.
- PC notes: desktop is 3840x2160 with display scaling (agent screenshots are 2048x1152 logical); the game starts at
  4K, switches to 2048x1152, and becomes a 2068x1172 window when SteamVR takes focus. SteamVR's vrmonitor.exe needs
  d3dx10_43.dll (DirectX End-User Runtime June 2010, not installed): its "System Error" box is owned by Windows and
  can't be closed by the agent.
- SteamVR is back on the real headset (run 065). For automated VR tests set steamvr.vrsettings
  `steamvr.requireHmd=false`, `forcedDriver="null"`, `driver_null.enable=true` (null window 400x300 at 1648,852).

## Next

1. Verify VRMode 1 in the headset (rotation/position direction, latency setting, FOV, comfort).
2. Stereo: render the main view twice per frame (per-eye view/projection incl. off-axis FOV), with per-eye TAA/DLSS
   history and frustum culling covering both eyes. Candidate approaches: run the render graph twice (patch the frame
   job loop) vs. alternate eyes per frame (AFR, simpler, halves temporal stability).
3. HUD on its own layer: the HUD target (R10G10B10A2) is composited to the swapchain in one draw (see frame-map.md).
