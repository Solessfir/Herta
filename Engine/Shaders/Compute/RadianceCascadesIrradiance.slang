#define GiBufferBase 385
#include "RadianceCascades.slangh"

// Integrates each cascade 0 probe's merged radiance against a cosine lobe into a 6x6 octahedral irradiance tile, with a border that repeats
// the texels across each edge so bilinear reads never mix in a neighbouring tile.
[[vk::binding(384, 0)]] [format("rgba16f")] RWTexture2D<float4> Irradiance;

groupshared float3 Radiance[32];

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 Group : SV_GroupID, uint3 Local : SV_GroupThreadID)
{
    uint Probe = Group.x + Group.y * 1024u;
    if (Probe >= min(Counters[CounterProbes], ProbeCapacity[0]))
        return;

    if (Local.x < 32u)
    {
        uint Record = Probe * 32u + Local.x;
        Radiance[Local.x] = LoadRadiance(Merged[Record]);
    }

    GroupMemoryBarrierWithGroupSync();
    int2 Texel = int2(Local.x % IrradianceTile, Local.x / IrradianceTile) - 1;
    int Last = int(IrradianceTexels) - 1;
    bool2 Outside = Texel < 0 || Texel > Last;
    // Border rows mirror horizontally, border columns vertically, and corners take the opposite corner.
    int2 Source = Texel;
    if (all(Outside))
        Source = int2(Texel.x < 0 ? Last : 0, Texel.y < 0 ? Last : 0);
    else if (Outside.y)
        Source = int2(Last - Texel.x, clamp(Texel.y, 0, Last));
    else if (Outside.x)
        Source = int2(clamp(Texel.x, 0, Last), Last - Texel.y);

    float3 Normal = OctahedralDecode((float2(Source) + 0.5) / float(IrradianceTexels));
    float3 Sum = 0.0;
    for (uint Direction = 0; Direction < 32u; ++Direction)
        Sum += Radiance[Direction] * saturate(dot(Normal, DecodeDirection(DirectionCenter(0, Direction))));

    // Each of the 32 equal-area directions stands for 4 pi / 32 steradians.
    Irradiance[int2(IrradianceTileOrigin(Probe)) + int2(Local.x % IrradianceTile, Local.x / IrradianceTile)] = float4(Sum * (4.0 * PI / 32.0 * StoredScale), 1.0);
}
