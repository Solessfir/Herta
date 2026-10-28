#include "GtaoShared.slangh"
[[vk::binding(0, 0)]] Texture2D<float> SceneDepth;
[[vk::binding(384, 0)]] [format("r32f")] RWTexture2D<float> ViewDepth0;
[[vk::binding(385, 0)]] [format("r32f")] RWTexture2D<float> ViewDepth1;
[[vk::binding(386, 0)]] [format("r32f")] RWTexture2D<float> ViewDepth2;
[[vk::binding(387, 0)]] [format("r32f")] RWTexture2D<float> ViewDepth3;
[[vk::binding(388, 0)]] [format("r32f")] RWTexture2D<float> ViewDepth4;

groupshared float Reduced[8][8];

float LoadViewDepth(int2 Pixel)
{
    float Depth = SceneDepth.Load(int3(min(Pixel, int2(Gtao.Extent.xy) - 1), 0));
    float ViewZ = (Gtao.DepthToView.x * Depth + Gtao.DepthToView.y) / (Gtao.DepthToView.z * Depth + Gtao.DepthToView.w);
    return Depth > 0.0 ? min(ViewZ, GtaoFarDepth) : GtaoFarDepth;
}

void Store(RWTexture2D<float> Target, int2 Pixel, float Value)
{
    uint Width, Height;
    Target.GetDimensions(Width, Height);
    if (all(uint2(Pixel) < uint2(Width, Height)))
        Target[Pixel] = Value;
}

// XeGTAO's filter: depths far in front of the farthest of the four fade out, so a coarse texel does not pull a background surface towards a thin foreground edge.
float DepthMipFilter(float A, float B, float C, float D)
{
    float4 Depths = float4(A, B, C, D);
    float Farthest = max(max(A, B), max(C, D));
    float FalloffRange = 0.615 * Gtao.Settings.x;
    float FalloffFrom = Gtao.Settings.x - FalloffRange;
    float4 Weights = saturate((Farthest - Depths) * (-1.0 / FalloffRange) + (FalloffFrom / FalloffRange + 1.0));
    return dot(Weights, Depths) / dot(Weights, 1.0);
}

// Each 8x8 group reduces a 16x16 tile through all five mips without leaving shared memory.
[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 Group : SV_GroupID, uint3 Thread : SV_GroupThreadID)
{
    int2 Pixel = int2(Group.xy * 16 + Thread.xy * 2);
    float A = LoadViewDepth(Pixel);
    float B = LoadViewDepth(Pixel + int2(1, 0));
    float C = LoadViewDepth(Pixel + int2(0, 1));
    float D = LoadViewDepth(Pixel + int2(1, 1));
    Store(ViewDepth0, Pixel, A);
    Store(ViewDepth0, Pixel + int2(1, 0), B);
    Store(ViewDepth0, Pixel + int2(0, 1), C);
    Store(ViewDepth0, Pixel + int2(1, 1), D);

    float Mip = DepthMipFilter(A, B, C, D);
    Store(ViewDepth1, int2(Group.xy * 8 + Thread.xy), Mip);
    Reduced[Thread.x][Thread.y] = Mip;
    GroupMemoryBarrierWithGroupSync();

    if (all((Thread.xy & 1) == 0))
    {
        Mip = DepthMipFilter(Reduced[Thread.x][Thread.y], Reduced[Thread.x + 1][Thread.y], Reduced[Thread.x][Thread.y + 1], Reduced[Thread.x + 1][Thread.y + 1]);
        Store(ViewDepth2, int2(Group.xy * 4 + Thread.xy / 2), Mip);
        Reduced[Thread.x][Thread.y] = Mip;
    }
    GroupMemoryBarrierWithGroupSync();

    if (all((Thread.xy & 3) == 0))
    {
        Mip = DepthMipFilter(Reduced[Thread.x][Thread.y], Reduced[Thread.x + 2][Thread.y], Reduced[Thread.x][Thread.y + 2], Reduced[Thread.x + 2][Thread.y + 2]);
        Store(ViewDepth3, int2(Group.xy * 2 + Thread.xy / 4), Mip);
        Reduced[Thread.x][Thread.y] = Mip;
    }
    GroupMemoryBarrierWithGroupSync();

    if (all(Thread.xy == 0))
        Store(ViewDepth4, int2(Group.xy), DepthMipFilter(Reduced[0][0], Reduced[4][0], Reduced[0][4], Reduced[4][4]));
}
