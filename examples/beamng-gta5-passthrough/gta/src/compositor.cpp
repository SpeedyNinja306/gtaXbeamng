#include "compositor.h"
#include "../../shared/gxb_frame.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <reshade.hpp>

using namespace reshade::api;

namespace
{
	constexpr const char *kEffect = "GTAxBeam.fx";
	constexpr float kCarRectMargin = 0.35f; // metres around the car's box where the effect still looks for it

	std::atomic<bool> g_registered{false};
	std::atomic<bool> g_active{false};
	std::atomic<float> g_hostFov{50.0f}, g_hostNear{0.15f}, g_hostFar{10000.0f};
	std::atomic<ULONGLONG> g_lastUploadAt{0};

	std::mutex g_viewLock;
	compositor::CarView g_view; // newest from the script

	HANDLE g_mapping = nullptr;
	const uint8_t *g_map = nullptr;
	int32_t g_slots = 0;
	int64_t g_stride = 0;
	int64_t g_lastPublish = -1;
	ULONGLONG g_nextOpenAttempt = 0;
	ULONGLONG g_nextLogAt = 0;
	unsigned g_uploads = 0;
	int64_t g_lagSum = 0, g_lagMax = 0, g_lagCount = 0;

	struct Layer
	{
		resource tex = {0};
		resource_view srv = {0};
	};
	Layer g_color, g_depth;
	uint32_t g_width = 0, g_height = 0;
	bool g_hasFrame = false;

	// the uploaded frame
	struct Frame
	{
		uint32_t rect[4] = {};
		gxb::CamPose cam;
	} g_frame;

	template <typename T>
	T read(const uint8_t *p)
	{
		T v;
		std::memcpy(&v, p, sizeof(T));
		return v;
	}

	bool open_mapping()
	{
		if (g_map != nullptr)
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
			{
				static bool told = false;
				if (!told && read<uint32_t>(header) == gxb::kMagic)
				{
					told = true;
					reshade::log::message(reshade::log::level::warning,
						"GTAxBeam: BeamNG's exporter is another version than this ASI: reinstall both halves");
				}
				UnmapViewOfFile(header);
			}
			CloseHandle(g_mapping);
			g_mapping = nullptr;
			return false;
		}
		g_slots = read<int32_t>(header + 12);
		g_stride = read<int64_t>(header + 16);
		UnmapViewOfFile(header);
		g_map = static_cast<const uint8_t *>(MapViewOfFile(g_mapping, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(gxb::kHeaderBytes + g_stride * g_slots)));
		if (g_map == nullptr)
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

	/// Upload the newest published BeamNG frame, if there is one we haven't shown yet. Only the car's rectangle travels;
	/// it lands in the textures' top-left corner (GTAxBeam.fx offsets by BngRect).
	void upload(effect_runtime *runtime, int hostTag)
	{
		const int64_t published = read<int64_t>(g_map + 32);
		if (published == g_lastPublish)
			return;
		const int32_t slot = read<int32_t>(g_map + 40);
		if (slot < 0 || slot >= g_slots)
			return;
		const uint8_t *desc = g_map + gxb::kSlotDescOffset + gxb::kSlotDescBytes * slot;
		const int64_t seq = read<int64_t>(desc);
		if (seq & 1)
			return;
		const uint32_t w = read<uint32_t>(desc + 24), h = read<uint32_t>(desc + 28);
		if (w == 0 || h == 0 || w > gxb::kMaxWidth || h > gxb::kMaxHeight)
			return;
		Frame f;
		for (int i = 0; i < 4; ++i)
			f.rect[i] = read<uint32_t>(desc + 32 + 4 * i);
		if (f.rect[0] + f.rect[2] > w || f.rect[1] + f.rect[3] > h)
			return;
		f.cam.tag = read<int64_t>(desc + 16);
		f.cam.flags = read<uint32_t>(desc + 48);
		f.cam.fov = read<float>(desc + 52);
		std::memcpy(f.cam.pos, desc + 56, 12);
		std::memcpy(f.cam.fwd, desc + 68, 12);
		std::memcpy(f.cam.up, desc + 80, 12);
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
		const uint32_t rw = f.rect[2], rh = f.rect[3];
		if (rw > 0 && rh > 0)
		{
			const uint8_t *base = g_map + gxb::kHeaderBytes + g_stride * slot;
			const size_t layer = size_t(rw) * rh * 4;
			subresource_data data;
			data.row_pitch = rw * 4;
			data.slice_pitch = static_cast<uint32_t>(layer);
			subresource_box box;
			box.right = rw;
			box.bottom = rh;
			box.back = 1;
			data.data = const_cast<uint8_t *>(base);
			dev->update_texture_region(data, g_color.tex, 0, &box);
			data.data = const_cast<uint8_t *>(base + layer);
			dev->update_texture_region(data, g_depth.tex, 0, &box);
		}
		if (read<int64_t>(desc) != seq)
			return; // BeamNG rewrote the slot mid-copy: show the next one instead
		g_frame = f;
		g_lastPublish = published;
		g_hasFrame = true;
		g_lastUploadAt = GetTickCount64();
		++g_uploads;
		if (f.cam.tag >= 0 && hostTag > 0)
		{
			const int64_t lag = hostTag - f.cam.tag;
			if (lag >= 0 && lag < 600)
			{
				g_lagSum += lag;
				g_lagMax = std::max(g_lagMax, lag);
				++g_lagCount;
			}
		}
	}

