#include "Fullscreen.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> Metering;

// One pixel per histogram bin. A single thread scattering into a local array spills it to local memory and serializes every cell, so each bin sums its own cells instead.
// Every pixel reads the same cell at once, so the loads broadcast.
[shader("fragment")]
float4 fragmentMain(FFullscreenOutput Input) : SV_Target
{
    int Bin = int(Input.Position.x);
    float Weight = 0.0;
    for (int Y = 0; Y < ExposureMeteringSize; ++Y)
    {
        for (int X = 0; X < ExposureMeteringSize; ++X)
        {
            float2 Cell = Metering.Load(int3(X, Y, 0)).rg;
            Weight += ExposureHistogramBin(Cell.x / max(Cell.y, 1e-6)) == Bin ? Cell.y : 0.0;
        }
    }

    return float4(Weight, 0.0, 0.0, 1.0);
}
