#include "GtaoShared.slangh"
[[vk::binding(0, 0)]] Texture2D<float> ViewDepth;
[[vk::binding(128, 0)]] SamplerState PointClamp;
[[vk::binding(384, 0)]] [format("rgba8")] RWTexture2D<float4> AmbientOcclusion;

// Sectors per slice. Each covers an equal share of the slice's cosine-weighted visibility.
static const uint SectorCount = 32;

// Occlusion runs at half resolution, on view depth mip 1.
float LoadViewDepth(int2 Pixel)
{
    return ViewDepth.Load(int3(clamp(Pixel, int2(0, 0), int2(Gtao.HalfExtent.xy) - 1), 1));
}

// Picks the neighbor on each axis with the smaller depth step, so normals stay flat up to silhouettes.
float3 ReconstructNormal(int2 Pixel, float2 UV, float3 Position)
{
    float3 Left = GtaoViewPosition(UV - float2(Gtao.HalfExtent.z, 0.0), LoadViewDepth(Pixel - int2(1, 0))) - Position;
    float3 Right = GtaoViewPosition(UV + float2(Gtao.HalfExtent.z, 0.0), LoadViewDepth(Pixel + int2(1, 0))) - Position;
    float3 Up = GtaoViewPosition(UV - float2(0.0, Gtao.HalfExtent.w), LoadViewDepth(Pixel - int2(0, 1))) - Position;
    float3 Down = GtaoViewPosition(UV + float2(0.0, Gtao.HalfExtent.w), LoadViewDepth(Pixel + int2(0, 1))) - Position;
    float3 X = abs(Left.z) < abs(Right.z) ? -Left : Right;
    float3 Y = abs(Up.z) < abs(Down.z) ? -Up : Down;
    float3 Normal = normalize(cross(X, Y));
    return dot(Normal, Position) > 0.0 ? -Normal : Normal;
}

// Lagarde's polynomial, within 0.0002 rad of acos and far cheaper.
float FastAcos(float X)
{
    float Result = (-0.156583 * abs(X) + GtaoHalfPi) * sqrt(1.0 - abs(X));
    return X >= 0.0 ? Result : GtaoPi - Result;
}

float2 FastAcos(float2 X)
{
    return float2(FastAcos(X.x), FastAcos(X.y));
}

// A 4x4 ordered pattern, which the denoiser's 4x4 window averages out exactly.
float2 SpatialNoise(uint2 Pixel)
{
    static const uint Bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    uint Index = Bayer[(Pixel.y & 3) * 4 + (Pixel.x & 3)];
    return (float2(Index, (Index * 7 + 5) & 15) + 0.5) / 16.0;
}

