#include "Fullscreen.slangh"
// The only texture sits at binding 0, which every fullscreen pipeline provides, so this module's vertex stage stays the shared fullscreen vertex.
[[vk::binding(0, 0)]] Texture2D<float4> MultipleScattering;

// Ray-marches the procedural atmosphere into a small upper-hemisphere table that the sky pass samples.
[shader("fragment")]
float4 fragmentMain(FFullscreenOutput Input) : SV_Target
{
    float3 Transmittance;
    return float4(SkyScattering(SkyViewDirection(Input.UV), MultipleScattering, Transmittance), 1.0);
}
