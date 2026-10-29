#define GiBufferBase 385
#include "RadianceCascades.slangh"

// Irradiance is gathered once per 2x2 block, at the pixel that traced the block's ray; every pixel takes the bilinear blend of the four nearest gathers
// that lie on its own surface plane, so light does not bleed across depth edges or creases. Thin features that no gather shares fall back to the nearest one.
[[vk::binding(0, 0)]] Texture2D<float> SceneDepth;
[[vk::binding(1, 0)]] Texture2D<float4> Gathered;
[[vk::binding(384, 0)]] [format("rgba16f")] RWTexture2D<float4> Output;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID)
{
    int2 Pixel = int2(Id.xy);
    if (any(Pixel >= int2(Visual.Viewport.zw)))
        return;

    float3 Position, Normal;
    if (!SurfaceAt(SceneDepth, Pixel, Position, Normal))
    {
        Output[Pixel] = 0.0;
        return;
    }

    uint Width, Height;
    Gathered.GetDimensions(Width, Height);
    // Probes are about a 48th of the viewing distance apart, so samples farther off the plane than a quarter of that belong to another surface.
    float3 FromCamera = abs(Position - CameraPosition());
    float Tolerance = 0.25 * SpacingRatio * max(FromCamera.x, max(FromCamera.y, FromCamera.z));
    // Gathers sit on each block's first pixel, so even pixels take their own block's and odd ones split between two.
    int2 Base = Pixel / 2;
    float2 Fraction = float2(Pixel & 1) * 0.5;
    float3 Sum = 0.0;
    float Weight = 0.0;
    float3 Nearest = 0.0;
    float NearestDistance = 1e30;
    for (uint Corner = 0; Corner < 4; ++Corner)
    {
        int2 Offset = int2(Corner & 1u, Corner >> 1);
        int2 Sample = clamp(Base + Offset, 0, int2(Width, Height) - 1);
        float4 Value = Gathered.Load(int3(Sample, 0));
        int2 Source = min(Sample * 2, int2(Visual.Viewport.zw) - 1);
        float Depth = SceneDepth.Load(int3(Source, 0));
        if (Value.a <= 0.0 || Depth <= 0.0)
            continue;

        float Distance = abs(dot(WorldPosition((float2(Source) + 0.5) * Visual.Viewport.xy, Depth) - Position, Normal));
        if (Distance < NearestDistance)
        {
            NearestDistance = Distance;
            Nearest = Value.rgb;
        }

        if (Distance > Tolerance)
            continue;

        float2 Axis = lerp(1.0 - Fraction, Fraction, float2(Offset));
        Sum += Value.rgb * (Axis.x * Axis.y);
        Weight += Axis.x * Axis.y;
    }

    Output[Pixel] = Weight > 0.0 ? float4(Sum / Weight, 1.0) : (NearestDistance < 1e30 ? float4(Nearest, 1.0) : 0.0);
}
