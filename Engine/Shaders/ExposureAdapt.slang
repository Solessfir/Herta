#include "Fullscreen.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> Metering;
[[vk::binding(1, 0)]] Texture2D<float4> Previous;

static const int BinCount = 64;
static const float MinimumLog = -10.0;
static const float MaximumLog = 16.0;

// Reduces the metering grid and eases the exposure towards it. Red holds EV100; alpha marks a valid value.
[shader("fragment")]
float4 fragmentMain(FFullscreenOutput Input) : SV_Target
{
    float Histogram[BinCount];
    for (int Bin = 0; Bin < BinCount; ++Bin)
    {
        Histogram[Bin] = 0.0;
    }

    float Total = 0.0;
    for (int Y = 0; Y < 32; ++Y)
    {
        for (int X = 0; X < 32; ++X)
        {
            float2 Cell = Metering.Load(int3(X, Y, 0)).rg;
            float LogLuminance = Cell.x / max(Cell.y, 1e-6);
            int Bin = clamp(int((LogLuminance - MinimumLog) / (MaximumLog - MinimumLog) * BinCount), 0, BinCount - 1);
            Histogram[Bin] += Cell.y;
            Total += Cell.y;
        }
    }

    // Averaging the 10th to 90th weighted percentile keeps a few highlights or deep shadows from swinging the exposure.
    float Low = Total * 0.1;
    float High = Total * 0.9;
    float Below = 0.0;
    float Sum = 0.0;
    float Weight = 0.0;
    for (int Bin = 0; Bin < BinCount; ++Bin)
    {
        float Overlap = max(0.0, min(Below + Histogram[Bin], High) - max(Below, Low));
        Sum += Overlap * (MinimumLog + (float(Bin) + 0.5) * (MaximumLog - MinimumLog) / float(BinCount));
        Weight += Overlap;
        Below += Histogram[Bin];
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
