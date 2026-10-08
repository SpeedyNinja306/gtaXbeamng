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
//   +24  u32 width                   +28  u32 height
// slot data at kHeaderBytes + stride * slot
//   colour: width * height RGBA8, rows top-down, premultiplied (alpha = BeamNG drew geometry there)
//   depth:  width * height float32, BeamNG's raw depth-buffer value
#pragma once
#include <cstdint>

namespace gxb
{
	constexpr const wchar_t *kMappingName = L"Local\\GTAxBeamFrame";
	constexpr uint32_t kMagic = 0x46425847; // "GXBF"
	constexpr uint32_t kVersion = 1;
	constexpr int kHeaderBytes = 4096;
	constexpr int kSlotDescOffset = 256;
	constexpr int kSlotDescBytes = 128;
	constexpr int kSlots = 3;
	constexpr uint32_t kMaxWidth = 3840, kMaxHeight = 2160;
	constexpr int64_t kSlotStride = int64_t(kMaxWidth) * kMaxHeight * 8;
	constexpr int64_t kMappingBytes = kHeaderBytes + kSlotStride * kSlots;
}
