#include "../VisualShared.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> SceneDepth;
// Nearest then farthest visible view depth, as float bits, which order like uints for positive values. The sky is left out.
[[vk::binding(384, 0)]] RWStructuredBuffer<uint> Bounds;

static const uint WaveSlots = 32;
groupshared uint GroupNear[WaveSlots];
groupshared uint GroupFar[WaveSlots];

// Each thread reduces a 4x4 block with four gathers, so a 16x16 group covers 64x64 pixels. Reducing in the group first leaves one atomic pair per
// group; one pair per wave made every wave contend for the same two addresses and cost ten times as much.
[shader("compute")]
[numthreads(16, 16, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID, uint Index : SV_GroupIndex)
{
    uint Width, Height;
    SceneDepth.GetDimensions(Width, Height);
    float2 Size = float2(Width, Height);
    // Reversed depth reaches 0 at the sky, so only covered pixels count; gathers past the edge clamp to pixels already counted.
    float Nearest = 1e30;
    float Farthest = 0.0;
    for (uint Y = 0; Y < 2; ++Y)
    {
        for (uint X = 0; X < 2; ++X)
        {
            float2 Corner = float2(Id.xy * 4 + uint2(X, Y) * 2 + 1);
            float4 Depths = SceneDepth.GatherRed(LinearSampler, Corner / Size);
            for (uint Tap = 0; Tap < 4; ++Tap)
            {
                float Depth = Depths[Tap];
                if (Depth > 0.0)
                {
                    // View depth is projective in device depth, so the nearest pixel has the largest depth and the farthest the smallest.
                    Nearest = min(Nearest, 1.0 / Depth);
                    Farthest = max(Farthest, 1.0 / Depth);
                }
            }
        }
    }

    uint Near = 0xFFFFFFFFu;
    uint Far = 0u;
    if (Farthest > 0.0)
    {
        // The reciprocal ranks pixels; converting just the two extremes keeps the per-pixel work to a division.
        float4 NearView = mul(Visual.ClipToView, float4(0.0, 0.0, 1.0 / Nearest, 1.0));
        float4 FarView = mul(Visual.ClipToView, float4(0.0, 0.0, 1.0 / Farthest, 1.0));
        Near = asuint(abs(NearView.z / NearView.w));
        Far = asuint(abs(FarView.z / FarView.w));
    }

    Near = WaveActiveMin(Near);
    Far = WaveActiveMax(Far);
    uint Wave = Index / WaveGetLaneCount();
    if (WaveIsFirstLane())
    {
        GroupNear[Wave] = Near;
        GroupFar[Wave] = Far;
    }

    GroupMemoryBarrierWithGroupSync();
    if (Index == 0)
    {
        for (uint Slot = 1; Slot < (256 + WaveGetLaneCount() - 1) / WaveGetLaneCount(); ++Slot)
        {
            Near = min(Near, GroupNear[Slot]);
            Far = max(Far, GroupFar[Slot]);
        }

        if (Far != 0u)
        {
            InterlockedMin(Bounds[0], Near);
            InterlockedMax(Bounds[1], Far);
        }
    }
}
