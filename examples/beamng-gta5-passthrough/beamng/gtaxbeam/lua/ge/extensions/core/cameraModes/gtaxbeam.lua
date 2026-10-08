-- Global camera that copies GTA's camera exactly (no smoothing, no input), fed by gtaxbeam_bridge. When GTA sends its
-- camera relative to the car, it is placed relative to BeamNG's car as it is in this very frame.

local C = {}
C.__index = C

function C:init()
  self.isGlobal = true
  self.hidden = true
end

function C:onCameraChanged(focused) end

function C:update(data)
  local bridge = gtaxbeam_bridge
  local cam = bridge and bridge.getCamera()
  if not cam then return false end
  data.res.pos:set(cam.pos)
  data.res.rot = cam.rot
  data.res.fov = cam.fov
  return true
end

return function(...)
  local o = ... or {}
  setmetatable(o, C)
  o:init()
  return o
end
