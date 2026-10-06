#include "VisualShared.slangh"

struct FTransform { column_major float4x4 MVP; column_major float4x4 ObjectToView; };
[[vk::push_constant]] ConstantBuffer<FTransform> Transform;
[[vk::binding(0, 0)]] Texture2D<float4> BaseColorTexture;
[[vk::binding(1, 0)]] Texture2D<float4> MetallicTexture;
[[vk::binding(2, 0)]] Texture2D<float4> RoughnessTexture;
[[vk::binding(3, 0)]] Texture2D<float4> NormalTexture;
[[vk::binding(4, 0)]] Texture2D<float4> OcclusionTexture;
[[vk::binding(5, 0)]] Texture2D<float4> EmissiveTexture;
[[vk::binding(6, 0)]] Texture2D<float4> DiffuseEnvironment;
[[vk::binding(7, 0)]] Texture2D<float4> SpecularEnvironment;
[[vk::binding(8, 0)]] Texture2D<float4> ShadowAtlas;

struct FVertexOutput
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
    float3 WorldPosition : TEXCOORD1;
    float3 Normal : TEXCOORD2;
    float4 Tangent : TEXCOORD3;
};

FVertexOutput TransformVertex(float3 Position, float2 UV, float3 Normal, float4 Tangent, float4x4 MVP, float4x4 ObjectToView)
{
    FVertexOutput Output;
    Output.Position = mul(MVP, float4(Position, 1.0));
    Output.UV = UV * Visual.Material.UV.xy + Visual.Material.UV.zw;
    Output.WorldPosition = mul(Visual.ViewToWorld, mul(ObjectToView, float4(Position, 1.0))).xyz;
    float3 X = mul((float3x3)ObjectToView, float3(1.0, 0.0, 0.0));
    float3 Y = mul((float3x3)ObjectToView, float3(0.0, 1.0, 0.0));
    float3 Z = mul((float3x3)ObjectToView, float3(0.0, 0.0, 1.0));
    float Sign = dot(X, cross(Y, Z)) < 0.0 ? -1.0 : 1.0;
    float3 VN = normalize((cross(Y, Z) * Normal.x + cross(Z, X) * Normal.y + cross(X, Y) * Normal.z) * Sign);
    Output.Normal = normalize(mul((float3x3)Visual.ViewToWorld, VN));
    Output.Tangent = float4(normalize(mul((float3x3)Visual.ViewToWorld, mul((float3x3)ObjectToView, Tangent.xyz))), Tangent.w * Sign);
    return Output;
}

[shader("vertex")]
FVertexOutput vertexMain(float3 Position : POSITION, float2 UV : TEXCOORD0, float3 Normal : NORMAL, float4 Tangent : TANGENT)
{
    return TransformVertex(Position, UV, Normal, Tangent, Transform.MVP, Transform.ObjectToView);
}

struct FInstanceInput
{
    float4 Clip0 : TEXCOORD1; float4 Clip1 : TEXCOORD2; float4 Clip2 : TEXCOORD3; float4 Clip3 : TEXCOORD4;
    float4 View0 : TEXCOORD5; float4 View1 : TEXCOORD6; float4 View2 : TEXCOORD7; float4 View3 : TEXCOORD8;
};

[shader("vertex")]
FVertexOutput instancedVertexMain(float3 Position : POSITION, float2 UV : TEXCOORD0, float3 Normal : NORMAL, float4 Tangent : TANGENT, FInstanceInput Instance)
{
    return TransformVertex(Position, UV, Normal, Tangent, transpose(float4x4(Instance.Clip0, Instance.Clip1, Instance.Clip2, Instance.Clip3)), transpose(float4x4(Instance.View0, Instance.View1, Instance.View2, Instance.View3)));
}

float Channel(float4 Sample, float Selection)
{
    int C = int(Selection);
    return C < 4 ? Sample[C] : dot(Sample.rgb, float3(0.2126, 0.7152, 0.0722));
}

