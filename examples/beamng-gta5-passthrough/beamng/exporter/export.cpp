// GTAxBeamExport.addon64: a ReShade add-on loaded into BeamNG.drive. After ReShade runs GTAxBeamExport.fx, it copies
// that effect's two textures (picture with coverage, raw depth) into CPU-readable textures and publishes them in the
// shared memory GTA's compositor reads (layout: shared/gxb_frame.h), with the camera the frame was rendered with
// (the bridge sends it over UDP right before each frame renders; latched here).
//
// Readback goes through a ring of kRing textures. Each copy signals a fence; every frame the newest copy the GPU has
// finished is published, so BeamNG's render thread never waits and a picture is out as soon as the GPU is done with
// it (without fences: the copy recorded kRing - 1 frames earlier). Only the rectangle that holds the car is published.
#include "../../shared/gxb_frame.h"
#include <winsock2.h>
#include <windows.h>
#include <algorithm>
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
	constexpr uint32_t kScanStep = 2; // coverage scan stride (px) for the car's rectangle
	constexpr uint32_t kRectPad = 4;  // px around it

	struct Readback
	{
		resource color = {0}, depth = {0};
		int64_t frame = -1; // BeamNG frame whose copy this holds, -1: free
		uint64_t fenceValue = 0;
		gxb::CamPose cam;
	};

	HANDLE g_mapping = nullptr;
	uint8_t *g_view = nullptr;
	SOCKET g_camSocket = INVALID_SOCKET;
	bool g_camSocketTried = false;
	gxb::CamPose g_camPose;
	Readback g_ring[kRing];
	uint32_t g_width = 0, g_height = 0;
	fence g_fence = {0};
	bool g_fenceTried = false;
	uint64_t g_fenceValue = 0;
	int64_t g_frame = 0;
	int64_t g_published = 0;
	int g_nextSlot = 0;

	// statistics for the log, every 5 s
	int64_t g_statFrames = 0, g_statPublished = 0, g_statLagSum = 0, g_statDropped = 0;

	template <typename T>
	void write(uint8_t *p, T v)
	{
		std::memcpy(p, &v, sizeof(T));
	}

	template <typename T>
	T read(const uint8_t *p)
	{
		T v;
		std::memcpy(&v, p, sizeof(T));
		return v;
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

	/// The newest render camera the bridge sent (it sends one right before BeamNG renders each frame).
	gxb::CamPose read_camera()
	{
		if (g_camSocket == INVALID_SOCKET && !g_camSocketTried)
		{
			g_camSocketTried = true;
			WSADATA wsa;
			if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0)
			{
				g_camSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
				sockaddr_in addr = {};
				addr.sin_family = AF_INET;
				addr.sin_port = htons(gxb::kCamPort);
				addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
				u_long nonBlocking = 1;
				if (g_camSocket == INVALID_SOCKET || bind(g_camSocket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
					ioctlsocket(g_camSocket, FIONBIO, &nonBlocking) != 0)
				{
					if (g_camSocket != INVALID_SOCKET)
						closesocket(g_camSocket);
					g_camSocket = INVALID_SOCKET;
					WSACleanup();
				}
			}
			reshade::log::message(g_camSocket != INVALID_SOCKET ? reshade::log::level::info : reshade::log::level::warning,
				g_camSocket != INVALID_SOCKET ? "GTAxBeamExport: listening for the bridge's camera on 127.0.0.1:47802"
											  : "GTAxBeamExport: can't listen for the bridge's camera (port 47802 taken?)");
		}
		if (g_camSocket == INVALID_SOCKET)
			return g_camPose;
		char buf[512];
		for (;;)
		{
			const int n = recv(g_camSocket, buf, sizeof(buf) - 1, 0);
			if (n <= 0)
				break;
			buf[n] = 0;
			gxb::CamPose p;
			long long tag = -1;
			unsigned flags = 0;
			if (sscanf_s(buf, "gxbcam %lld %u %f %f %f %f %f %f %f %f %f %f", &tag, &flags, &p.fov, &p.pos[0], &p.pos[1],
					&p.pos[2], &p.fwd[0], &p.fwd[1], &p.fwd[2], &p.up[0], &p.up[1], &p.up[2]) == 12)
			{
				p.tag = tag;
				p.flags = flags;
				g_camPose = p;
			}
		}
		return g_camPose;
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

	struct Rect
	{
		uint32_t x = 0, y = 0, w = 0, h = 0;
	};

	/// The rectangle of the picture BeamNG drew geometry into (alpha != 0), padded; empty when nothing was drawn.
	Rect coverage_rect(const uint8_t *src, uint32_t pitch)
	{
		uint32_t x0 = g_width, y0 = g_height, x1 = 0, y1 = 0;
		for (uint32_t y = 0; y < g_height; y += kScanStep)
		{
			const uint8_t *row = src + size_t(y) * pitch;
			bool any = false;
			for (uint32_t x = 0; x < g_width; x += kScanStep)
				if (row[size_t(x) * 4 + 3] != 0)
				{
					x0 = std::min(x0, x);
					x1 = std::max(x1, x);
					any = true;
				}
			if (any)
			{
				y0 = std::min(y0, y);
				y1 = y;
			}
		}
		Rect r;
		if (x0 > x1 || y0 > y1)
			return r;
		const uint32_t pad = kRectPad + kScanStep;
		r.x = x0 > pad ? x0 - pad : 0;
		r.y = y0 > pad ? y0 - pad : 0;
		r.w = std::min(g_width, x1 + pad + 1) - r.x;
		r.h = std::min(g_height, y1 + pad + 1) - r.y;
		return r;
	}

	/// Every 5 s in ReShade.log: frame rate, how many frames the published pictures lag behind, the car's rectangle and
	/// its raw depth range (to calibrate GTAxBeam.fx's BeamNG depth against a known camera distance).
	void log_stats(const uint8_t *color, const float *depth, const Rect &rc, const gxb::CamPose &cam)
	{
		static ULONGLONG next = 0;
		if (GetTickCount64() < next)
			return;
		const bool first = next == 0;
		next = GetTickCount64() + 5000;
		if (first)
			return;
		float lo = 1e30f, hi = -1e30f;
		size_t covered = 0;
		for (uint32_t i = 0; i < rc.w * rc.h; i += 2)
			if (color[size_t(i) * 4 + 3] != 0)
			{
				lo = std::min(lo, depth[i]);
				hi = std::max(hi, depth[i]);
				++covered;
			}
		char line[320];
		std::snprintf(line, sizeof(line),
			"GTAxBeamExport: %ux%u, %.1f fps, %.1f published/s, %.2f frames behind, %lld not ready; car rect %ux%u at %u,%u "
			"(covers %.1f%%), depth %.6g..%.6g; camera tag %lld%s",
			g_width, g_height, g_statFrames / 5.0, g_statPublished / 5.0, g_statPublished ? double(g_statLagSum) / g_statPublished : 0.0,
			static_cast<long long>(g_statDropped), rc.w, rc.h, rc.x, rc.y, 200.0 * covered / (double(g_width) * g_height),
			covered ? lo : 0.0f, covered ? hi : 0.0f, static_cast<long long>(cam.tag), (cam.flags & gxb::kCamRelative) ? ", car-relative" : "");
		reshade::log::message(reshade::log::level::info, line);
		g_statFrames = g_statPublished = g_statLagSum = g_statDropped = 0;
	}

	void publish(device *dev, const Readback &r)
	{
		subresource_data cm = {}, dm = {};
		if (!dev->map_texture_region(r.color, 0, nullptr, map_access::read_only, &cm) || !cm.data)
			return;
		if (!dev->map_texture_region(r.depth, 0, nullptr, map_access::read_only, &dm) || !dm.data)
		{
			dev->unmap_texture_region(r.color, 0);
			return;
		}
		const auto *csrc = static_cast<const uint8_t *>(cm.data);
		const auto *dsrc = static_cast<const uint8_t *>(dm.data);
		const Rect rc = coverage_rect(csrc, cm.row_pitch);

		const int slot = g_nextSlot;
		uint8_t *desc = g_view + gxb::kSlotDescOffset + gxb::kSlotDescBytes * slot;
		uint8_t *base = g_view + gxb::kHeaderBytes + gxb::kSlotStride * slot;
		const int64_t seq = read<int64_t>(desc);
		write<int64_t>(desc, seq | 1);
		MemoryBarrier();
		const size_t rowBytes = size_t(rc.w) * 4;
		uint8_t *cdst = base, *ddst = base + rowBytes * rc.h;
		for (uint32_t y = 0; y < rc.h; ++y)
		{
			std::memcpy(cdst + rowBytes * y, csrc + size_t(rc.y + y) * cm.row_pitch + size_t(rc.x) * 4, rowBytes);
			std::memcpy(ddst + rowBytes * y, dsrc + size_t(rc.y + y) * dm.row_pitch + size_t(rc.x) * 4, rowBytes);
		}
		dev->unmap_texture_region(r.depth, 0);
		dev->unmap_texture_region(r.color, 0);
		write<int64_t>(desc + 8, r.frame);
		write<int64_t>(desc + 16, r.cam.tag);
		write<uint32_t>(desc + 24, g_width);
		write<uint32_t>(desc + 28, g_height);
		write<uint32_t>(desc + 32, rc.x);
		write<uint32_t>(desc + 36, rc.y);
		write<uint32_t>(desc + 40, rc.w);
		write<uint32_t>(desc + 44, rc.h);
		write<uint32_t>(desc + 48, r.cam.flags);
		write<float>(desc + 52, r.cam.fov);
		std::memcpy(desc + 56, r.cam.pos, 12);
		std::memcpy(desc + 68, r.cam.fwd, 12);
		std::memcpy(desc + 80, r.cam.up, 12);
		MemoryBarrier();
		write<int64_t>(desc, (seq | 1) + 1);
		write<int32_t>(g_view + 40, slot);
		MemoryBarrier();
		write<int64_t>(g_view + 32, ++g_published);
		g_nextSlot = (g_nextSlot + 1) % gxb::kSlots;
		++g_statPublished;
		g_statLagSum += g_frame - r.frame;
		log_stats(cdst, reinterpret_cast<const float *>(ddst), rc, r.cam);
	}

	/// Publish the newest copy the GPU has finished, and free it and every older one.
	void publish_ready(device *dev)
	{
		const uint64_t done = g_fence.handle ? dev->get_completed_fence_value(g_fence) : 0;
		Readback *best = nullptr;
		for (Readback &r : g_ring)
		{
			if (r.frame < 0)
				continue;
			const bool ready = g_fence.handle ? r.fenceValue <= done : r.frame <= g_frame - (kRing - 1);
			if (ready && (!best || r.frame > best->frame))
				best = &r;
		}
		if (!best)
			return;
		publish(dev, *best);
		const int64_t upTo = best->frame;
		for (Readback &r : g_ring)
			if (r.frame >= 0 && r.frame <= upTo)
				r.frame = -1;
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
		// the fence only covers the copies if they go through the queue's immediate list, which gets flushed before it
		command_queue *queue = runtime->get_command_queue();
		if (!g_fenceTried)
		{
			g_fenceTried = true;
			const bool ok = queue && cmd == queue->get_immediate_command_list() && dev->create_fence(0, fence_flags::none, &g_fence);
			if (!ok)
				g_fence = {0};
			reshade::log::message(reshade::log::level::info,
				ok ? "GTAxBeamExport: fenced readback" : "GTAxBeamExport: no fence, readback lags a fixed 3 frames");
		}
		++g_statFrames;

		publish_ready(dev);

		Readback *r = nullptr;
		for (Readback &slot : g_ring)
			if (slot.frame < 0)
			{
				r = &slot;
				break;
			}
		if (!r)
		{
			++g_statDropped; // the GPU is kRing frames behind: skip this frame's copy
			++g_frame;
			return;
		}
		cmd->barrier(color, resource_usage::shader_resource, resource_usage::copy_source);
		cmd->barrier(depth, resource_usage::shader_resource, resource_usage::copy_source);
		cmd->copy_texture_region(color, 0, nullptr, r->color, 0, nullptr);
		cmd->copy_texture_region(depth, 0, nullptr, r->depth, 0, nullptr);
		cmd->barrier(color, resource_usage::copy_source, resource_usage::shader_resource);
		cmd->barrier(depth, resource_usage::copy_source, resource_usage::shader_resource);
		r->frame = g_frame;
		r->cam = read_camera();
		if (g_fence.handle)
		{
			queue->flush_immediate_command_list();
			queue->signal(g_fence, ++g_fenceValue);
			r->fenceValue = g_fenceValue;
		}
		++g_frame;
	}

	void on_destroy_effect_runtime(effect_runtime *runtime)
	{
		device *dev = runtime->get_device();
		destroy_ring(dev);
		if (g_fence.handle)
			dev->destroy_fence(g_fence);
		g_fence = {0};
		g_fenceTried = false;
		g_fenceValue = 0;
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
		if (g_camSocket != INVALID_SOCKET)
		{
			closesocket(g_camSocket);
			WSACleanup();
		}
		break;
	}
	return TRUE;
}
