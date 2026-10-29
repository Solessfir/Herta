#define GiBufferBase 384
#include "RayTracing.slangh"
#include "RadianceCascades.slangh"

// Ray splitting: one ray per 2x2 pixels from the surface under it, deposited into every cascade interval it crosses at that pixel's nearest probes.
[[vk::binding(3, 0)]] Texture2D<float> SceneDepth;
#include "RadianceCascadesHits.slangh"

// Marks a hit's cache cell as read and, while the cell has room for this frame's samples, adds the irradiance one cosine-weighted ray brings
// back. That ray's own hit counts the cache's light in turn, so bounces keep accumulating off screen as they do on screen.
void UpdateCache(uint Cell, FSurfacePoint Point, inout FRandom Random)
{
    CacheKeys[Cell * 2u + 1u] = uint(Visual.Material.BaseColor.z);
    uint Base = CacheBase + Cell * CacheFloats;
    if (Deposits[Base + 3u] >= CacheSamples)
        return;

    float U = Random.Next();
    float Phi = 2.0 * PI * Random.Next();
    float3 Direction = normalize(TangentToWorld(float3(sqrt(U) * cos(Phi), sqrt(U) * sin(Phi), sqrt(1.0 - U)), Point.GeometricNormal));
    FHit Hit;
    float3 Radiance;
    uint FarCell;
    if (TraceScene(Point.Position + Point.GeometricNormal * RayOffset, Direction, 1e30, Hit))
        Radiance = HitRadiance(ShadeHit(Hit, Direction, 0.25 * Hit.Distance), -Direction, false, Random, FarCell);
    else
        Radiance = EnvironmentRadiance(Direction);

    if (any(isnan(Radiance)) || any(isinf(Radiance)))
        return;

    // Cosine-weighted directions make pi times the radiance an estimate of irradiance.
    float Previous;
    InterlockedAdd(Deposits[Base], Radiance.r * PI, Previous);
    InterlockedAdd(Deposits[Base + 1u], Radiance.g * PI, Previous);
    InterlockedAdd(Deposits[Base + 2u], Radiance.b * PI, Previous);
    InterlockedAdd(Deposits[Base + 3u], 1.0, Previous);
}

// R2 (Roberts 2018) in 32-bit fixed point, so ray indices in the millions keep their spacing.
float2 R2(uint Index)
{
    return float2(uint2(Index * 3242174889u, Index * 2447445414u)) * (1.0 / 4294967296.0) + 0.5;
}

// Splits a ray into the intervals of a cascade 0 probe and its ancestors: unoccluded through those it passes, and Radiance where it ends.
void Deposit(uint Probe, uint Level, float3 Direction, float Distance, float3 Radiance)
{
    if (Probe == MissingProbe)
        return;

    float2 UV = EncodeDirection(Direction);
    for (uint Cascade = 0; Cascade < CascadeCount; ++Cascade)
    {
        bool bEnds = Distance <= IntervalEnd(Level, Cascade);
        uint Record = RecordIndex(Cascade, Probe, DirectionIndex(Cascade, UV));
        if (bEnds)
        {
            float Previous;
            InterlockedAdd(Deposits[Record * 3u], Radiance.r, Previous);
            InterlockedAdd(Deposits[Record * 3u + 1u], Radiance.g, Previous);
            InterlockedAdd(Deposits[Record * 3u + 2u], Radiance.b, Previous);
        }

        InterlockedAdd(RecordCounts[Record], bEnds ? 1u : 0x10001u);
        if (bEnds || Cascade + 1u == CascadeCount)
            return;

        Probe = Probes[(ProbeBase[Cascade] + Probe) * 3u + 2u];
        if (Probe == MissingProbe)
            return;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID)
{
    int2 Pixel = int2(Id.xy * 2u + uint2(Visual.Material.Surface.zw));
    float3 Position, Normal;
    if (any(Pixel >= int2(Visual.Viewport.zw)) || !SurfaceAt(SceneDepth, Pixel, Position, Normal))
        return;

    uint Level = LevelOfDetail(Position);
    int3 Cell = int3(floor(Position / BaseSpacing(Level)));
    uint Slot = FindSlot(0, MakeKey(Level, 0, Cell));
    if (Slot == MissingProbe || SlotProbe(0, Slot) == MissingProbe)
        return;

    // The direction depends on where the surface lies: its probe picks a stretch of the sequence, and its place among 8x8x8 cells inside the probe the point in it.
    // Order-dependent schemes, such as counting rays with atomics, would reshuffle directions between neighbours every frame. The whole sequence then
    // rotates by a per-frame offset, which irradiance accumulates over frames.
    uint3 Inside = min(uint3(frac(Position / BaseSpacing(Level)) * 8.0), 7u);
    uint Sequence = HashKey(MakeKey(Level, 0, Cell)) * 512u + Inside.x + Inside.y * 8u + Inside.z * 64u;
    float3 Direction = DecodeDirection(frac(R2(Sequence) + Visual.Material.Surface.xy));
    if (dot(Direction, Normal) < 0.0)
        Direction = -Direction;

    // The offset clears the depth-derived surface, which sits a fraction of a texel off the true one.
    float3 Origin = Position + Normal * (0.02 * BaseSpacing(Level));
    FHit Hit;
    bool bHit = TraceScene(Origin, Direction, 1e30, Hit);
    float3 Radiance;
    if (bHit)
    {
        FRandom Random;
        Random.State = PcgHash(Id.x + PcgHash(Id.y + PcgHash(uint(Visual.Material.BaseColor.z))));
        // Hit textures only tint bounce light, so a wide cone keeps their reads on coarse mips.
        FSurfacePoint Point = ShadeHit(Hit, Direction, 0.25 * Hit.Distance);
        uint Cell;
        Radiance = HitRadiance(Point, -Direction, true, Random, Cell);
        if (Cell != MissingProbe)
            UpdateCache(Cell, Point, Random);
    }
    else
    {
        Radiance = EnvironmentRadiance(Direction);
    }

    if (any(isnan(Radiance)) || any(isinf(Radiance)))
        Radiance = 0.0;

    // Surfaces in a level overlap feed the finer level's probes too, which their gather blends in.
    float Distance = bHit ? Hit.Distance : 1e30;
    Deposit(SlotProbe(0, Slot), Level, Direction, Distance, Radiance);
    if (LevelBlend(Position, Level) < 1.0)
        Deposit(FindProbe(0, MakeKey(Level - 1u, 0, int3(floor(Position / BaseSpacing(Level - 1u))))), Level - 1u, Direction, Distance, Radiance);
}
