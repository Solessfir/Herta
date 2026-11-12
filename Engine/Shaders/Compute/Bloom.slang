// Matches FBloomUniforms in MeshRenderer.cpp.
struct FBloomUniforms
{
    // 0 downsamples scene color, 1 downsamples a level, and 2 upsamples one level and adds the level below it.
    uint4 Settings;
};

[[vk::binding(64, 0)]] ConstantBuffer<FBloomUniforms> Bloom;
[[vk::binding(0, 0)]] Texture2D<float4> Source;
// The same-resolution downsampled level the upsample adds back.
[[vk::binding(1, 0)]] Texture2D<float4> Detail;
[[vk::binding(384, 0)]] [format("r11f_g11f_b10f")] RWTexture2D<float3> Target;
[[vk::binding(128, 0)]] SamplerState LinearSampler;

float3 Tap(float2 UV)
{
    // Clamping keeps one inf or NaN pixel from spreading through every level.
    float3 Color = Source.SampleLevel(LinearSampler, UV, 0).rgb;
    return any(isnan(Color)) ? 0.0 : min(Color, 65000.0);
}

// Karis's average weighs each tap by its inverse luminance, so a single very bright pixel cannot flicker the whole pyramid as it moves.
// Scene color is pre-exposed, so the weights act on exposed values in a night street as in sunlight.
float KarisWeight(float3 Color)
{
    return 1.0 / (1.0 + dot(Color, float3(0.2126, 0.7152, 0.0722)));
}

// The dual-filter Kawase pyramid (Bjorge, SIGGRAPH 2015), with the same taps as the editor's glass blur. Every level preserves the
// average of its source, so their sum over the level count conserves the scene's energy.
[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID)
{
    uint Width, Height;
    Target.GetDimensions(Width, Height);
    if (any(Id.xy >= uint2(Width, Height)))
        return;

    float2 UV = (float2(Id.xy) + 0.5) / float2(Width, Height);
    uint SourceWidth, SourceHeight;
    Source.GetDimensions(SourceWidth, SourceHeight);
    float2 Offset = 1.0 / float2(SourceWidth, SourceHeight);
    float3 Result;
    if (Bloom.Settings.x == 2)
    {
        float2 Half = Offset * 0.5;
        Result = Tap(UV + float2(-Offset.x, 0.0)) + Tap(UV + float2(Offset.x, 0.0)) + Tap(UV + float2(0.0, -Offset.y)) + Tap(UV + float2(0.0, Offset.y));
        Result += (Tap(UV - Half) + Tap(UV + Half) + Tap(UV + float2(Half.x, -Half.y)) + Tap(UV + float2(-Half.x, Half.y))) * 2.0;
        Result = Result / 12.0 + Detail.Load(int3(Id.xy, 0)).rgb;
    }
    else
    {
        float3 Taps[5] = {Tap(UV), Tap(UV - Offset), Tap(UV + Offset), Tap(UV + float2(Offset.x, -Offset.y)), Tap(UV + float2(-Offset.x, Offset.y))};
        float Weights[5] = {4.0, 1.0, 1.0, 1.0, 1.0};
        float Total = 0.0;
        Result = 0.0;
        for (int Index = 0; Index < 5; ++Index)
        {
            float Weight = Weights[Index] * (Bloom.Settings.x == 0 ? KarisWeight(Taps[Index]) : 1.0);
            Result += Taps[Index] * Weight;
            Total += Weight;
        }

        Result /= Total;
    }

    Target[Id.xy] = Result;
}
