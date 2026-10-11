#include "Fullscreen.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> SceneColor;
[[vk::binding(1, 0)]] Texture2D<float4> FogVolume;

// Meters the HDR scene into a 32x32 grid of weighted log luminance, centre-weighted like a camera's average metering.
// Red holds weight * log2(luminance) and green the weight, so averaging both keeps their ratio the weighted mean.
[shader("fragment")]
float4 fragmentMain(FFullscreenOutput Input) : SV_Target
{
    const float Offset = 0.25 / 32.0;
    float LogSum = 0.0;
    for (int Tap = 0; Tap < 4; ++Tap)
    {
        float2 UV = Input.UV + float2((Tap & 1) != 0 ? Offset : -Offset, (Tap & 2) != 0 ? Offset : -Offset);
        float3 Color = SceneColor.SampleLevel(LinearSampler, UV, 0).rgb;
        // Tone mapping composites fog at full resolution; a bilinear tap is enough for metering.
        if (Visual.Fog.x > 0.0)
        {
            float4 Fog = FogVolume.SampleLevel(LinearSampler, UV, 0);
            Color = Color * Fog.a + Fog.rgb;
        }

        float Luminance = dot(Color, float3(0.2126, 0.7152, 0.0722));
        LogSum += log2(clamp(Luminance, 1e-4, 65504.0));
    }

    float2 Center = Input.UV * 2.0 - 1.0;
    float Weight = 1.0 + 2.0 * saturate(1.0 - dot(Center, Center));
    return float4(LogSum * 0.25 * Weight, Weight, 0.0, 1.0);
}
