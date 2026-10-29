#define GiBufferBase 384
#include "RadianceCascades.slangh"

// The probe stages of Split Radiance Cascades, one per dispatch: Visual.Material.BaseColor.x selects the stage and y the cascade.
[[vk::binding(0, 0)]] Texture2D<float> SceneDepth;

static const uint StageClear = 0;
static const uint StageInsertPixels = 1;
static const uint StageCompact = 2;
static const uint StageInsertParents = 3;
static const uint StageLinks = 4;
static const uint StageMerge = 5;
static const uint StageResetRecords = 6;
static const uint StageResolveCache = 7;

// Radiance with alpha 1 where known.
groupshared float4 MergedShared[64];
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

// The higher cascade's merged radiance in Direction around a probe, sparse trilinear over the neighbours that exist and know it; alpha 0 when none do.
float4 InterpolateUpper(uint Cascade, uint GroupProbe, uint Direction)
{
    uint Upper = Cascade + 1u;
    float3 Sum = 0.0;
    float Weight = 0.0;
    for (uint Corner = 0; Corner < 8; ++Corner)
    {
        uint Probe = NeighbourProbes[GroupProbe][Corner];
        if (Probe == MissingProbe)
            continue;

        uint Word = Merged[MergedIndex(Upper, Probe, Direction)];
        if (Word == UnknownRadiance)
            continue;

        float W = NeighbourWeights[GroupProbe][Corner];
        Sum += LoadRadiance(Word) * W;
        Weight += W;
    }

    return Weight > 0.0 ? float4(Sum / Weight, 1.0) : 0.0;
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

        if (Thread < ProbeCapacity[0] * 2u + CacheCapacity * CacheFloats)
            Deposits[HistoryBase + Thread] = 0.0;
        if (Thread < CacheCapacity * 2u)
            CacheKeys[Thread] = 0u;
        return;
    }

    // Folds each hit cache cell's samples from this frame into its average, which new cells take evenly over their first frames.
    if (Stage == StageResolveCache)
    {
        if (Thread >= CacheCapacity || CacheKeys[Thread * 2u] == 0u)
            return;

        uint Base = CacheBase + Thread * CacheFloats;
        if (uint(Visual.Material.BaseColor.z) - CacheKeys[Thread * 2u + 1u] > CacheLifetime)
        {
            CacheKeys[Thread * 2u] = 0u;
            for (uint Word = 0; Word < CacheFloats; ++Word)
                Deposits[Base + Word] = 0.0;
            return;
        }

        float Count = Deposits[Base + 3u];
        if (Count <= 0.0)
            return;

        float3 Mean = float3(Deposits[Base], Deposits[Base + 1u], Deposits[Base + 2u]) / Count;
        float History = Deposits[Base + 7u];
        float Blend = max(1.0 / (History + 1.0), CacheBlend);
        Deposits[Base + 4u] = lerp(Deposits[Base + 4u], Mean.r, Blend);
        Deposits[Base + 5u] = lerp(Deposits[Base + 5u], Mean.g, Blend);
        Deposits[Base + 6u] = lerp(Deposits[Base + 6u], Mean.b, Blend);
        Deposits[Base + 7u] = min(History + 1.0, 1.0 / CacheBlend);
        for (uint Word = 0; Word < 4u; ++Word)
            Deposits[Base + Word] = 0.0;
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
        float4 Radiance = 0.0;
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

            uint Transparent = Counts >> 16;
            if (Total == 0u)
            {
                // No ray sampled this direction here: the cascade above may know it, and otherwise it stays unknown.
                Radiance = bTop ? 0.0 : InterpolateUpper(Cascade, GroupProbe, Direction);
            }
            else if (bTop || Transparent == 0u)
            {
                Radiance = float4(Interval / float(Total), 1.0);
            }
            else
            {
                float4 Upper = InterpolateUpper(Cascade, GroupProbe, Direction);
                // Without the cascade above, the rays that ended here are the best estimate of the direction.
                Radiance = Upper.a > 0.0 ? float4(Interval / float(Total) + Upper.rgb * (float(Transparent) / float(Total)), 1.0) : (Transparent < Total ? float4(Interval / float(Total - Transparent), 1.0) : 0.0);
            }
        }

        if (Cascade == 0)
        {
            if (bActive)
            {
                Merged[MergedIndex(0, Probe, Direction)] = Radiance.a > 0.0 ? StoreRadiance(Radiance.rgb) : UnknownRadiance;
            }
            return;
        }

        // Higher cascades only ever feed the cascade below, so they store the average of each group of four directions that are known.
        MergedShared[Local.x] = Radiance;
        GroupMemoryBarrierWithGroupSync();

        if (bActive && (Direction & 3u) == 0u)
        {
            float4 Sum = MergedShared[Local.x] + MergedShared[Local.x + 1u] + MergedShared[Local.x + 2u] + MergedShared[Local.x + 3u];
            Merged[MergedIndex(Cascade, Probe, Direction / 4u)] = Sum.a > 0.0 ? StoreRadiance(Sum.rgb / Sum.a) : UnknownRadiance;
        }
    }
}
