// GTAxBeamExport.addon64: a ReShade add-on loaded into BeamNG.drive. After ReShade runs GTAxBeamExport.fx, it copies
// that effect's two textures (picture with coverage, raw depth) into CPU-readable textures and publishes them in the
// shared memory GTA's compositor reads (layout: shared/gxb_frame.h).
//
// Readback goes through a ring of kRing textures: the copy recorded this frame is mapped kRing - 1 frames later, when
// the GPU has long finished it (D3D12 keeps up to 3 frames in flight), so BeamNG's render thread never waits on it.
#include "../../shared/gxb_frame.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <reshade.hpp>

using namespace reshade::api;

extern "C" __declspec(dllexport) const char *NAME = "GTAxBeamExport";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Publishes BeamNG's picture and depth for GTA V's compositor (GTA x BeamNG).";

namespace
{
	constexpr const char *kEffect = "GTAxBeamExport.fx";
	constexpr int kRing = 4;

	struct Readback
	{
		resource color = {0}, depth = {0};
		int64_t frame = -1; // BeamNG frame whose copy this holds, -1: none pending
	};

	HANDLE g_mapping = nullptr;
	uint8_t *g_view = nullptr;
	Readback g_ring[kRing];
	uint32_t g_width = 0, g_height = 0;
	int64_t g_frame = 0;
	int64_t g_published = 0;
	int g_nextSlot = 0;

	template <typename T>
	void write(uint8_t *p, T v)
	{
		std::memcpy(p, &v, sizeof(T));
	}

	bool open_mapping()
	{
		if (g_view)
			return true;
		g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(uint64_t(gxb::kMappingBytes) >> 32),
			DWORD(uint64_t(gxb::kMappingBytes) & 0xFFFFFFFFu), gxb::kMappingName);
		if (!g_mapping)
			return false;
		g_view = static_cast<uint8_t *>(MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
		if (!g_view)
		{
			CloseHandle(g_mapping);
			g_mapping = nullptr;
			return false;
		}
		write<int32_t>(g_view + 12, gxb::kSlots);
		write<int64_t>(g_view + 16, gxb::kSlotStride);
		write<int64_t>(g_view + 32, 0);
		write<int32_t>(g_view + 40, -1);
		write<uint32_t>(g_view + 4, gxb::kVersion);
		write<uint32_t>(g_view + 0, gxb::kMagic);
		reshade::log::message(reshade::log::level::info, "GTAxBeamExport: shared memory ready");
		return true;
	}

	void destroy_ring(device *dev)
	{
		for (Readback &r : g_ring)
		{
			if (r.color.handle)
				dev->destroy_resource(r.color);
			if (r.depth.handle)
				dev->destroy_resource(r.depth);
			r = Readback();
		}
		g_width = g_height = 0;
	}

	bool create_ring(device *dev, uint32_t w, uint32_t h)
	{
		for (Readback &r : g_ring)
		{
			if (!dev->create_resource(resource_desc(w, h, 1, 1, format::r8g8b8a8_unorm, 1, memory_heap::readback, resource_usage::copy_dest),
					nullptr, resource_usage::copy_dest, &r.color) ||
				!dev->create_resource(resource_desc(w, h, 1, 1, format::r32_float, 1, memory_heap::readback, resource_usage::copy_dest),
					nullptr, resource_usage::copy_dest, &r.depth))
			{
				destroy_ring(dev);
				return false;
			}
		}
		g_width = w;
		g_height = h;
		return true;
	}

	bool effect_texture(effect_runtime *runtime, const char *name, resource &out)
	{
		const effect_texture_variable var = runtime->find_texture_variable(kEffect, name);
		if (var.handle == 0)
			return false;
		resource_view srv = {0}, srgb = {0};
		runtime->get_texture_binding(var, &srv, &srgb);
		if (srv.handle == 0)
			return false;
		out = runtime->get_device()->get_resource_from_view(srv);
		return out.handle != 0;
	}

	/// Copy one mapped readback texture into the slot, row by row (the mapping's row pitch may be padded).
	bool copy_out(device *dev, resource tex, uint8_t *dst, uint32_t rowBytes)
	{
		subresource_data data = {};
		if (!dev->map_texture_region(tex, 0, nullptr, map_access::read_only, &data) || !data.data)
			return false;
		const auto *src = static_cast<const uint8_t *>(data.data);
		for (uint32_t y = 0; y < g_height; ++y)
			std::memcpy(dst + size_t(y) * rowBytes, src + size_t(y) * data.row_pitch, rowBytes);
		dev->unmap_texture_region(tex, 0);
		return true;
	}

