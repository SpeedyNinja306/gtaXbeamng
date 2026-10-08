// ReShade add-on half of the compositor: uploads BeamNG's latest frame (shared memory "Local\GTAxBeamFrame", written
// by the GTAxBeamExport add-on inside BeamNG; layout in shared/gxb_frame.h) into the BNGCOLOR / BNGDEPTH textures
// that GTAxBeam.fx composites into GTA's picture against GTA's depth buffer.
#pragma once

namespace compositor
{
	/// Registers with ReShade once it is loaded; call until it returns true.
	bool try_register(void *module);
	void unregister(void *module);
	/// Whether to composite this frame (the script turns it off when there is no BeamNG car, in menus and cutscenes).
	void set_active(bool active);
	/// GTA's rendered camera: vertical fov (degrees) and clip planes, to map and linearise its depth buffer.
	void set_host_camera(float fov, float near_clip, float far_clip);
	/// Whether BeamNG frames are arriving (one was uploaded in the last second): the proxy can be hidden.
	bool frames_arriving();
}
