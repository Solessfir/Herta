#define GiBufferBase 384
#include "RadianceCascades.slangh"

// The probe stages of Split Radiance Cascades, one per dispatch: Visual.Material.BaseColor.x selects the stage and y the cascade.
[[vk::binding(0, 0)]] Texture2D<float> SceneDepth;
[[vk::binding(1, 0)]] Texture2D<float4> Environment;

static const uint StageClear = 0;
static const uint StageInsertPixels = 1;
static const uint StageCompact = 2;
static const uint StageInsertParents = 3;
static const uint StageLinks = 4;
static const uint StageMerge = 5;
static const uint StageResetRecords = 6;

groupshared float3 MergedShared[64];
// The upper cascade's neighbours of the group's probes, found once per group instead of once per direction. Cascade 0 fits two probes in a group.
groupshared uint NeighbourProbes[2][8];
groupshared float NeighbourWeights[2][8];

// Merged radiance as each cascade stores it: cascade 0 per direction, higher ones averaged into the next lower cascade's directions.
uint MergedDirections(uint Cascade)
{
    return Cascade == 0 ? DirectionCount(0) : DirectionCount(Cascade) / 4u;
}

static const uint MergedBase[CascadeCount] = { 0u, 2097152u, 3145728u };

uint MergedIndex(uint Cascade, uint Probe, uint Direction)
{
    return MergedBase[Cascade] + Probe * MergedDirections(Cascade) + Direction;
}

uint ProbeCount(uint Cascade)
{
    return min(Counters[CounterProbes + Cascade], ProbeCapacity[Cascade]);
}

// One trilinear neighbour, one cascade up, of a probe: its index and weight, or MissingProbe.
void FindNeighbour(uint Cascade, uint Key, uint Corner, out uint Probe, out float Weight)
{
    uint Upper = Cascade + 1u;
    float3 Grid = float3(KeyCell(Key, Cascade)) * 0.5 - 0.25;
    int3 Base = int3(floor(Grid));
    float3 Fraction = Grid - float3(Base);
    int3 Offset = int3(Corner & 1u, (Corner >> 1) & 1u, Corner >> 2);
    Probe = FindProbe(Upper, MakeKey(KeyLevel(Key), Upper, Base + Offset));
    float3 Axis = lerp(1.0 - Fraction, Fraction, float3(Offset));
    Weight = Probe == MissingProbe ? 0.0 : Axis.x * Axis.y * Axis.z;
}

// The higher cascade's merged radiance in Direction around a probe, sparse trilinear over the neighbours that exist.
float3 InterpolateUpper(uint Cascade, uint GroupProbe, uint Direction)
{
    uint Upper = Cascade + 1u;
    float3 Sum = 0.0;
    float Weight = 0.0;
    for (uint Corner = 0; Corner < 8; ++Corner)
    {
        uint Probe = NeighbourProbes[GroupProbe][Corner];
        if (Probe == MissingProbe)
            continue;

        float W = NeighbourWeights[GroupProbe][Corner];
        Sum += LoadRadiance(Merged[MergedIndex(Upper, Probe, Direction)]) * W;
        Weight += W;
    }

    return Weight > 0.0 ? Sum / Weight : 0.0;
}