// GTAO with a visibility bitmask (Therrien et al. 2023): each sample occludes the sectors between its front face and a surface of finite thickness behind it, so light passes behind thin geometry.
[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID)
{
    int2 Pixel = int2(Id.xy);
    if (any(Id.xy >= uint2(Gtao.HalfExtent.xy)))
        return;

    float ViewZ = LoadViewDepth(Pixel);
    float Radius = Gtao.Settings.x;
    float PixelsPerMeter = 0.5 * Gtao.HalfExtent.y / (abs(Gtao.NdcToView.y) * ViewZ);
    float ScreenRadius = min(Radius * PixelsPerMeter, 128.0);
    if (ViewZ >= GtaoFarDepth || ScreenRadius < 1.0)
    {
        AmbientOcclusion[Pixel] = float4(0.5, 0.5, 0.0, 1.0);
        return;
    }

    float2 UV = (float2(Pixel) + 0.5) * Gtao.HalfExtent.zw;
    float3 Position = GtaoViewPosition(UV, ViewZ);
    float3 View = normalize(-Position);
    float3 Normal = ReconstructNormal(Pixel, UV, Position);
    float Thickness = Gtao.Settings.y;
    uint SliceCount = uint(Gtao.Settings.z);
    uint StepCount = uint(Gtao.Settings.w);
    float2 Noise = SpatialNoise(Id.xy);
    float2 AxisSigns = float2(sign(Gtao.NdcToView.x), -sign(Gtao.NdcToView.y));

    float Visibility = 0.0;
    float WeightSum = 0.0;
    float3 Bent = 0.0;
    for (uint Slice = 0; Slice < SliceCount; ++Slice)
    {
        float Phi = (Slice + Noise.x) * GtaoPi / SliceCount;
        // Screen direction in pixels with y down, and the view-space direction it moves along.
        float2 Omega = float2(cos(Phi), sin(Phi));
        float3 Direction = float3(Omega * AxisSigns, 0.0);
        float3 Ortho = normalize(Direction - dot(Direction, View) * View);
        float3 Axis = cross(Ortho, View);
        float3 Projected = Normal - Axis * dot(Normal, Axis);
        float ProjectedLength = length(Projected);
        float CosN = saturate(dot(Projected, View) / max(ProjectedLength, 1e-6));
        float NormalAngle = sign(dot(Ortho, Projected)) * FastAcos(CosN);

        uint Occluded = 0;
        for (uint SideIndex = 0; SideIndex < 2; ++SideIndex)
        {
            float Side = SideIndex == 0 ? 1.0 : -1.0;
            for (uint Step = 0; Step < StepCount; ++Step)
            {
                // Quadratic spacing concentrates samples near the pixel, where occlusion changes fastest.
                float S = (Step + Noise.y) / StepCount;
                float Distance = 1.0 + S * S * (ScreenRadius - 1.0);
                float2 SampleUV = UV + Side * Omega * Distance * Gtao.HalfExtent.zw;
                if (any(SampleUV < 0.0) || any(SampleUV > 1.0))
                    break;

                // XeGTAO's mip offset of 3.3, less one for the half-resolution pixels.
                float Mip = clamp(log2(Distance) - 2.3, 1.0, float(GtaoDepthMips - 1));
                float3 Delta = GtaoViewPosition(SampleUV, ViewDepth.SampleLevel(PointClamp, SampleUV, Mip)) - Position;
                if (dot(Delta, Delta) > Radius * Radius)
                    continue;

                float2 Horizons = Side * FastAcos(clamp(float2(dot(normalize(Delta), View), dot(normalize(Delta - View * Thickness), View)), -1.0, 1.0));
                float2 Sectors = saturate(0.5 + 0.5 * sin(clamp(Horizons - NormalAngle, -GtaoHalfPi, GtaoHalfPi)));
                uint First = uint(min(Sectors.x, Sectors.y) * SectorCount);
                uint Count = uint(ceil(max(Sectors.x, Sectors.y) * SectorCount)) - First;
                if (Count >= SectorCount)
                    Occluded = 0xFFFFFFFFu;
                else if (Count > 0)
                    Occluded |= ((1u << Count) - 1u) << First;
            }
        }

        Visibility += (1.0 - float(countbits(Occluded)) / SectorCount) * ProjectedLength;
        WeightSum += ProjectedLength;

        // The open sectors' summed direction. Sector k sits where the sine of its angle from the normal is (k + 0.5) / 16 - 1, so the sine sum is linear in
        // the open indices, which bit planes count exactly. Cosines use the mean of four sectors at each end, within 1% of the exact sum.
        uint Open = ~Occluded;
        float OpenCount = float(countbits(Open));
        float IndexSum = countbits(Open & 0xAAAAAAAAu) + 2.0 * countbits(Open & 0xCCCCCCCCu) + 4.0 * countbits(Open & 0xF0F0F0F0u) + 8.0 * countbits(Open & 0xFF00FF00u) + 16.0 * countbits(Open & 0xFFFF0000u);
        float SumSin = (IndexSum + 0.5 * OpenCount) / (SectorCount * 0.5) - OpenCount;
        float SumCos = 0.45793 * countbits(Open & 0xF000000Fu) + 0.77542 * countbits(Open & 0x0F0000F0u) + 0.92395 * countbits(Open & 0x00F00F00u) + 0.98965 * countbits(Open & 0x000FF000u);

        float CosA = cos(NormalAngle);
        float SinA = sin(NormalAngle);
        Bent += ProjectedLength * ((SumCos * CosA - SumSin * SinA) * View + (SumCos * SinA + SumSin * CosA) * Ortho);
    }

    float Occlusion = WeightSum > 0.0 ? Visibility / WeightSum : 1.0;
    float3 BentNormal = dot(Bent, Bent) > 1e-8 ? normalize(Bent) : Normal;
    AmbientOcclusion[Pixel] = float4(BentNormal * 0.5 + 0.5, Occlusion);
}
