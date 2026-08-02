#version 450

layout(set = 0, binding = 0) uniform texture2D FontTexture;
layout(set = 0, binding = 128) uniform sampler FontSampler;

layout(location = 0) in vec2 UV;
layout(location = 1) in vec4 Color;

layout(location = 0) out vec4 OutColor;

vec3 SrgbToLinear(const vec3 Value)
{
	const bvec3 LinearSegment = lessThanEqual(Value, vec3(0.04045));
	const vec3 Linear = Value / 12.92;
	const vec3 Exponential = pow((Value + 0.055) / 1.055, vec3(2.4));
	return mix(Exponential, Linear, LinearSegment);
}

void main()
{
	const vec4 TextureColor = texture(sampler2D(FontTexture, FontSampler), UV);
	OutColor = vec4(SrgbToLinear(Color.rgb), Color.a) * TextureColor;
}
