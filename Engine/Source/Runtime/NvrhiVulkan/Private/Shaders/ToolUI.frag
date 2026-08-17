#version 450

layout(set = 0, binding = 0) uniform texture2D FontTexture;
layout(set = 0, binding = 128) uniform sampler FontSampler;

layout(location = 0) in vec2 UV;
layout(location = 1) in vec4 Color;

layout(location = 0) out vec4 OutColor;

const int DitherPattern[64] = int[64](
	0, 48, 12, 60, 3, 51, 15, 63,
	32, 16, 44, 28, 35, 19, 47, 31,
	8, 56, 4, 52, 11, 59, 7, 55,
	40, 24, 36, 20, 43, 27, 39, 23,
	2, 50, 14, 62, 1, 49, 13, 61,
	34, 18, 46, 30, 33, 17, 45, 29,
	10, 58, 6, 54, 9, 57, 5, 53,
	42, 26, 38, 22, 41, 25, 37, 21);

void main()
{
	const vec3 ColorDerivative = fwidth(Color.rgb);
	const float GradientMask = step(0.0000001, max(ColorDerivative.r, max(ColorDerivative.g, ColorDerivative.b)));
	const ivec2 DitherPosition = ivec2(gl_FragCoord.xy) & 7;
	const float Threshold = (float(DitherPattern[DitherPosition.y * 8 + DitherPosition.x]) + 0.5) / 64.0;
	const vec3 ScaledColor = clamp(Color.rgb, 0.0, 1.0) * 255.0;
	const vec3 QuantizedColor = (floor(ScaledColor) + step(vec3(Threshold), fract(ScaledColor))) / 255.0;
	const vec4 DitheredColor = vec4(mix(Color.rgb, QuantizedColor, GradientMask), Color.a);
	OutColor = DitheredColor * texture(sampler2D(FontTexture, FontSampler), UV);
}
