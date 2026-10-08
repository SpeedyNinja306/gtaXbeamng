# MODLOG: BeamNG.drive vehicles inside GTA V

Goal: walk around GTA V story mode, get into a BeamNG vehicle, drive and crash it against GTA's world and
traffic with BeamNG's real soft-body physics and deformation, get out, keep playing. Both games run at once.

## Machine (gaming PC, 2026-10-07)
- Windows 11 (10.0.26200), NVIDIA GeForce RTX 5070, driver 617.42.
- GTA V **Legacy**, Steam 271590: `G:\SteamLibrary\steamapps\common\Grand Theft Auto V` (`GTA5.exe` x64,
  BattlEye present for Online only). No loaders installed yet.
  `GTA5.exe` 1.0.3889.0, matching ScriptHookV 3889.0 (fetched by `gta/fetch_deps.ps1`).
- BeamNG.drive **0.39.4** (build 20972), Steam 284160: `G:\SteamLibrary\steamapps\common\BeamNG.drive`
  (`Bin64\BeamNG.drive.x64.exe`, D3D12). No anti-cheat. User folder since 0.38:
  `%LOCALAPPDATA%\BeamNG\BeamNG.drive\current` (mods in `current\mods\unpacked`). The old
  `%LOCALAPPDATA%\BeamNG.drive\0.36` folder is a stale leftover; at first I read it and wrongly took the game
  to be 0.36. The window title and `integrity.json` show the real version.
- Toolchain: git 2.50, Python 3.13 (`C:\Python313`, + Pillow for `um win shot`), Visual Studio Build Tools
  2026 (18.10, VCTools workload; installed with winget `Microsoft.VisualStudio.BuildTools`). No WSL, no
  `uv`. `bin/um` is a bash script; on this PC run `python -m um ...` from the repo root.

## Route and why
Pattern 2 passthrough (mashup-mods skill), same shape as `examples/minecraft-gta5-passthrough`:
- **BeamNG is the physics + renderer for the car.** A Lua mod (GE extension `gtaxbeam_bridge`) runs in a
  void level with no ground. GTA's nearby world arrives over UDP and becomes invisible `ProceduralMesh`
  collision (`createObject('ProceduralMesh')`, `createMesh`, `be:reloadCollision()` — the same calls
  BeamNG's own `techCore.placeObject` uses). GTA's driving inputs go to `input.event(..., FILTER_DIRECT)`.
  BeamNG sends back pose / velocity / damage every frame.
- **GTA is the world.** A ScriptHookV ASI hides the player inside an invisible proxy vehicle whose pose copies
  BeamNG's car, so peds and traffic collide with it. It forwards controls, sends the camera, streams ground
  heights + wall hits around the car.
- **Rendering:** BeamNG renders from GTA's camera; a ReShade add-on in BeamNG exports colour + depth into
  shared memory; GTA's ReShade compositor depth-tests it into the frame (Minecraft example's path).
- **Why not alternatives:** reimplementing soft-body physics is out of the question; drawing BeamNG's mesh
  with GTA natives (DRAW_POLY) has no lighting/texturing and doesn't scale; BeamNG.tech's shared-memory
  camera sensor is licence-gated (`techCore` returns `missingLicenseFeature` on .drive).
- **Legacy, not Enhanced:** ScriptHookV + ReShade-as-ASI compositing is proven only on Legacy (DX11).

## BeamNG facts (read from the shipped 0.39 Lua, `lua/`)
- GE extensions: `lua/ge/extensions/<dir>/<file>.lua` -> name `<dir>_<file>`. Mods auto-load extensions from
  `scripts/<mod>/modScript.lua` (`core/modmanager.lua` runs every `/scripts/**/modScript.lua`; call
  `setExtensionUnloadMode(name, "manual")`).
- `socket` (LuaSocket) is a global in GE (`core/remoteController.lua` uses `socket.udp()` directly).
  `jsonEncode` / `jsonDecode` are globals (`common/utils.lua`).
- Vehicle (GE object): `getPositionXYZ`, `getVelocityXYZ`, `getDirectionVectorXYZ` (forward),
  `getDirectionVectorUpXYZ`, `setPositionRotation(x,y,z,qx,qy,qz,qw)`, `queueLuaCommand(str)`.
  Bounding box: `be:getObjectOOBBCenterXYZ(id)`, `be:getObjectOOBBHalfExtentsXYZ(id)`.
  Damage: `map.objects[id].damage`.
- Spawning: `core_vehicles.spawnNewVehicle(model, {config=, pos=vec3, rot=quat})`, `core_vehicles.replaceVehicle`.
- Level load: `freeroam_freeroam.startFreeroam(levelPathOrTable, startPoint, wasDelayed, spawnVehicle)` ->
  `core_levels.startLevel(path, ...)`.
