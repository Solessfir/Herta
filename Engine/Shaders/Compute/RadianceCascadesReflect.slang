#define GiBufferBase 385
#include "RayTracing.slangh"
#include "RadianceCascades.slangh"

// Scene reflections for glossy surfaces, once per 2x2 block at the pixel the gather uses. Each frame traces one sample of the surface's GGX lobe and
// shades its hit like the cascade rays do; reprojected history averages the samples into the lobe's filtered reflection.
[[vk::binding(3, 0)]] Texture2D<float> SceneDepth;
#include "RadianceCascadesHits.slangh"
// The prepass's shading normals and roughness; see EncodeSurface.
[[vk::binding(5, 0)]] Texture2D<float4> Surfaces;
// Last frame's gathers, read when Visual.Material.BaseColor.w is set.
[[vk::binding(6, 0)]] Texture2D<float4> History;
// The gathers' atlas: irradiance on the left half, and reflected radiance at 1/16 scale on the right, with the pixel's reversed depth in alpha, or 0
// where the surface is too rough, so the mesh pass keeps its environment reflection.
[[vk::binding(384, 0)]] [format("rgba16f")] RWTexture2D<float4> Output;

// Last frame's reflection where this pixel's surface was, a Gaussian over the nearby blocks that saw the same surface; alpha 0 where none did.
// Rougher lobes take a wider Gaussian, so averaging over frames also filters their noise across the surface.
float4 ReprojectHistory(int2 Pixel, float Depth, float Roughness, uint HalfWidth, uint Height)
{
    float2 UV = (float2(Pixel) + 0.5) * Visual.Viewport.xy;
    float4 Clip = mul(Visual.ClipToPreviousClip, float4(UV.x * 2.0 - 1.0, 1.0 - UV.y * 2.0, Depth, 1.0));
    if (Clip.w <= 0.0)
        return 0.0;

    float PreviousDepth = Clip.z / Clip.w;
    // Each block samples its first pixel, whose centre is 2 b + 0.5.
    float2 Block = ((Clip.xy / Clip.w * float2(0.5, -0.5) + 0.5) * Visual.Viewport.zw - 0.5) * 0.5;
    int2 Base = int2(floor(Block)) - 1;
    // Reversed depth is proportional to inverse distance, so ratios compare distances; a slope changes them by its gradient per pixel.
    int2 Size = int2(Visual.Viewport.zw);
    float Slope = max(abs(SceneDepth.Load(int3(min(Pixel + int2(1, 0), Size - 1), 0)) - Depth), abs(SceneDepth.Load(int3(min(Pixel + int2(0, 1), Size - 1), 0)) - Depth)) / Depth;
    float Sigma = lerp(0.5, 1.25, saturate((Roughness - 0.1) / 0.4));
    float4 Sum = 0.0;
    for (uint Tap = 0; Tap < 16; ++Tap)
    {
        int2 Sample = Base + int2(Tap & 3u, Tap >> 2);
        if (any(Sample < 0) || Sample.x >= int(HalfWidth) || Sample.y >= int(Height))
            continue;

        float4 Value = History.Load(int3(Sample.x + int(HalfWidth), Sample.y, 0));
        float2 Offset = float2(Sample) - Block;
        float Distance = length(Offset);
        if (Value.a > 0.0 && abs(Value.a / PreviousDepth - 1.0) <= 0.02 + 4.0 * Distance * Slope)
            Sum += float4(Value.rgb, 1.0) * exp(-0.5 * dot(Offset, Offset) / (Sigma * Sigma));
    }

    return Sum.a > 0.01 ? float4(Sum.rgb / Sum.a, 1.0) : 0.0;
}

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
    float Roughness = Surface.a;
    float3 Position, GeometricNormal;
    if (Roughness > ReflectionRoughness || !SurfaceAt(SceneDepth, Pixel, Position, GeometricNormal))
    {
        Output[Target] = 0.0;
        return;
    }

    float Depth = SceneDepth.Load(int3(Pixel, 0));
    float3 Eye = CameraPosition();
    float Distance = length(Position - Eye);
    float3 V = (Eye - Position) / Distance;
    FRandom Random;
    Random.State = PcgHash(Id.x + PcgHash(Id.y + PcgHash(uint(Visual.Material.BaseColor.z) ^ 0x5bd1e995u)));
    // Lobe samples below the surface carry no light, so they are drawn again, and the mirror direction, slid along the surface where a normal map
    // tilts even that below, stands in after a few tries.
    float3 Normal = DecodeSurfaceNormal(Surface);
    float Alpha = GgxAlpha(Roughness);
    float3 Reflected = reflect(-V, Normal);
    for (uint Try = 0; Try < 4u; ++Try)
    {
        float3 Sample = SampleGgx(V, Normal, Alpha, float2(Random.Next(), Random.Next()));
        if (dot(Sample, GeometricNormal) > 1e-3)
        {
            Reflected = Sample;
            break;
        }
    }

    float Below = dot(Reflected, GeometricNormal);
    if (Below < 1e-3)
        Reflected = normalize(Reflected + GeometricNormal * (1e-3 - Below));

    float PixelAngle = 2.0 * abs(Visual.ClipToView[1][1]) / Visual.Viewport.w;
    float3 Radiance;
    FHit Mirror;
    // Depth-reconstructed positions lose precision with distance, so the ray starts proportionally further off the surface.
    if (TraceScene(Position + GeometricNormal * (RayOffset + 4e-4 * Distance), Reflected, 1e30, Mirror))
    {
        uint Cell;
        Radiance = HitRadiance(ShadeHit(Mirror, Reflected, PixelAngle * (Distance + Mirror.Distance)), -Reflected, false, Random, Cell);
    }
    else
    {
        Radiance = EnvironmentRadiance(Reflected);
    }

    if (any(isnan(Radiance)) || any(isinf(Radiance)))
        Radiance = 0.0;

    float3 Result = Radiance * StoredScale;
    // Near-mirrors need no averaging, which would only smear their parallax; wider lobes average up to 32 frames.
    float Blend = 1.0 / (1.0 + 31.0 * saturate((Roughness - 0.05) / 0.4));
    if (Visual.Material.BaseColor.w > 0.5 && Blend < 1.0)
    {
        float4 Previous = ReprojectHistory(Pixel, Depth, Roughness, HalfWidth, Height);
        if (Previous.a > 0.0)
            Result = lerp(Previous.rgb, Result, Blend);
    }

    Output[Target] = float4(Result, Depth);
}
