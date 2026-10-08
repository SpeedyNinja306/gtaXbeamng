#include "compositor.h"
#include "../../shared/gxb_frame.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <reshade.hpp>

using namespace reshade::api;

namespace
{
	constexpr const char *kEffect = "GTAxBeam.fx";

	std::atomic<bool> g_registered{false};
	std::atomic<bool> g_active{false};
	std::atomic<float> g_hostFov{50.0f}, g_hostNear{0.15f}, g_hostFar{10000.0f};
	std::atomic<ULONGLONG> g_lastUploadAt{0};

	HANDLE g_mapping = nullptr;
	const uint8_t *g_view = nullptr;
	int32_t g_slots = 0;
	int64_t g_stride = 0;
	int64_t g_lastPublish = -1;
	ULONGLONG g_nextOpenAttempt = 0;
	ULONGLONG g_nextLogAt = 0;
	unsigned g_uploads = 0;

	struct Layer
	{
		resource tex = {0};
		resource_view srv = {0};
	};
	Layer g_color, g_depth;
	uint32_t g_width = 0, g_height = 0;
	bool g_hasFrame = false;

	template <typename T>
	T read(const uint8_t *p)
	{
		T v;
		std::memcpy(&v, p, sizeof(T));
		return v;
	}

	bool open_mapping()
	{
		if (g_view != nullptr)
			return true;
		if (GetTickCount64() < g_nextOpenAttempt)
			return false;
		g_nextOpenAttempt = GetTickCount64() + 1000;
		g_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, gxb::kMappingName);
		if (g_mapping == nullptr)
			return false;
		const auto *header = static_cast<const uint8_t *>(MapViewOfFile(g_mapping, FILE_MAP_READ, 0, 0, gxb::kHeaderBytes));
		if (header == nullptr || read<uint32_t>(header) != gxb::kMagic || read<uint32_t>(header + 4) != gxb::kVersion)
		{
			if (header != nullptr)
				UnmapViewOfFile(header);
			CloseHandle(g_mapping);
			g_mapping = nullptr;
			return false;
		}
		g_slots = read<int32_t>(header + 12);
		g_stride = read<int64_t>(header + 16);
		UnmapViewOfFile(header);
		g_view = static_cast<const uint8_t *>(MapViewOfFile(g_mapping, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(gxb::kHeaderBytes + g_stride * g_slots)));
		if (g_view == nullptr)
		{
			CloseHandle(g_mapping);
			g_mapping = nullptr;
			return false;
		}
		reshade::log::message(reshade::log::level::info, "GTAxBeam: connected to BeamNG's frame export");
		return true;
	}

	void destroy_layers(device *dev)
	{
		for (Layer *layer : {&g_color, &g_depth})
		{
			if (layer->srv.handle != 0)
				dev->destroy_resource_view(layer->srv);
			if (layer->tex.handle != 0)
				dev->destroy_resource(layer->tex);
			*layer = Layer();
		}
		g_width = g_height = 0;
		g_hasFrame = false;
	}

	bool create_layer(device *dev, Layer &layer, uint32_t w, uint32_t h, format fmt)
	{
		if (!dev->create_resource(
				resource_desc(w, h, 1, 1, fmt, 1, memory_heap::default_, resource_usage::shader_resource | resource_usage::copy_dest),
				nullptr, resource_usage::shader_resource, &layer.tex))
			return false;
		return dev->create_resource_view(layer.tex, resource_usage::shader_resource, resource_view_desc(fmt), &layer.srv);
	}

	void bind(effect_runtime *runtime)
	{
		runtime->update_texture_bindings("BNGCOLOR", g_color.srv, g_color.srv);
		runtime->update_texture_bindings("BNGDEPTH", g_depth.srv, g_depth.srv);
	}

	/// Upload the newest published BeamNG frame, if there is one we haven't shown yet.
	void upload(effect_runtime *runtime)
	{
		const int64_t published = read<int64_t>(g_view + 32);
		if (published == g_lastPublish)
			return;
		const int32_t slot = read<int32_t>(g_view + 40);
		if (slot < 0 || slot >= g_slots)
			return;
		const uint8_t *desc = g_view + gxb::kSlotDescOffset + gxb::kSlotDescBytes * slot;
		const int64_t seq = read<int64_t>(desc);
		if (seq & 1)
			return;
		const uint32_t w = read<uint32_t>(desc + 24), h = read<uint32_t>(desc + 28);
		if (w == 0 || h == 0 || w > gxb::kMaxWidth || h > gxb::kMaxHeight)
			return;
		device *dev = runtime->get_device();
		if (w != g_width || h != g_height)
		{
			destroy_layers(dev);
			if (!create_layer(dev, g_color, w, h, format::r8g8b8a8_unorm) || !create_layer(dev, g_depth, w, h, format::r32_float))
			{
				destroy_layers(dev);
				return;
			}
			g_width = w;
			g_height = h;
			bind(runtime);
		}
		const uint8_t *base = g_view + gxb::kHeaderBytes + g_stride * slot;
		const size_t layer = size_t(w) * h * 4;
		subresource_data data;
		data.row_pitch = w * 4;
		data.slice_pitch = static_cast<uint32_t>(layer);
		data.data = const_cast<uint8_t *>(base);
		dev->update_texture_region(data, g_color.tex, 0);
		data.data = const_cast<uint8_t *>(base + layer);
		dev->update_texture_region(data, g_depth.tex, 0);
		if (read<int64_t>(desc) != seq)
			return; // BeamNG rewrote the slot mid-copy: show the next one instead
		g_lastPublish = published;
		g_hasFrame = true;
		g_lastUploadAt = GetTickCount64();
		++g_uploads;
	}

	/// Reload GTAxBeam.fx when the file changes (ReShade doesn't watch it), so tweaks don't need a GTA restart.
	void watch_effect_file(effect_runtime *runtime)
	{
		static ULONGLONG next = 0;
		static FILETIME last = {};
		static wchar_t path[MAX_PATH] = {};
		if (GetTickCount64() < next)
			return;
		next = GetTickCount64() + 1000;
		if (path[0] == 0)
		{
			GetModuleFileNameW(nullptr, path, MAX_PATH);
			wchar_t *slash = wcsrchr(path, L'\\');
			if (slash)
				wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"reshade-shaders\\Shaders\\GTAxBeam.fx");
		}
		WIN32_FILE_ATTRIBUTE_DATA info;
		if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info))
			return;
		if (last.dwLowDateTime != 0 && CompareFileTime(&info.ftLastWriteTime, &last) != 0)
			runtime->reload_effect_next_frame(kEffect);
		last = info.ftLastWriteTime;
	}

	void on_present(effect_runtime *runtime)
	{
		watch_effect_file(runtime);
	}

	void on_begin_effects(effect_runtime *runtime, command_list *, resource_view, resource_view)
	{
		bool on = g_active && open_mapping();
		if (on)
			upload(runtime);
		// a frame older than this means BeamNG stopped (closed, paused, its window minimised): show GTA alone
		on = on && g_hasFrame && GetTickCount64() - g_lastUploadAt.load() < 1000;
		// The technique stays enabled (preset); BngActive gates it, so GTA passes through untouched until a BeamNG
		// frame is here. (Toggling techniques from inside this callback crashes ReShade.)
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "BngActive"); v.handle != 0)
			runtime->set_uniform_value_bool(v, on);
		if (GetTickCount64() >= g_nextLogAt)
		{
			g_nextLogAt = GetTickCount64() + 10000;
			char line[160];
			std::snprintf(line, sizeof(line), "GTAxBeam: active %d, mapped %d, %u uploads in 10 s, BeamNG frame %ux%u", g_active.load() ? 1 : 0,
				g_view ? 1 : 0, g_uploads, g_width, g_height);
			reshade::log::message(reshade::log::level::info, line);
			g_uploads = 0;
		}
		if (!on)
			return;
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "HostPlanes"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_hostNear.load(), g_hostFar.load());
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "HostTan"); v.handle != 0)
			runtime->set_uniform_value_float(v, std::tan(g_hostFov.load() * 3.14159265f / 360.0f));
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "BngSize"); v.handle != 0)
			runtime->set_uniform_value_float(v, float(g_width), float(g_height));
	}

	void on_reloaded_effects(effect_runtime *runtime)
	{
		if (g_width != 0)
			bind(runtime);
	}

	void on_destroy_effect_runtime(effect_runtime *runtime)
	{
		destroy_layers(runtime->get_device());
	}
}

namespace compositor
{
	bool try_register(void *module)
	{
		if (g_registered)
			return true;
		if (!reshade::register_addon(static_cast<HMODULE>(module)))
			return false;
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
		reshade::register_event<reshade::addon_event::reshade_present>(on_present);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
		g_registered = true;
		reshade::log::message(reshade::log::level::info, "GTAxBeam: registered");
		return true;
	}

	void unregister(void *module)
	{
		if (g_registered.exchange(false))
			reshade::unregister_addon(static_cast<HMODULE>(module));
	}

	void set_active(bool active)
	{
		g_active = active;
	}

	void set_host_camera(float fov, float near_clip, float far_clip)
	{
		g_hostFov = fov;
		g_hostNear = near_clip;
		g_hostFar = far_clip;
	}

	bool frames_arriving()
	{
		return g_registered && g_lastUploadAt.load() != 0 && GetTickCount64() - g_lastUploadAt.load() < 1000;
	}
}
