// Composites BeamNG.drive's car into GTA V (GTA x BeamNG). The GTAxBeam add-on (in GTAxBeam.asi) uploads the part of
// BeamNG's latest frame that holds the car (BngRect) into BNGCOLOR (premultiplied: alpha = BeamNG drew geometry there)
// and BNGDEPTH (BeamNG's raw depth), and sets the camera uniforms. BeamNG renders from GTA's camera relative to the car,
// a few frames before GTA shows the picture: each GTA pixel's view ray is moved into the camera BeamNG rendered with
// (WarpRow*, WarpT) and marched until it meets the car; the car shows where it is nearer than GTA's depth buffer.
#include "ReShade.fxh"

texture BngColorTex : BNGCOLOR;
texture BngDepthTex : BNGDEPTH;
sampler sBngColor { Texture = BngColorTex; AddressU = CLAMP; AddressV = CLAMP; };
sampler sBngDepth { Texture = BngDepthTex; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };

// Set by the add-on: true only while BeamNG frames are arriving; until then GTA passes through untouched.
uniform bool BngActive = false;
// Set by the add-on: GTA's camera near/far clip, tan(GTA's vertical fov / 2), BeamNG's frame size, tan(BeamNG's
// vertical fov / 2), and the rectangle of BeamNG's frame that was uploaded (x, y, width, height in pixels; it sits in
// the textures' top-left corner).
uniform float2 HostPlanes = float2(0.15, 10000.0);
uniform float HostTan = 0.466;
uniform float2 BngSize = float2(1600.0, 900.0);
uniform float BngTan = 0.466;
uniform float4 BngRect = float4(0.0, 0.0, 0.0, 0.0);
// Set by the add-on: rows of the rotation and the translation from GTA's current camera space to the camera space of
// the uploaded frame (both relative to the car; x right, y up, -z forward), and the car's rectangle on GTA's screen
// (uv min, uv max). WarpOn is false when the frame wasn't rendered car-relative.
uniform bool WarpOn = false;
uniform float3 WarpRow0 = float3(1.0, 0.0, 0.0);
uniform float3 WarpRow1 = float3(0.0, 1.0, 0.0);
uniform float3 WarpRow2 = float3(0.0, 0.0, 1.0);
uniform float3 WarpT = float3(0.0, 0.0, 0.0);
uniform float4 CarRect = float4(0.0, 0.0, 1.0, 1.0);

uniform bool Reproject < ui_category = "Motion"; ui_label = "Re-project to GTA's camera";
	ui_tooltip = "Move BeamNG's (a few frames older) picture onto GTA's current view of the car."; > = true;
uniform float CarLead < ui_category = "Motion"; ui_type = "drag"; ui_min = -3.0; ui_max = 3.0; ui_step = 0.05; ui_label = "Car lead (frames)";
	ui_tooltip = "Moves the drawn car along its velocity by this many frames. If at speed the car slides ahead of (or behind) the road, its shadow and GTA traffic, adjust until it sits still."; > = 0.0;

uniform bool HostReversedZ < ui_category = "Calibration"; ui_label = "GTA depth is reversed"; > = true;
// BeamNG 0.39.4 (D3D12) in the gtaxbeam_void level, measured with host/depthcal.py: reversed Z, near 0.1, far 4172
uniform int BngDepthMode < ui_category = "Calibration"; ui_type = "combo"; ui_label = "BeamNG depth";
	ui_items = "Reversed (1 near, 0 far)\0Standard (0 near, 1 far)\0"; > = 0;
uniform float BngNear < ui_category = "Calibration"; ui_type = "drag"; ui_min = 0.01; ui_max = 2.0; ui_step = 0.005; ui_label = "BeamNG near clip (m)"; > = 0.1;
uniform float BngFar < ui_category = "Calibration"; ui_type = "drag"; ui_min = 100.0; ui_max = 100000.0; ui_step = 10.0; ui_label = "BeamNG far clip (m)"; > = 4172.0;
uniform float FovScale < ui_category = "Calibration"; ui_type = "drag"; ui_min = 0.5; ui_max = 2.0; ui_step = 0.001; ui_label = "BeamNG fov scale";
	ui_tooltip = "tan(BeamNG vertical fov / 2) over GTA's. 1 when BeamNG's fov is vertical like GTA's."; > = 1.0;
