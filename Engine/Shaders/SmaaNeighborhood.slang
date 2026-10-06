#include "Smaa.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> Color;
[[vk::binding(1, 0)]] Texture2D<float4> Weights;

[shader("fragment")]
float4 fragmentMain(FFullscreenOutput Input) : SV_Target
{
    float4 Offset;
    SMAANeighborhoodBlendingVS(Input.UV, Offset);
    return SMAANeighborhoodBlendingPS(Input.UV, Offset, Color, Weights);
}
