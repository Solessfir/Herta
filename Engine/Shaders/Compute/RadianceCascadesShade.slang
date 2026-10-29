#define GiBufferBase 385
#include "RadianceCascades.slangh"

// Diffuse irradiance from the eight cascade 0 probes around a surface, sparse trilinear over those that exist, once per 2x2 block at the pixel that
// traced its ray. Alpha holds that pixel's reversed depth where probes covered it, so the mesh pass can find which gathers share each pixel's surface,
// and 0 elsewhere, where it keeps its environment lighting.
[[vk::binding(0, 0)]] Texture2D<float> SceneDepth;
[[vk::binding(1, 0)]] Texture2D<float4> IrradianceAtlas;
[[vk::binding(384, 0)]] [format("rgba16f")] RWTexture2D<float4> Output;

// Irradiance from one level's eight cascade 0 probes around a surface, with alpha 1 when any existed.
float4 GatherLevel(float3 Position, float3 Normal, uint Level)
{
    float Spacing = BaseSpacing(Level);
    float3 Grid = Position / Spacing - 0.5;
    int3 Base = int3(floor(Grid));
    float3 Fraction = Grid - float3(Base);
    uint AtlasWidth, AtlasHeight;
    IrradianceAtlas.GetDimensions(AtlasWidth, AtlasHeight);
    float2 Octahedral = OctahedralEncode(Normal) * float(IrradianceTexels) + 1.0;
    float3 Sum = 0.0;
    float Weight = 0.0;
    for (uint Corner = 0; Corner < 8; ++Corner)
    {
        int3 Offset = int3(Corner & 1u, (Corner >> 1) & 1u, Corner >> 2);
        int3 Cell = Base + Offset;
        uint Probe = FindProbe(0, MakeKey(Level, 0, Cell));
        // Probes well behind the surface sit inside whatever it bounds, and would leak its darkness or light.
        float3 Center = (float3(Cell) + 0.5) * Spacing;
        if (Probe == MissingProbe || dot(Center - Position, Normal) < -0.5 * Spacing)
            continue;

        float3 Axis = lerp(1.0 - Fraction, Fraction, float3(Offset));
        float W = Axis.x * Axis.y * Axis.z;
        float2 UV = (float2(IrradianceTileOrigin(Probe)) + Octahedral) / float2(AtlasWidth, AtlasHeight);
        // Tiles hold irradiance times the share of directions their probe knew.
        float4 Sample = IrradianceAtlas.SampleLevel(LinearSampler, UV, 0);
        Sum += Sample.rgb * W;
        Weight += Sample.a * W;
    }

    return Weight > 1e-4 ? float4(Sum / Weight, 1.0) : 0.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID)
{
    // The atlas's right half holds reflections.
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

    uint Level = LevelOfDetail(Position);
    float Blend = LevelBlend(Position, Level);
    float4 Coarse = GatherLevel(Position, Normal, Level);
    float4 Fine = Blend < 1.0 ? GatherLevel(Position, Normal, Level - 1u) : 0.0;
    // Where one level found no probes, the other covers the pixel alone.
    float CoarseWeight = Fine.a > 0.0 ? (Coarse.a > 0.0 ? Blend : 0.0) : 1.0;
    Output[Id.xy] = Coarse.a > 0.0 || Fine.a > 0.0 ? float4(Coarse.rgb * CoarseWeight + Fine.rgb * (1.0 - CoarseWeight), SceneDepth.Load(int3(Pixel, 0))) : 0.0;
}
