#version 450

layout(set = 0, binding = 0) uniform texture2D FontTexture;
layout(set = 0, binding = 128) uniform sampler FontSampler;

layout(push_constant) uniform FPushConstants
{
	vec2 Scale;
	vec2 Translate;
	// Zero for an sRGB target; otherwise the HDR10 target's paper white in cd/m^2.
	float PaperWhite;
	// 0 is sRGB-encoded UI content, 1 linear scene color relative to paper white, and 2 already in the target's encoding.
	float Encoding;
} PushConstants;

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

vec3 DecodeSrgb(const vec3 Encoded)
{
	return mix(Encoded / 12.92, pow((Encoded + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), Encoded));
}

vec3 EncodeSrgb(const vec3 Linear)
{
	const vec3 Clamped = clamp(Linear, 0.0, 1.0);
	return mix(Clamped * 12.92, 1.055 * pow(Clamped, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), Clamped));
}

// Linear Rec.709 relative to paper white to PQ-encoded Rec.2020, as HDR10 expects.
vec3 EncodeHdr10(const vec3 Linear)
{
	const mat3 Rec709ToRec2020 = mat3(0.627404, 0.069097, 0.016391, 0.329283, 0.919540, 0.088013, 0.043313, 0.011362, 0.895595);
	const vec3 Y = pow(clamp(Rec709ToRec2020 * max(Linear, 0.0) * (PushConstants.PaperWhite / 10000.0), 0.0, 1.0), vec3(0.1593017578125));
	return pow((0.8359375 + 18.8515625 * Y) / (1.0 + 18.6875 * Y), vec3(78.84375));
}

void main()
{
	const vec3 ColorDerivative = fwidth(Color.rgb);
	const float GradientMask = step(0.0000001, max(ColorDerivative.r, max(ColorDerivative.g, ColorDerivative.b)));
	const ivec2 DitherPosition = ivec2(gl_FragCoord.xy) & 7;
	const float Threshold = (float(DitherPattern[DitherPosition.y * 8 + DitherPosition.x]) + 0.5) / 64.0;
	const vec3 ScaledColor = clamp(Color.rgb, 0.0, 1.0) * 255.0;
	const vec3 QuantizedColor = (floor(ScaledColor) + step(vec3(Threshold), fract(ScaledColor))) / 255.0;
	const vec4 DitheredColor = vec4(mix(Color.rgb, QuantizedColor, GradientMask), Color.a);
	const vec4 Sampled = texture(sampler2D(FontTexture, FontSampler), UV);
	const bool bHdr = PushConstants.PaperWhite > 0.0;
	if (PushConstants.Encoding > 1.5)
	{
		OutColor = DitheredColor * Sampled;
		return;
	}

	// ImGui blends in display encoding, so HDR targets blend PQ values much as SDR targets blend sRGB values.
	if (PushConstants.Encoding > 0.5)
	{
		const vec3 Linear = Sampled.rgb * DecodeSrgb(DitheredColor.rgb);
		OutColor = vec4(bHdr ? EncodeHdr10(Linear) : EncodeSrgb(Linear), Sampled.a * DitheredColor.a);
		return;
	}

	const vec4 Ui = DitheredColor * Sampled;
	OutColor = bHdr ? vec4(EncodeHdr10(DecodeSrgb(Ui.rgb)), Ui.a) : Ui;
}
