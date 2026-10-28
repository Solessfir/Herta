#include "../Smaa.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> Edges;
[[vk::binding(1, 0)]] Texture2D<float4> Area;
[[vk::binding(2, 0)]] Texture2D<float4> Search;
[[vk::binding(384, 0)]] [format("rgba8")] RWTexture2D<float4> Weights;
[[vk::binding(385, 0)]] RWStructuredBuffer<uint> EdgePixels;
[[vk::binding(386, 0)]] RWStructuredBuffer<uint> Arguments;

// Runs once per listed edge pixel through an indirect dispatch. Every other pixel keeps the cleared zero weight.
[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID)
{
    if (Id.x >= Arguments[3])
        return;

    uint Packed = EdgePixels[Id.x];
    uint2 Pixel = uint2(Packed & 0xFFFFu, Packed >> 16);
    float2 UV = (float2(Pixel) + 0.5) * Visual.Viewport.xy;
    float2 PixelCoordinate;
    float4 Offset[3];
    SMAABlendingWeightCalculationVS(UV, PixelCoordinate, Offset);
    Weights[Pixel] = SMAABlendingWeightCalculationPS(UV, PixelCoordinate, Offset, Edges, Area, Search, 0.0);
}
