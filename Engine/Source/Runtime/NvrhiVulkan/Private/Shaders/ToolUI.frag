#version 450

layout(set = 0, binding = 0) uniform texture2D FontTexture;
layout(set = 0, binding = 128) uniform sampler FontSampler;

layout(location = 0) in vec2 UV;
layout(location = 1) in vec4 Color;

layout(location = 0) out vec4 OutColor;

void main()
{
	OutColor = Color * texture(sampler2D(FontTexture, FontSampler), UV);
}
