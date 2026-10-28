#include "../VisualShared.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> Scattering;
[[vk::binding(1, 0)]] Texture2D<float> FogDepth;
[[vk::binding(384, 0)]] [format("rgba16f")] RWTexture2D<float4> FogVolume;

// Averages the ray-march dither with a 3x3 Gaussian at fog resolution, where it costs a sixteenth of doing it per screen pixel.
// Depth similarity keeps fog from bleeding across silhouettes, and the composite only has to upsample.
[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID)
{
    uint Width, Height;
    FogVolume.GetDimensions(Width, Height);
    if (any(Id.xy >= uint2(Width, Height)))
        return;

    int2 Texel = int2(Id.xy);
    int2 Last = int2(Width, Height) - 1;
    float Depth = FogDepth.Load(int3(Texel, 0));
    float DepthFalloff = -1.0 / max(0.0001, Depth * 0.1);
    float4 Sum = 0.0;
    float Total = 0.0;
    for (int Y = -1; Y <= 1; ++Y)
    {
        for (int X = -1; X <= 1; ++X)
        {
            int2 Tap = clamp(Texel + int2(X, Y), int2(0, 0), Last);
            // exp(-0.5) for each axis off centre.
            float Weight = (X == 0 ? 1.0 : 0.60653066) * (Y == 0 ? 1.0 : 0.60653066) * exp(abs(Depth - FogDepth.Load(int3(Tap, 0))) * DepthFalloff);
            Sum += Scattering.Load(int3(Tap, 0)) * Weight;
            Total += Weight;
        }
    }

    // The centre always weighs 1.
    FogVolume[Texel] = Sum / Total;
}
