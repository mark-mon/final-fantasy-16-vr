# FF16 frame map (capture `ff16_frame15828.rdc`, 2026-10-24)

Scene: Hideaway kitchen, gameplay with HUD. Window 2048x1152. Captured under RenderDoc with Streamline bypassed (no DLSS).
Outputs: `agent\outbox\rdc_ff16_frame15828\` (summary, actions, 120 pass thumbnails) and `agent\outbox\rdc2_ff16_frame15828\`
(dispatches, post chain, post thumbnails, constant buffers).

Totals: 25,855 actions, 8,106 draws, 618 dispatches, 409 passes. Resource ids below are RenderDoc ids in this capture
(not stable across captures; identify targets by format/size/order instead).

## Render scale

All 2048x1152 targets, but the 3D scene viewport is **1024x576 (50 %)** (viewport of G-buffer and lighting draws, scene
occupies the top-left quarter of every scene target). The TAA pass does **not** upscale (history and output stay
1024x576). Where the 1024x576 scene is scaled to 2048x1152 is not pinned down yet: the graded scene leaves #16342 via
copies at eid 150020-150024 (src/dst still to be read). Check the in-game resolution scale / upscaler setting and
recapture with 100 % to simplify.

## Pass order

| eid | Pass | Targets |
|---|---|---|
| 62636-70632 | uploads, clears, GPU culling dispatches | |
| 70642-82086 | sun shadow cascades, ~1,150 indirect draws | depth #2647 2048² R32 (also #2680, #2684) |
| 82166-86049 | depth prepass + velocity (characters only) | depth #16275 D32S8, velocity #16305 RG16F, mask #16362 R8 |
| 86071-106962 | **G-buffer** (characters in a separate pass after the world) | #16282 RGBA8 albedo, #16283 RGBA16F, #16294 RGBA16F (+ more) |
| 107053-107580 | decals | #16282, #16304 RGBA8 |
| 106984, 141033 | volumetric fog froxels | 3D 256x144 RGBA16F #16538, #16549 |
| 107615-109034 | second depth (#16278) + #16386/87/88 RGBA8 | effects/characters layer (TBD) |
| 109189-138211 | local-light shadow atlas, ~3,900 draws | depth #1158669 2048² R16 |
| 139368 | SSAO-like dispatch | #16307 R8 |
| 140172-140724 | **lighting** | #16330 / #16327 R11G11B10 → HDR #16342 RGBA16F |
| 141308-150078 | transparents / forward (many short passes) | #16342 + #16365 R16F + #16305 velocity + #16395 RGBA8 |
| 149783 | motion-vector combine | #16354 RG16F |
| 149792-149800 | **motion-blur tile max / neighbour max** (1/3 res) | 682x384 RG16F #16487/#16488 |
| 149819-149849 | auto exposure (histogram, average) | 1D 96x1 R32F #5807 |
| 149859 | classification | #16476 R8_UINT |
| **149870** | **TAA** (cs #924630): reads history #16358, current #16342, velocity #16354, #16365, #16359 | → #16363 |
| 149877 | save TAA history | #16358 R11G11B10 |
| **149889** | **motion blur** (cs #924634): #16363 + velocity + tile max #16488 | → #16342 |
| 149899-150001 | bloom down/up chain 512x288 → 8x4, composite | → #16342 |
| 150008 | tonemap (exposure) | → #16363 |
| 150017 | colour grading, 33³ LUT | → #16342 |
| 150020-150024 | copies (scene out of #16342? history?) | TBD |
| 150096-150548 | UI element rendering (icons/text into #16282/#16304/#16386-88, depth #16278 for 3D UI) | |
| 150560-150774 | **UI composite into #16106** (R10G10B10A2, 2048x1152, bound as UAV), ~30 draws | **HUD-only layer** |
| 150795 | copy #16106 → swapchain (vs #2254 / ps #2258) | swapchain #16105 |
| 150802 | small dispatch (frame stats) | |

## Camera constant buffer

`cbuffer0` of G-buffer / lighting VS and PS = buffer #14371, 2,304 B. Two 1,024-byte blocks: **current frame @0,
previous frame @1024** (TAA reprojection). Column-vector convention (`M * v`), rows stored as float4. View space looks
down −Z. Offsets within a block:

| Offset | Content (values from the capture) |
|---|---|
| 0 | view, 3x4 (rows right / up / forward, w = −dot(axis, campos)) |
| 48 | view-projection 4x4 (absolute world) |
| 112 | projection: [1.5455 0 −jx 0; 0 2.7475 −jy 0; 0 0 0 0.2; 0 0 −1 0] → reversed-Z infinite, near 0.2, vertical FOV 40°, aspect 16:9 |
| 176 | inverse view 3x4 (translation = camera position) |
| 224 | inverse projection |
| 288 / 352 | inverse view-projection (camera-relative / absolute) |
| 416 | **camera-relative view-projection** (no translation) |
| 480 / 528 | camera-relative view / inverse view (rotation only) |
| 592 | camera position (18.694, 1.746, 9.440, 1) |
| 608 | camera position, near (0.2) |
| 624 | −forward |
| 640-748 | second copy of view + view-projection (non-jittered? to verify) |
| 752 | TAA jitter (x, y) |
| 816-928 | frustum planes (normal, d) used for culling: must cover both eyes in stereo |
| 944 | yaw-only rotation (horizontal facing) |
| 1008 | (5 = 1/near, 0.006, 3.7009, 5) |

Plan for the head-tracked camera: find the CPU-side source of this struct (scan game memory for the camera position
+ view rows at runtime, then hook the function that fills it) and inject per-eye view/projection there, including
frustum planes and the previous-frame block.