- Camera: `commands.setFreeCamera()`, `core_camera.setPosRot(0, px,py,pz, qx,qy,qz,qw)` (free cam only),
  `core_camera.setFOV(0, deg)`. `quatFromDir(dir, up)` in `common/mathlib.lua`.
- Input: vehicle-Lua `input.event(type, value, filter)`; `FILTER_DIRECT = 2`. `steering` -1 = left
  (`scenario/scenarios.lua`: steer_left step = -1), same sign as GTA `INPUT_VEH_MOVE_LR`.
- Pause: `simTimeAuthority.pause(bool)` (global, `lua/ge/simTimeAuthority.lua`).
- Both games: metres, Z up, right-handed, "forward" = +Y at zero rotation. World mapping GTA -> BeamNG is the
  identity (BeamNG's void level is placed at GTA's coordinates).

## Measured in game (BeamNG 0.39.4, `host/fakegta.py`)
- **ProceduralMesh collision is one-sided, and the colliding face is clockwise seen from the side it pushes
  towards** (DirectX order). With counter-clockwise ground the ETK 800 fell 30 m straight through. With both
  windings it "rested", but the wheel nodes were trapped between the two faces: engine at 2000 rpm in D,
  throttle 0.7, brakes off, tyres intact, wheel speed 0, and about 450 damage from standing still. So ground is
  one clockwise face. Each wall segment is a 0.3 m slab with an outward face on each side.
