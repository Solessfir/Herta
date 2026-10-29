#define GiBufferBase 385
#include "RadianceCascades.slangh"

// Integrates each cascade 0 probe's merged radiance against a cosine lobe into a 6x6 octahedral irradiance tile, with a border that repeats
// the texels across each edge so bilinear reads never mix in a neighbouring tile.
[[vk::binding(384, 0)]] [format("rgba16f")] RWTexture2D<float4> IrradianceAtlas;
// Last frame's tiles and hash slots. Each probe keeps a running average of its tile, because one frame's few rays per probe leave probe-sized blotches.
// Visual.Material.Emissive holds that frame's camera, with w 0 when there is none.
[[vk::binding(0, 0)]] Texture2D<float4> PreviousAtlas;
[[vk::binding(16, 0)]] StructuredBuffer<uint> PreviousSlots;

// New probes average their first frames evenly, then each frame replaces this share, which bounds how far a still view changes between frames.
// It keeps about 50 frames, so lighting changes settle in under a second at 60 Hz.
static const float HistoryBlend = 0.02;

// Radiance with alpha 1 where known.
groupshared float4 Radiance[32];

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 Group : SV_GroupID, uint3 Local : SV_GroupThreadID)
{
    uint Probe = Group.x + Group.y * 1024u;
    if (Probe >= min(Counters[CounterProbes], ProbeCapacity[0]))
        return;

    uint PreviousProbe = MissingProbe;
    if (Visual.Material.Emissive.w > 0.5)
    {
        uint Key = Probes[(ProbeBase[0] + Probe) * 3u];
        PreviousProbe = FindPreviousProbe(PreviousSlots, Visual.Material.Emissive.xyz, KeyLevel(Key), KeyCell(Key, 0));
    }

    uint Parity = uint(Visual.Material.BaseColor.z) & 1u;
    float History = PreviousProbe == MissingProbe ? 0.0 : Deposits[HistoryBase + (Parity ^ 1u) * ProbeCapacity[0] + PreviousProbe];
    if (Local.x == 0u)
        Deposits[HistoryBase + Parity * ProbeCapacity[0] + Probe] = min(History + 1.0, 1.0 / HistoryBlend);

    if (Local.x < 32u)
    {
        uint Record = Probe * 32u + Local.x;
        uint Word = Merged[Record];
        Radiance[Local.x] = Word == UnknownRadiance ? 0.0 : float4(LoadRadiance(Word), 1.0);
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
    // Known directions stand in for the unknown ones within the cosine lobe; alpha is the share they cover, so gathers can weigh the probe by it.
    float3 Sum = 0.0;
    float Known = 0.0;
    float All = 0.0;
    for (uint Direction = 0; Direction < 32u; ++Direction)
    {
        float Cosine = saturate(dot(Normal, DecodeDirection(DirectionCenter(0, Direction))));
        Sum += Radiance[Direction].rgb * Cosine;
        Known += Radiance[Direction].a * Cosine;
        All += Cosine;
    }

    // Each of the 32 equal-area directions stands for 4 pi / 32 steradians. Tiles store irradiance times the known share, so bilinear reads stay weighted.
    float Coverage = All > 0.0 ? Known / All : 0.0;
    float3 Irradiance = Known > 0.0 ? Sum * (All / Known) * (4.0 * PI / 32.0) : 0.0;
    float4 Tile = float4(Irradiance * (Coverage * StoredScale), Coverage);
    int2 Offset = int2(Local.x % IrradianceTile, Local.x / IrradianceTile);
    if (PreviousProbe != MissingProbe)
        Tile = lerp(PreviousAtlas[int2(IrradianceTileOrigin(PreviousProbe)) + Offset], Tile, 1.0 / (History + 1.0));
    IrradianceAtlas[int2(IrradianceTileOrigin(Probe)) + Offset] = Tile;
}