	/// Camera-space axes (x right, y up, z backwards: the camera looks down -z) of a camera given by forward and up.
	void camera_axes(const float fwdIn[3], const float upIn[3], float x[3], float y[3], float z[3])
	{
		float f[3] = {fwdIn[0], fwdIn[1], fwdIn[2]};
		float l = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
		for (float &c : f)
			c /= l > 1e-6f ? l : 1.0f;
		const float d = upIn[0] * f[0] + upIn[1] * f[1] + upIn[2] * f[2];
		float u[3] = {upIn[0] - f[0] * d, upIn[1] - f[1] * d, upIn[2] - f[2] * d};
		l = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
		for (float &c : u)
			c /= l > 1e-6f ? l : 1.0f;
		x[0] = f[1] * u[2] - f[2] * u[1];
		x[1] = f[2] * u[0] - f[0] * u[2];
		x[2] = f[0] * u[1] - f[1] * u[0];
		for (int i = 0; i < 3; ++i)
		{
			y[i] = u[i];
			z[i] = -f[i];
		}
	}

	float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

	void set_float(effect_runtime *runtime, const char *name, const float *v, size_t n)
	{
		if (const effect_uniform_variable u = runtime->find_uniform_variable(kEffect, name); u.handle != 0)
			runtime->set_uniform_value_float(u, v, n);
	}

	float get_float(effect_runtime *runtime, const char *name, float fallback)
	{
		float v = fallback;
		if (const effect_uniform_variable u = runtime->find_uniform_variable(kEffect, name); u.handle != 0)
			runtime->get_uniform_value_float(u, &v, 1);
		return v;
	}

