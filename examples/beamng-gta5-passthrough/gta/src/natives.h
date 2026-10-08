// The GTA V natives the BeamNG passthrough calls, by hash (names and signatures checked against alloc8or's
// native DB; ScriptHookV translates the original PC hashes for the running build).
#pragma once
#include <types.h>
#include <nativeCaller.h>

namespace natives
{
	// camera
	inline Vector3 GetFinalRenderedCamCoord() { return invoke<Vector3>(0xA200EB1EE790F448); }
	inline Vector3 GetFinalRenderedCamRot(int order) { return invoke<Vector3>(0x5B4E4C817FCC2DFB, order); }
	inline float GetFinalRenderedCamFov() { return invoke<float>(0x80EC114669DAEFF4); }
	inline float GetFinalRenderedCamNearClip() { return invoke<float>(0xD0082607100D7193); }
	inline float GetFinalRenderedCamFarClip() { return invoke<float>(0xDFC8CBC606FDB0FC); }
	inline void InvalidateIdleCam() { invoke<Void>(0xF4F2C0D4EE209E20); }
	inline void InvalidateCinematicVehicleIdleMode() { invoke<Void>(0x9E4CFFF989258472); }
	inline void SetGameplayCamMotionBlurScalingThisUpdate(float s) { invoke<Void>(0x487A82C650EB7799, s); }
	inline void SetGameplayCamMaxMotionBlurStrengthThisUpdate(float s) { invoke<Void>(0x0225778816FDC28C, s); }

	// game state
	inline BOOL IsScreenFadedOut() { return invoke<BOOL>(0xB16FCE9DDC7BA182); }
	inline BOOL IsCutsceneActive() { return invoke<BOOL>(0x991251AFC3981F84); }
	inline BOOL IsPlayerSwitchInProgress() { return invoke<BOOL>(0xD9D2CFFF49FAB35F); }
	inline BOOL IsPauseMenuActive() { return invoke<BOOL>(0xB0034A223497FFCB); }
	inline int GetFrameCount() { return invoke<int>(0xFC8202EFC642E6F2); }
	inline int GetGameTimer() { return invoke<int>(0x9CD27B0045628463); }
	inline int GetClockHours() { return invoke<int>(0x25223CA6B4D20B7F); }
	inline int GetClockMinutes() { return invoke<int>(0x13D2B8ADD79640F2); }
	inline void GetActualScreenResolution(int *x, int *y) { invoke<Void>(0x873C9F3104101DD3, x, y); }
	inline void ShakeGameplayCam(const char *name, float intensity) { invoke<Void>(0xFD55E49555E017CF, name, intensity); }

	// controls
	inline void DisableControlAction(int group, int control, BOOL disable) { invoke<Void>(0xFE99B66D079CF6BC, group, control, disable); }
	inline float GetDisabledControlNormal(int group, int control) { return invoke<float>(0x11E65974A982637C, group, control); }
	inline BOOL IsUsingKeyboardAndMouse(int control) { return invoke<BOOL>(0xA571D46727E2B718, control); }
	inline BOOL IsDisabledControlJustPressed(int group, int control) { return invoke<BOOL>(0x91AEF906BCA88877, group, control); }
	inline BOOL IsControlJustPressed(int group, int control) { return invoke<BOOL>(0x580417101DDB492F, group, control); }

	// player
	inline Ped PlayerPedId() { return invoke<Ped>(0xD80958FC74E988A6); }
	inline Player PlayerId() { return invoke<Player>(0x4F8644AF03D0E0D6); }
	inline BOOL IsPedInAnyVehicle(Ped p, BOOL atGetIn) { return invoke<BOOL>(0x997ABD671D25CA0B, p, atGetIn); }
	inline Vehicle GetVehiclePedIsIn(Ped p, BOOL includeEntering) { return invoke<Vehicle>(0x9A9112A0FE9A4713, p, includeEntering); }
	inline Vehicle GetVehiclePedIsTryingToEnter(Ped p) { return invoke<Vehicle>(0x814FA8BE5449445D, p); }
	inline void SetPedIntoVehicle(Ped p, Vehicle v, int seat) { invoke<Void>(0xF75B0D629E1C063D, p, v, seat); }
	inline void SetPedCanBeKnockedOffVehicle(Ped p, int state) { invoke<Void>(0x7A6535691B477C48, p, state); }
	inline void TaskEnterVehicle(Ped p, Vehicle v, int timeout, int seat, float speed, int flag)
	{
		invoke<Void>(0xC20E50AA46D09CA8, p, v, timeout, seat, speed, flag, 0);
	}

