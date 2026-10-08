// BeamNG side of the compositor. Writes BeamNG's picture with coverage (where its depth buffer holds geometry: the
// car; the void level's sky is at the far plane) and its raw depth into two textures, which the GTAxBeamExport add-on
// reads back and publishes to GTA through shared memory.
#include "ReShade.fxh"

texture GxbColorTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
texture GxbDepthTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R32F; };

void PS_Export(float4 pos : SV_Position, float2 uv : TEXCOORD, out float4 color : SV_Target0, out float depth : SV_Target1)
{
	const float d = tex2Dlod(ReShade::DepthBuffer, float4(uv, 0, 0)).x;
	// both conventions: cleared depth is 0 (reversed Z) or 1
	const float covered = (d > 1e-6 && d < 1.0 - 1e-6) ? 1.0 : 0.0;
	color = float4(tex2D(ReShade::BackBuffer, uv).rgb * covered, covered);
	depth = d;
}

technique GTAxBeamExport < ui_tooltip = "GTA x BeamNG: publishes this picture and its depth for GTA's compositor."; >
{
	pass
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Export;
		RenderTarget0 = GxbColorTex;
		RenderTarget1 = GxbDepthTex;
	}
}
