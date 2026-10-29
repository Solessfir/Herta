#define FIELD_IRRADIANCE_BINDING 4
#define FIELD_DISTANCE_BINDING 5
#define FIELD_PROBES_BINDING 19
#define FIELD_USED_BINDING 385
#define FIELD_USED_BY 1
#include "RayTracing.slangh"
#include "IrradianceField.slangh"
#include "IrradianceFieldHits.slangh"

// Scene reflections for smooth surfaces, once per 2x2 block at the pixel the gather uses: one mirror ray, its hit shaded like the field's probe
// rays. Nothing is random, so a still view never changes and a moving one never shows noise; wider lobes keep the environment's filtered
// reflection in the mesh pass, which no single ray could stand in for.
[[vk::binding(3, 0)]] Texture2D<float> SceneDepth;
// The prepass's shading normals and roughness; see EncodeSurface.
[[vk::binding(6, 0)]] Texture2D<float4> Surfaces;
// The gathers' atlas: irradiance on the left half, and reflected radiance at 1/16 scale on the right, with the pixel's reversed depth in alpha, or 0
// where the surface is too rough, so the mesh pass keeps its environment reflection.
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
    float4 Surface = Surfaces.Load(int3(Pixel, 0));
    float3 Position, GeometricNormal;
    if (Surface.a > ReflectionRoughness || !SurfaceAt(SceneDepth, Pixel, Position, GeometricNormal))
    {
        Output[Target] = 0.0;
        return;
    }

    float3 Eye = CameraPosition();
    float Distance = length(Position - Eye);
    float3 V = (Eye - Position) / Distance;
    // A normal map can tilt the mirror direction below the surface; it then slides along the surface instead.
    float3 Reflected = reflect(-V, DecodeSurfaceNormal(Surface));
    float Below = dot(Reflected, GeometricNormal);
    if (Below < 1e-3)
        Reflected = normalize(Reflected + GeometricNormal * (1e-3 - Below));

    float PixelAngle = 2.0 * abs(Visual.ClipToView[1][1]) / Visual.Viewport.w;
    float3 Radiance;
    FHit Mirror;
    // Depth-reconstructed positions lose precision with distance, so the ray starts proportionally further off the surface.
    if (TraceScene(Position + GeometricNormal * (RayOffset + 4e-4 * Distance), Reflected, 1e30, Mirror))
    {
        FSurfacePoint Point = ShadeHit(Mirror, Reflected, PixelAngle * (Distance + Mirror.Distance));
        // Light sampling at the hit draws from a seed tied to where the hit is, so it stays the same as the camera moves.
        FRandom Random;
        uint3 Place = uint3(int3(floor(Point.Position * 100.0)));
        Random.State = PcgHash(Place.x ^ PcgHash(Place.y ^ PcgHash(Place.z)));
        Radiance = FieldHitRadiance(Point, -Reflected, Random);
    }
    else
    {
        Radiance = EnvironmentRadiance(Reflected);
    }

    if (any(isnan(Radiance)) || any(isinf(Radiance)))
        Radiance = 0.0;

    Output[Target] = float4(Radiance * StoredScale, SceneDepth.Load(int3(Pixel, 0)));
}
