# Handoff: BeamNG.drive cars in GTA V (state on 2026-10-07)

Read this first, then `MODLOG.md` (measurements, causes and fixes so far) and `PROTOCOL.md` (link messages).

## What works
- BeamNG simulates the car; GTA is the world. GTA streams ground tiles and walls (invisible collision in BeamNG),
  sends its camera, inputs and nearby traffic; BeamNG sends the car's pose. Link: UDP 127.0.0.1:47801.
- Spawn (F6), enter (F, now forced with TASK_ENTER_VEHICLE), drive, crash damage from scenery and from GTA cars
  (hidden BeamNG "ghost" cars follow the nearest 3 GTA vehicles), F9 recover, F5 debug, F7 remove.
- Compositor: ReShade 6.8.0 in both games. BeamNG (`Bin64\dxgi.dll`, D3D12 only, D3D11 doesn't work in 0.39.4)
  exports picture + depth to shared memory `Local\GTAxBeamFrame` (`shared/gxb_frame.h`); GTA
  (`ReShade64.asi` + the add-on half inside `GTAxBeam.asi`) draws it with `gta/shaders/GTAxBeam.fx`, depth-tested
  against GTA's depth. The GTA proxy car is hidden (alpha 0) while frames arrive.
- Calibrated: BeamNG depth is reversed Z, near 0.1, far ~4172 m; BeamNG fov is vertical like GTA's
  (`host/depthcal.py`).

## User's latest report (with screenshot of a wrecked ETK 800 on a GTA sidewalk)
"Car still looks photoshopped but it updates and takes damage like it should. Very glitchy/laggy when it drives
and stutters, still looks weird in motion. We need it to match like it's part of the game."

## Next work, in order
1. **Stutter / lag in motion.** Likely causes, to measure before fixing:
   - BeamNG's picture is several frames old: the exporter's readback ring is 4 deep (`kRing` in
     `beamng/exporter/export.cpp`), plus BeamNG renders from the camera GTA sent a frame earlier. Measure latency
     (frame counter in the slot descriptor vs. GTA's camera frame `f` in the `cam` message), then shrink the ring
     with a fence and/or re-project (the Minecraft example's `compositor.cpp` / `MCPassthrough.fx` already do pose
     re-projection with depth: needs BeamNG's render camera pose written into the slot descriptor).
   - GTA's camera follows the proxy, which trails BeamNG's car (velocity steering, `proxy_follow` in
     `gta/src/script.cpp`), so the drawn car shifts against the view.
   - Frame pacing: BeamNG runs uncapped in the background (~130 fps) on the same GPU as GTA.
2. **Make it look part of GTA.** Ideas, roughly by impact:
   - Paint reflects BeamNG's empty void sky: give the void level a sky/cubemap and sun matching GTA's
     time/weather (bridge already syncs time of day with `tod`).
   - Relight / colour-grade BeamNG's car from GTA's picture (port `relight()` from `MCPassthrough.fx`).
   - Stronger ground shadow / ambient occlusion under the car; BeamNG's own shadow lands on invisible ground.
   - Edge anti-aliasing, matching sharpness/resolution, motion blur, GTA's haze at distance.
3. Later: peds as BeamNG colliders, ghost car sizes per GTA model, README.

## How to run and test
- Build: `gta\build.bat` (ASI), `beamng\exporter\build.bat` (add-on). Deps: `gta\fetch_deps.ps1`.
- Install: `gta\install.ps1` (GTA must be closed), `beamng\install.ps1` (mod), `beamng\install_reshade.ps1`.
- Hot-reload the BeamNG bridge after `beamng\install.ps1`: `python host/send.py reload`.
- Tests without GTA (BeamNG running, GTA closed so the link isn't shared):
  `python host/fakegta.py` (6 physics checks), `python host/ramtest.py` (ghost ram + hidden in picture),
  `python host/depthcal.py` (depth/fov calibration).
- Logs: `<GTA>\GTAxBeam.log`, `<GTA>\ReShade.log`, `<BeamNG>\Bin64\ReShade.log`,
  `%LOCALAPPDATA%\BeamNG\BeamNG.drive\current\beamng.log`.
- GTA's fullscreen picture can't be captured by the agent; ask the user for screenshots.

## Standing rules from the user
- Ask before installing loaders into game folders, changing the registry or publishing. Story mode only, never GTA
  Online; BattlEye off (`args.txt`: `-nobattleye`).
- Kill processes by exact PID only. Never commit game files. Only commit when asked.
- Don't click GTA's or BeamNG's dialogs for the user (privacy choices, warnings). Use the latest tool versions.