	/// Every 5 s: the raw depth range over the car (covered pixels) and at the screen centre, in ReShade.log, to
	/// calibrate GTAxBeam.fx's BeamNG depth settings against the camera's known distance to the car.
	void log_depth(const uint8_t *base, size_t colorBytes)
	{
		static ULONGLONG next = 0;
		if (GetTickCount64() < next)
			return;
		next = GetTickCount64() + 5000;
		const auto *depth = reinterpret_cast<const float *>(base + colorBytes);
		float lo = 1e30f, hi = -1e30f;
		size_t covered = 0;
		for (uint32_t y = 0; y < g_height; y += 2)
			for (uint32_t x = 0; x < g_width; x += 2)
				if (base[(size_t(y) * g_width + x) * 4 + 3] != 0)
				{
					const float d = depth[size_t(y) * g_width + x];
					lo = d < lo ? d : lo;
					hi = d > hi ? d : hi;
					++covered;
				}
		char line[200];
		std::snprintf(line, sizeof(line), "GTAxBeamExport: %ux%u published %lld, car covers %.1f%%, depth %.6g..%.6g, centre %.6g",
			g_width, g_height, static_cast<long long>(g_published), 400.0 * covered / (double(g_width) * g_height), covered ? lo : 0.0f,
			covered ? hi : 0.0f, depth[size_t(g_height / 2) * g_width + g_width / 2]);
		reshade::log::message(reshade::log::level::info, line);
	}

	void publish(device *dev, Readback &r)
	{
		const int slot = g_nextSlot;
		uint8_t *desc = g_view + gxb::kSlotDescOffset + gxb::kSlotDescBytes * slot;
		uint8_t *base = g_view + gxb::kHeaderBytes + gxb::kSlotStride * slot;
		int64_t seq;
		std::memcpy(&seq, desc, sizeof(seq));
		write<int64_t>(desc, seq | 1);
		MemoryBarrier();
		const size_t colorBytes = size_t(g_width) * g_height * 4;
		const bool ok = copy_out(dev, r.color, base, g_width * 4) && copy_out(dev, r.depth, base + colorBytes, g_width * 4);
		write<int64_t>(desc + 8, r.frame);
		write<uint32_t>(desc + 24, g_width);
		write<uint32_t>(desc + 28, g_height);
		MemoryBarrier();
		write<int64_t>(desc, (seq | 1) + 1);
		if (!ok)
			return;
		log_depth(base, colorBytes);
		write<int32_t>(g_view + 40, slot);
		MemoryBarrier();
		write<int64_t>(g_view + 32, ++g_published);
		g_nextSlot = (g_nextSlot + 1) % gxb::kSlots;
	}

	void on_finish_effects(effect_runtime *runtime, command_list *cmd, resource_view, resource_view)
	{
		resource color = {0}, depth = {0};
		if (!effect_texture(runtime, "GxbColorTex", color) || !effect_texture(runtime, "GxbDepthTex", depth))
			return; // the effect isn't loaded (yet)
		if (!open_mapping())
			return;
		device *dev = runtime->get_device();
		const resource_desc desc = dev->get_resource_desc(color);
		const uint32_t w = desc.texture.width, h = desc.texture.height;
		if (w == 0 || h == 0 || w > gxb::kMaxWidth || h > gxb::kMaxHeight)
			return;
		if (w != g_width || h != g_height)
		{
			destroy_ring(dev);
			if (!create_ring(dev, w, h))
				return;
		}

		const int i = int(g_frame % kRing);
		Readback &r = g_ring[i];
		cmd->barrier(color, resource_usage::shader_resource, resource_usage::copy_source);
		cmd->barrier(depth, resource_usage::shader_resource, resource_usage::copy_source);
		cmd->copy_texture_region(color, 0, nullptr, r.color, 0, nullptr);
		cmd->copy_texture_region(depth, 0, nullptr, r.depth, 0, nullptr);
		cmd->barrier(color, resource_usage::copy_source, resource_usage::shader_resource);
		cmd->barrier(depth, resource_usage::copy_source, resource_usage::shader_resource);
		r.frame = g_frame;

		// the oldest copy in the ring (recorded kRing - 1 frames ago) is done by now
		Readback &old = g_ring[(i + 1) % kRing];
		if (old.frame >= 0)
		{
			publish(dev, old);
			old.frame = -1;
		}
		++g_frame;
	}

	void on_destroy_effect_runtime(effect_runtime *runtime)
	{
		destroy_ring(runtime->get_device());
	}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		// BeamNG's UI (CEF) runs helper processes from the same exe, which load ReShade too: only the game publishes
		if (wcsstr(GetCommandLineW(), L"--type=") != nullptr)
			return FALSE;
		if (!reshade::register_addon(module))
			return FALSE;
		reshade::register_event<reshade::addon_event::reshade_finish_effects>(on_finish_effects);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
		break;
	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(module);
		if (g_view)
			UnmapViewOfFile(g_view);
		if (g_mapping)
			CloseHandle(g_mapping);
		break;
	}
	return TRUE;
}