uniform float2 Offset < ui_category = "Calibration"; ui_type = "drag"; ui_min = -0.1; ui_max = 0.1; ui_step = 0.0005; ui_label = "BeamNG picture offset"; > = float2(0.0, 0.0);
uniform float DepthBias < ui_type = "drag"; ui_min = 0.0; ui_max = 2.0; ui_step = 0.01; ui_label = "Depth bias (m)";
	ui_tooltip = "How far behind GTA's surface the car may still show (tyres on the road, GTA's kerbs)."; > = 0.15;
uniform float EdgeSoftness < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Soften the car's edges"; > = 0.7;
uniform float EdgeErode < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Trim sky fringe";
	ui_tooltip = "BeamNG's anti-aliasing blends the car's outline with its own sky: fade out pixels on the outline."; > = 0.5;
uniform float ContactShadow < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Contact shadow"; > = 0.55;
uniform float ContactRadius < ui_type = "drag"; ui_min = 0.1; ui_max = 3.0; ui_step = 0.05; ui_label = "Contact shadow size (m)"; > = 0.9;
uniform int DebugView < ui_type = "combo"; ui_items = "Composite\0GTA depth (1 m bands)\0BeamNG depth (1 m bands)\0Depth difference\0BeamNG picture only\0"; > = 0;

texture CompositeTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
sampler sComposite { Texture = CompositeTex; AddressU = CLAMP; AddressV = CLAMP; };
// x: car coverage, y: car depth (m), z: GTA depth (m)
texture InfoTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA16F; };
sampler sInfo { Texture = InfoTex; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };

float bng_linear(float d)
{
	const float n = BngNear, f = BngFar;
	if (BngDepthMode == 0)
		return d > 0.0 ? n * f / (n + d * (f - n)) : 1e9;
	return d < 1.0 ? n * f / (f - d * (f - n)) : 1e9;
}

float host_linear(float d)
{
	const float n = HostPlanes.x, f = HostPlanes.y;
	if (HostReversedZ)
		return d > 0.0 ? n * f / (n + d * (f - n)) : 1e9;
	return d < 1.0 ? n * f / (f - d * (f - n)) : 1e9;
}

float3 bands(float z)
{
	return lerp(float3(0.1, 0.1, 0.1), float3(1.0, 0.85, 0.3), step(0.5, frac(z))) * saturate(1.5 - z / 100.0);
}

/// Whether a point of BeamNG's picture (uv over the whole frame) is inside the uploaded rectangle.
bool in_rect(float2 puv)
{
	const float2 p = puv * BngSize - BngRect.xy;
	return BngRect.z > 0.0 && all(p >= 0.0) && all(p <= BngRect.zw);
}

/// Texture coordinate of a point of BeamNG's picture (kept half a texel inside the rectangle: the texels beyond it
/// are left over from older frames).
float2 rect_tex(float2 puv)
{
	const float2 p = clamp(puv * BngSize - BngRect.xy, 0.5, max(BngRect.zw - 0.5, 0.5));
	return p / BngSize;
}

float4 bng_color(float2 puv)
{
	return in_rect(puv) ? tex2Dlod(sBngColor, float4(rect_tex(puv), 0, 0)) : 0.0;
}

/// Linear depth (m) of BeamNG's picture at puv; 1e9 where it drew nothing.
float bng_depth(float2 puv)
{
	return in_rect(puv) ? bng_linear(tex2Dlod(sBngDepth, float4(rect_tex(puv), 0, 0)).r) : 1e9;
}

/// Where this GTA pixel's view ray lands in BeamNG's picture without re-projection (same camera).
float2 bng_uv(float2 uv)
{
	const float aspectHost = BUFFER_WIDTH * BUFFER_RCP_HEIGHT, aspectBng = BngSize.x / BngSize.y;
	const float2 ray = float2((uv.x * 2.0 - 1.0) * aspectHost, 1.0 - uv.y * 2.0) * HostTan;
	const float tanBng = BngTan * FovScale;
	const float2 n = ray / float2(tanBng * aspectBng, tanBng);
	return float2(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5) + Offset;
}

