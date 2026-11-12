#define FIELD_IRRADIANCE_BINDING 3
#define FIELD_DISTANCE_BINDING 4
#define FIELD_PROBES_BINDING 19
#define FIELD_CELLS_BINDING 385
#define FIELD_USED_BINDING 386
#define FIELD_REQUESTS_BINDING 387
#define FIELD_USED_BY 1
#define CASCADE_PARENT_BINDING 5
#include "RayTracing.slangh"
#include "RadianceCascades.slangh"
#include "IrradianceFieldHits.slangh"

// One level's shells: a column of groups per probe in the level's list, one thread per direction of its radiance tile. A ray that meets a
// surface inside the shell keeps the light leaving it; one that leaves the shell takes the level above's light for its direction, which is the
// mean of the four directions it covers there, from the probes around it whose shells continue it. Visual.Material.BaseColor.y is the level, and a
// non-zero w clears a new atlas instead, because the device never lets a texture be sampled before it is written.
[[vk::binding(20, 0)]] StructuredBuffer<uint> Lists;
// Each probe's cell, level, and update count; see FieldProbeLevel.
[[vk::binding(21, 0)]] StructuredBuffer<int4> ProbeCells;
[[vk::binding(384, 0)]] [format("rgba16f")] RWTexture2D<float4> Radiance;

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 Group : SV_GroupID, uint Thread : SV_GroupIndex)
{
    if (Visual.Material.BaseColor.w != 0.0)
    {
        Radiance[uint2(Group.x * 64 + Thread, Group.y)] = 0.0;
        return;
    }

    uint Level = uint(Visual.Material.BaseColor.y);
    uint Probe = Lists[CascadeListBase[Level] + Group.x];
    int3 Cell = ProbeCells[Probe].xyz;
    float3 Origin = CascadeProbePosition(Level, Cell, Probe);
    bool bTop = Level + 1 == FieldLevels;
    if (Thread == 0 && Group.y == 0 && !bTop)
        CascadeTouchParents(Level + 1, Origin);

    uint Texels = CascadeTexels(Level);
    uint Index = Group.y * 64 + Thread;
    int2 Texel = int2(Index % Texels, Index / Texels);
    float3 Direction = FieldOctahedralDecode((float2(Texel) + 0.5) / float(Texels));
    float Start = CascadeShellStart(Level);
    float End = CascadeShellEnd(Level);
    float4 Result;
    FHit Hit;
    if (TraceScene(Origin + Direction * Start, Direction, End - Start, Hit))
    {
        // Texture detail finer than the spacing between rays would only alias, so hits read mips about that wide.
        float Distance = Start + Hit.Distance;
        FSurfacePoint Point = ShadeHit(Hit, Direction, Distance * 3.5 / float(Texels));
        if (Point.bBackFace)
        {
            // The shell began inside geometry, behind a wall the rays that read it stopped at or never crossed, so it knows nothing here.
            Result = 0.0;
        }
        else
        {
            // Light sampling draws the same choices every frame.
            FRandom Random;
            Random.State = PcgHash(FieldProbeSeed(Level, Cell) + Index);
            Result = float4(FieldHitRadiance(Point, -Direction, Random) * StoredScale, 1.0);
        }
    }
    else if (bTop)
    {
        Result = float4(EnvironmentRadiance(Direction) * StoredScale, 1.0);
    }
    else
    {
        Result = CascadeMerge(Level + 1, Origin, Direction, End);
        if (Result.a <= 0.0)
            FieldUsed[CascadeRadianceFrame + Probe] = CascadeIncomplete;
    }

    if (any(isnan(Result)) || any(isinf(Result)))
        Result = 0.0;

    // Border texels mirror the interior across each edge, so bilinear reads over a tile's edge continue the octahedral map.
    int2 Origin2 = int2(CascadeTileOrigin(Level, Probe)) + 1;
    int Last = int(Texels) - 1;
    Radiance[Origin2 + Texel] = Result;
    bool2 bLow = Texel == 0;
    bool2 bHigh = Texel == Last;
    if (bLow.x || bHigh.x)
        Radiance[Origin2 + int2(bLow.x ? -1 : Last + 1, Last - Texel.y)] = Result;

    if (bLow.y || bHigh.y)
        Radiance[Origin2 + int2(Last - Texel.x, bLow.y ? -1 : Last + 1)] = Result;

    if ((bLow.x || bHigh.x) && (bLow.y || bHigh.y))
        Radiance[Origin2 + int2(bLow.x ? Last + 1 : -1, bLow.y ? Last + 1 : -1)] = Result;
}
