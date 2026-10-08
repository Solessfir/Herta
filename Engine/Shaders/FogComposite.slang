#include "Fullscreen.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> SceneColor;
[[vk::binding(1, 0)]] Texture2D<float4> FogVolume;
[[vk::binding(2, 0)]] Texture2D<float4> DepthTexture;

[shader("fragment")]
float4 fragmentMain(FFullscreenOutput Input) : SV_Target
{
    float4 Color = SceneColor.SampleLevel(LinearSampler, Input.UV, 0);
    float Depth = DepthTexture.SampleLevel(LinearSampler, Input.UV, 0).r;
    uint W, H;
    FogVolume.GetDimensions(W, H);
    float2 Pixel = Input.UV * float2(W, H) - 0.5;
    float2 Center = round(Pixel);
    float4 Fog = 0.0;
    float Total = 0.0;
    // A 3x3 Gaussian over the low-resolution volume both upsamples it and averages the per-pixel ray-march dither; depth similarity keeps fog from bleeding across silhouettes.
    for (int Y = -1; Y <= 1; ++Y)
    for (int X = -1; X <= 1; ++X)
    {
        float2 Texel = Center + float2(X, Y);
        float2 UV = (Texel + 0.5) / float2(W, H);
        float2 Offset = Texel - Pixel;
        float OtherDepth = DepthTexture.SampleLevel(LinearSampler, UV, 0).r;
        float Weight = exp(-0.5 * dot(Offset, Offset));
        Weight *= exp(-abs(Depth - OtherDepth) / max(0.0001, Depth * 0.1));
        Fog += FogVolume.SampleLevel(LinearSampler, UV, 0) * Weight;
        Total += Weight;
    }
    Fog = Total > 0.00001 ? Fog / Total : FogVolume.SampleLevel(LinearSampler, Input.UV, 0);
    return float4(clamp(Color.rgb * Fog.a + Fog.rgb, 0.0, 65504.0), 1.0);
}
