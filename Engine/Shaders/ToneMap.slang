#include "Fullscreen.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> SceneColor;

[shader("fragment")]
float4 fragmentMain(FFullscreenOutput Input) : SV_Target
{
    float3 X = max(0.0, SceneColor.SampleLevel(LinearSampler, Input.UV, 0).rgb * Visual.Controls.z);
    float3 Mapped = saturate((X * (2.51 * X + 0.03)) / (X * (2.43 * X + 0.59) + 0.14));
    return float4(Mapped, 1.0);
}
