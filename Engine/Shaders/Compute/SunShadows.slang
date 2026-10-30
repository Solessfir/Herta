#include "../VisualShared.slangh"

// The cascaded sun's visibility once per pixel of the depth prepass, times the contact shadows already in the target when Visual.Material.BaseColor.y
// is set, so the mesh pass reads one texel instead of filtering soft shadows inside its light loop. Visual.Material.BaseColor.x is the sun's light.
// It needs the shading normals the prepass writes for global illumination.
[[vk::binding(0, 0)]] Texture2D<float> SceneDepth;
[[vk::binding(1, 0)]] Texture2D<float4> ShadowAtlas;
// The prepass's shading normals, the mesh pass's own; see EncodeSurface.
[[vk::binding(2, 0)]] Texture2D<float4> Surfaces;
[[vk::binding(16, 0)]] StructuredBuffer<FVisualLight> Lights;
[[vk::binding(384, 0)]] [format("r8")] RWTexture2D<float> SunShadow;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID)
{
    int2 Pixel = int2(Id.xy);
    if (any(Pixel >= int2(Visual.Viewport.zw)))
        return;

    // The sky, and pixels only custom-material surfaces cover, which skip the prepass and keep its cleared texel.
    float Depth = SceneDepth.Load(int3(Pixel, 0));
    float4 Surface = Surfaces.Load(int3(Pixel, 0));
    if (Depth <= 0.0 || all(Surface.rgb == 0.0))
    {
        SunShadow[Pixel] = 1.0;
        return;
    }

    // The mesh pass's own normal, which faces the sun where it does even on the far side of two-sided cloth, where the depth's normal faces the
    // camera, which showed the cloth's triangles.
    float3 Position = WorldPosition((float2(Pixel) + 0.5) * Visual.Viewport.xy, Depth);
    float3 Normal = DecodeSurfaceNormal(Surface);

    // The mesh pass skips lights behind the surface, so those pixels skip the filtering, which is most of the cost.
    FVisualLight Sun = Lights[uint(Visual.Material.BaseColor.x)];
    if (dot(Normal, Sun.DirectionRange.xyz) >= 0.0)
    {
        SunShadow[Pixel] = 0.0;
        return;
    }

    float Visibility = LightVisibility(ShadowAtlas, ShadowAtlas, Sun, Position, Normal);
    if (Visual.Material.BaseColor.y != 0.0)
        Visibility *= SunShadow[Pixel];

    SunShadow[Pixel] = Visibility;
}
