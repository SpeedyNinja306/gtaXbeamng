// Layout of the shared memory "Local\GTAxBeamFrame": BeamNG's picture, written by the GTAxBeamExport add-on inside
// BeamNG, read by the compositor inside GTA. All little-endian, no padding surprises: fixed byte offsets.
//
// header (kHeaderBytes)
//   +0   u32 magic (kMagic)          +4   u32 version (kVersion)
//   +12  i32 slots                   +16  i64 slot stride in bytes
//   +32  i64 published counter       +40  i32 newest complete slot
// slot descriptors at kSlotDescOffset + kSlotDescBytes * slot
//   +0   i64 seq (odd while the slot is being written)
//   +8   i64 frame counter of the BeamNG frame
//   +16  i64 tag of the camera BeamNG rendered with (GTA's frame counter in its cam message), -1: none
//   +24  u32 width                   +28  u32 height          (the whole picture)
//   +32  u32 rect x, +36 rect y, +40 rect width, +44 rect height  (the part holding the car; width 0: nothing drawn)
//   +48  u32 flags (kCamRelative)    +52  f32 BeamNG's vertical fov (degrees), 0: unknown
//   +56  f32[3] camera position, +68 f32[3] forward, +80 f32[3] up: with kCamRelative, in the car's box frame
//        (x right, y forward, z up, origin at the box centre), else in world space
// slot data at kHeaderBytes + stride * slot, only the rect:
//   colour: rect width * rect height RGBA8, rows top-down, premultiplied (alpha = BeamNG drew geometry there)
//   depth:  rect width * rect height float32, BeamNG's raw depth-buffer value
//
// Render camera: the bridge (BeamNG's Lua, in its camera mode, right before BeamNG renders the frame) sends it as one
// UDP datagram to 127.0.0.1:kCamPort; the exporter latches the newest one with the picture of that frame. Text:
//   "gxbcam <tag> <flags> <vertical fov deg> <pos x y z> <fwd x y z> <up x y z>" (same meaning as in the slot
//   descriptor). BeamNG's LuaJIT doesn't allow declaring C functions, so Lua can't map shared memory itself.
#pragma once
#include <cstdint>

namespace gxb
{
	constexpr const wchar_t *kMappingName = L"Local\\GTAxBeamFrame";
	constexpr uint32_t kMagic = 0x46425847; // "GXBF"
	constexpr uint32_t kVersion = 2;
	constexpr int kHeaderBytes = 4096;
	constexpr int kSlotDescOffset = 256;
	constexpr int kSlotDescBytes = 128;
	constexpr int kSlots = 3;
	constexpr uint32_t kMaxWidth = 3840, kMaxHeight = 2160;
	constexpr int64_t kSlotStride = int64_t(kMaxWidth) * kMaxHeight * 8;
	constexpr int64_t kMappingBytes = kHeaderBytes + kSlotStride * kSlots;

	constexpr uint16_t kCamPort = 47802;
	constexpr uint32_t kCamRelative = 1;

	struct CamPose
	{
		int64_t tag = -1;
		uint32_t flags = 0;
		float fov = 0;
		float pos[3] = {}, fwd[3] = {0, 1, 0}, up[3] = {0, 0, 1};
	};
}
