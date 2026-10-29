#define GiBufferBase 384
#include "RayTracing.slangh"
#include "RadianceCascades.slangh"

// Ray splitting: one ray per 2x2 pixels from the surface under it, deposited into every cascade interval it crosses at that pixel's nearest probes.
[[vk::binding(3, 0)]] Texture2D<float> SceneDepth;

// R2 (Roberts 2018) in 32-bit fixed point, so ray indices in the millions keep their spacing.
float2 R2(uint Index)
{
    return float2(uint2(Index * 3242174889u, Index * 2447445414u)) * (1.0 / 4294967296.0) + 0.5;
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

    uint Sequence;
    InterlockedAdd(Slots[(SlotBase[0] + Slot) * 4u + 3u], 1u, Sequence);
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
        Radiance = Point.Material.Emissive + SampleLights(Point, Surface, V, SpecularProbability(Surface), false, Random, WeightSum);
    }
    else
    {
        Radiance = EnvironmentRadiance(Direction);
    }

    if (any(isnan(Radiance)) || any(isinf(Radiance)))
        Radiance = 0.0;

    float Distance = bHit ? Hit.Distance : 1e30;
    float2 UV = EncodeDirection(Direction);
    uint Probe = SlotProbe(0, Slot);
    for (uint Cascade = 0; Cascade < CascadeCount; ++Cascade)
    {
        bool bEnds = Distance <= IntervalEnd(Level, Cascade);
        if (Probe != MissingProbe)
        {
            uint Record = RecordIndex(Cascade, Probe, DirectionIndex(Cascade, UV));
            if (bEnds)
            {
                float Previous;
                InterlockedAdd(Deposits[Record * 3u], Radiance.r, Previous);
                InterlockedAdd(Deposits[Record * 3u + 1u], Radiance.g, Previous);
                InterlockedAdd(Deposits[Record * 3u + 2u], Radiance.b, Previous);
            }

            InterlockedAdd(RecordCounts[Record], bEnds ? 1u : 0x10001u);
        }

        if (bEnds || Probe == MissingProbe || Cascade + 1u == CascadeCount)
            break;

        Probe = Probes[(ProbeBase[Cascade] + Probe) * 3u + 2u];
    }
}
