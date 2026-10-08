#version 450

layout(set = 0, binding = 0) uniform texture2D SourceTexture;
layout(set = 0, binding = 128) uniform sampler LinearSampler;

layout(location = 0) in vec2 UV;
layout(location = 1) in vec4 Color;
layout(location = 0) out vec4 OutColor;

vec4 SampleSource(vec2 Coordinate)
{
	return texture(sampler2D(SourceTexture, LinearSampler), Coordinate);
}

// Dual-filter Kawase blur (Bjorge, SIGGRAPH 2015). Red carries the tap offset in source texels over 2; green selects upsampling.
void main()
{
	const bool bUpsample = Color.g > 0.5;
	const vec2 Texel = 1.0 / vec2(textureSize(sampler2D(SourceTexture, LinearSampler), 0));
	const vec2 Offset = Texel * Color.r * 2.0;
	if (bUpsample)
	{
		const vec2 Half = Offset * 0.5;
		vec4 Result = SampleSource(UV + vec2(-Offset.x, 0.0));
		Result += SampleSource(UV + vec2(Offset.x, 0.0));
		Result += SampleSource(UV + vec2(0.0, -Offset.y));
		Result += SampleSource(UV + vec2(0.0, Offset.y));
		Result += SampleSource(UV + vec2(-Half.x, -Half.y)) * 2.0;
		Result += SampleSource(UV + vec2(Half.x, -Half.y)) * 2.0;
		Result += SampleSource(UV + vec2(-Half.x, Half.y)) * 2.0;
		Result += SampleSource(UV + vec2(Half.x, Half.y)) * 2.0;
		OutColor = Result / 12.0;
		return;
	}

	vec4 Result = SampleSource(UV) * 4.0;
	Result += SampleSource(UV - Offset);
	Result += SampleSource(UV + Offset);
	Result += SampleSource(UV + vec2(Offset.x, -Offset.y));
	Result += SampleSource(UV + vec2(-Offset.x, Offset.y));
	OutColor = Result / 8.0;
}
