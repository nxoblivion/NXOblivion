// oblivion (2026-11-03): Renut's pixel shaders for the Bink movies, port-written: the two pixel shaders Oblivion
// compiles at boot (sub_8211C3A8 -> the D3DX compiler wrapper sub_82484BE0, ps_2_0) and no game file holds, so no tool
// can translate them from the player's files. Three planes (container XXH3 819818509cd1e1ed) and with alpha
// (RENUT_MOVIE_ALPHA, 7a0ce24f6a7ded83: four planes). No game shader code: what the shaders have to do, in Renut's
// translator's interface (peggle2-nx's shaders/port/renut/peggle2_renut_movie_ps.hlsl, the method).
//
// tools/renut/port_shaders/port_shaders.py puts the translator's shader_common.h and its pixel shader prologue in front
// of this file and writes the result where the translator takes a shader's HLSL from (BANJO_HLSL_OVERRIDE); the
// signature and the epilogue (alpha test, blend skip, output trim) are the ones the translator writes for every pixel
// shader.
//
// A movie frame is three single-channel planes (Y, Cr, Cb; with alpha a fourth, A), each read in its first channel at
// TEXCOORD0. With c = (Y, Cr, Cb, c3.x) the colour is (dot(c0, c), dot(c1, c), dot(c2, c)) and its alpha c3.w (times A).
// Registers: textures s0 Y, s1 Cr, s2 Cb, s3 A; constants c0 to c3, set by the game per frame.

#ifndef __spirv__
[shader("pixel")]
#endif
void main(
	in float4 iPos : SV_Position,
	in float4 iTexCoord0 : TEXCOORD0,
	in float4 iTexCoord1 : TEXCOORD1,
	in float4 iTexCoord2 : TEXCOORD2,
	in float4 iTexCoord3 : TEXCOORD3,
	in float4 iTexCoord4 : TEXCOORD4,
	in float4 iTexCoord5 : TEXCOORD5,
	in float4 iTexCoord6 : TEXCOORD6,
	in float4 iTexCoord7 : TEXCOORD7,
	in float4 iTexCoord8 : TEXCOORD8,
	in float4 iTexCoord9 : TEXCOORD9,
	in float4 iTexCoord10 : TEXCOORD10,
	in float4 iTexCoord11 : TEXCOORD11,
	in float4 iTexCoord12 : TEXCOORD12,
	in float4 iTexCoord13 : TEXCOORD13,
	in float4 iTexCoord14 : TEXCOORD14,
	in float4 iTexCoord15 : TEXCOORD15,
	in float4 iColor0 : COLOR0,
	in float4 iColor1 : COLOR1,
	in float4 iNormal0 : NORMAL0,
	in float4 iNormal1 : NORMAL1,
	in float4 iNormal2 : NORMAL2,
	in float4 iTangent0 : TANGENT0,
	in float4 iBinormal0 : BINORMAL0,
	in float4 iFog0 : FOG0,
	in float2 iPointCoord : POINTCOORD,
#ifdef __spirv__
	in bool iFace : SV_IsFrontFace
#else
	in uint iFace : SV_IsFrontFace
#endif
,
	out float4 oC0 : SV_Target0)
{
	const float2 uv = iTexCoord0.xy;
	float4 c;
	c.x = tfetch2D(s0_Texture2DDescriptorIndex, s0_SamplerDescriptorIndex, uv, float2(0, 0)).x;
	c.y = tfetch2D(s1_Texture2DDescriptorIndex, s1_SamplerDescriptorIndex, uv, float2(0, 0)).x;
	c.z = tfetch2D(s2_Texture2DDescriptorIndex, s2_SamplerDescriptorIndex, uv, float2(0, 0)).x;
	c.w = g_C(3).x;
#ifdef RENUT_MOVIE_ALPHA
	oC0.w = g_C(3).w * tfetch2D(s3_Texture2DDescriptorIndex, s3_SamplerDescriptorIndex, uv, float2(0, 0)).x;
#else
	oC0.w = g_C(3).w;
#endif
	oC0.x = dot(g_C(0), c);
	oC0.y = dot(g_C(1), c);
	oC0.z = dot(g_C(2), c);
	alphaTest(oC0.w, g_AlphaTestFunc, g_AlphaThreshold);
	banjoBlendSkip(oC0);
	oC0 = banjoTrim(oC0, 0u);
}