float3 BRDF(float3 N, float3 V, float3 L, float3 Color, float Metallic, float Roughness)
{
    float3 H = normalize(V + L);
    float NoL = saturate(dot(N, L));
    float NoV = max(0.0001, saturate(dot(N, V)));
    float NoH = saturate(dot(N, H));
    float VoH = saturate(dot(V, H));
    float Alpha = max(0.0025, Roughness * Roughness);
    float A2 = Alpha * Alpha;
    float D = A2 / max(0.00001, PI * pow(NoH * NoH * (A2 - 1.0) + 1.0, 2.0));
    float GV = NoL * sqrt(NoV * NoV * (1.0 - A2) + A2);
    float GL = NoV * sqrt(NoL * NoL * (1.0 - A2) + A2);
    float Visibility = 0.5 / max(0.00001, GV + GL);
    float3 F0 = lerp(0.04, Color, Metallic);
    float3 F = F0 + (1.0 - F0) * pow(1.0 - VoH, 5.0);
    return ((1.0 - F) * (1.0 - Metallic) * Color / PI + D * Visibility * F) * NoL;
}

float3 RectLighting(FVisualLight Light, float3 Position, float3 N, float3 V, float3 Color, float Metallic, float Roughness)
{
    float3 Result = 0.0;
    float Area = max(0.0001, Light.RightWidth.w * Light.UpHeight.w);
    float3 Radiance = Light.ColorIntensity.rgb * Light.ColorIntensity.w / (PI * Area);
    // Area quadrature evaluates both diffuse and GGX across the one-sided emitter.
    for (int Y = 0; Y < 4; ++Y)
    for (int X = 0; X < 4; ++X)
    {
        float3 P = Light.PositionType.xyz + Light.RightWidth.xyz * ((float(X) + 0.5) / 4.0 - 0.5) * Light.RightWidth.w + Light.UpHeight.xyz * ((float(Y) + 0.5) / 4.0 - 0.5) * Light.UpHeight.w;
        float3 Delta = P - Position;
        float Distance2 = max(0.01, dot(Delta, Delta));
        float3 L = Delta / sqrt(Distance2);
        float CosEmitter = saturate(dot(-L, Light.DirectionRange.xyz));
        float Fade = pow(saturate(1.0 - pow(sqrt(Distance2) / Light.DirectionRange.w, 4.0)), 2.0);
        Result += BRDF(N, V, L, Color, Metallic, Roughness) * Radiance * (Area / 16.0) * CosEmitter * Fade / Distance2;
    }
    return Result;
}

