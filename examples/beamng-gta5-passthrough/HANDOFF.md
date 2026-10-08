# Handoff: BeamNG.drive cars in GTA V (state on 2026-10-07)

Read this first, then `MODLOG.md` (measurements, causes and fixes so far) and `PROTOCOL.md` (link messages).

## What works
- BeamNG simulates the car; GTA is the world. GTA streams ground tiles and walls (invisible collision in BeamNG),
  sends its camera, inputs and nearby traffic; BeamNG sends the car's pose. Link: UDP 127.0.0.1:47801.
- Spawn (F6), enter (F, now forced with TASK_ENTER_VEHICLE), drive, crash damage from scenery and from GTA cars
  (hidden BeamNG "ghost" cars follow the nearest 3 GTA vehicles), F9 recover, F5 debug, F7 remove.
- Compositor: ReShade 6.8.0 in both games. BeamNG (`Bin64\dxgi.dll`, D3D12 only, D3D11 doesn't work in 0.39.4)
  exports picture + depth (the car's rectangle) to shared memory `Local\GTAxBeamFrame` (`shared/gxb_frame.h`); GTA
  (`ReShade64.asi` + the add-on half inside `GTAxBeam.asi`) draws it with `gta/shaders/GTAxBeam.fx`, depth-tested
  against GTA's depth. The GTA proxy car is hidden (alpha 0) while frames arrive.
- Calibrated: BeamNG depth is reversed Z, near 0.1, far ~4172 m; BeamNG fov is vertical like GTA's
  (`host/depthcal.py`).

## User's latest report (with screenshot of a wrecked ETK 800 on a GTA sidewalk)
"Car still looks photoshopped but it updates and takes damage like it should. Very glitchy/laggy when it drives
and stutters, still looks weird in motion. We need it to match like it's part of the game."

## Stutter work done (built, installed in both games, host tests pass; not yet tried in game)
Details and numbers in `MODLOG.md`, "Stutter work".
- Car-relative camera: GTA sends its camera relative to the proxy (`cam.rel`), BeamNG renders from that offset to
  its own car, so the car can't drift against GTA's view however old the picture is (synctest: 0.0-0.1 px).
- Exporter layout v2 (`shared/gxb_frame.h`): fenced readback (1 frame behind), only the car's rectangle, and each
  picture tagged with the camera that rendered it (bridge -> exporter over UDP 127.0.0.1:47802).
- `GTAxBeam.fx` re-projects the picture from its tagged camera to GTA's current one (ray-march over BeamNG's depth,
  only inside the car's projected box). Toggles in ReShade's overlay, category Motion: `Reproject`, `CarLead`.
- Collision rebuilds (7-19 ms stalls each, several a second in the last session) made rarer: 3x3 tile window
  with hysteresis, walls as world cells resent at most 1/s.

## First run with it
Looks better and less choppy. The car spawned under the map because the script took the host tests' leftover car
as the new one; fixed in `script.cpp` and confirmed in the next run (spawn at player ground z ~4.1).

## Diagnosis (2026-10-07, spawn-fix run — do not chase look next)
Spawn worked. Do **not** pivot off BeamNG-sim + GTA-world + composite: that is the only way to get real BeamNG
deformation in GTA. Do pivot the *milestone*: "feels like a GTA car" is several jobs. Playability is broken by
collision; the photoshopped look is later (Minecraft-in-GTA is also pasted; a car shows it more because GTA cars
are lit by GTA).

This run: spawn 1 on the road, crash at 15 m/s (dmg 37k). Spawn 2 parked, GTA traffic rammed it (24 → 489k) — ghost
cars work. Spawn 3: 37 m/s, z 5.8 → 1.1 in 2 s, then z 0.6 at 30–49 m/s under the map. Screenshot 1 is that
underworld. Screenshot 2 is the good case: crumple is real, lighting/edges still pasted. Picture lag ~1–3 GTA
frames, spikes 130–154 (~2 s); uploads 23–90/s; rebuilds 5–27 per 10 s. Overlay Home often eaten in fullscreen.

Oversights: ground probes from the car +4 m (fallen car makes underworld tiles forever; F9 recovers onto the
wrong ground); 2 m heightfield, 3×3×64 m, 1 s lookahead, kNoGround is a hole, old tiles dropped before replacements
exist; walls 35 m / 0.25–1 s late; BeamNG uncapped on GTA's GPU; coverage is alpha so broken glass vanishes;
proxy invincible so guns/melee never reach BeamNG; peds are not streamed; enter is TASK_ENTER_VEHICLE on an alpha-0
Tailgater while BeamNG still draws its own occupant.

## Next work, in order
1. **Stay on GTA's road.** Probe tiles from the player/camera height, never a fallen car. If car z is >2 m below
   GetGroundZ, rebuild from the street and place the car there (F9 too). Don't drop a tile until its replacement
   has been sent. Treat missing GetGroundZ as "keep last height", not a hole. Stream farther ahead of velocity.
2. **Stop the self-fight.** Cap BeamNG near GTA's frame rate; keep rebuilds rare. Then re-measure jitter.
3. **Coverage from depth** (or keep glass opaque in the export) so broken windows don't punch through to GTA.
4. Later: hide BeamNG's driver; proxy bullet/melee → BeamNG impulses; ped colliders. Enter will never look native
   while the visible car is a picture of a different vehicle.
5. **Look last:** void sky/cubemap + `tod` sun, then `relight()` from `MCPassthrough.fx`, then shadow/haze.

ReShade overlay is Home. If it does nothing in fullscreen, bind F8 in GTA's ReShade.ini (ask first).

## How to run and test
- Build: `gta\build.bat` (ASI), `beamng\exporter\build.bat` (add-on). Deps: `gta\fetch_deps.ps1`.
- Install: `gta\install.ps1` (GTA must be closed), `beamng\install.ps1` (mod), `beamng\install_reshade.ps1`.
- Hot-reload the BeamNG bridge after `beamng\install.ps1`: `python host/send.py reload`.
- Tests without GTA (BeamNG running, GTA closed so the link isn't shared):
  `python host/fakegta.py` (6 physics checks, run first: it builds the test pad), `python host/ramtest.py`
  (ghost ram + hidden in picture), `python host/synctest.py` (car-relative camera, picture/camera tagging,
  latency), `python host/depthcal.py` (depth/fov calibration).
- The exporter add-on is loaded by BeamNG at startup: after rebuilding it, restart BeamNG (the user presses Cancel
  on BeamNG's dxgi.dll warning) and run `beamng\install_reshade.ps1` while BeamNG is closed.
- Logs: `<GTA>\GTAxBeam.log`, `<GTA>\ReShade.log`, `<BeamNG>\Bin64\ReShade.log`,
  `%LOCALAPPDATA%\BeamNG\BeamNG.drive\current\beamng.log`.
- GTA's fullscreen picture can't be captured by the agent; ask the user for screenshots.

## Standing rules from the user
- Ask before installing loaders into game folders, changing the registry or publishing. Story mode only, never GTA
  Online; BattlEye off (`args.txt`: `-nobattleye`).
- Kill processes by exact PID only. Never commit game files. Only commit when asked.
- Don't click GTA's or BeamNG's dialogs for the user (privacy choices, warnings). Use the latest tool versions.