- With the clockwise faces (all 6 checks pass): it rests at ref z 30.2 on z = 30 ground with 0 damage. Steer -1
  plus throttle 0.4 turns it 81° left in 2.5 s. Throttle 0.7 reaches 20.8 m/s in 58 m. It hits the proxy wall
  and stops 2 m short of it (the car's front half) with heavy deformation and 28 `crash` events. The watchdog
  holds it still.
- First launch of 0.39 shows an "Enable Online features?" privacy dialog that pauses physics, and loading a
  level from Lua leaves the main menu drawn over it. The bridge now re-asserts pause state every frame
  while linked, and sends `ChangeState {state='play'}` when the void level is ready. The privacy choice
  belongs to the user; don't click it.
- Invisible collision: `baseColorFactor` alpha 0 + `translucent` still rendered an opaque grey slab. The v1.5
  material needs `opacityFactor: 0` (+ `alphaTest`, `alphaRef 127`). With that the ground isn't drawn at
  all, and all 6 checks still pass, so render state doesn't affect collision.
- Hot reload: `python host/fakegta.py --reload` re-reads `bridge.lua` from the unpacked mod (re-run
  `beamng/install.ps1` first). That saves the ~40 s game restart per change.

## First in-game drive (2026-10-07)
- It works end to end. F6 in Los Santos streamed 9 tiles, the ETK 800 settled at street height (damage 2),
  GTA's enter put the player in the proxy, and GTA's controls drove the car at up to 19 m/s. A pole at 19 m/s
  took damage 2 -> 72,000 and left the car wrapped around the pole.
- User report: "wheels kinda in the ground, handles and drives like shit, stuck in the pole". Causes found:
  - **BeamNG caps itself at 30 fps when unfocused** (`fpsLimitBackgroundEnabled`, default true, 30), and it is
    never focused while GTA is played. State out and inputs in both run at BeamNG's frame rate, so the proxy
    jumped at 30 Hz and the inputs lagged. BeamNG.tech's `techCore` turns this setting off for the same reason.
    The bridge now does that while linked and restores the user's value on unload.
  - Keyboard inputs are 0/1, and FILTER_DIRECT applied them unsmoothed: instant full steering lock and full
    throttle. GTA now sends `kb` (`IS_USING_KEYBOARD_AND_MOUSE`, 0xA571D46727E2B718), and the bridge uses
    FILTER_KBD (0) or FILTER_PAD (1).
  - Proxy height: it aligned box centres, which ignores the different body shapes. Now the proxy's model min z
    (tyre bottoms) sits on BeamNG's box bottom (`pos - up * half.z`). `half` is [width, length, height] even
    when wrecked (measured 1.01, 2.335, 0.695). Between BeamNG updates the proxy is extrapolated along the
    velocity (up to 0.1 s).
  - Walls were resent every completed sweep (every 3 frames), and each resend rebuilds BeamNG's collision. Now
    they're sent only when the set changes, at most every 250 ms.
  - BeamNG stalls more than 2.5 s while loading a vehicle, so GTA reported "lost BeamNG" mid-spawn. The link
    timeout is now 15 s during LoadingLevel/Spawning.

- Second drive, user report: "the wheels don't spin, damage doesn't show, hitting cars or people makes them
  disappear". The first two are what you'd expect: GTA showed its stand-in tailgater, not BeamNG's car. The third:
  the proxy was teleported every frame (`SET_ENTITY_COORDS`), and teleporting into a ped or car makes GTA throw it
  out with a huge impulse. Now the proxy is steered by velocity (BeamNG's velocity + 12/s x position error) and
  only snapped when more than 3 m off (spawn, F9).

## Compositor (BeamNG's car drawn into GTA's picture)
- BeamNG side: ReShade 6.8.0 (add-on build) as `Bin64\dxgi.dll`, `GTAxBeamExport.fx` writes the picture with
  coverage (depth buffer not clear) and the raw depth, and `GTAxBeamExport.addon64` reads them back (ring of 4,
  no GPU waits) into `Local\GTAxBeamFrame` (`shared/gxb_frame.h`). It refuses to load in BeamNG's CEF helper
  processes (same exe, `--type=` on the command line).
- **BeamNG 0.39.4 has no working D3D11 renderer**: `-gfx d3d11` (the exe's names are d3d11 / d3d12 / vulkan)
  lists only the Null adapter and stops with "no valid rendering device", with or without ReShade. ReShade works
  on its D3D12 renderer as dxgi.dll. BeamNG warns about the third-party DLL at every start until you press Cancel.
- Depth, measured with `host/depthcal.py` (camera 8/20/60/150 m behind the car, centre pixel from the shared
  memory): **reversed Z, near 0.1 m, far 4172 m** (fit residual 2e-12; the fitted rear-surface offset, 2.29 m,
  matches the ETK 800's half length). These are GTAxBeam.fx's defaults.
- Fov: BeamNG's camera fov is **vertical**, like GTA's. depthcal back-projects every car pixel with its depth: a
  vertical 50 degree fov puts the widest point at 0.91 m (the ETK 800's body is 1.82 m wide), a horizontal one at
  0.81 m. So the bridge passes GTA's fov through unchanged and the effect's fov scale is 1.
- GTA side: ReShade as `ReShade64.asi` (loaded by ScriptHookV's ASI loader), the add-on half inside GTAxBeam.asi
  uploads the newest frame into BNGCOLOR/BNGDEPTH, `GTAxBeam.fx` maps each GTA pixel's view ray into BeamNG's
  picture (same camera, vertical fov, any aspect), depth-tests against GTA's reversed-Z depth, softens the outline,
  trims BeamNG's sky fringe and adds a contact shadow. The proxy hides automatically while frames arrive.
- No re-projection yet: BeamNG's picture is a frame or two older than GTA's, so the car can lag the camera on
  fast turns.
- BeamNG's window should have GTA's aspect (16:9); a narrower window crops the car at the sides of GTA's screen.

## First composite in game (2026-10-07)
- The pipeline ran (1920x1057 BeamNG frames, ~130 uploads/s in GTA, proxy hid itself), but: "looks like
  photoshop, can't get in the car, doesn't take damage when rammed with another car".
  - Can't get in: the proxy was hidden with SET_ENTITY_VISIBLE false, and GTA won't enter an invisible vehicle.
    Now it is hidden by alpha 0, and INPUT_ENTER near the car gives TASK_ENTER_VEHICLE for it.
  - Rammed by GTA cars: they never existed in BeamNG. Now GTA sends the nearest 3 vehicles within 30 m
    (`traffic`), and the bridge drives a pool of hidden BeamNG cars along with them (setClusterPosRelRot +
    applyClusterVelocityScaleAdd every frame, setMeshAlpha 0, parked with setActive(0)).
    `host/ramtest.py`: a GTA car at 15 m/s into the parked BeamNG car, damage 559 -> 10051; the ghost adds no
    pixels to the exported picture (11712 vs 11713).
  - Looks: BeamNG lights the car itself and nothing matches it to GTA yet. Needs a screenshot to target.

## Steps
- [x] Recon (`python -m um scan`), toolchain inventory, BeamNG API reading.
- [x] BeamNG mod + void level; fake GTA host (`host/fakegta.py`) oracle passes: the car rests on proxy ground,
      drives and steers on forwarded input, and stops at a proxy wall with damage.
- [x] GTA ASI builds (`gta/build.bat`, MSVC 18.10, /W3 clean). Installed into GTA V Legacy with
      `gta/install.ps1` (user approved).
- [ ] GTA ASI in game: link, spawn/enter/exit, proxy vehicle, inputs, camera, ground/wall probes.
- [x] Hide BeamNG's HUD for the exported frame (`ui_visibility.set(false)` on level ready; `hud{on}` to show).
- [x] Compositor BeamNG half: exporter publishes at BeamNG's frame rate, depth calibrated.
- [ ] Compositor in GTA: first composite in game (latency, GTA depth buffer found by ReShade).

## Open questions / to verify in game
- Cost of `be:reloadCollision()` per tile update.
- Dynamic GTA traffic as BeamNG colliders (v1: only GTA side reacts via the proxy vehicle).
