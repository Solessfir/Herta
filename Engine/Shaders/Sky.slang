#include "Fullscreen.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> Environment;
[[vk::binding(1, 0)]] Texture2D<float4> SkyView;

[shader("fragment")]
float4 fragmentMain(FFullscreenOutput Input) : SV_Target
{
    if (Visual.Sky.y < 0.5) return float4(0.035, 0.035, 0.035, 1.0);
    float3 Direction = CameraRay(Input.UV);
    float3 Color;
    if (Visual.Atmosphere.w > 0.5 && Visual.AtmosphereGeometry.z < 0.5)
    {
        // Below the horizon, the sky's horizon colour in that direction stands in for distant haze; mirroring would repeat the sunrise glow upside down.
        float2 Horizontal = Direction.xz / max(length(Direction.xz), 1e-5);
        float3 View = Direction.y >= 0.0 ? Direction : float3(Horizontal.x, 0.0, Horizontal.y);
        Color = ComposeProceduralSky(Direction, SkyView.SampleLevel(LinearSampler, SkyViewUV(View), 0).rgb);
    }
    else
    {
        Color = Environment.SampleLevel(LinearSampler, DirectionUV(Direction), 0).rgb;
    }

    return float4(clamp(Color, 0.0, 65504.0), 1.0);
}
