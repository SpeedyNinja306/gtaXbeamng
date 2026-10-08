-- GTA V <-> BeamNG.drive link. Message format: examples/beamng-gta5-passthrough/PROTOCOL.md.
-- Inert until GTA (or host/fakegta.py) sends the first datagram.

local M = {}

local logTag = 'gtaxbeam'
local PORT = 47801
local LEVEL_NAME = 'gtaxbeam_void'
local WATCHDOG_S = 0.5
local MAX_DGRAM = 65535
local CRASH_DELTA = 50

local udp, peerIp, peerPort
local clock, lastRecv = 0, nil
local watchdogPaused, userPaused = false, false

local vid, hadVehicle = nil, false
local seq = 0
local lastDamage
local pendingPlace

local pieces, pieceMsgs = {}, {}
local pieceSerial = 0
local collisionDirty = false
local debugVisible = false
-- BeamNG collides with triangles that are clockwise seen from the side they push towards (measured on 0.39:
-- 'ccw' ground lets the car fall through, 'both' traps wheel nodes between the two faces). 'ccw'/'both' are for tests.
local winding = 'cw'
local WALL_HALF_THICK = 0.15
local reloadRequested = false

local camera
local fovHorizontal = false
local inputState = {th = 0, br = 0, st = 0, hb = 0, filter = 2}
local savedBackgroundLimit -- the user's fpsLimitBackgroundEnabled, restored on unload

local function v3(a) return vec3(a[1], a[2], a[3]) end
local function r3(x) return math.floor(x * 1000 + 0.5) / 1000 end
local function r4(x) return math.floor(x * 10000 + 0.5) / 10000 end

local function send(tbl)
  if udp and peerIp then udp:sendto(jsonEncode(tbl), peerIp, peerPort) end
end

local function levelLoaded()
  return scenetree.MissionGroup ~= nil and getCurrentLevelIdentifier() == LEVEL_NAME
end

local function getVeh()
  if not vid then return nil end
  local veh = getObjectByID(vid)
  if not veh then vid = nil end
  return veh
end

-- GTA owns pause: BeamNG's own UI (menus, first-run dialogs) also pauses physics, so re-assert every frame.
local function setPaused()
  local want = watchdogPaused or userPaused
  if (not be:getEnabled()) ~= want then simTimeAuthority.pause(want) end
end

-- collision ---------------------------------------------------------------

local function material()
  return debugVisible and 'gtaxbeam_debug' or 'gtaxbeam_invisible'
end

