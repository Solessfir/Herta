#define GiBufferBase 384
#include "RayTracing.slangh"
#include "RadianceCascades.slangh"

// Ray splitting: one ray per 2x2 pixels from the surface under it, deposited into every cascade interval it crosses at that pixel's nearest probes.
[[vk::binding(3, 0)]] Texture2D<float> SceneDepth;
// The previous frame's cascade 0 probes, which give hits the light they received last frame: each frame adds a bounce, so light keeps bouncing.
// Visual.Material.Emissive holds that frame's camera, with w 0 when there is none.
[[vk::binding(4, 0)]] Texture2D<float4> PreviousIrradiance;
[[vk::binding(19, 0)]] StructuredBuffer<uint> PreviousSlots;

uint PreviousProbe(uint Key)
{
    if (Key == 0u)
        return MissingProbe;

    uint Mask = SlotCapacity[0] - 1u;
    uint Slot = HashKey(Key) & Mask;
    for (uint Step = 0; Step < ProbeSteps; ++Step)
    {
        uint Existing = PreviousSlots[(SlotBase[0] + Slot) * 2u];
        if (Existing == Key)
            return PreviousSlots[(SlotBase[0] + Slot) * 2u + 1u];
        if (Existing == 0u)
            return MissingProbe;
        Slot = (Slot + 1u) & Mask;
    }

    return MissingProbe;
}

// Irradiance at a hit from last frame's probe whose cell holds it; zero where there is none, such as surfaces off screen.
// Bounced light is smooth and the probes average it again, so one probe serves; searching the eight around an empty cell cost 0.1 to 0.45 ms of rays
// at 3200x1809 and moved the error against the path tracer by 0.2%.
float3 BouncedIrradiance(float3 Position, float3 Normal)
{
    if (Visual.Material.Emissive.w < 0.5)
        return 0.0;

    float3 Eye = Visual.Material.Emissive.xyz;
    uint Level = LevelOfDetailFrom(Eye, Position);
    uint Probe = PreviousProbe(MakeKeyFrom(CameraCellFrom(Eye, Level), Level, 0, int3(floor(Position / BaseSpacing(Level)))));
    if (Probe == MissingProbe)
        return 0.0;

    uint AtlasWidth, AtlasHeight;
    PreviousIrradiance.GetDimensions(AtlasWidth, AtlasHeight);
    float2 UV = (float2(IrradianceTileOrigin(Probe)) + OctahedralEncode(Normal) * float(IrradianceTexels) + 1.0) / float2(AtlasWidth, AtlasHeight);
    return PreviousIrradiance.SampleLevel(LinearSampler, UV, 0).rgb / StoredScale;
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

    // The direction depends only on where the surface lies: its probe picks a stretch of the sequence, and its place among 8x8x8 cells inside the probe the point in it.
    // Order-dependent schemes, such as counting rays with atomics, hand a surface new directions every frame, which flickers without accumulation.
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
        float3 V = -Direction;
        FSurface Surface = MakeSurface(SpecularAlbedo, Point.Material.Base.rgb, Point.Material.Metallic, Point.Material.Roughness, Point.Material.DiffuseRoughness, Point.Material.EdgeColor, saturate(dot(Point.Material.Normal, V)));
        float WeightSum;
        Radiance = Point.Material.Emissive + SampleLights(Point, Surface, V, SpecularProbability(Surface), false, Random, WeightSum) + Surface.DiffuseReflectance / PI * BouncedIrradiance(Point.Position, Point.GeometricNormal);
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
