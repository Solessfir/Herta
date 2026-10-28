#include "SmaaEdgeEncoding.slangh"
#include "Smaa.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> ColorTexture;

[shader("fragment")]
float4 fragmentMain(FFullscreenOutput Input) : SV_Target
{
    float4 Offset[3];
    SMAAEdgeDetectionVS(Input.UV, Offset);
    return float4(SMAAColorEdgeDetectionPS(Input.UV, Offset, ColorTexture), 0.0, 0.0);
}
