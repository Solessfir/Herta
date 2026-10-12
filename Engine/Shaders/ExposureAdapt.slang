#include "Fullscreen.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> Histogram;
[[vk::binding(1, 0)]] Texture2D<float4> Previous;

// Reduces the metering histogram and eases the exposure towards it. Red holds EV100; alpha marks a valid value.
[shader("fragment")]
float4 fragmentMain(FFullscreenOutput Input) : SV_Target
{
    float Total = 0.0;
    for (int Bin = 0; Bin < ExposureHistogramBinCount; ++Bin)
    {
        Total += Histogram.Load(int3(Bin, 0, 0)).r;
    }

    // Averaging the 10th to 90th weighted percentile keeps a few highlights or deep shadows from swinging the exposure.
    float Low = Total * 0.1;
    float High = Total * 0.9;
    float Below = 0.0;
    float Sum = 0.0;
    float Weight = 0.0;
    for (int Bin = 0; Bin < ExposureHistogramBinCount; ++Bin)
    {
        float Count = Histogram.Load(int3(Bin, 0, 0)).r;
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
        return float4(Target, 0.0, 0.0, 1.0);
    }

    // Eyes adapt to brighter scenes faster than to darker ones.
    float Rate = Target > Last.r ? 3.0 : 1.5;
    return float4(lerp(Last.r, Target, 1.0 - exp(-Visual.Exposure.z * Rate)), 0.0, 0.0, 1.0);
}
