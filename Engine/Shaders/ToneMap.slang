#include "Fullscreen.slangh"
#include "FogComposite.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> SceneColor;
[[vk::binding(1, 0)]] Texture2D<float4> AutoExposure;
[[vk::binding(2, 0)]] Texture2D<float4> ToneMapLut;
[[vk::binding(3, 0)]] Texture2D<float4> FogVolume;
[[vk::binding(4, 0)]] Texture2D<float4> DepthTexture;
[[vk::binding(5, 0)]] Texture2D<float> FogDepth;

// An outer square at the configured peak around an inner square far above any display. Raising the peak until the inner square disappears finds where the display clips.
float3 CalibrationPattern(float2 UV)
{
    float2 Pixel = (UV - 0.5) * Visual.Viewport.zw;
    float Distance = max(abs(Pixel.x), abs(Pixel.y)) / min(Visual.Viewport.z, Visual.Viewport.w);
    float Luminance = Distance < 0.08 ? 10000.0 : Distance < 0.2 ? Visual.Display.z : 0.0;
    return (Luminance / Visual.Display.y).xxx;
}

[shader("fragment")]
float4 fragmentMain(FFullscreenOutput Input) : SV_Target
{
    bool bHdr = Visual.Display.x > 0.5;
    if (bHdr && Visual.Display.w > 0.5)
    {
        return float4(CalibrationPattern(Input.UV), 1.0);
    }

    float Scale = Visual.Exposure.x > 0.5 ? 1.0 / (1.2 * exp2(AutoExposure.Load(int3(0, 0, 0)).r)) : Visual.Controls.z;
    float3 Color = SceneColor.Load(int3(Input.Position.xy, 0)).rgb;
    // Fog composites here rather than in its own pass, saving a full-resolution HDR write and read.
    if (Visual.Fog.x > 0.0)
    {
        float4 Fog = UpsampleFog(FogVolume, FogDepth, Input.UV, DepthTexture.Load(int3(Input.Position.xy, 0)).r);
        Color = Color * Fog.a + Fog.rgb;
    }

    float3 Exposed = max(0.0, Color * Scale);

    // GT7 lives in ToneMapLut.slang, baked for this display into a LUT addressed in log2 exposure.
    float3 Index = saturate((log2(max(Exposed, 1e-8)) - ToneMapLutMinimumLog2) / (ToneMapLutMaximumLog2 - ToneMapLutMinimumLog2)) * (ToneMapLutSize - 1.0);
    float Slice = floor(Index.b);
    float2 UV = float2(Index.r + 0.5, Index.g + 0.5) / float2(ToneMapLutSize * ToneMapLutSize, ToneMapLutSize);
    float2 SliceStep = float2(1.0 / ToneMapLutSize, 0.0);
    float3 Lower = ToneMapLut.SampleLevel(LinearSampler, UV + SliceStep * Slice, 0).rgb;
    float3 Upper = ToneMapLut.SampleLevel(LinearSampler, UV + SliceStep * min(Slice + 1.0, ToneMapLutSize - 1.0), 0).rgb;
    float3 Mapped = lerp(Lower, Upper, Index.b - Slice);

    // ponytail: HDR output clips to Rec.709 primaries; a Rec.2020 display target would keep the wider gamut GT7 produces.
    return float4(bHdr ? max(Mapped, 0.0) : saturate(Mapped), 1.0);
}