-- a, b, c are counter-clockwise seen from the side the surface should push nodes towards.
local function addTri(faces, a, b, c)
  if winding ~= 'cw' then
    faces[#faces + 1] = {v = a, n = 0, u = 0}
    faces[#faces + 1] = {v = b, n = 0, u = 0}
    faces[#faces + 1] = {v = c, n = 0, u = 0}
  end
  if winding ~= 'ccw' then
    faces[#faces + 1] = {v = a, n = 0, u = 0}
    faces[#faces + 1] = {v = c, n = 0, u = 0}
    faces[#faces + 1] = {v = b, n = 0, u = 0}
  end
end

local function meshOf(verts, faces)
  if #faces == 0 then return nil end
  return {verts = verts, faces = faces, normals = {{x = 0, y = 0, z = 1}}, uvs = {{u = 0, v = 0}}, material = material()}
end

local function tileMesh(m)
  local n, step, h = m.n, m.step, m.h
  local function valid(k)
    local z = h[k + 1]
    return z ~= nil and z > -9999
  end
  local verts, faces = {}, {}
  for j = 0, n - 1 do
    for i = 0, n - 1 do
      local k = j * n + i
      verts[#verts + 1] = {x = i * step, y = j * step, z = valid(k) and h[k + 1] or 0}
    end
  end
  for j = 0, n - 2 do
    for i = 0, n - 2 do
      local a = j * n + i
      local b, c = a + 1, a + n
      local d = c + 1
      if valid(a) and valid(b) and valid(c) and valid(d) then
        addTri(faces, a, b, d)
        addTri(faces, a, d, c)
      end
    end
  end
  return meshOf(verts, faces), vec3(m.ox, m.oy, 0)
end

local function wallsMesh(m)
  local segs = m.segs or {}
  if #segs == 0 then return nil end
  local ox, oy = segs[1][1], segs[1][2]
  local verts, faces = {}, {}
  -- Each segment is a thin slab: one outward-facing quad per side, so it pushes nodes away from both sides.
  local function quad(x1, y1, x2, y2, zb, zt)
    local k = #verts
    verts[k + 1] = {x = x1 - ox, y = y1 - oy, z = zb}
    verts[k + 2] = {x = x2 - ox, y = y2 - oy, z = zb}
    verts[k + 3] = {x = x2 - ox, y = y2 - oy, z = zt}
    verts[k + 4] = {x = x1 - ox, y = y1 - oy, z = zt}
    addTri(faces, k, k + 1, k + 2)
    addTri(faces, k, k + 2, k + 3)
  end
  for _, s in ipairs(segs) do
    local dx, dy = s[3] - s[1], s[4] - s[2]
    local len = math.sqrt(dx * dx + dy * dy)
    if len > 1e-3 then
      -- quad(p1 -> p2) faces right of the segment direction: (dy, -dx)
      local nx, ny = dy / len * WALL_HALF_THICK, -dx / len * WALL_HALF_THICK
      quad(s[1] + nx, s[2] + ny, s[3] + nx, s[4] + ny, s[5], s[6])
      quad(s[3] - nx, s[4] - ny, s[1] - nx, s[2] - ny, s[5], s[6])
    end
  end
  return meshOf(verts, faces), vec3(ox, oy, 0)
end

local function deletePiece(id)
  local p = pieces[id]
  if p then
    p:delete()
    pieces[id] = nil
    collisionDirty = true
  end
end

local function buildPiece(id, m)
  deletePiece(id)
  pieceMsgs[id] = m
  local mesh, origin
  if m.t == 'tile' then mesh, origin = tileMesh(m) else mesh, origin = wallsMesh(m) end
  if not mesh then return end
  local proc = createObject('ProceduralMesh')
  if not proc then return end
  pieceSerial = pieceSerial + 1
  proc:registerObject('gtaxbeam_piece_' .. pieceSerial)
  proc.canSave = false
  local group = scenetree.findObject('GTAxBeamCollision')
  if not group then
    group = createObject('SimGroup')
    group:registerObject('GTAxBeamCollision')
    group.canSave = false
    scenetree.MissionGroup:addObject(group)
  end
  group:addObject(proc.obj)
  proc:createMesh({{mesh}})
  proc:setPosition(origin)
  proc.scale = vec3(1, 1, 1)
  pieces[id] = proc
  collisionDirty = true
end

local function clearPieces()
  for id in pairs(pieces) do deletePiece(id) end
  pieceMsgs = {}
end

-- vehicle -----------------------------------------------------------------

local function placeVehicle(veh, m, reset)
  local rot = quatFromDir(v3(m.fwd), v3(m.up))
  -- safeTeleport applies BeamNG's 180-degree model flip to a quatFromDir(forward, up) rotation itself.
  spawn.safeTeleport(veh, v3(m.pos), rot, true, nil, false, false, reset)
end

local function spawnVehicle(m)
  local opt = {
    config = m.config,
    pos = v3(m.pos) + vec3(0, 0, 0.5),
    rot = quatFromDir(v3(m.fwd), v3(m.up)),
    autoEnterVehicle = true,
  }
  local veh = getVeh()
  if veh then
    veh = core_vehicles.replaceVehicle(m.model, opt, veh)
  else
    veh = core_vehicles.spawnNewVehicle(m.model, opt)
  end
  if not veh then
    log('E', logTag, 'spawn failed: ' .. tostring(m.model))
    return
  end
  vid = veh:getID()
  lastDamage = nil
  pendingPlace = {m = m, reset = true, frames = 2}
  log('I', logTag, 'spawned ' .. tostring(m.model) .. ' as ' .. tostring(vid))
end

local function applyInput()
  local veh = getVeh()
  if not veh then return end
  local s = inputState
  local f = s.filter
  veh:queueLuaCommand(string.format(
    "input.event('throttle',%.3f,%d) input.event('brake',%.3f,%d) input.event('steering',%.3f,%d) input.event('parkingbrake',%.3f,%d)",
    s.th, f, s.br, f, s.st, f, s.hb, f))
end

local function sendState()
  local veh = getVeh()
  if not veh then
    if hadVehicle then
      hadVehicle = false
      send({t = 'gone'})
    end
    return
  end
  hadVehicle = true
  seq = seq + 1
  local px, py, pz = veh:getPositionXYZ()
  local cx, cy, cz = be:getObjectOOBBCenterXYZ(vid)
  local hx, hy, hz = be:getObjectOOBBHalfExtentsXYZ(vid)
  local fx, fy, fz = veh:getDirectionVectorXYZ()
  local ux, uy, uz = veh:getDirectionVectorUpXYZ()
  local vx, vy, vz = veh:getVelocityXYZ()
  local o = map and map.objects and map.objects[vid]
  local dmg = (o and o.damage) or 0
  send({
    t = 'veh', seq = seq, vid = vid,
    pos = {r3(cx), r3(cy), r3(cz)},
    ref = {r3(px), r3(py), r3(pz)},
    fwd = {r4(fx), r4(fy), r4(fz)},
    up = {r4(ux), r4(uy), r4(uz)},
    vel = {r3(vx), r3(vy), r3(vz)},
    half = {r3(hx), r3(hy), r3(hz)},
    dmg = r3(dmg),
  })
  if lastDamage and dmg - lastDamage > CRASH_DELTA then
    send({t = 'crash', dmg = r3(dmg), delta = r3(dmg - lastDamage), pos = {r3(cx), r3(cy), r3(cz)}})
  end
  lastDamage = dmg
end

-- GTA traffic ---------------------------------------------------------------
-- Each GTA vehicle near the car (traffic message) gets a hidden BeamNG car from a small pool that is moved to its
-- pose and given its velocity every frame, so a GTA car hitting the BeamNG car deforms it. Spawned one at a time
-- (each spawn stalls BeamNG for a moment), parked with setActive(0) when not needed.

local GHOST_MODEL = 'fullsize'
local GHOST_POOL = 3
local GHOST_SPAWN_GAP_S = 3
local ghosts = {}          -- {vid, gta = GTA handle or nil, offset = ref node in the car's frame (right, fwd, up)}
local trafficCars = {}
local nextGhostSpawn = 0

local function ghostVeh(g)
  local veh = getObjectByID(g.vid)
  if not veh then g.dead = true end
  return veh
end

local function frameOf(veh)
  local f = vec3(veh:getDirectionVectorXYZ())
  local u = vec3(veh:getDirectionVectorUpXYZ())
  return f, u, f:cross(u)
end

local function spawnGhost()
  local own = getVeh()
  if not own then return end
  local p = own:getPosition()
  local veh = core_vehicles.spawnNewVehicle(GHOST_MODEL, {
    pos = vec3(p.x, p.y, p.z + 300 + 20 * #ghosts),
    rot = quatFromDir(vec3(0, 1, 0), vec3(0, 0, 1)),
    autoEnterVehicle = false,
  })
  if not veh then return end
  veh:setMeshAlpha(0, '')
  veh:setActive(0)
  ghosts[#ghosts + 1] = {vid = veh:getID()}
  log('I', logTag, 'ghost car ' .. veh:getID() .. ' ready (' .. #ghosts .. '/' .. GHOST_POOL .. ')')
end

local function parkGhost(g)
  g.gta = nil
  local veh = ghostVeh(g)
  if not veh then return end
  local o = map and map.objects and map.objects[g.vid]
  if o and (o.damage or 0) > 0 then veh:queueLuaCommand('obj:requestReset(RESET_PHYSICS)') end
  veh:setActive(0)
end

local function driveGhost(g, c)
  local veh = ghostVeh(g)
  if not veh then return end
  local f, u, r = frameOf(veh)
  if not g.offset then
    -- the ref node relative to the box centre, in the car's own frame (measured once, undeformed)
    local d = veh:getPosition() - vec3(be:getObjectOOBBCenterXYZ(g.vid))
    g.offset = vec3(d:dot(r), d:dot(f), d:dot(u))
  end
  local tf, tu = v3(c.f), v3(c.u)
  local tr = tf:cross(tu)
  local o = g.offset
  local pos = v3(c.c) + tr * o.x + tf * o.y + tu * o.z
  local diff = quatFromDir(f, u):inversed() * quatFromDir(tf, tu)
  local ref = veh:getRefNodeId()
  veh:setClusterPosRelRot(ref, pos.x, pos.y, pos.z, diff.x, diff.y, diff.z, diff.w)
  veh:applyClusterVelocityScaleAdd(ref, 0, c.v[1], c.v[2], c.v[3])
end

local function updateGhosts()
  for i = #ghosts, 1, -1 do
    if ghosts[i].dead or not getObjectByID(ghosts[i].vid) then table.remove(ghosts, i) end
  end
  local own = getVeh()
  if not own or not levelLoaded() then return end
  if #trafficCars > 0 and #ghosts < GHOST_POOL and clock >= nextGhostSpawn then
    nextGhostSpawn = clock + GHOST_SPAWN_GAP_S
    spawnGhost()
  end
  local wanted = {}
  for _, c in ipairs(trafficCars) do wanted[c.id] = c end
  for _, g in ipairs(ghosts) do
    if g.gta and not wanted[g.gta] then parkGhost(g) end
  end
  for _, c in ipairs(trafficCars) do
    local mine
    for _, g in ipairs(ghosts) do
      if g.gta == c.id then mine = g end
    end
    if not mine then
      for _, g in ipairs(ghosts) do
        if not g.gta then
          local veh = ghostVeh(g)
          if veh then
            mine = g
            g.gta = c.id
            veh:setActive(1)
            veh:setMeshAlpha(0, '')
          end
          break
        end
      end
    end
    if mine then driveGhost(mine, c) end
  end
end

local function deleteGhosts()
  for _, g in ipairs(ghosts) do
    local veh = getObjectByID(g.vid)
    if veh then veh:delete() end
  end
  ghosts, trafficCars = {}, {}
end

-- camera ------------------------------------------------------------------

local function ensureCameraMode()
  if not core_camera then return end
  if core_camera.getActiveGlobalCameraName(0) ~= 'gtaxbeam' then
    core_camera.setGlobalCameraByName('gtaxbeam')
  end
end

local function setCamera(m)
  camera = camera or {pos = vec3(), rot = quat(0, 0, 0, 1), fov = 60}
  camera.pos:set(m.pos[1], m.pos[2], m.pos[3])
  camera.rot = quatFromDir(v3(m.fwd), v3(m.up))
  if m.fov then
    if fovHorizontal and m.aspect then
      camera.fov = math.deg(2 * math.atan(math.tan(math.rad(m.fov) / 2) * m.aspect))
    else
      camera.fov = m.fov
    end
  end
  ensureCameraMode()
end

-- level -------------------------------------------------------------------

local function loadLevel()
  if levelLoaded() then
    send({t = 'hello', v = 1, level = true, vid = vid or false})
    return
  end
  if not freeroam_freeroam then extensions.load('freeroam_freeroam') end
  local level = core_levels.getLevelByName(LEVEL_NAME)
  if not level then
    log('E', logTag, 'level ' .. LEVEL_NAME .. ' not found; is the mod installed?')
    return
  end
  freeroam_freeroam.startFreeroam(level, nil, nil, false)
end

-- messages ----------------------------------------------------------------

local handlers = {}

handlers.hello = function() send({t = 'hello', v = 1, level = levelLoaded(), vid = vid or false}) end
handlers.level = function() loadLevel() end
handlers.spawn = function(m) spawnVehicle(m) end
handlers.place = function(m)
  local veh = getVeh()
  if veh then placeVehicle(veh, m, false) end
end
handlers.remove = function()
  local veh = getVeh()
  if veh then veh:delete() end
  vid = nil
  deleteGhosts()
end
handlers.traffic = function(m)
  local cars = {}
  for _, c in ipairs(m.cars or {}) do
    if c.id and c.c and c.f and c.u and c.v then cars[#cars + 1] = c end
  end
  trafficCars = cars
end
handlers.input = function(m)
  inputState.th = m.th or 0
  inputState.br = m.br or 0
  inputState.st = m.st or 0
  inputState.hb = m.hb or 0
  -- kb: BeamNG's keyboard ramps (FILTER_KBD); false: game pad (FILTER_PAD); absent: exact values (FILTER_DIRECT)
  if m.kb == nil then inputState.filter = 2 elseif m.kb then inputState.filter = 0 else inputState.filter = 1 end
  applyInput()
end
handlers.cam = function(m) setCamera(m) end
-- Development: re-read this file from the unpacked mod folder (applied at the end of the frame).
handlers.reload = function() reloadRequested = true end
handlers.tile = function(m) buildPiece(m.id, m) end
handlers.walls = function(m) buildPiece(m.id, m) end
handlers.drop = function(m)
  deletePiece(m.id)
  pieceMsgs[m.id] = nil
end
handlers.clear = function() clearPieces() end
handlers.pause = function(m)
  userPaused = m.on and true or false
  setPaused()
end
handlers.debug = function(m)
  debugVisible = m.on and true or false
  local msgs = pieceMsgs
  pieceMsgs = {}
  for id, pm in pairs(msgs) do buildPiece(id, pm) end
end
handlers.hud = function(m)
  if ui_visibility then ui_visibility.set(m.on and true or false) end
end
handlers.tod = function(m)
  -- BeamNG's TimeOfDay: 0 = noon, 0.5 = midnight.
  if core_environment and m.h then
    core_environment.setTimeOfDay({time = ((m.h - 12) / 24) % 1, play = false})
  end
end
-- Diagnostics: the vehicle's own view of its controls and drivetrain, answered as a 'probe' message.
local PROBE_LUA = [[
local w = {}
for _, wd in pairs(wheels.wheels or {}) do w[#w + 1] = wd.isTireDeflated and 1 or 0 end
local e = electrics.values
obj:queueGameEngineLua('gtaxbeam_bridge.onProbe(' .. serialize({
  th = e.throttle, thIn = e.throttle_input, br = e.brake, brIn = e.brake_input, st = e.steering_input,
  pb = e.parkingbrake, gear = e.gear, rpm = e.rpm, ign = e.ignitionLevel, running = e.engineRunning,
  ws = e.wheelspeed, deflated = w,
}) .. ')')
]]
handlers.probe = function()
  local veh = getVeh()
  if veh then veh:queueLuaCommand(PROBE_LUA) end
end
M.onProbe = function(d)
  if type(d) ~= 'table' then return end
  d.t = 'probe'
  send(d)
end
handlers.cfg = function(m)
  if m.fovHorizontal ~= nil then fovHorizontal = m.fovHorizontal and true or false end
  if m.winding == 'ccw' or m.winding == 'cw' or m.winding == 'both' then
    winding = m.winding
    local msgs = pieceMsgs
    pieceMsgs = {}
    for id, pm in pairs(msgs) do buildPiece(id, pm) end
  end
end

local function handle(m)
  local h = handlers[m.t]
  if h then h(m) end
end

local function poll()
  if not udp then return end
  for _ = 1, 512 do
    local data, ip, port = udp:receivefrom(MAX_DGRAM)
    if not data then break end
    peerIp, peerPort = ip, port
    lastRecv = clock
    -- BeamNG is never the focused window while GTA is played; its 30 fps background cap would throttle the link
    if savedBackgroundLimit == nil then
      savedBackgroundLimit = settings.getValue('fpsLimitBackgroundEnabled') and true or false
      settings.setValue('fpsLimitBackgroundEnabled', false)
    end
    if watchdogPaused then
      watchdogPaused = false
      setPaused()
    end
    local m = jsonDecode(data, logTag)
    if type(m) == 'table' then
      local ok, err = pcall(handle, m)
      if not ok then log('E', logTag, 'message ' .. tostring(m.t) .. ': ' .. tostring(err)) end
    end
  end
end

-- hooks -------------------------------------------------------------------

local function onExtensionLoaded()
  udp = socket.udp()
  local ok, err = udp:setsockname('127.0.0.1', PORT)
  if not ok then
    log('E', logTag, 'cannot bind 127.0.0.1:' .. PORT .. ': ' .. tostring(err))
    udp:close()
    udp = nil
    return
  end
  udp:settimeout(0)
  log('I', logTag, 'listening on 127.0.0.1:' .. PORT)
  -- After a live reload: drop the old collision pieces and keep driving the same vehicle.
  if levelLoaded() then
    local group = scenetree.findObject('GTAxBeamCollision')
    if group then
      group:deleteAllObjects()
      be:reloadCollision()
    end
    local veh = getPlayerVehicle(0)
    if veh then vid = veh:getID() end
    -- the old instance's ghost cars: the void level holds nothing else but the player's car
    for i = be:getObjectCount() - 1, 0, -1 do
      local o = be:getObject(i)
      if o and o:getID() ~= vid then o:delete() end
    end
  end
end

local function onExtensionUnloaded()
  if savedBackgroundLimit ~= nil then
    settings.setValue('fpsLimitBackgroundEnabled', savedBackgroundLimit)
    savedBackgroundLimit = nil
  end
  if udp then
    udp:close()
    udp = nil
  end
end

local function onUpdate(dtReal)
  clock = clock + (dtReal or 0)
  poll()
  if lastRecv and not watchdogPaused and clock - lastRecv > WATCHDOG_S then
    watchdogPaused = true
    inputState.th, inputState.br, inputState.st = 0, 0, 0
    applyInput()
  end
  if lastRecv then setPaused() end
  if collisionDirty then
    be:reloadCollision()
    collisionDirty = false
  end
  if pendingPlace then
    pendingPlace.frames = pendingPlace.frames - 1
    if pendingPlace.frames <= 0 then
      local veh = getVeh()
      if veh then placeVehicle(veh, pendingPlace.m, pendingPlace.reset) end
      pendingPlace = nil
    end
  end
  if not watchdogPaused then updateGhosts() end
  sendState()
  if reloadRequested then
    reloadRequested = false
    log('I', logTag, 'reloading from disk')
    extensions.reload('gtaxbeam_bridge')
  end
end

local function onClientPostStartMission()
  pieces, pieceMsgs = {}, {}
  if levelLoaded() then
    log('I', logTag, 'void level ready')
    -- Loading from Lua leaves the main menu drawn over the level; the frame we export must be clean.
    guihooks.trigger('ChangeState', {state = 'play'})
    if ui_visibility then ui_visibility.set(false) end
    send({t = 'hello', v = 1, level = true, vid = vid or false})
  end
end

local function onClientEndMission()
  pieces, pieceMsgs = {}, {}
  vid = nil
  ghosts, trafficCars = {}, {}
end

M.getCamera = function() return camera end

M.onExtensionLoaded = onExtensionLoaded
M.onExtensionUnloaded = onExtensionUnloaded
M.onUpdate = onUpdate
M.onPreRender = poll
M.onClientPostStartMission = onClientPostStartMission
M.onClientEndMission = onClientEndMission

return M
