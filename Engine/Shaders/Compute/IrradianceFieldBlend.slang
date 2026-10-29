#include "IrradianceField.slangh"

// One 16x16 group per traced probe: integrates its rays into the probe's irradiance and distance-moment tiles, blended with what the tiles held, then
// moves the probe out of geometry its fixed rays found it inside, as Majercik et al. 2021 describe.
[[vk::binding(384, 0)]] [format("rgba16f")] RWTexture2D<float4> IrradianceAtlas;
[[vk::binding(385, 0)]] [format("rg16f")] RWTexture2D<float2> DistanceAtlas;
// Each probe's offset and state as four floats; see FieldProbes.
[[vk::binding(386, 0)]] RWStructuredBuffer<float> Probes;
// Each slot's cell in the first three of four ints and, in the fourth, how many times its probe has blended since it last started over.
[[vk::binding(387, 0)]] RWStructuredBuffer<int> Cells;
[[vk::binding(16, 0)]] StructuredBuffer<float4> RayData;
[[vk::binding(17, 0)]] StructuredBuffer<uint> TraceList;

// The share of each texel kept from earlier frames, which smooths how light settles as each update adds a bounce, and how it follows changes. A new
// probe averages its first updates evenly instead, so newly seen places light up within a few frames rather than seconds.
static const float Hysteresis = 0.97;
// Sharper than the irradiance's cosine, so the moments resolve walls; 128 rays leave gaps a sharper lobe would show.
static const float DistanceExponent = 30.0;

groupshared float4 SharedRays[FieldRays];
groupshared float3 SharedDirections[FieldRays];
groupshared float3 SharedIrradiance[FieldIrradianceTexels * FieldIrradianceTexels];
groupshared float2 SharedDistance[FieldDistanceTexels * FieldDistanceTexels];

// Probes move only over their first updates. Moving on afterwards nudged probes back and forth between pushing away from a surface and returning
// towards their corner, and every move changed their light, which showed as flickering patches.
static const float RelocationUpdates = 8.0;