float3 warp(float3 p)
{
	return float3(dot(WarpRow0, p), dot(WarpRow1, p), dot(WarpRow2, p)) + WarpT;
}

/// BeamNG picture uv of a point in the camera space BeamNG rendered with; false when off the picture or behind.
bool bng_project(float3 pm, out float2 puv)
{
	puv = 0.0;
	if (pm.z > -1e-3)
		return false;
	const float tanBng = BngTan * FovScale;
	const float2 n = pm.xy / -pm.z / float2(tanBng * BngSize.x / BngSize.y, tanBng);
	puv = float2(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5) + Offset;
	return all(abs(n) <= 1.0);
}

/// March this GTA pixel's view ray from near to zFar, moving each point into the camera BeamNG rendered with. The
/// first point behind BeamNG's surface there is where the car is seen: refine on the surface (fixed point on its
/// depth along GTA's ray) and return its picture uv and GTA view depth.
bool reproject(float2 uv, float zFar, out float2 puv, out float zg)
{
	const float3 ray = float3((uv.x * 2.0 - 1.0) * HostTan * BUFFER_WIDTH * BUFFER_RCP_HEIGHT, (1.0 - uv.y * 2.0) * HostTan, -1.0);
	const float zNear = 0.3;
	zFar = max(zFar, zNear * 1.01);
	puv = 0.0;
	zg = 1e9;
	[loop] for (int i = 0; i < 24; ++i)
	{
		const float z = zNear * pow(zFar / zNear, i / 23.0);
		const float3 pm = warp(ray * z);
		float2 n;
		if (!bng_project(pm, n))
			continue;
		if (bng_depth(n) > -pm.z + 0.03)
			continue; // still in front of whatever BeamNG drew there
		float zk = z;
		puv = n;
		[loop] for (int k = 0; k < 3; ++k)
		{
			const float3 mk = warp(ray * zk);
			float2 nk;
			if (!bng_project(mk, nk))
				break;
			const float zb = bng_depth(nk);
			if (zb > 1e8)
				break;
			puv = nk;
			// BeamNG's surface point there, back in GTA's camera space: its depth along GTA's view axis
			const float tanBng = BngTan * FovScale;
			const float3 q = float3(((nk - Offset) * float2(2.0, -2.0) + float2(-1.0, 1.0)) * float2(tanBng * BngSize.x / BngSize.y, tanBng), -1.0) * zb - WarpT;
			zk = -(q.x * WarpRow0.z + q.y * WarpRow1.z + q.z * WarpRow2.z);
		}
		zg = zk;
		return true;
	}
	return false;
}

void PS_Composite(float4 pos : SV_Position, float2 uv : TEXCOORD, out float4 outColor : SV_Target0, out float4 outInfo : SV_Target1)
{
	const float3 host = tex2D(ReShade::BackBuffer, uv).rgb;
	outInfo = 0.0;
	outColor = float4(host, 1.0);
	if (!BngActive)
		return;
	const float zh = host_linear(tex2Dlod(ReShade::DepthBuffer, float4(uv, 0, 0)).x);
	outInfo = float4(0.0, 0.0, zh, 0.0);
	if (DebugView == 1)
	{
		outColor = float4(bands(zh), 1.0);
		return;
	}
	float2 buv;
	float zb;
	if (WarpOn && Reproject)
	{
		if (any(uv < CarRect.xy) || any(uv > CarRect.zw))
		{
			if (DebugView == 2 || DebugView == 3)
				outColor = float4(host * 0.3, 1.0);
			return;
		}
		if (!reproject(uv, min(zh + DepthBias, 400.0), buv, zb))
		{
			if (DebugView == 2 || DebugView == 3)
				outColor = float4(host * 0.3, 1.0);
			return;
		}
	}
	else
	{
		buv = bng_uv(uv);
		zb = bng_depth(buv);
	}
	float4 car = bng_color(buv);
	if (EdgeErode > 0.0 && car.a > 0.0)
	{
		// on the outline (a neighbour in BeamNG's picture is sky), fade the pixel: it holds some of BeamNG's sky
		const float2 px = 1.0 / BngSize;
		const float m = min(min(bng_color(buv + float2(px.x, 0)).a, bng_color(buv - float2(px.x, 0)).a),
			min(bng_color(buv + float2(0, px.y)).a, bng_color(buv - float2(0, px.y)).a));
		car *= lerp(1.0, m, EdgeErode);
	}
	if (DebugView == 2)
	{
		outColor = float4(car.a > 0.0 ? bands(zb) : host * 0.3, 1.0);
		return;
	}
	if (DebugView == 3)
	{
		outColor = float4(car.a > 0.0 ? float3(saturate((zh - zb) * 0.5 + 0.5), saturate((zb - zh) * 0.5 + 0.5), 0.0) : host * 0.3, 1.0);
		return;
	}
	if (DebugView == 4)
	{
		outColor = float4(car.rgb + host * 0.25 * (1.0 - car.a), 1.0);
		return;
	}
	const float visible = zb < zh + DepthBias ? 1.0 : 0.0;
	const float cover = car.a * visible;
	outColor = float4(car.rgb * visible + host * (1.0 - cover), 1.0);
	outInfo = float4(cover, cover > 0.0 ? zb : 0.0, zh, 0.0);
}

