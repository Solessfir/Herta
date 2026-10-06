#include "Fullscreen.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> Environment;

[shader("fragment")]
float4 fragmentMain(FFullscreenOutput Input) : SV_Target
{
    if (Visual.Sky.y < 0.5) return float4(0.035, 0.035, 0.035, 1.0);
    float3 Direction = CameraRay(Input.UV);
    float3 Color = Visual.Atmosphere.w > 0.5 && Visual.AtmosphereGeometry.z < 0.5 ? ProceduralSky(Direction) * 15000.0 : Environment.SampleLevel(LinearSampler, DirectionUV(Direction), 0).rgb;
    return float4(clamp(Color, 0.0, 65504.0), 1.0);
}
