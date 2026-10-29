#define FIELD_IRRADIANCE_BINDING 3
#define FIELD_DISTANCE_BINDING 4
#define FIELD_PROBES_BINDING 19
#define FIELD_CELLS_BINDING 385
#define FIELD_USED_BINDING 386
#define FIELD_REQUESTS_BINDING 387
#define FIELD_USED_BY 1
#include "RayTracing.slangh"
#include "IrradianceField.slangh"
#include "IrradianceFieldHits.slangh"

// One group per probe in the trace list, one thread per ray: radiance and hit distance, negative where the ray met a back face, as four floats per
// ray. Misses keep a far distance so the moments read them as open.
[[vk::binding(20, 0)]] StructuredBuffer<uint> TraceList;
// Each probe's cell, level, and update count; see FieldProbeLevel.
[[vk::binding(21, 0)]] StructuredBuffer<int4> ProbeCells;
[[vk::binding(384, 0)]] RWStructuredBuffer<float> RayData;

[shader("compute")]
[numthreads(128, 1, 1)]
void computeMain(uint3 Group : SV_GroupID, uint Ray : SV_GroupIndex)
{
    uint Probe = TraceList[Group.x];
    int4 Place = ProbeCells[Probe];
    uint Level = FieldProbeLevel(Place.w);
    int3 Cell = Place.xyz;
    float3 Origin = float3(Cell) * FieldSpacing(Level) + FieldProbes[Probe].xyz;
    uint Seed = FieldProbeSeed(Level, Cell);
    float3 Direction = FieldRayDirection(Ray, Seed);
    float4 Result;
    FHit Hit;
    if (!TraceScene(Origin, Direction, 1e30, Hit))
    {
        Result = float4(EnvironmentRadiance(Direction), 1e4);
    }
    else
    {
        // Texture detail finer than the spacing between rays would only add noise, so hits read mips about that wide.
        FSurfacePoint Point = ShadeHit(Hit, Direction, Hit.Distance * 0.3);
        if (Point.bBackFace)
        {
            Result = float4(0.0, 0.0, 0.0, -Hit.Distance);
        }
        else
        {
            // Light sampling draws the same choices every frame too.
            FRandom Random;
            Random.State = PcgHash(Seed + Ray);
            Result = float4(FieldHitRadiance(Point, -Direction, Random), Hit.Distance);
        }
    }

    if (any(isnan(Result)) || any(isinf(Result)))
        Result = float4(0.0, 0.0, 0.0, Result.w);

    uint Base = (Group.x * FieldRays + Ray) * 4;
    for (uint Word = 0; Word < 4; ++Word)
        RayData[Base + Word] = Result[Word];
}
