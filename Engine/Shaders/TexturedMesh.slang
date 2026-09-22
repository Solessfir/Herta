struct FTransform
{
    column_major float4x4 MVP;
};

[[vk::push_constant]] ConstantBuffer<FTransform> Transform;
[[vk::binding(0, 0)]] Texture2D<float4> ColorTexture;
[[vk::binding(128, 0)]] SamplerState ColorSampler;

struct FVertexOutput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

[shader("vertex")]
FVertexOutput vertexMain(float3 Position : POSITION, float2 TexCoord : TEXCOORD0)
{
    FVertexOutput Output;
    Output.Position = mul(Transform.MVP, float4(Position, 1.0));
    Output.TexCoord = TexCoord;
    return Output;
}

[shader("fragment")]
float4 fragmentMain(FVertexOutput Input) : SV_Target
{
    return ColorTexture.Sample(ColorSampler, Input.TexCoord);
}
