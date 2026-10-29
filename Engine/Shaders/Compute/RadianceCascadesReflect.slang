#define GiBufferBase 385
#include "RayTracing.slangh"
#include "RadianceCascades.slangh"

// Scene reflections for glossy surfaces, once per 2x2 block at the pixel the gather uses. A ray from the camera finds the surface's material, normal map
// included, because the forward mesh pass keeps no G-buffer; glossy ones trace one mirror ray and shade its hit like the cascade rays do.
[[vk::binding(3, 0)]] Texture2D<float> SceneDepth;
#include "RadianceCascadesHits.slangh"
// The gathers' atlas: irradiance on the left half, and reflected radiance at 1/16 scale on the right, with the pixel's reversed depth in alpha, or 0
// where the surface is too rough or no camera ray found it, so the mesh pass keeps its environment reflection.
[[vk::binding(384, 0)]] [format("rgba16f")] RWTexture2D<float4> Output;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID)
{
    uint Width, Height;
    Output.GetDimensions(Width, Height);
    uint HalfWidth = Width / 2u;
    if (Id.x >= HalfWidth || Id.y >= Height)
        return;

    uint2 Target = uint2(Id.x + HalfWidth, Id.y);
    int2 Pixel = min(int2(Id.xy) * 2, int2(Visual.Viewport.zw) - 1);
    float Depth = SceneDepth.Load(int3(Pixel, 0));
    float3 Position, Normal;
    if (!SurfaceAt(SceneDepth, Pixel, Position, Normal))
    {
        Output[Target] = 0.0;
        return;
    }

    float3 Eye = CameraPosition();
    float Distance = length(Position - Eye);
    float3 View = (Position - Eye) / Distance;
    FHit Hit;
    // The camera ray only searches around the depth buffer's surface. Missing it, as at a silhouette, leaves the pixel to the environment.
    float Margin = 0.02 * Distance + 0.02;
    if (!TraceSurface(Eye, View, max(Distance - Margin, 0.0), Distance + Margin, Hit))
    {
        Output[Target] = 0.0;
        return;
    }

    float PixelAngle = 2.0 * abs(Visual.ClipToView[1][1]) / Visual.Viewport.w;
    FSurfacePoint Point = ShadeHit(Hit, View, PixelAngle * Hit.Distance);
    if (Point.Material.Roughness > ReflectionRoughness)
    {
        Output[Target] = 0.0;
        return;
    }

    // Normal maps can tilt the mirror direction below the surface; it then slides along the surface instead.
    float3 Reflected = reflect(View, Point.Material.Normal);
    float Below = dot(Reflected, Point.GeometricNormal);
    if (Below < 1e-3)
        Reflected = normalize(Reflected + Point.GeometricNormal * (1e-3 - Below));

    FRandom Random;
    Random.State = PcgHash(Id.x + PcgHash(Id.y + PcgHash(uint(Visual.Material.BaseColor.z) ^ 0x5bd1e995u)));
    float3 Radiance;
    FHit Mirror;
    if (TraceScene(Point.Position + Point.GeometricNormal * RayOffset, Reflected, 1e30, Mirror))
    {
        uint Cell;
        Radiance = HitRadiance(ShadeHit(Mirror, Reflected, PixelAngle * (Hit.Distance + Mirror.Distance)), -Reflected, false, Random, Cell);
    }
    else
    {
        Radiance = EnvironmentRadiance(Reflected);
    }

    if (any(isnan(Radiance)) || any(isinf(Radiance)))
        Radiance = 0.0;

    Output[Target] = float4(Radiance * StoredScale, Depth);
}
