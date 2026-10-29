# FF16 VR: test agent and RenderDoc capture

Test PC: **gamepc5070** (RTX 5070 12 GB, driver 581.32, Ryzen 7 7800X3D, 32 GB RAM, Windows 11 26200).
The old PC (GUNSHOOTER, 3080 Ti, 16 GB) is retired for testing.

## Folders (connected to Claude sessions)

| Folder | What |
|---|---|
| `D:\vrconversions\ff16\ff16vr` | mod source (`src\`), `build.sh`, `release\` (dinput8.dll/.pdb, openxr_loader.dll, config.txt), `agent\` (copy of the agent), `third_party\` |
| `D:\vrconversions\ff16\agent` | the running agent: `RUN_agent.cmd` → `agent.ps1`, helpers `ff16tools.ps1`, `tools\rdc_summary.py`, `tools\rdc_frame_map.py` |
| `D:\vrconversions\ff16\captures` | RenderDoc captures (`ff16_frame*.rdc`), `failed_pso\` (shaders the driver rejected) |
| `D:\vrconversions\sharedtools` | RenderDoc 1.46 (`renderdoc\RenderDoc_1.46_64`, portable), vrkit, toolchains |
| `D:\SteamLibrary\steamapps\common\FINAL FANTASY XVI` | game; needs `steam_appid.txt` = 2515020 (the agent creates it) |

## Agent

- Drop `inbox\NNN_name.cmd.ps1` (first line `# timeout: N`), result in `outbox\NNN_name.out.txt`, screenshots in `outbox\shots\`. `agent.alive` is a heartbeat (`state=idle|running ...`). Commands run one at a time.
- Helpers are re-read for every command, so `ff16tools.ps1` can be updated without restarting the agent. Keep both copies (`agent\` and `ff16vr\agent\`) in sync.
- Scripts must parse in Windows PowerShell 5.1 and stay ASCII. Check them with pwsh 7.4.6 (`[Parser]::ParseFile`) before deploying.
- Bridge gotcha: stage every new file version in a fresh folder and check the size on the PC after copying.

Key helpers: `Install-FF16Build`, `Set-FF16Config 'K=V',...`, `Start-FF16Gameplay -LoadSec 75 -TitleWaitSec 1100`,
`Invoke-FF16Capture`, `Invoke-FF16RdcSummary <rdc> [-Script x.py -OutName prefix -ScriptEnv @{...}]`,
`Set-QRenderDocNoPrompts`, `Get-FF16CrashInfo`, `Compress-FF16Files`.

Notes:
- `Install-FF16Build` copies the DLLs and pdb only, not `config.txt`.
- **Close the game with `Close-FF16` (WM_CLOSE), not `Stop-FF16` (kill).** A graceful close keeps the pipeline
  cache; boot to title is then ~20 s instead of a 20+ minute recompile.
- **Pipeline cache:** `%LOCALAPPDATA%\SquareEnix\FINAL FANTASY XVI\ffxvi_b96bc3fb.psol` (~800 MB). RenderDoc runs
  replace it with an empty one. `Backup-FF16PipelineCache` before and `Restore-FF16PipelineCache` after every
  RenderDoc run (copies to `D:\vrconversions\ff16\psol_backup`).
- Helpers return their log lines too: `$x = Invoke-FF16Capture` is an array; use `$x[-1]` (or a fixed path) as the
  .rdc path.
- `Start-FF16Gameplay` detects the title from screen pixels: anything covering the screen (e.g. SteamVR's null-HMD
  desktop window, which OpenXR starts automatically when VROutput = 1) breaks it. The null HMD window is set to
  400x300 at 1648,852 (`driver_null.windowX/Y/Width/Height` in steamvr.vrsettings).
- Camera tools: `Invoke-FF16CamProbe` (see camera-and-tracking.md), `ff16vr_camtest.trigger` ("yaw pitch" / "off").
- New PC: no "driver incompatible" box; title screen after ~51 s without RenderDoc, ~51 s with RenderDoc too (the compile is fast now).

## RenderDoc capture mode (works as of 2026-10-24)

config: `VROutput = 0`, `RenderDoc = 1`, `RenderDocDelayMs = -1`, `RenderDocNvAPI = 1`.

What the mod does in this mode:
1. Bypasses Streamline (`sl.interposer.dll` D3D12CreateDevice / CreateDXGIFactory2 are replaced by the real ones); RenderDoc loads at D3D12CreateDevice; the swap chain is created through a RenderDoc-wrapped factory. DLSS is therefore unavailable in captures.
2. Sets RenderDoc's `AllowUnsupportedVendorExtensions = 0x10DE` (NvAPI passthrough). Not required for the fix below, kept so captures behave closer to native.
3. **PSO guard** (`src\pso_guard.cpp`), installed on RenderDoc's device:
   - **`CreatePipelineLibrary` → always RenderDoc's empty library.** This was the crash. RenderDoc refuses pipeline libraries (saved blob → `D3D12_ERROR_DRIVER_VERSION_MISMATCH` with no object; dry run → `DXGI_ERROR_UNSUPPORTED`), and FF16 then calls `ID3D12PipelineLibrary::LoadGraphicsPipeline` (vtable slot 9) on a null library at `ffxvi.exe+0x9F9173` and shows "An unexpected error has occurred" from its crash handler. With the empty library every Load* says "not found" and the game compiles each pipeline.
   - Failed compute PSOs are dumped to `captures\failed_pso\` and replaced by a do-nothing CS. FF16 always has **15 AMD-only `[WaveSize(64)]` compute shaders** that NVIDIA rejects (E_INVALIDARG); they also fail natively and the game copes, so this is harmless.
   - Failures are retried without `CachedPSO` first (never needed so far).
   - `PsoGuard = 1` installs it in log-only mode without RenderDoc (natively the game loads PSOs from its library, so it sees few creations).
4. Capture: `Invoke-FF16Capture` (trigger file) → `captures\ff16_frameN.rdc` (~4.3 GB for the Hideaway kitchen), 15 s to write. Game runs ~2 fps under RenderDoc.

Earlier hypotheses that were wrong: not RAM (crashed on 32 GB too); not the NVAPI GPU lookups RenderDoc blocks; not CachedPSO.

## qrenderdoc analysis

- qrenderdoc shows a **modal analytics opt-in dialog on first run, before any `--python` script runs**. `Set-QRenderDocNoPrompts` sets `Analytics_TotalOptOut` (and `CheckUpdate_AllowChecks = false`) in `%APPDATA%\qrenderdoc\UI.config` (`rdocConfigData: 1` JSON). `Invoke-FF16RdcSummary` calls it and has a start watchdog (screenshot + kill if no `progress.log` within 180 s).
- qrenderdoc is portable, so computer use cannot be granted on it (masked); use the agent's own screenshots.
- `tools\rdc_summary.py` (~70 s): actions.txt, passes, textures, 120 pass thumbnails.
- `tools\rdc_frame_map.py` (~8.5 min): dispatches.txt (RW/RO per dispatch), post_chain.txt, post thumbnails, cbuffers.txt + raw `cb_*.bin` with automatic projection/view matrix search. Its automatic post-start picks the shadow atlas as "main depth"; pass `FF16_POST_EID` (e.g. 149641) to start at the real post chain. Copy actions' `outputs` in RenderDoc are the bound RTs, not the copy destination; read copy src/dst from the structured data instead.

## SteamVR null headset

`Steam\config\steamvr.vrsettings`: `steamvr.requireHmd = false`, `forcedDriver = "null"`, `activateMultipleDrivers = true`, `driver_null.enable = true`; also `drivers\null\resources\settings\default.vrsettings` enable = true. Backups `*.bak_20261024_201014`. Null HMD: 1512x1680 per eye render target, 90 Hz, window 2160x1200. SteamVR is the active OpenXR runtime.

## Build

Linux container: unzip `toolchain.zip`, LLVM 22.1.3 (`LLVM-22.1.3-Linux-X64.tar.xz`), `TOOLCHAIN=<dir> LLVM=<dir> ./build.sh`. Needs `third_party\openxr\include` next to `src\`.
