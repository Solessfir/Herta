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

void main()
{
	const bool Vertical = Color.g > 0.5;
	const float SourceRadius = Color.r * 160.0 * (Vertical ? 0.5 : 1.0);
	const vec2 Texel = 1.0 / vec2(textureSize(sampler2D(SourceTexture, LinearSampler), 0));
	const vec2 Step = (Vertical ? vec2(0.0, 1.0) : vec2(1.0, 0.0)) * Texel * (SourceRadius / 6.0);
	vec4 Result = vec4(0.0);
	float TotalWeight = 0.0;
	for (int Index = -6; Index <= 6; ++Index)
	{
		const float Position = float(Index) / 3.0;
		const float Weight = exp(-0.5 * Position * Position);
		Result += SampleSource(UV + Step * float(Index)) * Weight;
		TotalWeight += Weight;
	}
	OutColor = Result / TotalWeight;
}
