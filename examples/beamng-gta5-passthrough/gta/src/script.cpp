// ScriptHookV half of the BeamNG passthrough (PROTOCOL.md). BeamNG simulates the car; GTA is the world.
//
// - GTA's ground around the car goes to BeamNG as heightfield tiles, steep surfaces hit by radial probes as walls.
// - A GTA "proxy" vehicle copies BeamNG's pose and velocity every frame, so GTA's peds and traffic collide with it
//   and the player can get in and out with GTA's own enter/exit. GTA's vehicle camera follows it.
// - While the player drives the proxy, GTA's driving controls are disabled and forwarded to BeamNG.
// - GTA's rendered camera goes to BeamNG every frame (for the compositor, which draws BeamNG's car into GTA).
//
// Keys: F6 spawn the BeamNG car in front of you (again: replace it), F9 put it back on its wheels,
//       F5 debug view (show the proxy / BeamNG's collision), F7 remove everything.
#include "compositor.h"
#include "link.h"
#include "natives.h"
#include <main.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace
{
	constexpr unsigned short kBeamngPort = 47801;
	constexpr int kTileN = 33;
	constexpr float kTileStep = 2.0f;
	constexpr float kTileSize = (kTileN - 1) * kTileStep;
	constexpr int kTileKeep = 2;         // tiles further than this (in tiles) from the centre are dropped
	constexpr int kGroundProbesPerFrame = 384;
	constexpr float kNoGround = -10000.0f;
	constexpr int kWallRays = 72;
	constexpr int kWallRaysPerFrame = 24;
	constexpr float kWallRange = 35.0f;
	constexpr float kWallMaxNormalZ = 0.6f; // steeper than ~53 degrees counts as a wall
	constexpr int kProbeFlags = 1 | 16;    // map + objects (not vehicles or peds)
	constexpr int kWallResendMs = 250;
	constexpr float kProxySnapDist = 3.0f; // metres off BeamNG's pose before the proxy is teleported instead of steered
	constexpr float kProxyGain = 12.0f;    // 1/s: position error turned into extra velocity
	constexpr int kLinkTimeoutMs = 2500;
	constexpr int kLinkTimeoutLoadingMs = 15000; // BeamNG stalls while it loads a level or a vehicle

	constexpr int kTrafficMax = 3;         // GTA vehicles mirrored into BeamNG as ghost cars (bridge's pool size)
	constexpr float kTrafficRange = 30.0f; // metres from the BeamNG car

	// INPUT_* control ids
	constexpr int kVehMoveLR = 59, kVehAccelerate = 71, kVehBrake = 72, kVehHandbrake = 76, kEnter = 23;

	struct Config
	{
		char model[64] = "etk800";
		char config[160] = "";
		char proxy[64] = "tailgater";
		bool proxyVisible = true; // whenever the compositor isn't drawing BeamNG's car, the proxy is what you see
	} g_cfg;

	enum class Phase
	{
		Idle,
		LoadingLevel,
		Streaming,
		Spawning,
		Active,
	};

	struct VehState
	{
		bool valid = false;
		float pos[3] = {}, ref[3] = {}, fwd[3] = {0, 1, 0}, up[3] = {0, 0, 1}, vel[3] = {}, half[3] = {1, 2, 0.7f};
		double dmg = 0;
		int at = 0;
	};

	struct Tile
	{
		std::vector<float> h;
		int next = 0;
		float z0 = 0;
		bool sent = false;
	};

	struct WallHit
	{
		bool hit = false;
		float x = 0, y = 0, z = 0, dist = 0;
	};

	HMODULE g_module = nullptr;
	std::atomic<bool> g_keySpawn{false}, g_keyRecover{false}, g_keyDebug{false}, g_keyRemove{false};

	Phase g_phase = Phase::Idle;
	bool g_linkUp = false, g_levelReady = false, g_debug = false;
	int g_lastRecvAt = -100000, g_nextHelloAt = 0, g_nextTodAt = 0, g_phaseAt = 0;

	VehState g_veh;
	Vehicle g_proxy = 0;
	Hash g_proxyHash = 0;
	float g_proxyCentre[3] = {}; // the proxy model point (box centre x/y, lowest z) that sits on BeamNG's box bottom centre
	bool g_driving = false;
	bool g_composited = false; // BeamNG's picture is being drawn into GTA's: the proxy hides

	float g_spawnPos[3] = {}, g_spawnFwd[3] = {0, 1, 0};
	std::map<std::pair<int, int>, Tile> g_tiles;
	WallHit g_wallHits[kWallRays];
	int g_wallNext = 0;

	// ---------------------------------------------------------------- helpers

	int now() { return natives::GetGameTimer(); }

	char g_logPath[MAX_PATH] = "";

	// GTAxBeam.log next to the ASI, truncated at every game start.
	void log_line(const char *fmt, ...)
	{
		if (!g_logPath[0])
			return;
		FILE *f = std::fopen(g_logPath, "a");
		if (!f)
			return;
		std::fprintf(f, "%10.3f ", GetTickCount64() / 1000.0);
		va_list args;
		va_start(args, fmt);
		std::vfprintf(f, fmt, args);
		va_end(args);
		std::fputc('\n', f);
		std::fclose(f);
	}

	void notify(const char *fmt, ...)
	{
		char buf[256];
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		natives::Notify(buf);
		log_line("%s", buf);
	}

	void normalize(float v[3])
	{
		const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (l > 1e-6f)
			for (int i = 0; i < 3; ++i)
				v[i] /= l;
	}

	void cross(const float a[3], const float b[3], float out[3])
	{
		out[0] = a[1] * b[2] - a[2] * b[1];
		out[1] = a[2] * b[0] - a[0] * b[2];
		out[2] = a[0] * b[1] - a[1] * b[0];
	}

	float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

	/// Orthonormal right / forward / up from BeamNG's forward and up vectors.
	void basis(const float fwdIn[3], const float upIn[3], float r[3], float f[3], float u[3])
	{
		std::memcpy(f, fwdIn, sizeof(float) * 3);
		normalize(f);
		const float d = dot(upIn, f);
		for (int i = 0; i < 3; ++i)
			u[i] = upIn[i] - f[i] * d;
		normalize(u);
		cross(f, u, r);
	}

	/// Quaternion (x, y, z, w) of the rotation whose columns are r, f, u (GTA: x right, y forward, z up).
	void quat_from_basis(const float r[3], const float f[3], const float u[3], float q[4])
	{
		const float m00 = r[0], m01 = f[0], m02 = u[0];
		const float m10 = r[1], m11 = f[1], m12 = u[1];
		const float m20 = r[2], m21 = f[2], m22 = u[2];
		const float trace = m00 + m11 + m22;
		if (trace > 0)
		{
			const float s = std::sqrt(trace + 1.0f) * 2;
			q[3] = 0.25f * s;
			q[0] = (m21 - m12) / s;
			q[1] = (m02 - m20) / s;
			q[2] = (m10 - m01) / s;
		}
		else if (m00 > m11 && m00 > m22)
		{
			const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2;
			q[3] = (m21 - m12) / s;
			q[0] = 0.25f * s;
			q[1] = (m01 + m10) / s;
			q[2] = (m02 + m20) / s;
		}
		else if (m11 > m22)
		{
			const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2;
			q[3] = (m02 - m20) / s;
			q[0] = (m01 + m10) / s;
			q[1] = 0.25f * s;
			q[2] = (m12 + m21) / s;
		}
		else
		{
			const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2;
			q[3] = (m10 - m01) / s;
			q[0] = (m02 + m20) / s;
			q[1] = (m12 + m21) / s;
			q[2] = 0.25f * s;
		}
	}

	/// GTA camera rotation (order 2, degrees: x pitch, y roll, z yaw) to forward and up vectors.
	void cam_vectors(const Vector3 &rot, float f[3], float u[3])
	{
		const float d2r = 3.14159265f / 180.0f;
		const float p = rot.x * d2r, roll = rot.y * d2r, yaw = rot.z * d2r;
		f[0] = -std::sin(yaw) * std::cos(p);
		f[1] = std::cos(yaw) * std::cos(p);
		f[2] = std::sin(p);
		const float r0[3] = {std::cos(yaw), std::sin(yaw), 0};
		float u0[3];
		cross(r0, f, u0);
		for (int i = 0; i < 3; ++i)
			u[i] = u0[i] * std::cos(roll) - r0[i] * std::sin(roll);
	}

	void load_config()
	{
		char path[MAX_PATH];
		GetModuleFileNameA(g_module, path, MAX_PATH);
		char *ext = std::strrchr(path, '.');
		if (ext)
			std::strcpy(ext, ".ini");
		GetPrivateProfileStringA("beamng", "model", g_cfg.model, g_cfg.model, sizeof(g_cfg.model), path);
		GetPrivateProfileStringA("beamng", "config", g_cfg.config, g_cfg.config, sizeof(g_cfg.config), path);
		GetPrivateProfileStringA("gta", "proxy", g_cfg.proxy, g_cfg.proxy, sizeof(g_cfg.proxy), path);
		g_cfg.proxyVisible = GetPrivateProfileIntA("gta", "proxyVisible", g_cfg.proxyVisible ? 1 : 0, path) != 0;
	}

	void set_phase(Phase p)
	{
		static const char *names[] = {"Idle", "LoadingLevel", "Streaming", "Spawning", "Active"};
		log_line("phase %s -> %s", names[(int)g_phase], names[(int)p]);
		g_phase = p;
		g_phaseAt = now();
	}

	// ---------------------------------------------------------------- collision streaming

	std::pair<int, int> tile_of(float x, float y)
	{
		return {int(std::floor(x / kTileSize)), int(std::floor(y / kTileSize))};
	}

	void send_tile(const std::pair<int, int> &key, const Tile &t)
	{
		std::string s;
		s.reserve(12 * t.h.size() + 160);
		char head[160];
		std::snprintf(head, sizeof(head), "{\"t\":\"tile\",\"id\":\"g%d_%d\",\"ox\":%.2f,\"oy\":%.2f,\"n\":%d,\"step\":%.2f,\"h\":[",
			key.first, key.second, key.first * kTileSize, key.second * kTileSize, kTileN, kTileStep);
		s += head;
		char num[24];
		for (size_t i = 0; i < t.h.size(); ++i)
		{
			const int n = std::snprintf(num, sizeof(num), i ? ",%.2f" : "%.2f", t.h[i]);
			s.append(num, n);
		}
		s += "]}";
		bng::send(s.data(), int(s.size()));
	}

	/// Probe GTA's ground for the 3x3 tiles around (cx, cy); true once all of them have been sent.
	bool stream_ground(float cx, float cy, float cz)
	{
		const auto centre = tile_of(cx, cy);
		for (auto it = g_tiles.begin(); it != g_tiles.end();)
		{
			if (std::abs(it->first.first - centre.first) > kTileKeep || std::abs(it->first.second - centre.second) > kTileKeep)
			{
				bng::sendf("{\"t\":\"drop\",\"id\":\"g%d_%d\"}", it->first.first, it->first.second);
				it = g_tiles.erase(it);
			}
			else
				++it;
		}
		// centre first, then its neighbours
		static const int order[9][2] = {{0, 0}, {0, 1}, {1, 0}, {0, -1}, {-1, 0}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
		int budget = kGroundProbesPerFrame;
		bool ready = true;
		for (const auto &o : order)
		{
			const std::pair<int, int> key{centre.first + o[0], centre.second + o[1]};
			auto found = g_tiles.find(key);
			if (found == g_tiles.end())
			{
				Tile t;
				t.h.assign(kTileN * kTileN, kNoGround);
				t.z0 = cz + 4.0f;
				found = g_tiles.emplace(key, std::move(t)).first;
			}
			Tile &t = found->second;
			while (budget > 0 && t.next < kTileN * kTileN)
			{
				const int i = t.next % kTileN, j = t.next / kTileN;
				const float x = key.first * kTileSize + i * kTileStep, y = key.second * kTileSize + j * kTileStep;
				float gz = 0;
				t.h[t.next] = natives::GetGroundZFor3dCoord(x, y, t.z0, &gz, TRUE, FALSE) ? gz : kNoGround;
				++t.next;
				--budget;
			}
			if (t.next == kTileN * kTileN && !t.sent)
			{
				send_tile(key, t);
				t.sent = true;
			}
			ready = ready && t.sent;
		}
		return ready;
	}

	/// Radial probes around the car for steep surfaces (buildings, walls, poles); sent as wall quads once per sweep.
	void stream_walls(const float c[3], float groundZ)
	{
		const Ped ped = natives::PlayerPedId();
		const float z = groundZ + 0.9f;
		for (int k = 0; k < kWallRaysPerFrame; ++k)
		{
			const int i = g_wallNext;
			g_wallNext = (g_wallNext + 1) % kWallRays;
			const float a = i * 6.2831853f / kWallRays;
			const float dx = std::cos(a), dy = std::sin(a);
			WallHit &w = g_wallHits[i];
			w.hit = false;
			const int handle = natives::StartShapeTestLosProbeSync(c[0], c[1], z, c[0] + dx * kWallRange, c[1] + dy * kWallRange, z,
				kProbeFlags, g_proxy ? g_proxy : ped);
			BOOL hit = FALSE;
			Vector3 end = {}, normal = {};
			Entity entity = 0;
			if (natives::GetShapeTestResult(handle, &hit, &end, &normal, &entity) == 2 && hit && std::abs(normal.z) < kWallMaxNormalZ)
			{
				w.hit = true;
				w.x = end.x;
				w.y = end.y;
				w.z = end.z;
				w.dist = std::sqrt((end.x - c[0]) * (end.x - c[0]) + (end.y - c[1]) * (end.y - c[1]));
			}
			if (g_wallNext != 0)
				continue;
			// a sweep is complete: join neighbouring hits at similar distances, give lone hits (poles) a short face
			std::string s = "{\"t\":\"walls\",\"id\":\"radial\",\"segs\":[";
			bool first = true;
			char seg[160];
			for (int n = 0; n < kWallRays; ++n)
			{
				const WallHit &h = g_wallHits[n];
				if (!h.hit)
					continue;
				const WallHit &next = g_wallHits[(n + 1) % kWallRays];
				const WallHit &prev = g_wallHits[(n + kWallRays - 1) % kWallRays];
				float x1, y1, x2, y2;
				if (next.hit && std::abs(next.dist - h.dist) < 2.5f)
				{
					x1 = h.x, y1 = h.y, x2 = next.x, y2 = next.y;
				}
				else if (!prev.hit || std::abs(prev.dist - h.dist) >= 2.5f)
				{
					const float a = n * 6.2831853f / kWallRays;
					const float px = -std::sin(a) * 0.4f, py = std::cos(a) * 0.4f;
					x1 = h.x - px, y1 = h.y - py, x2 = h.x + px, y2 = h.y + py;
				}
				else
					continue;
				const int len = std::snprintf(seg, sizeof(seg), "%s[%.2f,%.2f,%.2f,%.2f,%.2f,%.2f]", first ? "" : ",", x1, y1, x2, y2,
					h.z - 1.5f, h.z + 3.5f);
				s.append(seg, len);
				first = false;
			}
			s += "]}";
			// every resend rebuilds BeamNG's collision, so only when something changed and at most every kWallResendMs
			static std::string lastSent;
			static int lastSentAt = -100000;
			if (s == lastSent || now() - lastSentAt < kWallResendMs)
				continue;
			lastSent = s;
			lastSentAt = now();
			if (first)
				bng::sendf("{\"t\":\"drop\",\"id\":\"radial\"}");
			else
				bng::send(s.data(), int(s.size()));
		}
	}

	void clear_streaming()
	{
		g_tiles.clear();
		for (WallHit &w : g_wallHits)
			w = WallHit();
		g_wallNext = 0;
		bng::sendf("{\"t\":\"clear\"}");
	}

	// ---------------------------------------------------------------- proxy vehicle

	void proxy_delete()
	{
		if (g_proxy && natives::DoesEntityExist(g_proxy))
		{
			const Ped ped = natives::PlayerPedId();
			if (natives::GetVehiclePedIsIn(ped, FALSE) == g_proxy)
			{
				const Vector3 p = natives::GetEntityCoords(g_proxy, TRUE);
				natives::SetEntityCoordsNoOffset(ped, p.x + 2.0f, p.y, p.z + 0.5f);
			}
			Vehicle v = g_proxy;
			natives::SetEntityAsMissionEntity(v);
			natives::DeleteEntity(&v);
		}
		g_proxy = 0;
		g_driving = false;
		g_composited = false;
	}

	void proxy_apply_visibility()
	{
		if (!g_proxy)
			return;
		// Hidden by alpha, never by SET_ENTITY_VISIBLE: GTA won't let the player enter an invisible vehicle.
		const bool shown = g_cfg.proxyVisible && !g_composited;
		natives::SetEntityVisible(g_proxy, TRUE, FALSE);
		natives::SetEntityAlpha(g_proxy, shown ? 255 : (g_debug ? 120 : 0));
	}

	bool proxy_create()
	{
		if (!g_proxyHash)
		{
			g_proxyHash = natives::GetHashKey(g_cfg.proxy);
			if (!natives::IsModelValid(g_proxyHash))
			{
				notify("GTAxBeam: proxy model '%s' is not a GTA vehicle", g_cfg.proxy);
				g_proxyHash = natives::GetHashKey("tailgater");
			}
		}
		natives::RequestModel(g_proxyHash);
		if (!natives::HasModelLoaded(g_proxyHash))
			return false;
		Vector3 mn = {}, mx = {};
		natives::GetModelDimensions(g_proxyHash, &mn, &mx);
		// x/y: centre on BeamNG's box; z: the model's lowest point (tyre bottoms) on BeamNG's box bottom
		g_proxyCentre[0] = (mn.x + mx.x) * 0.5f;
		g_proxyCentre[1] = (mn.y + mx.y) * 0.5f;
		g_proxyCentre[2] = mn.z;
		const float heading = std::atan2(-g_veh.fwd[0], g_veh.fwd[1]) * 57.29578f;
		g_proxy = natives::CreateVehicle(g_proxyHash, g_veh.pos[0], g_veh.pos[1], g_veh.pos[2], heading);
		natives::SetModelAsNoLongerNeeded(g_proxyHash);
		if (!g_proxy)
			return false;
		natives::SetEntityAsMissionEntity(g_proxy);
		natives::SetEntityHasGravity(g_proxy, FALSE);
		natives::SetEntityInvincible(g_proxy, TRUE);
		natives::SetEntityCanBeDamaged(g_proxy, FALSE);
		natives::SetEntityProofs(g_proxy, TRUE, TRUE, TRUE, TRUE, TRUE);
		natives::SetVehicleCanBeVisiblyDamaged(g_proxy, FALSE);
		natives::SetVehicleHasBeenOwnedByPlayer(g_proxy, TRUE);
		natives::SetVehicleRadioEnabled(g_proxy, FALSE);
		proxy_apply_visibility();
		return true;
	}

	/// Put the proxy where BeamNG's car is, moving at its velocity (so GTA's collisions carry momentum).
	/// BeamNG reports at its own frame rate; in between, extrapolate along the velocity so GTA's faster frames stay smooth.
	void proxy_follow()
	{
		if (!g_proxy || !g_veh.valid)
			return;
		float r[3], f[3], u[3], q[4];
		basis(g_veh.fwd, g_veh.up, r, f, u);
		quat_from_basis(r, f, u, q);
		const float dt = std::clamp((now() - g_veh.at) / 1000.0f, 0.0f, 0.1f);
		float bottom[3];
		for (int i = 0; i < 3; ++i)
			bottom[i] = g_veh.pos[i] + g_veh.vel[i] * dt - u[i] * g_veh.half[2];
		const float *c = g_proxyCentre;
		const float x = bottom[0] - (r[0] * c[0] + f[0] * c[1] + u[0] * c[2]);
		const float y = bottom[1] - (r[1] * c[0] + f[1] * c[1] + u[1] * c[2]);
		const float z = bottom[2] - (r[2] * c[0] + f[2] * c[1] + u[2] * c[2]);
		// Teleporting into a ped or car makes GTA eject it with a huge impulse (they vanish), so steer by velocity:
		// BeamNG's velocity plus a correction towards the target. Snap only when far off (spawn, recover).
		const Vector3 p = natives::GetEntityCoords(g_proxy, FALSE);
		const float ex = x - p.x, ey = y - p.y, ez = z - p.z;
		if (ex * ex + ey * ey + ez * ez > kProxySnapDist * kProxySnapDist)
		{
			natives::SetEntityCoordsNoOffset(g_proxy, x, y, z);
			natives::SetEntityVelocity(g_proxy, g_veh.vel[0], g_veh.vel[1], g_veh.vel[2]);
		}
		else
			natives::SetEntityVelocity(g_proxy, g_veh.vel[0] + ex * kProxyGain, g_veh.vel[1] + ey * kProxyGain,
				g_veh.vel[2] + ez * kProxyGain);
		natives::SetEntityQuaternion(g_proxy, q[0], q[1], q[2], q[3]);
	}

	void draw_box()
	{
		float r[3], f[3], u[3];
		basis(g_veh.fwd, g_veh.up, r, f, u);
		float corners[8][3];
		for (int k = 0; k < 8; ++k)
		{
			const float sx = (k & 1) ? 1.0f : -1.0f, sy = (k & 2) ? 1.0f : -1.0f, sz = (k & 4) ? 1.0f : -1.0f;
			for (int i = 0; i < 3; ++i)
				corners[k][i] = g_veh.pos[i] + r[i] * sx * g_veh.half[0] + f[i] * sy * g_veh.half[1] + u[i] * sz * g_veh.half[2];
		}
		static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
		for (const auto &e : edges)
			natives::DrawLine(corners[e[0]][0], corners[e[0]][1], corners[e[0]][2], corners[e[1]][0], corners[e[1]][1], corners[e[1]][2],
				255, 140, 0, 255);
	}

	// ---------------------------------------------------------------- BeamNG messages

	void handle_message(const std::string &m)
	{
		g_lastRecvAt = now();
		if (!g_linkUp)
		{
			g_linkUp = true;
			notify("GTAxBeam: BeamNG connected");
		}
		if (bng::is_type(m, "veh"))
		{
			VehState v;
			if (bng::vec3(m, "pos", v.pos) && bng::vec3(m, "fwd", v.fwd) && bng::vec3(m, "up", v.up))
			{
				bng::vec3(m, "ref", v.ref);
				bng::vec3(m, "vel", v.vel);
				bng::vec3(m, "half", v.half);
				bng::num(m, "dmg", v.dmg);
				v.valid = true;
				v.at = now();
				g_veh = v;
			}
		}
		else if (bng::is_type(m, "hello"))
		{
			bool level = false;
			bng::boolean(m, "level", level);
			if (level && !g_levelReady)
				notify("GTAxBeam: BeamNG void level ready");
			g_levelReady = level;
		}
		else if (bng::is_type(m, "gone"))
		{
			g_veh.valid = false;
			if (g_phase == Phase::Active)
			{
				proxy_delete();
				set_phase(Phase::Idle);
				notify("GTAxBeam: the BeamNG car is gone");
			}
		}
	}

	// ---------------------------------------------------------------- per frame

	void send_camera()
	{
		const Vector3 c = natives::GetFinalRenderedCamCoord();
		const Vector3 r = natives::GetFinalRenderedCamRot(2);
		float f[3], u[3];
		cam_vectors(r, f, u);
		bng::sendf("{\"t\":\"cam\",\"f\":%d,\"pos\":[%.4f,%.4f,%.4f],\"fwd\":[%.5f,%.5f,%.5f],\"up\":[%.5f,%.5f,%.5f],\"fov\":%.3f,"
				   "\"near\":%.4f,\"far\":%.1f}",
			natives::GetFrameCount(), c.x, c.y, c.z, f[0], f[1], f[2], u[0], u[1], u[2], natives::GetFinalRenderedCamFov(),
			natives::GetFinalRenderedCamNearClip(), natives::GetFinalRenderedCamFarClip());
	}

	/// GTA's vehicles near the BeamNG car (the nearest kTrafficMax, the player's own included): BeamNG moves a hidden
	/// ghost car along with each, so ramming the BeamNG car with a GTA car crumples it in BeamNG.
	void send_traffic()
	{
		static int handles[512];
		static bool sentEmpty = false;
		const int n = worldGetAllVehicles(handles, 512);
		std::vector<std::pair<float, int>> nearby;
		for (int i = 0; i < n; ++i)
		{
			if (handles[i] == g_proxy)
				continue;
			const Vector3 p = natives::GetEntityCoords(handles[i], FALSE);
			const float dx = p.x - g_veh.pos[0], dy = p.y - g_veh.pos[1], dz = p.z - g_veh.pos[2];
			const float d2 = dx * dx + dy * dy + dz * dz;
			if (d2 < kTrafficRange * kTrafficRange)
				nearby.emplace_back(d2, handles[i]);
		}
		std::sort(nearby.begin(), nearby.end());
		if (nearby.size() > size_t(kTrafficMax))
			nearby.resize(kTrafficMax);
		if (nearby.empty())
		{
			if (!sentEmpty)
				bng::sendf("{\"t\":\"traffic\",\"cars\":[]}");
			sentEmpty = true;
			return;
		}
		sentEmpty = false;
		std::string msg = "{\"t\":\"traffic\",\"cars\":[";
		for (size_t k = 0; k < nearby.size(); ++k)
		{
			const int h = nearby[k].second;
			Vector3 mn = {}, mx = {}, a = {}, b = {}, up = {}, pos = {};
			natives::GetModelDimensions(natives::GetEntityModel(h), &mn, &mx);
			natives::GetEntityMatrix(h, &a, &b, &up, &pos);
			const Vector3 fv = natives::GetEntityForwardVector(h);
			float r[3], f[3], u[3];
			const float fwdIn[3] = {fv.x, fv.y, fv.z}, upIn[3] = {up.x, up.y, up.z};
			basis(fwdIn, upIn, r, f, u);
			const float lc[3] = {(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f};
			const float c[3] = {pos.x + r[0] * lc[0] + f[0] * lc[1] + u[0] * lc[2], pos.y + r[1] * lc[0] + f[1] * lc[1] + u[1] * lc[2],
				pos.z + r[2] * lc[0] + f[2] * lc[1] + u[2] * lc[2]};
			const Vector3 v = natives::GetEntityVelocity(h);
			char buf[384];
			std::snprintf(buf, sizeof(buf),
				"%s{\"id\":%d,\"c\":[%.3f,%.3f,%.3f],\"f\":[%.4f,%.4f,%.4f],\"u\":[%.4f,%.4f,%.4f],\"v\":[%.3f,%.3f,%.3f],\"h\":[%.3f,%.3f,%.3f]}",
				k ? "," : "", h, c[0], c[1], c[2], f[0], f[1], f[2], u[0], u[1], u[2], v.x, v.y, v.z, (mx.x - mn.x) * 0.5f,
				(mx.y - mn.y) * 0.5f, (mx.z - mn.z) * 0.5f);
			msg += buf;
		}
		msg += "]}";
		bng::send(msg.data(), int(msg.size()));
	}

	void begin_spawn()
	{
		const Ped ped = natives::PlayerPedId();
		const Vector3 p = natives::GetEntityCoords(ped, TRUE);
		const Vector3 fv = natives::GetEntityForwardVector(ped);
		float gz = p.z - 1.0f;
		natives::GetGroundZFor3dCoord(p.x + fv.x * 6.0f, p.y + fv.y * 6.0f, p.z + 3.0f, &gz, TRUE, FALSE);
		g_spawnPos[0] = p.x + fv.x * 6.0f;
		g_spawnPos[1] = p.y + fv.y * 6.0f;
		g_spawnPos[2] = gz;
		// side-on to the player, so they walk up to the door
		g_spawnFwd[0] = fv.y;
		g_spawnFwd[1] = -fv.x;
		g_spawnFwd[2] = 0;
		if (g_phase == Phase::Active)
		{
			bng::sendf("{\"t\":\"spawn\",\"model\":\"%s\",\"config\":\"%s\",\"pos\":[%.3f,%.3f,%.3f],\"fwd\":[%.4f,%.4f,0],\"up\":[0,0,1]}",
				g_cfg.model, g_cfg.config, g_spawnPos[0], g_spawnPos[1], g_spawnPos[2], g_spawnFwd[0], g_spawnFwd[1]);
			return;
		}
		if (!g_levelReady)
		{
			bng::sendf("{\"t\":\"level\"}");
			notify("GTAxBeam: loading BeamNG's void level...");
			set_phase(Phase::LoadingLevel);
		}
		else
			set_phase(Phase::Streaming);
		clear_streaming();
	}

	void tick()
	{
		if (g_keyDebug.exchange(false))
		{
			g_debug = !g_debug;
			bng::sendf("{\"t\":\"debug\",\"on\":%s}", g_debug ? "true" : "false");
			proxy_apply_visibility();
			notify("GTAxBeam: debug view %s", g_debug ? "on" : "off");
		}
		if (g_keyRemove.exchange(false))
		{
			proxy_delete();
			bng::sendf("{\"t\":\"remove\"}");
			clear_streaming();
			g_veh.valid = false;
			set_phase(Phase::Idle);
			notify("GTAxBeam: removed");
		}

		std::string msg;
		while (bng::poll(msg))
			handle_message(msg);
		const bool loading = g_phase == Phase::LoadingLevel || g_phase == Phase::Spawning;
		if (g_linkUp && now() - g_lastRecvAt > (loading ? kLinkTimeoutLoadingMs : kLinkTimeoutMs))
		{
			g_linkUp = false;
			g_levelReady = false;
			notify("GTAxBeam: lost BeamNG");
		}
		if (now() >= g_nextHelloAt)
		{
			g_nextHelloAt = now() + 1000;
			bng::sendf("{\"t\":\"hello\",\"v\":1}");
		}

		const bool blocked = natives::IsScreenFadedOut() || natives::IsCutsceneActive() || natives::IsPlayerSwitchInProgress();
		compositor::set_active(!blocked && g_linkUp && g_phase == Phase::Active && g_veh.valid && !natives::IsPauseMenuActive());
		compositor::set_host_camera(natives::GetFinalRenderedCamFov(), natives::GetFinalRenderedCamNearClip(),
			natives::GetFinalRenderedCamFarClip());
		if (blocked)
			return;

		if (g_keySpawn.exchange(false))
		{
			if (!g_linkUp)
				notify("GTAxBeam: BeamNG isn't running (or the gtaxbeam mod isn't installed)");
			else
				begin_spawn();
		}
		if (g_keyRecover.exchange(false) && g_phase == Phase::Active && g_veh.valid)
		{
			float f[3] = {g_veh.fwd[0], g_veh.fwd[1], 0};
			normalize(f);
			bng::sendf("{\"t\":\"place\",\"pos\":[%.3f,%.3f,%.3f],\"fwd\":[%.4f,%.4f,0],\"up\":[0,0,1]}", g_veh.pos[0], g_veh.pos[1],
				g_veh.pos[2] - g_veh.half[2], f[0], f[1]);
		}

		if (!g_linkUp)
			return;
		if (now() >= g_nextTodAt)
		{
			g_nextTodAt = now() + 5000;
			bng::sendf("{\"t\":\"tod\",\"h\":%.3f}", natives::GetClockHours() + natives::GetClockMinutes() / 60.0f);
		}
		if (g_levelReady)
			send_camera();

		switch (g_phase)
		{
		case Phase::Idle:
			break;
		case Phase::LoadingLevel:
			if (g_levelReady)
			{
				clear_streaming();
				set_phase(Phase::Streaming);
			}
			break;
		case Phase::Streaming:
			if (stream_ground(g_spawnPos[0], g_spawnPos[1], g_spawnPos[2]))
			{
				bng::sendf("{\"t\":\"spawn\",\"model\":\"%s\",\"config\":\"%s\",\"pos\":[%.3f,%.3f,%.3f],\"fwd\":[%.4f,%.4f,0],\"up\":[0,0,1]}",
					g_cfg.model, g_cfg.config, g_spawnPos[0], g_spawnPos[1], g_spawnPos[2], g_spawnFwd[0], g_spawnFwd[1]);
				g_veh.valid = false;
				set_phase(Phase::Spawning);
			}
			break;
		case Phase::Spawning:
			if (g_veh.valid && proxy_create())
			{
				set_phase(Phase::Active);
				notify("GTAxBeam: BeamNG %s ready - get in with F", g_cfg.model);
			}
			else if (now() - g_phaseAt > 30000)
			{
				notify("GTAxBeam: BeamNG didn't spawn the car (see BeamNG's log)");
				set_phase(Phase::Idle);
			}
			break;
		case Phase::Active:
		{
			const Ped ped = natives::PlayerPedId();
			const bool driving = natives::GetVehiclePedIsIn(ped, FALSE) == g_proxy;
			if (driving != g_driving)
			{
				g_driving = driving;
				log_line(driving ? "player entered the car" : "player left the car");
				if (!driving)
					bng::sendf("{\"t\":\"input\",\"th\":0,\"br\":0,\"st\":0,\"hb\":1}");
			}
			if (driving)
			{
				for (int c : {kVehAccelerate, kVehBrake, kVehMoveLR, kVehHandbrake})
					natives::DisableControlAction(0, c, TRUE);
				// kb: keyboard keys are all-or-nothing, so BeamNG should ramp them like its own keyboard input
				bng::sendf("{\"t\":\"input\",\"th\":%.3f,\"br\":%.3f,\"st\":%.3f,\"hb\":%.3f,\"kb\":%s}",
					natives::GetDisabledControlNormal(0, kVehAccelerate), natives::GetDisabledControlNormal(0, kVehBrake),
					natives::GetDisabledControlNormal(0, kVehMoveLR), natives::GetDisabledControlNormal(0, kVehHandbrake),
					natives::IsUsingKeyboardAndMouse(0) ? "true" : "false");
				natives::InvalidateIdleCam();
				natives::InvalidateCinematicVehicleIdleMode();
			}
			proxy_follow();
			static int nextStatusAt = 0;
			if (now() >= nextStatusAt)
			{
				nextStatusAt = now() + 2000;
				const float *v = g_veh.vel;
				log_line("car pos %.1f %.1f %.1f speed %.1f dmg %.0f tiles %d driving %d", g_veh.pos[0], g_veh.pos[1],
					g_veh.pos[2], std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), g_veh.dmg, (int)g_tiles.size(),
					g_driving ? 1 : 0);
			}
			if (compositor::frames_arriving() != g_composited)
			{
				g_composited = !g_composited;
				log_line(g_composited ? "BeamNG's picture is arriving: proxy hidden" : "BeamNG's picture stopped: proxy shown");
				proxy_apply_visibility();
			}
			if ((!g_cfg.proxyVisible && !g_composited) || g_debug)
				draw_box();
			// stream ahead of the car: about a second of travel
			const float ahead[3] = {g_veh.pos[0] + g_veh.vel[0], g_veh.pos[1] + g_veh.vel[1], g_veh.pos[2]};
			stream_ground(ahead[0], ahead[1], ahead[2]);
			stream_walls(g_veh.pos, g_veh.pos[2] - g_veh.half[2]);
			send_traffic();
			if (!driving)
			{
				const Vector3 p = natives::GetEntityCoords(ped, TRUE);
				const float dx = p.x - g_veh.pos[0], dy = p.y - g_veh.pos[1];
				if (dx * dx + dy * dy < 25.0f && !natives::IsPedInAnyVehicle(ped, TRUE))
				{
					natives::HelpText("Press ~INPUT_ENTER~ to drive the BeamNG car");
					// GTA's own enter picks whatever vehicle it likes nearby: make F always mean this one
					if (natives::IsControlJustPressed(0, kEnter) && natives::GetVehiclePedIsTryingToEnter(ped) != g_proxy)
					{
						natives::TaskEnterVehicle(ped, g_proxy, 10000, -1, 2.0f, 1);
						log_line("enter task given");
					}
				}
			}
			break;
		}
		}
	}

	void script_main()
	{
		GetModuleFileNameA(g_module, g_logPath, MAX_PATH);
		if (char *ext = std::strrchr(g_logPath, '.'))
			std::strcpy(ext, ".log");
		if (FILE *f = std::fopen(g_logPath, "w"))
			std::fclose(f);
		load_config();
		const bool linkOk = bng::start(kBeamngPort);
		log_line("script started: model %s proxy %s proxyVisible %d link %s", g_cfg.model, g_cfg.proxy,
			g_cfg.proxyVisible ? 1 : 0, linkOk ? "ok" : "FAILED");
		while (true)
		{
			if (!compositor::try_register(g_module))
			{
				static bool told = false;
				if (!told && GetTickCount64() > 60000)
				{
					told = true;
					log_line("ReShade isn't loaded (no ReShade64.asi?): BeamNG's car can't be drawn, the proxy stands in");
				}
			}
			tick();
			WAIT(0);
		}
	}

	void on_keyboard(DWORD key, WORD, BYTE, BOOL, BOOL, BOOL wasDownBefore, BOOL isUpNow)
	{
		if (isUpNow || wasDownBefore)
			return;
		switch (key)
		{
		case VK_F5:
			g_keyDebug = true;
			break;
		case VK_F6:
			g_keySpawn = true;
			break;
		case VK_F7:
			g_keyRemove = true;
			break;
		case VK_F9:
			g_keyRecover = true;
			break;
		}
	}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		g_module = module;
		scriptRegister(module, script_main);
		keyboardHandlerRegister(on_keyboard);
		break;
	case DLL_PROCESS_DETACH:
		compositor::unregister(module);
		scriptUnregister(module);
		keyboardHandlerUnregister(on_keyboard);
		bng::stop();
		break;
	}
	return TRUE;
}