float3 SkyRadiance(float3 Direction)
{
    if (Visual.Sky.x <= 0.0)
        return 0.0;

    uint Width, Height;
    Environment.GetDimensions(Width, Height);
    float2 UV = DirectionUV(Direction);
    UV.y = clamp(UV.y, 0.5 / Height, 1.0 - 0.5 / Height);
    return Environment.SampleLevel(LinearSampler, UV, 0).rgb * Visual.Sky.x * Visual.SkyColor.rgb;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID, uint3 Local : SV_GroupThreadID)
{
    uint Stage = uint(Visual.Material.BaseColor.x);
    uint Cascade = uint(Visual.Material.BaseColor.y);
    // The pixel stage runs as rows of 64 across the view; the rest index probes, slots, or records directly.
    uint Thread = Id.x;

    if (Stage == StageClear)
    {
        if (Thread < (SlotBase[CascadeCount - 1] + SlotCapacity[CascadeCount - 1]) * 2u)
            Slots[Thread] = 0u;
        if (Thread < 8u)
            Counters[Thread] = 0u;
        return;
    }

    // Only new storage needs this; merging clears what it reads. Each thread takes four records to stay within the dispatch size limit.
    if (Stage == StageResetRecords)
    {
        for (uint Record = Thread * 4u; Record < min(Thread * 4u + 4u, RecordTotal); ++Record)
        {
            Deposits[Record * 3u] = 0.0;
            Deposits[Record * 3u + 1u] = 0.0;
            Deposits[Record * 3u + 2u] = 0.0;
            RecordCounts[Record] = 0u;
        }
        return;
    }

    // Only the pixel of each 2x2 block that traces a ray inserts a probe; probes are several pixels wide, and the gather's sparse trilinear covers the rest.
    if (Stage == StageInsertPixels)
    {
        uint Width = (uint(Visual.Viewport.z) + 1u) / 2u;
        int2 Pixel = int2(Thread % Width, Id.y) * 2 + int2(Visual.Material.Surface.zw);
        float3 Position, Normal;
        if (Thread >= Width || any(Pixel >= int2(Visual.Viewport.zw)) || !SurfaceAt(SceneDepth, Pixel, Position, Normal))
            return;

        uint Level = LevelOfDetail(Position);
        if (InsertKey(0, MakeKey(Level, 0, int3(floor(Position / BaseSpacing(Level))))) == MissingProbe)
            InterlockedAdd(Counters[CounterOverflow], 1u);
        if (LevelBlend(Position, Level) < 1.0 && InsertKey(0, MakeKey(Level - 1u, 0, int3(floor(Position / BaseSpacing(Level - 1u))))) == MissingProbe)
            InterlockedAdd(Counters[CounterOverflow], 1u);
        return;
    }

    if (Stage == StageCompact)
    {
        if (Thread >= SlotCapacity[Cascade])
            return;

        uint Index = (SlotBase[Cascade] + Thread) * 2u;
        uint Key = Slots[Index];
        if (Key == 0u)
            return;

        uint Probe;
        InterlockedAdd(Counters[CounterProbes + Cascade], 1u, Probe);
        if (Probe >= ProbeCapacity[Cascade])
        {
            Slots[Index + 1u] = MissingProbe;
            InterlockedAdd(Counters[CounterOverflow], 1u);
            return;
        }

        Slots[Index + 1u] = Probe;
        Probes[(ProbeBase[Cascade] + Probe) * 3u] = Key;
        Probes[(ProbeBase[Cascade] + Probe) * 3u + 1u] = Thread;
        return;
    }

    if (Stage == StageInsertParents)
    {
        // Cascade is the parent cascade; each probe of the one below inserts the parent it nests in.
        uint Child = Cascade - 1u;
        if (Thread >= ProbeCount(Child))
            return;

        uint Key = Probes[(ProbeBase[Child] + Thread) * 3u];
        if (InsertKey(Cascade, MakeKey(KeyLevel(Key), Cascade, FloorShift(KeyCell(Key, Child), 1u))) == MissingProbe)
            InterlockedAdd(Counters[CounterOverflow], 1u);
        return;
    }

    // Rays climb the cascades through these links instead of hashing at every level.
    if (Stage == StageLinks)
    {
        if (Thread >= ProbeCount(Cascade))
            return;

        uint Key = Probes[(ProbeBase[Cascade] + Thread) * 3u];
        Probes[(ProbeBase[Cascade] + Thread) * 3u + 2u] = FindProbe(Cascade + 1u, MakeKey(KeyLevel(Key), Cascade + 1u, FloorShift(KeyCell(Key, Cascade), 1u)));
        return;
    }

    // The top cascade's records outnumber one dimension's dispatch limit, so merging runs in rows of 32768 groups.
    if (Stage == StageMerge)
    {
        Thread = Id.x + Id.y * 32768u * 64u;
        uint Directions = DirectionCount(Cascade);
        uint Probe = Thread / Directions;
        uint Direction = Thread % Directions;
        bool bActive = Probe < ProbeCount(Cascade);
        bool bTop = Cascade + 1u == CascadeCount;
        uint GroupProbe = Directions < 64u ? Local.x / Directions : 0u;
        if (!bTop && Local.x < 16u && Local.x / 8u <= (Directions < 64u ? 64u / Directions - 1u : 0u))
        {
            uint Owner = (Thread - Local.x) / Directions + Local.x / 8u;
            uint Neighbour = MissingProbe;
            float Weight = 0.0;
            if (Owner < ProbeCount(Cascade))
                FindNeighbour(Cascade, Probes[(ProbeBase[Cascade] + Owner) * 3u], Local.x % 8u, Neighbour, Weight);
            NeighbourProbes[Local.x / 8u][Local.x % 8u] = Neighbour;
            NeighbourWeights[Local.x / 8u][Local.x % 8u] = Weight;
        }

        GroupMemoryBarrierWithGroupSync();
        float3 Radiance = 0.0;
        if (bActive)
        {
            uint Record = RecordIndex(Cascade, Probe, Direction);
            uint Counts = RecordCounts[Record];
            uint Total = Counts & 0xFFFFu;
            // Most directions of the higher cascades receive no ray, so only records that did are read and cleared for the next frame.
            float3 Interval = 0.0;
            if (Counts != 0u)
            {
                Interval = float3(Deposits[Record * 3u], Deposits[Record * 3u + 1u], Deposits[Record * 3u + 2u]);
                Deposits[Record * 3u] = 0.0;
                Deposits[Record * 3u + 1u] = 0.0;
                Deposits[Record * 3u + 2u] = 0.0;
                RecordCounts[Record] = 0u;
            }

            if (Total == 0u)
            {
                // No ray sampled this direction: the top cascade assumes open sky, lower ones pass the cascade above through.
                Radiance = bTop ? SkyRadiance(DecodeDirection(DirectionCenter(Cascade, Direction))) : InterpolateUpper(Cascade, GroupProbe, Direction);
            }
            else
            {
                float Transmittance = float(Counts >> 16) / float(Total);
                Radiance = Interval / float(Total) + (bTop || Transmittance <= 0.0 ? 0.0 : Transmittance * InterpolateUpper(Cascade, GroupProbe, Direction));
            }
        }

        if (Cascade == 0)
        {
            if (bActive)
            {
                Merged[MergedIndex(0, Probe, Direction)] = StoreRadiance(Radiance);
            }
            return;
        }

        // Higher cascades only ever feed the cascade below, so they store each group of four directions' average.
        MergedShared[Local.x] = Radiance;
        GroupMemoryBarrierWithGroupSync();
        if (bActive && (Direction & 3u) == 0u)
        {
            float3 Average = (MergedShared[Local.x] + MergedShared[Local.x + 1u] + MergedShared[Local.x + 2u] + MergedShared[Local.x + 3u]) * 0.25;
            Merged[MergedIndex(Cascade, Probe, Direction / 4u)] = StoreRadiance(Average);
        }
    }
}
