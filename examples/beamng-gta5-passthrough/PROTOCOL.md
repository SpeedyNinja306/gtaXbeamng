# GTA <-> BeamNG link

UDP on `127.0.0.1`. BeamNG's bridge listens on port **47801** and replies to whichever address sent it the
most recent datagram. One JSON object per datagram, with a type field `t`. Coordinates are world metres in
GTA's frame, which is also BeamNG's frame (the void level sits at GTA's coordinates): X east, Y north, Z up.
Orientations travel as a forward and an up unit vector.

## GTA -> BeamNG

| t | fields | effect |
|---|---|---|
| `hello` | `v` | handshake; BeamNG answers `hello` |
| `level` | | load the void level `gtaxbeam_void` (no vehicle) |
| `spawn` | `model`, `config`?, `pos`, `fwd`, `up` | spawn the BeamNG vehicle, or replace the current one; `pos` is the point on the ground under the car's centre |
| `place` | `pos`, `fwd`, `up` | teleport the vehicle (keeps damage) |
| `remove` | | delete the vehicle |
| `input` | `th`, `br`, `st`, `hb` | throttle 0..1, brake 0..1, steering -1 (left)..1, handbrake 0..1 |
| `cam` | `pos`, `fwd`, `up`, `fov`, `f`?, `aspect`?, `rel`? | drive BeamNG's camera (vertical FOV, degrees). `f`: tag (GTA's frame counter), stamped on every picture rendered with this camera. `rel` `{p, f, u}`: the same camera in the box frame of the car GTA draws (origin at the box centre, x right, y forward, z up); when present BeamNG renders from that offset to its own car at render time, so the car's motion drops out of the picture |
| `tile` | `id`, `ox`, `oy`, `n`, `step`, `h` | ground heightfield: `n`x`n` heights, row-major (row = y), origin (`ox`,`oy`); `h` <= -9999 means no ground |
| `walls` | `id`, `segs` | vertical wall quads, each `[x1, y1, x2, y2, zBottom, zTop]` |
| `drop` | `id` | delete one tile or wall set |
| `clear` | | delete every collision piece |
| `pause` | `on` | pause / resume BeamNG's physics |
| `debug` | `on` | draw collision pieces with a visible material |
| `traffic` | `cars`: list of `{id, c, f, u, v, h}` | GTA vehicles near the car (box centre, forward, up, velocity, half extents; the nearest 3 within 30 m, every frame; `[]` once when none). BeamNG moves a hidden ghost car (pool of 3 `fullsize`, spawned one per 3 s) along with each, so they can hit the BeamNG car |
| `hud` | `on` | show / hide BeamNG's UI (hidden when the void level loads, so the exported frame is clean) |
| `tod` | `h` | time of day, GTA hours 0..24 (BeamNG's sun follows GTA's clock) |
| `cfg` | `fovHorizontal`?, `winding`? | `fovHorizontal`: treat `cam.fov` as vertical and convert to horizontal with `cam.aspect`. `winding` (`cw` default, `ccw`, `both`): collision face order, for tests only |
| `probe` | | diagnostics: BeamNG answers `probe` with the vehicle's own control state |
| `reload` | | development: re-read `bridge.lua` from the mod folder (keeps the vehicle, drops collision) |

## BeamNG -> GTA

| t | fields | meaning |
|---|---|---|
| `hello` | `v`, `level`, `vid` | handshake reply; `level` is true when the void level is loaded |
| `veh` | `seq`, `vid`, `pos`, `ref`, `fwd`, `up`, `vel`, `half`, `dmg` | every frame while a vehicle exists; `pos` is the bounding-box centre, `ref` the reference node, `half` the box half-extents along its own axes, `dmg` BeamNG's damage total |
| `crash` | `dmg`, `delta`, `pos` | damage jumped by `delta` this frame |
| `gone` | | the vehicle no longer exists |
| `probe` | `th`, `thIn`, `br`, `brIn`, `st`, `pb`, `gear`, `rpm`, `ign`, `running`, `ws`, `deflated` | vehicle electrics (applied and raw inputs, gear, rpm, ignition, wheel speed m/s, per-wheel tyre deflated 0/1) |

## Bridge -> exporter (inside BeamNG)
Right before BeamNG renders each frame, the bridge's camera mode sends the camera it renders with as one text
datagram to `127.0.0.1:47802`, where the ReShade exporter listens:
`gxbcam <tag> <flags> <fov> <px py pz> <fx fy fz> <ux uy uz>` (flags 1: car-relative, pose in the car's box frame;
0: world pose). The exporter stamps the newest one on that frame's picture (`shared/gxb_frame.h`), and GTA uses it
to re-project the picture to its current camera. BeamNG's LuaJIT doesn't allow declaring C functions, so the
bridge can't write shared memory itself.

## Watchdog
If no datagram arrives for 0.5 s, BeamNG pauses physics and zeroes the inputs, so the car doesn't roll
away while GTA is paused or loading. The next datagram resumes it (unless GTA sent `pause` with `on`).
While GTA is connected the bridge re-asserts this pause state every frame, because BeamNG's own UI (menus,
first-run dialogs) also pauses physics.
