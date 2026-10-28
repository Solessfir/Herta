#include "../SmaaEdgeEncoding.slangh"
// The reference edge detection discards pixels without edges; a compute thread returns no edge instead.
#define discard return float2(0.0, 0.0)
#include "../Smaa.slangh"
#undef discard
[[vk::binding(0, 0)]] Texture2D<float4> ColorTexture;
[[vk::binding(384, 0)]] [format("rgba8")] RWTexture2D<float4> Edges;
[[vk::binding(385, 0)]] RWStructuredBuffer<uint> EdgePixels;
// The weight pass's indirect group count, 1, 1, then the number of edge pixels.
[[vk::binding(386, 0)]] RWStructuredBuffer<uint> Arguments;

// Jimenez's Filmic SMAA: detect edges for every pixel, but list only the pixels that have one, so the weight pass runs on those alone.
[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID)
{
    float2 Edge = 0.0;
    if (all(Id.xy < uint2(Visual.Viewport.zw)))
    {
        float2 UV = (float2(Id.xy) + 0.5) * Visual.Viewport.xy;
        float4 Offset[3];
        SMAAEdgeDetectionVS(UV, Offset);
        Edge = SMAAColorEdgeDetectionPS(UV, Offset, ColorTexture);
        Edges[Id.xy] = float4(Edge, 0.0, 0.0);
    }

    // One atomic per wave reserves its slots. Each 64th slot starts another weight group.
    bool bEdge = any(Edge > 0.0);
    uint WaveCount = WaveActiveCountBits(bEdge);
    if (WaveCount == 0)
        return;

    uint Base = 0;
    if (WaveIsFirstLane())
    {
        InterlockedAdd(Arguments[3], WaveCount, Base);
        uint Groups = (Base + WaveCount + 63) / 64 - (Base + 63) / 64;
        if (Groups != 0)
            InterlockedAdd(Arguments[0], Groups);
    }

    Base = WaveReadLaneFirst(Base);
    if (bEdge)
        EdgePixels[Base + WavePrefixCountBits(bEdge)] = Id.x | (Id.y << 16);
}
