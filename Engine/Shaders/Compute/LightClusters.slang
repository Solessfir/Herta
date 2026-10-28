#include "../VisualShared.slangh"
[[vk::binding(384, 0)]] [format("r32ui")] RWTexture2D<uint> Clusters;

// One thread per cluster: a 64-pixel screen tile and one exponential depth slice. Its bits are the lights whose bounding sphere touches the cluster's view-space box.
[shader("compute")]
[numthreads(4, 4, 4)]
void computeMain(uint3 Id : SV_DispatchThreadID)
{
    uint TileSize = uint(Visual.LightGrid.x);
    uint TilesX = uint(Visual.LightGrid.w);
    uint TilesY = (uint(Visual.Viewport.w) + TileSize - 1) / TileSize;
    if (Id.x >= TilesX || Id.y >= TilesY || Id.z >= LightClusterSlices)
        return;

    // Inverts Slice = log2(ViewZ) * Scale + Bias. The last slice reaches to infinity.
    float Near = Id.z == 0 ? 0.0 : exp2((float(Id.z) - Visual.LightGrid.z) / Visual.LightGrid.y);
    float Far = Id.z + 1 == LightClusterSlices ? 1e30 : exp2((float(Id.z + 1) - Visual.LightGrid.z) / Visual.LightGrid.y);
    float2 PixelMinimum = float2(Id.xy * TileSize);
    float2 PixelMaximum = min(float2((Id.xy + 1) * TileSize), Visual.Viewport.zw);
    float3 Minimum = 1e30;
    float3 Maximum = -1e30;
    for (uint Corner = 0; Corner < 4; ++Corner)
    {
        float2 UV = float2(Corner & 1 ? PixelMaximum.x : PixelMinimum.x, Corner & 2 ? PixelMaximum.y : PixelMinimum.y) * Visual.Viewport.xy;
        float4 Point = mul(Visual.ClipToView, float4(UV.x * 2.0 - 1.0, 1.0 - UV.y * 2.0, 0.5, 1.0));
        // A view ray scaled to unit depth, so multiplying by a slice depth lands on that slice.
        float3 Ray = Point.xyz / Point.w;
        Ray /= Ray.z;
        Minimum = min(Minimum, min(Ray * Near, Ray * Far));
        Maximum = max(Maximum, max(Ray * Near, Ray * Far));
    }

    float3 Eye = mul(Visual.ViewToWorld, float4(0.0, 0.0, 0.0, 1.0)).xyz;
    uint Mask = 0;
    for (uint I = 0; I < uint(Visual.Controls.x); ++I)
    {
        FVisualLight Light = Visual.Lights[I];
        int Type = int(Light.PositionType.w);
        if (Type == 0)
        {
            Mask |= 1u << I;
            continue;
        }

        if (Type == 1)
            continue;

        // Rect range is measured from the emitter's surface.
        float Radius = Light.DirectionRange.w + (Type == 4 ? 0.5 * length(float2(Light.RightWidth.w, Light.UpHeight.w)) : 0.0);
        float3 Center = mul(Light.PositionType.xyz - Eye, (float3x3)Visual.ViewToWorld);
        float3 Outside = max(Minimum - Center, 0.0) + max(Center - Maximum, 0.0);
        if (dot(Outside, Outside) <= Radius * Radius)
            Mask |= 1u << I;
    }

    Clusters[uint2(Id.z * TilesX + Id.x, Id.y)] = Mask;
}
