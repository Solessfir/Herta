#include "Fullscreen.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> SceneColor;
[[vk::binding(1, 0)]] Texture2D<float4> AutoExposure;

[shader("fragment")]
float4 fragmentMain(FFullscreenOutput Input) : SV_Target
{
    float Scale = Visual.Exposure.x > 0.5 ? 1.0 / (1.2 * exp2(AutoExposure.Load(int3(0, 0, 0)).r)) : Visual.Controls.z;
    float3 X = max(0.0, SceneColor.SampleLevel(LinearSampler, Input.UV, 0).rgb * Scale);
    float3 Mapped = saturate((X * (2.51 * X + 0.03)) / (X * (2.43 * X + 0.59) + 0.14));
    return float4(Mapped, 1.0);
}
