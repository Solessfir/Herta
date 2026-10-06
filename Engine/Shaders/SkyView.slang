#include "Fullscreen.slangh"

// Ray-marches the procedural atmosphere into a small upper-hemisphere table that the sky pass samples.
[shader("fragment")]
float4 fragmentMain(FFullscreenOutput Input) : SV_Target
{
    float3 Transmittance;
    return float4(SkyScattering(SkyViewDirection(Input.UV), Transmittance), 1.0);
}
