#include "GtaoShared.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> NoisyOcclusion;
[[vk::binding(1, 0)]] Texture2D<float> ViewDepth;
[[vk::binding(384, 0)]] [format("rgba8")] RWTexture2D<float4> AmbientOcclusion;

// An 8x8 group of full-resolution pixels reads 8x8 half-resolution texels: its own 4x4 and two more on each side.
groupshared float TileDepth[8][8];
groupshared float4 TileOcclusion[8][8];

float LoadViewDepth(int2 Pixel)
{
    return ViewDepth.Load(int3(clamp(Pixel, int2(0, 0), int2(Gtao.Extent.xy) - 1), 0));
}

// Denoises and upsamples in one pass. Each full-resolution pixel averages the 4x4 half-resolution texels centered on it, which covers every value of the GTAO
// noise pattern once. Taps are weighted by how far they leave the pixel's depth plane, so slanted floors still blur fully but silhouettes do not.
[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID, uint3 Group : SV_GroupID, uint3 Thread : SV_GroupThreadID)
{
    int2 Origin = int2(Group.xy * 4) - 2;
    int2 Tap = clamp(Origin + int2(Thread.xy), int2(0, 0), int2(Gtao.HalfExtent.xy) - 1);
    TileDepth[Thread.y][Thread.x] = ViewDepth.Load(int3(Tap, 1));
    TileOcclusion[Thread.y][Thread.x] = NoisyOcclusion.Load(int3(Tap, 0));
    GroupMemoryBarrierWithGroupSync();

    int2 Pixel = int2(Id.xy);
    if (any(Id.xy >= uint2(Gtao.Extent.xy)))
        return;

    float ViewZ = LoadViewDepth(Pixel);
    float Left = ViewZ - LoadViewDepth(Pixel - int2(1, 0));
    float Right = LoadViewDepth(Pixel + int2(1, 0)) - ViewZ;
    float Up = ViewZ - LoadViewDepth(Pixel - int2(0, 1));
    float Down = LoadViewDepth(Pixel + int2(0, 1)) - ViewZ;
    float2 Slope = float2(abs(Left) < abs(Right) ? Left : Right, abs(Up) < abs(Down) ? Up : Down);
    float Tolerance = 0.02 * ViewZ + 0.01;

    // Even pixels take half texels -2..1 around their own, odd ones -1..2.
    int2 First = (Pixel >> 1) - 2 + (Pixel & 1) - Origin;
    float4 Sum = 0.0;
    float WeightSum = 0.0;
    for (int Y = 0; Y < 4; ++Y)
    {
        for (int X = 0; X < 4; ++X)
        {
            int2 Tile = First + int2(X, Y);
            // A half texel's center is 2 * index + 0.5 in full-resolution pixels.
            float2 Offset = float2(Tile + Origin) * 2.0 + 0.5 - float2(Pixel);
            float Weight = saturate(1.0 - abs(TileDepth[Tile.y][Tile.x] - (ViewZ + dot(Slope, Offset))) / Tolerance);
            Sum += TileOcclusion[Tile.y][Tile.x] * Weight;
            WeightSum += Weight;
        }
    }

    // Every tap can fall off the plane at single-pixel features; the nearest texel is the best guess there.
    if (WeightSum < 1e-3)
    {
        int2 Nearest = (Pixel >> 1) - Origin;
        Sum = TileOcclusion[Nearest.y][Nearest.x];
        WeightSum = 1.0;
    }

    AmbientOcclusion[Pixel] = Sum / WeightSum;
}