// The probe's new offset and state from its fixed rays: a probe that sees mostly back faces is inside geometry and moves through the nearest one,
// one too close to a surface moves away from it, and one with room to spare drifts back towards its cell corner. Offsets stay within 0.45 of the
// spacing, and probes still inside are left out of shading. Settled probes keep their offset, and with their fixed rays, their classification.
float4 Relocate(float4 Probe, float Spacing, float Updates)
{
    uint Backfaces = 0;
    float ClosestBack = 1e30;
    float3 ClosestBackDirection = 0.0;
    float ClosestFront = 1e30;
    float3 ClosestFrontDirection = 0.0;
    float FarthestFront = 0.0;
    float3 FarthestFrontDirection = 0.0;
    for (uint Ray = 0; Ray < FieldFixedRays; ++Ray)
    {
        float Distance = SharedRays[Ray].w;
        float3 Direction = SharedDirections[Ray];
        if (Distance < 0.0)
        {
            ++Backfaces;
            if (-Distance < ClosestBack)
            {
                ClosestBack = -Distance;
                ClosestBackDirection = Direction;
            }
        }
        else
        {
            if (Distance < ClosestFront)
            {
                ClosestFront = Distance;
                ClosestFrontDirection = Direction;
            }

            if (Distance > FarthestFront)
            {
                FarthestFront = Distance;
                FarthestFrontDirection = Direction;
            }
        }
    }

    float MinimumFront = 0.2 * Spacing;
    bool bInside = Backfaces * 4 > FieldFixedRays;
    float3 Offset = Probe.xyz;
    if (Updates < RelocationUpdates)
    {
        if (bInside)
        {
            Offset += ClosestBackDirection * (ClosestBack + MinimumFront * 0.5);
        }
        else if (ClosestFront < MinimumFront)
        {
            float3 Away = FarthestFrontDirection * min(FarthestFront, MinimumFront);
            if (dot(Away, ClosestFrontDirection) <= 0.0)
                Offset += Away;
        }
        else if (dot(Offset, Offset) > 0.0)
        {
            float Length = length(Offset);
            Offset -= Offset / Length * min(ClosestFront - MinimumFront, Length);
        }

        if (any(abs(Offset) > 0.45 * Spacing))
            Offset = Probe.xyz;
    }

    return float4(Offset, bInside ? FieldProbeInside : FieldProbeActive);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void computeMain(uint3 Group : SV_GroupID, uint Thread : SV_GroupIndex)
{
    uint Slot = TraceList[Group.x];
    uint Level;
    int3 Cell = FieldSlotCell(Slot, Level);
    if (Thread < FieldRays)
    {
        SharedRays[Thread] = RayData[Group.x * FieldRays + Thread];
        SharedDirections[Thread] = FieldRayDirection(Thread, FieldProbeSeed(Level, Cell));
    }

    GroupMemoryBarrierWithGroupSync();
    float4 Probe = float4(Probes[Slot * 4], Probes[Slot * 4 + 1], Probes[Slot * 4 + 2], Probes[Slot * 4 + 3]);
    // Probes that were new or inside geometry last frame have nothing worth keeping.
    float Updates = Probe.w == FieldProbeActive ? float(Cells[Slot * 4 + 3]) : 0.0;
    float Keep = min(Hysteresis, Updates / (Updates + 1.0));
    float Spacing = FieldSpacing(Level);
    uint2 IrradianceOrigin = FieldTileOrigin(Slot, FieldIrradianceTile);
    uint2 DistanceOrigin = FieldTileOrigin(Slot, FieldDistanceTile);

    if (Thread < FieldIrradianceTexels * FieldIrradianceTexels)
    {
        uint2 Texel = uint2(Thread % FieldIrradianceTexels, Thread / FieldIrradianceTexels);
        float3 Direction = FieldOctahedralDecode((float2(Texel) + 0.5) / float(FieldIrradianceTexels));
        float3 Sum = 0.0;
        float Weights = 0.0;
        for (uint Ray = 0; Ray < FieldRays; ++Ray)
        {
            float4 Sample = SharedRays[Ray];
            // Back faces carry no light into the room the probe serves.
            if (Sample.w < 0.0)
                continue;

            float Weight = saturate(dot(Direction, SharedDirections[Ray]));
            Sum += Sample.rgb * Weight;
            Weights += Weight;
        }

        // The cosine-weighted mean radiance times pi is irradiance.
        float3 Irradiance = Weights > 0.0 ? Sum / Weights * (PI * StoredScale) : 0.0;
        float3 Previous = IrradianceAtlas[IrradianceOrigin + 1 + Texel].rgb;
        // Light that changes by more than a third, such as a lamp switching on, should not take a second to arrive.
        float3 Change = abs(Irradiance - Previous);
        float Brightest = max(max(Irradiance.r, max(Irradiance.g, Irradiance.b)), max(Previous.r, max(Previous.g, Previous.b)));
        float KeepTexel = max(Change.r, max(Change.g, Change.b)) > Brightest / 3.0 ? min(Keep, 0.7) : Keep;
        SharedIrradiance[Thread] = lerp(Irradiance, Previous, KeepTexel);
    }

    if (Thread < FieldDistanceTexels * FieldDistanceTexels)
    {
        uint2 Texel = uint2(Thread % FieldDistanceTexels, Thread / FieldDistanceTexels);
        float3 Direction = FieldOctahedralDecode((float2(Texel) + 0.5) / float(FieldDistanceTexels));
        float MaximumDistance = 1.5 * Spacing * sqrt(3.0);
        float2 Sum = 0.0;
        float Weights = 0.0;
        for (uint Ray = 0; Ray < FieldRays; ++Ray)
        {
            float4 Sample = SharedRays[Ray];
            // Back faces count as nearer than they are, so a probe inside geometry reads as occluded from outside it.
            float Distance = min(Sample.w < 0.0 ? -Sample.w * 0.2 : Sample.w, MaximumDistance);
            float Weight = pow(saturate(dot(Direction, SharedDirections[Ray])), DistanceExponent);
            Sum += float2(Distance, Distance * Distance) * Weight;
            Weights += Weight;
        }

        float2 Previous = DistanceAtlas[DistanceOrigin + 1 + Texel];
        float2 Moments = Weights > 1e-4 ? Sum / Weights : (Keep > 0.0 ? Previous : float2(MaximumDistance, MaximumDistance * MaximumDistance));
        SharedDistance[Thread] = Weights > 1e-4 ? lerp(Moments, Previous, Keep) : Moments;
    }

    GroupMemoryBarrierWithGroupSync();
    if (Thread < FieldIrradianceTile * FieldIrradianceTile)
    {
        int2 Texel = int2(Thread % FieldIrradianceTile, Thread / FieldIrradianceTile) - 1;
        int2 Source = FieldBorderSource(Texel, FieldIrradianceTexels);
        IrradianceAtlas[IrradianceOrigin + uint2(Texel + 1)] = float4(SharedIrradiance[Source.y * FieldIrradianceTexels + Source.x], 1.0);
    }

    int2 DistanceTexel = int2(Thread % FieldDistanceTile, Thread / FieldDistanceTile) - 1;
    int2 DistanceSource = FieldBorderSource(DistanceTexel, FieldDistanceTexels);
    DistanceAtlas[DistanceOrigin + uint2(DistanceTexel + 1)] = SharedDistance[DistanceSource.y * FieldDistanceTexels + DistanceSource.x];

    if (Thread == 0)
    {
        float4 Moved = Relocate(Probe, Spacing, Updates);
        for (uint Word = 0; Word < 4; ++Word)
            Probes[Slot * 4 + Word] = Moved[Word];
        Cells[Slot * 4 + 3] = Moved.w == FieldProbeActive ? int(min(Updates + 1.0, 255.0)) : 0;
    }
}
