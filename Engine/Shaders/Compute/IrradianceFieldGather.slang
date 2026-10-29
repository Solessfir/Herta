#define FIELD_IRRADIANCE_BINDING 2
#define FIELD_DISTANCE_BINDING 3
#define FIELD_PROBES_BINDING 16
#define FIELD_USED_BINDING 385
#define FIELD_USED_BY 0
#include "IrradianceField.slangh"

// Diffuse irradiance from the field once per 2x2 block, at the block's first pixel with the prepass's shading normal, into the gathers' left half
// at 1/16 scale with that pixel's reversed depth in alpha, or 0 where the field has nothing, so the mesh pass keeps its environment lighting. The
// probes it reads are the ones that trace next frame.
[[vk::binding(0, 0)]] Texture2D<float> SceneDepth;
// The prepass's shading normals and roughness; see EncodeSurface.
[[vk::binding(1, 0)]] Texture2D<float4> Surfaces;
[[vk::binding(384, 0)]] [format("rgba16f")] RWTexture2D<float4> Output;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID)
{
    uint Width, Height;
    Output.GetDimensions(Width, Height);
    if (any(Id.xy >= uint2(Width / 2u, Height)))
        return;

    int2 Pixel = min(int2(Id.xy) * 2, int2(Visual.Viewport.zw) - 1);
    float3 Position, Normal;
    if (!SurfaceAt(SceneDepth, Pixel, Position, Normal))
    {
        Output[Id.xy] = 0.0;
        return;
    }

    // Custom-material surfaces skip the prepass and keep its cleared texel, so they shade with the depth's normal.
    float4 Surface = Surfaces.Load(int3(Pixel, 0));
    if (any(Surface.rgb != 0.0))
        Normal = DecodeSurfaceNormal(Surface);

    float4 Irradiance = FieldIrradiance(Position, Normal);
    Output[Id.xy] = Irradiance.a > 0.0 ? float4(Irradiance.rgb * StoredScale, SceneDepth.Load(int3(Pixel, 0))) : 0.0;
}
