#include "../VisualShared.slangh"
[[vk::binding(0, 0)]] Texture2D<float> SceneDepth;
[[vk::binding(384, 0)]] [format("rg16f")] RWTexture2D<float2> MotionVectors;

// Pixels without object motion, such as the sky and custom-material surfaces, move only with the camera: reproject their depth into the previous frame.
[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID)
{
    if (any(Id.xy >= uint2(Visual.Viewport.zw)) || MotionVectors[Id.xy].x < MotionVectorUnknown.x)
        return;

    float2 UV = (float2(Id.xy) + 0.5) * Visual.Viewport.xy;
    float4 Clip = float4(UV.x * 2.0 - 1.0, 1.0 - UV.y * 2.0, SceneDepth.Load(int3(Id.xy, 0)), 1.0);
    // Reversed infinite depth puts the sky at 0, which reprojects as a direction.
    float4 Previous = mul(Visual.ClipToPreviousClip, Clip);
    MotionVectors[Id.xy] = Previous.w > 0.0 ? UV - (Previous.xy / Previous.w * float2(0.5, -0.5) + 0.5) : 0.0;
}