/// The finished picture: the car's edges softened (GTA's anti-aliasing ran before the car was added) and GTA's
/// surfaces next to and under the car darkened (BeamNG's own shadow falls on its invisible ground, not GTA's).
float3 PS_Final(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float3 color = tex2D(sComposite, uv).rgb;
	if (!BngActive || DebugView != 0)
		return color;
	const float2 px = BUFFER_PIXEL_SIZE;
	const float4 info = tex2D(sInfo, uv);
	if (EdgeSoftness > 0.0)
	{
		const float c1 = tex2D(sInfo, uv + float2(px.x, 0)).x, c2 = tex2D(sInfo, uv - float2(px.x, 0)).x;
		const float c3 = tex2D(sInfo, uv + float2(0, px.y)).x, c4 = tex2D(sInfo, uv - float2(0, px.y)).x;
		const float edge = saturate(abs(c1 - info.x) + abs(c2 - info.x) + abs(c3 - info.x) + abs(c4 - info.x));
		if (edge > 0.0)
		{
			const float3 n = tex2D(sComposite, uv + float2(px.x, 0)).rgb + tex2D(sComposite, uv - float2(px.x, 0)).rgb
				+ tex2D(sComposite, uv + float2(0, px.y)).rgb + tex2D(sComposite, uv - float2(0, px.y)).rgb;
			color = lerp(color, (color * 2.0 + n) / 6.0, edge * EdgeSoftness);
		}
	}
	if (ContactShadow > 0.0 && info.x < 0.5 && info.z < 200.0)
	{
		const float zh = info.z;
		const float r = clamp(ContactRadius / max(zh, 0.5) * BUFFER_HEIGHT / (2.0 * HostTan), 2.0, 64.0);
		float occ = 0.0, weight = 0.0;
		[unroll] for (int i = 0; i < 12; ++i)
		{
			const float a = i * 2.39996 + 0.6; // golden-angle spiral
			const float rr = r * sqrt((i + 0.5) / 12.0);
			const float2 o = float2(cos(a), sin(a)) * rr;
			const float4 sm = tex2Dlod(sInfo, float4(uv + o * px, 0, 0));
			const float w = o.y < 0.0 ? 1.0 : 0.35; // the car stands on the ground: its shadow is below it on screen
			weight += w;
			if (sm.x > 0.5 && abs(sm.y - zh) < ContactRadius * 1.5)
				occ += w * (1.0 - rr / (r + 1.0));
		}
		color *= 1.0 - ContactShadow * saturate(occ / max(weight, 1e-3) * 2.2);
	}
	return color;
}

technique GTAxBeam < ui_tooltip = "GTA x BeamNG: draws BeamNG's car into GTA. Gated automatically by the GTAxBeam script."; >
{
	pass Composite
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Composite;
		RenderTarget0 = CompositeTex;
		RenderTarget1 = InfoTex;
	}
	pass Final
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Final;
	}
}
