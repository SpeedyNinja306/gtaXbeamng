// ReShade add-on half of the compositor: uploads BeamNG's latest frame (shared memory "Local\GTAxBeamFrame", written
// by the GTAxBeamExport add-on inside BeamNG; layout in shared/gxb_frame.h) into the BNGCOLOR / BNGDEPTH textures
// that GTAxBeam.fx composites into GTA's picture against GTA's depth buffer.
#pragma once

namespace compositor
{
	/// GTA's camera in the frame of the car GTA draws (the proxy's box: origin at its centre, x right, y forward, z up),
	/// with the car's half extents and its velocity in that frame. tag is the GTA frame counter sent with it.
	struct CarView
	{
		bool valid = false;
		int tag = 0;
		float pos[3] = {}, fwd[3] = {0, 1, 0}, up[3] = {0, 0, 1};
		float half[3] = {1, 2.3f, 0.7f};
		float vel[3] = {};
		float frameTime = 1.0f / 60.0f;
	};

	/// Registers with ReShade once it is loaded; call until it returns true.
	bool try_register(void *module);
	void unregister(void *module);
	/// Whether to composite this frame (the script turns it off when there is no BeamNG car, in menus and cutscenes).
	void set_active(bool active);
	/// GTA's rendered camera: vertical fov (degrees) and clip planes, to map and linearise its depth buffer.
	void set_host_camera(float fov, float near_clip, float far_clip);
	/// GTA's camera relative to the car, every frame: BeamNG's picture is re-projected onto it.
	void set_car_view(const CarView &view);
	/// Whether BeamNG frames are arriving (one was uploaded in the last second): the proxy can be hidden.
	bool frames_arriving();
}