	// entities
	inline Vector3 GetEntityCoords(Entity e, BOOL alive) { return invoke<Vector3>(0x3FEF770D40960D5A, e, alive); }
	inline float GetEntityHeading(Entity e) { return invoke<float>(0xE83D4F9BA2A38914, e); }
	inline Vector3 GetEntityForwardVector(Entity e) { return invoke<Vector3>(0x0A794A5A57F8DF91, e); }
	inline Vector3 GetEntityVelocity(Entity e) { return invoke<Vector3>(0x4805D2B1D8CF94A9, e); }
	inline Hash GetEntityModel(Entity e) { return invoke<Hash>(0x9F47B058362C84B5, e); }
	// the first two outputs are named inconsistently across native DBs; only up and position are relied on
	inline void GetEntityMatrix(Entity e, Vector3 *a, Vector3 *b, Vector3 *up, Vector3 *pos) { invoke<Void>(0xECB2FC7235A7D137, e, a, b, up, pos); }
	inline BOOL DoesEntityExist(Entity e) { return invoke<BOOL>(0x7239B21A38F536BA, e); }
	inline void SetEntityAsMissionEntity(Entity e) { invoke<Void>(0xAD738C3085FE7E11, e, TRUE, TRUE); }
	inline void DeleteEntity(Entity *e) { invoke<Void>(0xAE3CBE5BF394C9C9, e); }
	inline void SetEntityCoordsNoOffset(Entity e, float x, float y, float z) { invoke<Void>(0x239A3351AC1DA385, e, x, y, z, FALSE, FALSE, FALSE); }
	inline void SetEntityQuaternion(Entity e, float x, float y, float z, float w) { invoke<Void>(0x77B21BE7AC540F07, e, x, y, z, w); }
	inline void SetEntityVelocity(Entity e, float x, float y, float z) { invoke<Void>(0x1C99BB7B6E96D16F, e, x, y, z); }
	inline void SetEntityHasGravity(Entity e, BOOL t) { invoke<Void>(0x4A4722448F18EEF5, e, t); }
	inline void SetEntityVisible(Entity e, BOOL visible, BOOL p2) { invoke<Void>(0xEA1C610A04DB6BBB, e, visible, p2); }
	inline void SetEntityAlpha(Entity e, int alpha) { invoke<Void>(0x44A0870B7E92D7C0, e, alpha, FALSE); }
	inline void SetEntityCollision(Entity e, BOOL t, BOOL keepPhysics) { invoke<Void>(0x1A9205C1B9EE827F, e, t, keepPhysics); }
	inline void SetEntityInvincible(Entity e, BOOL t) { invoke<Void>(0x3882114BDE571AD4, e, t, TRUE); }
	inline void SetEntityCanBeDamaged(Entity e, BOOL t) { invoke<Void>(0x1760FFA8AB074D66, e, t); }
	inline void SetEntityProofs(Entity e, BOOL bullet, BOOL fire, BOOL explosion, BOOL collision, BOOL melee)
	{
		invoke<Void>(0xFAEE099C6F890BB8, e, bullet, fire, explosion, collision, melee, TRUE, TRUE, FALSE);
	}

	// models and vehicles
	inline Hash GetHashKey(const char *s) { return invoke<Hash>(0xD24D37CC275948CC, s); }
	inline BOOL IsModelValid(Hash m) { return invoke<BOOL>(0xC0296A2EDF545E92, m); }
	inline void RequestModel(Hash m) { invoke<Void>(0x963D27A58DF860AC, m); }
	inline BOOL HasModelLoaded(Hash m) { return invoke<BOOL>(0x98A4EB5D89A0C952, m); }
	inline void SetModelAsNoLongerNeeded(Hash m) { invoke<Void>(0xE532F5D78798DAAB, m); }
	inline void GetModelDimensions(Hash m, Vector3 *mn, Vector3 *mx) { invoke<Void>(0x03E8D3D5F549087A, m, mn, mx); }
	inline Vehicle CreateVehicle(Hash model, float x, float y, float z, float h) { return invoke<Vehicle>(0xAF35D0D2583051B0, model, x, y, z, h, FALSE, TRUE, FALSE); }
	inline void SetVehicleEngineOn(Vehicle v, BOOL on) { invoke<Void>(0x2497C4717C8B881E, v, on, TRUE, TRUE); }
	inline void SetVehicleCanBeVisiblyDamaged(Vehicle v, BOOL t) { invoke<Void>(0x4C7028F78FFD3681, v, t); }
	inline void SetVehicleHasBeenOwnedByPlayer(Vehicle v, BOOL t) { invoke<Void>(0x2B5F9D2AF1F1722D, v, t); }
	inline void SetVehicleRadioEnabled(Vehicle v, BOOL t) { invoke<Void>(0x3B988190C0AA6C0B, v, t); }

	// world probes
	inline BOOL GetGroundZFor3dCoord(float x, float y, float z, float *groundZ, BOOL ignoreWater, BOOL p5)
	{
		return invoke<BOOL>(0xC906A7DAB05C8D2B, x, y, z, groundZ, ignoreWater, p5);
	}
	// START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE: the result is ready the same frame
	inline int StartShapeTestLosProbeSync(float x1, float y1, float z1, float x2, float y2, float z2, int flags, Entity ignore)
	{
		return invoke<int>(0x377906D8A31E5586, x1, y1, z1, x2, y2, z2, flags, ignore, 7);
	}
	inline int GetShapeTestResult(int handle, BOOL *hit, Vector3 *end, Vector3 *normal, Entity *entity)
	{
		return invoke<int>(0x3D87450E15D98694, handle, hit, end, normal, entity);
	}

	// drawing and text
	inline void DrawLine(float x1, float y1, float z1, float x2, float y2, float z2, int r, int g, int b, int a)
	{
		invoke<Void>(0x6B7256074AE34680, x1, y1, z1, x2, y2, z2, r, g, b, a);
	}
	inline void Notify(const char *text)
	{
		invoke<Void>(0x202709F4C58A0424, "STRING");
		invoke<Void>(0x6C188BE134E074AA, text);
		invoke<int>(0x2ED7843F8F801023, FALSE, FALSE);
	}
	inline void HelpText(const char *text)
	{
		invoke<Void>(0x8509B634FBE7DA11, "STRING");
		invoke<Void>(0x6C188BE134E074AA, text);
		invoke<Void>(0x238FFE5C7B0498A6, 0, FALSE, FALSE, -1);
	}
}