	/// Re-projection uniforms: the rigid transform from GTA's current camera space to the camera space BeamNG rendered
	/// the uploaded frame with (both relative to the car), and the car's rectangle on GTA's screen.
	void set_warp(effect_runtime *runtime, const compositor::CarView &view)
	{
		const bool relative = (g_frame.cam.flags & gxb::kCamRelative) != 0;
		const bool warp = view.valid && relative;
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "WarpOn"); v.handle != 0)
			runtime->set_uniform_value_bool(v, warp);
		const float d2r = 3.14159265f / 180.0f;
		const float tanHost = std::tan(g_hostFov.load() * d2r * 0.5f);
		const float bngTan = g_frame.cam.fov > 1.0f ? std::tan(g_frame.cam.fov * d2r * 0.5f) : tanHost;
		set_float(runtime, "BngTan", &bngTan, 1);
		const float rect[4] = {float(g_frame.rect[0]), float(g_frame.rect[1]), float(g_frame.rect[2]), float(g_frame.rect[3])};
		set_float(runtime, "BngRect", rect, 4);
		float carRect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
		if (warp)
		{
			// GTA's camera, moved by the car's lead (in frames of the car's velocity: the proxy as rendered can be a
			// frame ahead of or behind the pose the script read)
			const float lead = get_float(runtime, "CarLead", 0.0f) * view.frameTime;
			float tn[3];
			for (int i = 0; i < 3; ++i)
				tn[i] = view.pos[i] - view.vel[i] * lead;
			float nx[3], ny[3], nz[3], ux[3], uy[3], uz[3];
			camera_axes(view.fwd, view.up, nx, ny, nz);
			camera_axes(g_frame.cam.fwd, g_frame.cam.up, ux, uy, uz);
			const float *ucol[3] = {ux, uy, uz}, *ncol[3] = {nx, ny, nz};
			float rows[3][3], t[3];
			const float dt[3] = {tn[0] - g_frame.cam.pos[0], tn[1] - g_frame.cam.pos[1], tn[2] - g_frame.cam.pos[2]};
			for (int i = 0; i < 3; ++i)
			{
				for (int j = 0; j < 3; ++j)
					rows[i][j] = dot3(ucol[i], ncol[j]);
				t[i] = dot3(ucol[i], dt);
			}
			set_float(runtime, "WarpRow0", rows[0], 3);
			set_float(runtime, "WarpRow1", rows[1], 3);
			set_float(runtime, "WarpRow2", rows[2], 3);
			set_float(runtime, "WarpT", t, 3);

			// the car's box (plus a margin) on GTA's screen, in uv; anything behind the camera: the whole screen
			uint32_t sw = 0, sh = 0;
			runtime->get_screenshot_width_and_height(&sw, &sh);
			const float aspect = sh ? float(sw) / float(sh) : 16.0f / 9.0f;
			float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
			bool behind = false;
			for (int k = 0; k < 8 && !behind; ++k)
			{
				const float corner[3] = {((k & 1) ? 1.0f : -1.0f) * (view.half[0] + kCarRectMargin),
					((k & 2) ? 1.0f : -1.0f) * (view.half[1] + kCarRectMargin), ((k & 4) ? 1.0f : -1.0f) * (view.half[2] + kCarRectMargin)};
				const float d[3] = {corner[0] - tn[0], corner[1] - tn[1], corner[2] - tn[2]};
				const float cz = dot3(nz, d);
				if (cz > -0.1f)
				{
					behind = true;
					break;
				}
				const float sx = dot3(nx, d) / (-cz * tanHost * aspect), sy = dot3(ny, d) / (-cz * tanHost);
				const float u = sx * 0.5f + 0.5f, v = 0.5f - sy * 0.5f;
				lo[0] = std::min(lo[0], u);
				lo[1] = std::min(lo[1], v);
				hi[0] = std::max(hi[0], u);
				hi[1] = std::max(hi[1], v);
			}
			if (!behind)
			{
				carRect[0] = lo[0];
				carRect[1] = lo[1];
				carRect[2] = hi[0];
				carRect[3] = hi[1];
			}
		}
		set_float(runtime, "CarRect", carRect, 4);
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
		compositor::CarView view;
		{
			std::lock_guard<std::mutex> lock(g_viewLock);
			view = g_view;
		}
		bool on = g_active && open_mapping();
		if (on)
			upload(runtime, view.valid ? view.tag : 0);
		// a frame older than this means BeamNG stopped (closed, paused, its window minimised): show GTA alone
		on = on && g_hasFrame && GetTickCount64() - g_lastUploadAt.load() < 1000;
		// The technique stays enabled (preset); BngActive gates it, so GTA passes through untouched until a BeamNG
		// frame is here. (Toggling techniques from inside this callback crashes ReShade.)
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "BngActive"); v.handle != 0)
			runtime->set_uniform_value_bool(v, on);
		if (GetTickCount64() >= g_nextLogAt)
		{
			g_nextLogAt = GetTickCount64() + 10000;
			char line[260];
			std::snprintf(line, sizeof(line),
				"GTAxBeam: active %d, mapped %d, %u uploads in 10 s, BeamNG frame %ux%u (car rect %ux%u), picture %.2f GTA frames "
				"behind its camera (most %lld), %s",
				g_active.load() ? 1 : 0, g_map ? 1 : 0, g_uploads, g_width, g_height, g_frame.rect[2], g_frame.rect[3],
				g_lagCount ? double(g_lagSum) / g_lagCount : 0.0, static_cast<long long>(g_lagMax),
				(g_frame.cam.flags & gxb::kCamRelative) ? "car-relative" : "world camera");
			reshade::log::message(reshade::log::level::info, line);
			g_uploads = 0;
			g_lagSum = g_lagMax = g_lagCount = 0;
		}
		if (!on)
			return;
		const float planes[2] = {g_hostNear.load(), g_hostFar.load()};
		set_float(runtime, "HostPlanes", planes, 2);
		const float tanHost = std::tan(g_hostFov.load() * 3.14159265f / 360.0f);
		set_float(runtime, "HostTan", &tanHost, 1);
		const float size[2] = {float(g_width), float(g_height)};
		set_float(runtime, "BngSize", size, 2);
		set_warp(runtime, view);
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

	void set_car_view(const CarView &view)
	{
		std::lock_guard<std::mutex> lock(g_viewLock);
		g_view = view;
	}

	bool frames_arriving()
	{
		return g_registered && g_lastUploadAt.load() != 0 && GetTickCount64() - g_lastUploadAt.load() < 1000;
	}
}
