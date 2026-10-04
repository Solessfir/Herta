struct FTransform
{
    column_major float4x4 MVP;
    column_major float4x4 ObjectToView;
};

[[vk::push_constant]] ConstantBuffer<FTransform> Transform;
[[vk::binding(0, 0)]] Texture2D<float4> ColorTexture;
[[vk::binding(128, 0)]] SamplerState ColorSampler;

struct FVertexOutput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
    float3 ViewPosition : TEXCOORD1;
};

[shader("vertex")]
FVertexOutput vertexMain(float3 Position : POSITION, float2 TexCoord : TEXCOORD0)
{
    FVertexOutput Output;
    Output.Position = mul(Transform.MVP, float4(Position, 1.0));
    Output.TexCoord = TexCoord;
    Output.ViewPosition = mul(Transform.ObjectToView, float4(Position, 1.0)).xyz;
    return Output;
}

struct FInstanceInput
{
    float4 Clip0 : TEXCOORD1;
    float4 Clip1 : TEXCOORD2;
    float4 Clip2 : TEXCOORD3;
    float4 Clip3 : TEXCOORD4;
    float4 View0 : TEXCOORD5;
    float4 View1 : TEXCOORD6;
    float4 View2 : TEXCOORD7;
    float4 View3 : TEXCOORD8;
};

[shader("vertex")]
FVertexOutput instancedVertexMain(float3 Position : POSITION, float2 TexCoord : TEXCOORD0, FInstanceInput Instance)
{
    FVertexOutput Output;
    Output.Position = Instance.Clip0 * Position.x + Instance.Clip1 * Position.y + Instance.Clip2 * Position.z + Instance.Clip3;
    Output.TexCoord = TexCoord;
    Output.ViewPosition = (Instance.View0 * Position.x + Instance.View1 * Position.y + Instance.View2 * Position.z + Instance.View3).xyz;
    return Output;
}

[shader("fragment")]
float4 fragmentMain(FVertexOutput Input) : SV_Target
{
    float4 Albedo = ColorTexture.Sample(ColorSampler, Input.TexCoord);
    // Flat normals from screen-space derivatives until cooked meshes carry normals. Back faces are culled, so the visible side faces the camera.
    float3 Normal = normalize(cross(ddx(Input.ViewPosition), ddy(Input.ViewPosition)));
    Normal = dot(Normal, Input.ViewPosition) > 0.0 ? -Normal : Normal;
    // Studio light slightly above the camera, in Left-Up-Forward view space.
    const float3 LightDirection = normalize(float3(0.0, 0.5, -1.0));
    const float Diffuse = saturate(dot(Normal, LightDirection));
    return float4(Albedo.rgb * (0.35 + 0.65 * Diffuse), Albedo.a);
}
