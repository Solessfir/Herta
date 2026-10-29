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
        uint Existing = PreviousSlots[(SlotBase[0] + Slot) * 4u];
        if (Existing == Key)
            return PreviousSlots[(SlotBase[0] + Slot) * 4u + 1u];
        if (Existing == 0u)
            return MissingProbe;
        Slot = (Slot + 1u) & Mask;
    }

    return MissingProbe;
}

// Irradiance at a hit from last frame's probes around it, as the gather computes it on screen; zero where none were visible.
float3 BouncedIrradiance(float3 Position, float3 Normal)
{
    if (Visual.Material.Emissive.w < 0.5)
        return 0.0;

    float3 Eye = Visual.Material.Emissive.xyz;
    uint Level = LevelOfDetailFrom(Eye, Position);
    float Spacing = BaseSpacing(Level);
    int3 Camera = CameraCellFrom(Eye, Level);
    float3 Grid = Position / Spacing - 0.5;
    int3 Base = int3(floor(Grid));
    float3 Fraction = Grid - float3(Base);
    uint AtlasWidth, AtlasHeight;
    PreviousIrradiance.GetDimensions(AtlasWidth, AtlasHeight);
    float2 Octahedral = OctahedralEncode(Normal) * float(IrradianceTexels) + 1.0;
    float3 Sum = 0.0;
    float Weight = 0.0;
    for (uint Corner = 0; Corner < 8; ++Corner)
    {
        int3 Offset = int3(Corner & 1u, (Corner >> 1) & 1u, Corner >> 2);
        int3 Cell = Base + Offset;
        uint Probe = PreviousProbe(MakeKeyFrom(Camera, Level, 0, Cell));
        float3 Center = (float3(Cell) + 0.5) * Spacing;
        if (Probe == MissingProbe || dot(Center - Position, Normal) < -0.5 * Spacing)
            continue;

        float3 Axis = lerp(1.0 - Fraction, Fraction, float3(Offset));
        float W = Axis.x * Axis.y * Axis.z;
        float2 UV = (float2(IrradianceTileOrigin(Probe)) + Octahedral) / float2(AtlasWidth, AtlasHeight);
        Sum += PreviousIrradiance.SampleLevel(LinearSampler, UV, 0).rgb * W;
        Weight += W;
    }

    return Weight > 1e-4 ? Sum / (Weight * StoredScale) : 0.0;
}

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
        Radiance = Point.Material.Emissive + SampleLights(Point, Surface, V, SpecularProbability(Surface), false, Random, WeightSum) + Surface.DiffuseReflectance / PI * BouncedIrradiance(Point.Position, Point.GeometricNormal);
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
