#version 450

layout(push_constant) uniform FPushConstants
{
	vec2 Scale;
	vec2 Translate;
} PushConstants;

layout(location = 0) in vec2 Position;
layout(location = 1) in vec2 UV;
layout(location = 2) in vec4 Color;

layout(location = 0) out vec2 OutUV;
layout(location = 1) out vec4 OutColor;

void main()
{
	OutUV = UV;
	OutColor = Color;
	gl_Position = vec4(Position * PushConstants.Scale + PushConstants.Translate, 0.0, 1.0);
}
