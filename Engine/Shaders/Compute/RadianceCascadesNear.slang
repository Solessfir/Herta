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

// One group per probe in the near list, one thread per near ray: radiance and hit distance, negative where the ray met a back face, as four
// floats per ray. A ray that meets nothing within the near range takes the cascades' light: level 0 from the level 1 probes around it, and the
// other levels from their own shell. Misses keep a far distance so the moments read them as open.
[[vk::binding(20, 0)]] StructuredBuffer<uint> Lists;
// Each probe's cell, level, and update count; see FieldProbeLevel.
[[vk::binding(21, 0)]] StructuredBuffer<int4> ProbeCells;
// Levels 2 and 3's radiance; level 1's is CascadeParentAtlas.
[[vk::binding(6, 0)]] Texture2D<float4> Radiance2;
[[vk::binding(7, 0)]] Texture2D<float4> Radiance3;
[[vk::binding(384, 0)]] RWStructuredBuffer<float> RayData;

// A probe's own shell for a direction, or no coverage before it has traced it.
float4 OwnRadiance(uint Level, uint Probe, float3 Direction)
{
    if (FieldUsed[CascadeRadianceFrame + Probe] == 0u)
        return 0.0;

    float2 Texel = float2(CascadeTileOrigin(Level, Probe)) + 1.0 + FieldOctahedralEncode(Direction) * float(CascadeTexels(Level));
    uint Width, Height;
    float4 Sample;
    if (Level == 1)
    {
        CascadeParentAtlas.GetDimensions(Width, Height);
        Sample = CascadeParentAtlas.SampleLevel(LinearSampler, Texel / float2(Width, Height), 0);
    }
    else if (Level == 2)
    {
        Radiance2.GetDimensions(Width, Height);
        Sample = Radiance2.SampleLevel(LinearSampler, Texel / float2(Width, Height), 0);
    }
    else
    {
        Radiance3.GetDimensions(Width, Height);
        Sample = Radiance3.SampleLevel(LinearSampler, Texel / float2(Width, Height), 0);
    }

    return Sample.a > 1e-4 ? float4(Sample.rgb / Sample.a, 1.0) : 0.0;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 Group : SV_GroupID, uint Ray : SV_GroupIndex)
{
    uint Probe = Lists[Group.x];
    int4 Place = ProbeCells[Probe];
    uint Level = FieldProbeLevel(Place.w);
    int3 Cell = Place.xyz;
    float3 Origin = CascadeProbePosition(Level, Cell, Probe);
    if (Ray == 0 && Level == 0)
        CascadeTouchParents(1, Origin);

    float3 Direction = FieldOctahedralDecode((float2(Ray % CascadeNearTexels, Ray / CascadeNearTexels) + 0.5) / float(CascadeNearTexels));
    float Range = CascadeNearRange(Level);
    float4 Result;
    FHit Hit;
    if (TraceScene(Origin, Direction, Range, Hit))
    {
        FSurfacePoint Point = ShadeHit(Hit, Direction, Hit.Distance * 0.4);
        if (Point.bBackFace)
        {
            Result = float4(0.0, 0.0, 0.0, -Hit.Distance);
        }
        else
        {
            // Light sampling draws the same choices every frame.
            FRandom Random;
            Random.State = PcgHash(FieldProbeSeed(Level, Cell) + Ray);
            Result = float4(FieldHitRadiance(Point, -Direction, Random), Hit.Distance);
        }
    }
    else
    {
        float4 Far = Level == 0 ? CascadeMerge(1, Origin, Direction, Range) : OwnRadiance(Level, Probe, Direction);
        Result = Far.a > 0.0 ? float4(Far.rgb / StoredScale, 1e4) : float4(0.0, 0.0, 0.0, CascadeNoData);
    }

    if (any(isnan(Result)) || any(isinf(Result)))
        Result = float4(0.0, 0.0, 0.0, Result.w);

    uint Base = (Group.x * CascadeNearRays + Ray) * 4;
    for (uint Word = 0; Word < 4; ++Word)
        RayData[Base + Word] = Result[Word];
}
