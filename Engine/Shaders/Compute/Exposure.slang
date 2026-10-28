#include "../VisualShared.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> SceneColor;
[[vk::binding(1, 0)]] Texture2D<float4> FogVolume;
[[vk::binding(2, 0)]] Texture2D<float4> Previous;
[[vk::binding(384, 0)]] [format("rgba32f")] RWTexture2D<float4> Adapted;

static const uint ExposureThreads = 256;
static const uint CellsPerThread = ExposureMeteringSize * ExposureMeteringSize / ExposureThreads;
// Cell weights run from 1 to 3; fixed point keeps the shared histogram on integer atomics.
static const float WeightScale = 4096.0;

groupshared uint Histogram[ExposureHistogramBinCount];

// Meters a 32x32 grid of the HDR scene, histograms it, and eases the exposure towards it, all in one group so no intermediate leaves the SM.
[shader("compute")]
[numthreads(256, 1, 1)]
void computeMain(uint Thread : SV_GroupIndex)
{
    if (Thread < ExposureHistogramBinCount)
    {
        Histogram[Thread] = 0;
    }

    GroupMemoryBarrierWithGroupSync();

    for (uint Index = 0; Index < CellsPerThread; ++Index)
    {
        uint Cell = Thread + Index * ExposureThreads;
        float2 CellUV = (float2(Cell % ExposureMeteringSize, Cell / ExposureMeteringSize) + 0.5) / float(ExposureMeteringSize);
        // Four bilinear taps per cell average its 2x2 quarter-cell corners, centre-weighted like a camera's average metering.
        const float Offset = 0.25 / float(ExposureMeteringSize);
        float LogSum = 0.0;
        for (int Tap = 0; Tap < 4; ++Tap)
        {
            float2 UV = CellUV + float2((Tap & 1) != 0 ? Offset : -Offset, (Tap & 2) != 0 ? Offset : -Offset);
            float3 Color = SceneColor.SampleLevel(LinearSampler, UV, 0).rgb;
            // Tone mapping composites fog at full resolution; a bilinear tap is enough for metering.
            if (Visual.Fog.x > 0.0)
            {
                float4 Fog = FogVolume.SampleLevel(LinearSampler, UV, 0);
                Color = Color * Fog.a + Fog.rgb;
            }

            LogSum += log2(clamp(dot(Color, float3(0.2126, 0.7152, 0.0722)), 1e-4, 65504.0));
        }

        float2 Center = CellUV * 2.0 - 1.0;
        float Weight = 1.0 + 2.0 * saturate(1.0 - dot(Center, Center));
        InterlockedAdd(Histogram[ExposureHistogramBin(LogSum * 0.25)], uint(Weight * WeightScale + 0.5));
    }

    GroupMemoryBarrierWithGroupSync();
    if (Thread != 0)
    {
        return;
    }

    float Total = 0.0;
    for (int Bin = 0; Bin < ExposureHistogramBinCount; ++Bin)
    {
        Total += float(Histogram[Bin]);
    }

    // Averaging the 10th to 90th weighted percentile keeps a few highlights or deep shadows from swinging the exposure.
    float Low = Total * 0.1;
    float High = Total * 0.9;
    float Below = 0.0;
    float Sum = 0.0;
    float Weight = 0.0;
    for (int Bin = 0; Bin < ExposureHistogramBinCount; ++Bin)
    {
        float Count = float(Histogram[Bin]);
        float Overlap = max(0.0, min(Below + Count, High) - max(Below, Low));
        Sum += Overlap * (ExposureMinimumLog + (float(Bin) + 0.5) * (ExposureMaximumLog - ExposureMinimumLog) / float(ExposureHistogramBinCount));
        Weight += Overlap;
        Below += Count;
    }

    // Reflected-light metering puts the average at middle grey: log2(L * S / K) with ISO 100 and K = 12.5, so EV100 = log2(L) + 3.
    // Like Unreal's default auto-exposure bias, one stop brighter keeps sunlit scenes with bright sky near "sunny 16" instead of underexposed.
    // EV100 1, a night street, is the darkest the camera adapts to.
    float Target = clamp(Sum / max(Weight, 1e-6) + 2.0 - Visual.Exposure.y, 1.0, 18.0);
    float4 Last = Previous.Load(int3(0, 0, 0));
    if (Visual.Exposure.w > 0.5 || Last.a < 0.5 || !isfinite(Last.r))
    {
        Adapted[uint2(0, 0)] = float4(Target, 0.0, 0.0, 1.0);
        return;
    }

    // Eyes adapt to brighter scenes faster than to darker ones.
    float Rate = Target > Last.r ? 3.0 : 1.5;
    Adapted[uint2(0, 0)] = float4(lerp(Last.r, Target, 1.0 - exp(-Visual.Exposure.z * Rate)), 0.0, 0.0, 1.0);
}
