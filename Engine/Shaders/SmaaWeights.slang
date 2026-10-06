#include "Smaa.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> Edges;
[[vk::binding(1, 0)]] Texture2D<float4> Area;
[[vk::binding(2, 0)]] Texture2D<float4> Search;

[shader("fragment")]
float4 fragmentMain(FFullscreenOutput Input) : SV_Target
{
    float2 Pixel;
    float4 Offset[3];
    SMAABlendingWeightCalculationVS(Input.UV, Pixel, Offset);
    return SMAABlendingWeightCalculationPS(Input.UV, Pixel, Offset, Edges, Area, Search, 0.0);
}