[shader("fragment")]
float4 fragmentMain(FVertexOutput Input) : SV_Target
{
    uint Flags = uint(Visual.Material.TextureFlags.x);
    float4 Base = Visual.Material.BaseColor;
    if ((Flags & 1u) != 0u) Base *= BaseColorTexture.Sample(LinearSampler, Input.UV);
    if (Visual.Material.Channels1.z > 0.5) clip(Base.a - Visual.Material.Channels1.w);
    float Metallic = Visual.Material.Surface.x;
    float Roughness = Visual.Material.Surface.y;
    if ((Flags & 2u) != 0u) Metallic *= Channel(MetallicTexture.Sample(LinearSampler, Input.UV), Visual.Material.Channels0.y);
    if ((Flags & 4u) != 0u) Roughness *= Channel(RoughnessTexture.Sample(LinearSampler, Input.UV), Visual.Material.Channels0.z);
    Roughness = clamp(Roughness, 0.045, 1.0);
    float3 N = normalize(Input.Normal);
    float3 T = normalize(Input.Tangent.xyz - N * dot(N, Input.Tangent.xyz));
    float3 B = cross(N, T) * Input.Tangent.w;
    if ((Flags & 8u) != 0u)
    {
        float3 Map = NormalTexture.Sample(LinearSampler, Input.UV).xyz * 2.0 - 1.0;
        Map.xy *= Visual.Material.Surface.z;
        float3 MappedNormal = T * Map.x + B * Map.y + N * Map.z;
        N = dot(MappedNormal, MappedNormal) > 0.000001 ? normalize(MappedNormal) : N;
    }
    float AO = 1.0;
    if ((Flags & 16u) != 0u) AO = lerp(1.0, Channel(OcclusionTexture.Sample(LinearSampler, Input.UV), Visual.Material.Channels1.x), Visual.Material.Surface.w);
    float3 Emissive = Visual.Material.Emissive.rgb * Visual.Material.Emissive.w;
    if ((Flags & 32u) != 0u) Emissive *= EmissiveTexture.Sample(LinearSampler, Input.UV).rgb;
    float3 V = normalize(mul(Visual.ViewToWorld, float4(0.0, 0.0, 0.0, 1.0)).xyz - Input.WorldPosition);
    float3 Result = Emissive;
    for (int I = 0; I < int(Visual.Controls.x); ++I)
    {
        FVisualLight Light = Visual.Lights[I];
        if (int(Light.PositionType.w) == 1) continue;
        float Shadow = LightVisibility(ShadowAtlas, Light, Input.WorldPosition, N);
        if (int(Light.PositionType.w) == 4)
            Result += RectLighting(Light, Input.WorldPosition, N, V, Base.rgb, Metallic, Roughness) * Shadow;
        else
        {
            float3 L;
            float Attenuation = LightAttenuation(Light, Input.WorldPosition, L);
            Result += BRDF(N, V, L, Base.rgb, Metallic, Roughness) * Light.ColorIntensity.rgb * Light.ColorIntensity.w * Attenuation * Shadow;
        }
    }
    if (Visual.Sky.x > 0.0)
    {
        uint DiffuseWidth, DiffuseHeight;
        DiffuseEnvironment.GetDimensions(DiffuseWidth, DiffuseHeight);
        float2 DiffuseUV = DirectionUV(N);
        // Material maps repeat, but environment latitude must not blend the opposite poles.
        DiffuseUV.y = clamp(DiffuseUV.y, 0.5 / DiffuseHeight, 1.0 - 0.5 / DiffuseHeight);
        float3 Diffuse = DiffuseEnvironment.SampleLevel(LinearSampler, DiffuseUV, 0).rgb;
        float3 R = reflect(-V, N);
        float SpecularLod = Roughness * Visual.Sky.z;
        uint SpecularWidth, SpecularHeight, SpecularMips;
        SpecularEnvironment.GetDimensions(uint(ceil(SpecularLod)), SpecularWidth, SpecularHeight, SpecularMips);
        float2 SpecularUV = DirectionUV(R);
        SpecularUV.y = clamp(SpecularUV.y, 0.5 / SpecularHeight, 1.0 - 0.5 / SpecularHeight);
        float3 Specular = SpecularEnvironment.SampleLevel(LinearSampler, SpecularUV, SpecularLod).rgb;
        float NoV = saturate(dot(N, V));
        float3 F0 = lerp(0.04, Base.rgb, Metallic);
        float4 C = Roughness * float4(-1.0, -0.0275, -0.572, 0.022) + float4(1.0, 0.0425, 1.04, -0.04);
        float A004 = min(C.x * C.x, exp2(-9.28 * NoV)) * C.x + C.y;
        float2 AB = float2(-1.04, 1.04) * A004 + C.zw;
        Result += ((1.0 - Metallic) * Base.rgb * Diffuse / PI + Specular * (F0 * AB.x + AB.y)) * AO * Visual.Sky.x * Visual.SkyColor.rgb;
    }
    if (Visual.Controls.x == 0.0 && Visual.Sky.x == 0.0 && (Visual.Sky.w < 0.5 || Visual.Sky.w > 1.5))
        Result += Base.rgb * (0.35 + 0.65 * saturate(dot(N, normalize(mul((float3x3)Visual.ViewToWorld, float3(0.0, 0.5, -1.0))))));
    if (Visual.Sky.w < 0.5) return float4(Result / (1.0 + Result), 1.0);
    return float4(clamp(Result, 0.0, 65504.0), 1.0);
}
