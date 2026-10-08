#include "Fullscreen.slangh"
// sRGB encoding for edge detection. HDR targets exceed 1, so above white it continues logarithmically with the curve's slope at 1, which keeps the SDR thresholds meaningful for highlights.
float4 Gamma(float4 C)
{
    float3 Linear = max(0.0, C.rgb);
    float3 Encoded = lerp(Linear * 12.92, 1.055 * pow(min(Linear, 1.0), 1.0 / 2.4) - 0.055, step(0.0031308, Linear));
    return float4(lerp(Encoded, 1.0 + (1.055 / 2.4) * log(max(Linear, 1.0)), step(1.0, Linear)), C.a);
}
#define SMAASamplePoint(tex, coord) Gamma(SmaaPoint(tex, coord))
#define SMAASample(tex, coord) Gamma(tex.SampleLevel(LinearSampler, coord, 0))
#define SMAASampleOffset(tex, coord, offset) Gamma(tex.SampleLevel(LinearSampler, coord + offset * Visual.Viewport.xy, 0))
#include "Smaa.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> ColorTexture;

[shader("fragment")]
float4 fragmentMain(FFullscreenOutput Input) : SV_Target
{
    float4 Offset[3];
    SMAAEdgeDetectionVS(Input.UV, Offset);
    return float4(SMAAColorEdgeDetectionPS(Input.UV, Offset, ColorTexture), 0.0, 0.0);
}
