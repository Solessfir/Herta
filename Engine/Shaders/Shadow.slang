#include "VisualShared.slangh"
struct FTransform { column_major float4x4 MVP; column_major float4x4 ObjectToView; };
[[vk::push_constant]] ConstantBuffer<FTransform> Transform;
[[vk::binding(0, 0)]] Texture2D<float4> BaseColorTexture;

struct FShadowOutput
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
};

[shader("vertex")]
FShadowOutput vertexMain(float3 Position : POSITION, float2 UV : TEXCOORD0)
{
    FShadowOutput Output;
    Output.Position = mul(Transform.MVP, float4(Position, 1.0));
    Output.UV = UV * Visual.Material.UV.xy + Visual.Material.UV.zw;
    return Output;
}

struct FInstanceInput
{
    float4 View0 : TEXCOORD1; float4 View1 : TEXCOORD2; float4 View2 : TEXCOORD3; float4 View3 : TEXCOORD4;
};

[shader("vertex")]
FShadowOutput instancedVertexMain(float3 Position : POSITION, float2 UV : TEXCOORD0, FInstanceInput Instance)
{
    FShadowOutput Output;
    float4 ViewPosition = Instance.View0 * Position.x + Instance.View1 * Position.y + Instance.View2 * Position.z + Instance.View3;
    Output.Position = mul(Transform.MVP, ViewPosition);
    Output.UV = UV * Visual.Material.UV.xy + Visual.Material.UV.zw;
    return Output;
}

[shader("fragment")]
void fragmentMain(FShadowOutput Input)
{
    if (Visual.Material.Channels1.z > 0.5)
    {
        float Alpha = Visual.Material.BaseColor.a;
        if ((uint(Visual.Material.TextureFlags.x) & 1u) != 0u) Alpha *= BaseColorTexture.Sample(LinearSampler, Input.UV).a;
        clip(Alpha - Visual.Material.Channels1.w);
    }
}
