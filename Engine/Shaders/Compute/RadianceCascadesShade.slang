#define GiBufferBase 385
#include "RadianceCascades.slangh"

// Diffuse irradiance at every pixel from the eight cascade 0 probes around its surface, sparse trilinear over those that exist.
// Alpha is 1 where probes covered the pixel; elsewhere the mesh pass keeps its environment lighting.
[[vk::binding(0, 0)]] Texture2D<float> SceneDepth;
[[vk::binding(1, 0)]] Texture2D<float4> IrradianceAtlas;
[[vk::binding(384, 0)]] [format("rgba16f")] RWTexture2D<float4> Output;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID)
{
    int2 Pixel = int2(Id.xy);
    float3 Position, Normal;
    if (any(Pixel >= int2(Visual.Viewport.zw)))
        return;

    if (!SurfaceAt(SceneDepth, Pixel, Position, Normal))
    {
        Output[Pixel] = 0.0;
        return;
    }

    uint Level = LevelOfDetail(Position);
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
        Sum += IrradianceAtlas.SampleLevel(LinearSampler, UV, 0).rgb * W;
        Weight += W;
    }

    Output[Pixel] = Weight > 1e-4 ? float4(Sum / Weight, 1.0) : 0.0;
}
