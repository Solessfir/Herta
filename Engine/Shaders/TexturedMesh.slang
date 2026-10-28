#include "VisualShared.slangh"
#include "VisualLights.slangh"

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
[[vk::binding(9, 0)]] Texture2D<float4> LtcMatrix;
[[vk::binding(10, 0)]] Texture2D<float4> LtcAmplitude;
// View-space bent normal in rgb and visibility in a, from the GTAO compute passes.
[[vk::binding(11, 0)]] Texture2D<float4> ScreenOcclusion;
// GGX directional albedo terms A, B, and C by view cosine and roughness; see SpecularAlbedoTableSize in Visuals.h.
[[vk::binding(12, 0)]] Texture2D<float4> SpecularAlbedo;

struct FVertexOutput
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
    float3 WorldPosition : TEXCOORD1;
    float3 Normal : TEXCOORD2;
    float4 Tangent : TEXCOORD3;
    // Last frame's clip position, with w 0 when unknown.
    float4 PreviousClip : TEXCOORD4;
};

FVertexOutput TransformVertex(float3 Position, float2 UV, float3 Normal, float4 Tangent, float4x4 MVP, float4x4 ObjectToView)
{
    FVertexOutput Output;
    Output.PreviousClip = 0.0;
    // The depth prepass runs this same function; precise keeps the compiler from fusing it differently, so equal depth tests pass.
    precise float4 ClipPosition = mul(MVP, float4(Position, 1.0));
    Output.Position = ClipPosition;
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
    float4 Previous0 : TEXCOORD9; float4 Previous1 : TEXCOORD10; float4 Previous2 : TEXCOORD11; float4 Previous3 : TEXCOORD12;
};

[shader("vertex")]
FVertexOutput instancedVertexMain(float3 Position : POSITION, float2 UV : TEXCOORD0, float3 Normal : NORMAL, float4 Tangent : TANGENT, FInstanceInput Instance)
{
    FVertexOutput Output = TransformVertex(Position, UV, Normal, Tangent, transpose(float4x4(Instance.Clip0, Instance.Clip1, Instance.Clip2, Instance.Clip3)), transpose(float4x4(Instance.View0, Instance.View1, Instance.View2, Instance.View3)));
    Output.PreviousClip = mul(transpose(float4x4(Instance.Previous0, Instance.Previous1, Instance.Previous2, Instance.Previous3)), float4(Position, 1.0));
    return Output;
}

// The instanced depth prepass with motion vectors: the same cutout test as Shadow.slang, then this pixel's screen motion.
[shader("fragment")]
float2 velocityFragmentMain(FVertexOutput Input) : SV_Target
{
    if (Visual.Material.Channels1.z > 0.5)
    {
        float Alpha = Visual.Material.BaseColor.a;
        if ((uint(Visual.Material.TextureFlags.x) & 1u) != 0u)
            Alpha *= BaseColorTexture.Sample(LinearSampler, Input.UV).a;

        clip(Alpha - Visual.Material.Channels1.w);
    }

    if (Input.PreviousClip.w <= 0.0)
        return MotionVectorUnknown;

    float2 Previous = Input.PreviousClip.xy / Input.PreviousClip.w * float2(0.5, -0.5) + 0.5;
    return Input.Position.xy * Visual.Viewport.xy - Previous;
}

float Channel(float4 Sample, float Selection)
{
    int C = int(Selection);
    return C < 4 ? Sample[C] : dot(Sample.rgb, float3(0.2126, 0.7152, 0.0722));
}

// EON, energy-preserving Oren-Nayar (Portsmouth, Kutz, and Hill 2024, as adopted by OpenPBR), with the fast albedo fit. Roughness 0 is Lambert.
static const float EonConstant1 = 0.5 - 2.0 / (3.0 * PI);
static const float EonConstant2 = 2.0 / 3.0 - 28.0 / (15.0 * PI);

float EonFujiiAlbedo(float Mu, float Roughness)
{
    float Complement = 1.0 - Mu;
    float GOverPi = Complement * (0.0571085289 + Complement * (0.491881867 + Complement * (-0.332181442 + Complement * 0.0714429953)));
    return (1.0 + Roughness * GOverPi) / (1.0 + EonConstant1 * Roughness);
}

float3 EonMultipleScatterAlbedo(float3 Rho, float AverageAlbedo)
{
    return Rho * Rho * AverageAlbedo / (1.0 - Rho * (1.0 - AverageAlbedo));
}


// Hemispherical reflectance seen from Mu, which stands in for the lobe under environment and area lights.
float3 EonAlbedo(float3 Rho, float Roughness, float Mu)
{
    float Albedo = EonFujiiAlbedo(Mu, Roughness);
    float AverageAlbedo = (1.0 + EonConstant2 * Roughness) / (1.0 + EonConstant1 * Roughness);
    return Rho * Albedo + EonMultipleScatterAlbedo(Rho, AverageAlbedo) * (1.0 - Albedo);
}

// Everything about a pixel's material that does not depend on the light, applied once to the light sums and the environment.
struct FSurface
{
    // EON per light is DiffuseSingle * (1 + DiffuseRoughness * s / t) + DiffuseMultiple * (1 - E(NoL)), already scaled by what the specular layer leaves and divided by pi.
    float3 DiffuseSingle;
    float3 DiffuseMultiple;
    float DiffuseRoughness;
    // Hemispherical diffuse reflectance towards the view, which stands in for the lobe under environment and rect lights.
    float3 DiffuseReflectance;
    float Roughness;
    // Fresnel times multiple-scattering compensation: Specular0 + Specular90 * (1 - VoH)^5 - SpecularF82 * VoH * (1 - VoH)^6.
    float3 Specular0;
    float3 Specular90;
    float3 SpecularF82;
    // The specular lobe's directional albedo with compensation, for environment light.
    float3 SpecularReflectance;
};

FSurface MakeSurface(float3 BaseColor, float Metallic, float Roughness, float DiffuseRoughness, float3 EdgeColor, float NoV)
{
    const float TableSize = 32.0;
    float3 Table = SpecularAlbedo.SampleLevel(LinearSampler, float2(NoV, Roughness) * ((TableSize - 1.0) / TableSize) + 0.5 / TableSize, 0).xyz;
    float Single = max(Table.x + Table.y, 1e-4);
    float3 F0 = lerp(0.04, BaseColor, Metallic);
    // F82-tint (Hoffman 2019, as in OpenPBR): at cos 1/7, about 82 degrees, metal reflectance becomes EdgeColor times Schlick's.
    const float MuBar = 1.0 / 7.0;
    float3 SchlickBar = BaseColor + (1.0 - BaseColor) * pow(1.0 - MuBar, 5.0);
    // EdgeColor and DiffuseRoughness are per material, so skipping their unused defaults never diverges.
    float3 F82 = 0.0;
    [branch]
    if (any(EdgeColor < 1.0))
        F82 = Metallic * SchlickBar * (1.0 - EdgeColor) / (MuBar * pow(1.0 - MuBar, 6.0));

    // Single-scattering GGX loses energy as roughness grows; Turquin's (2019) scale restores it.
    float3 Compensation = 1.0 + F0 * (1.0 / Single - 1.0);
    FSurface Surface;
    Surface.Roughness = Roughness;
    Surface.Specular0 = Compensation * F0;
    Surface.Specular90 = Compensation * (1.0 - F0);
    Surface.SpecularF82 = Compensation * F82;
    Surface.SpecularReflectance = max(Surface.Specular0 * Table.x + Compensation * Table.y - Surface.SpecularF82 * Table.z, 0.0);
    // OpenPBR's albedo scaling: diffuse gets what the dielectric specular layer does not reflect, so the layers sum to at most one.
    float DielectricAlbedo = (0.04 * Table.x + Table.y) * (1.0 + 0.04 * (1.0 / Single - 1.0));
    float DiffuseWeight = (1.0 - Metallic) * saturate(1.0 - DielectricAlbedo);
    Surface.DiffuseRoughness = DiffuseRoughness;
    Surface.DiffuseSingle = DiffuseWeight / PI * BaseColor;
    Surface.DiffuseMultiple = 0.0;
    Surface.DiffuseReflectance = DiffuseWeight * BaseColor;
    [branch]
    if (DiffuseRoughness > 0.0)
    {
        float EonScale = 1.0 / (1.0 + EonConstant1 * DiffuseRoughness);
        float AverageAlbedo = EonScale * (1.0 + EonConstant2 * DiffuseRoughness);
        Surface.DiffuseSingle *= EonScale;
        Surface.DiffuseMultiple = DiffuseWeight / PI * EonMultipleScatterAlbedo(BaseColor, AverageAlbedo) * max(1e-7, 1.0 - EonFujiiAlbedo(NoV, DiffuseRoughness)) / max(1e-7, 1.0 - AverageAlbedo);
        Surface.DiffuseReflectance = DiffuseWeight * EonAlbedo(BaseColor, DiffuseRoughness, NoV);
    }
    return Surface;
}

// What the lights deliver before the material applies. The pass is register-limited, and with material terms in the light loop the driver recomputed them for every light; these sums keep the loop free of them.
struct FLightSum
{
    // Irradiance, scaled by EON's 1 + r * s / t for rough diffuse (Portsmouth, Kutz, and Hill 2024); the material multiplies by FSurface.DiffuseSingle.
    float3 Diffuse;
    // Irradiance times EON's 1 - E(NoL), for FSurface.DiffuseMultiple.
    float3 DiffuseMultiple;
    // The GGX lobe, and the same times (1 - VoH)^5 and VoH * (1 - VoH)^6, for the Fresnel terms in FSurface.
    float3 Specular0;
    float3 Specular90;
    float3 SpecularF82;
};

void AddLight(inout FLightSum Sum, float3 Radiance, float3 N, float3 V, float3 L, float Roughness, float DiffuseRoughness)
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
    float3 Lit = Radiance * NoL;
    float DiffuseScale = 1.0;
    // Diffuse roughness is per material, so this never diverges.
    [branch]
    if (DiffuseRoughness > 0.0)
    {
        float S = dot(L, V) - NoL * NoV;
        DiffuseScale += DiffuseRoughness * (S > 0.0 ? S / max(NoL, NoV) : S);
        Sum.DiffuseMultiple += Lit * max(1e-7, 1.0 - EonFujiiAlbedo(NoL, DiffuseRoughness));
    }

    Sum.Diffuse += Lit * DiffuseScale;
    float3 Specular = Lit * D * Visibility;
    float C = 1.0 - VoH;
    float C5 = C * C * C * C * C;
    Sum.Specular0 += Specular;
    Sum.Specular90 += Specular * C5;
    Sum.SpecularF82 += Specular * (VoH * C5 * C);
}

// Linearly transformed cosines (Heitz et al. 2016), following three.js: the edge integral uses a rational fit of theta / sin(theta) / 2pi.
float3 LtcEdge(float3 A, float3 B)
{
    float X = dot(A, B);
    float Y = abs(X);
    float Fit = (0.8543985 + (0.4965155 + 0.0145206 * Y) * Y) / (3.4175940 + (4.1616724 + Y) * Y);
    return cross(A, B) * (X > 0.0 ? Fit : 0.5 * rsqrt(max(1.0 - X * X, 1e-7)) - Fit);
}

// Integrates the clamped cosine, after Transform, over a quad; an emitter filling the hemisphere gives 1.
float LtcQuad(float3x3 Transform, float3 Corners[4])
{
    float3 Projected[4];
    for (int I = 0; I < 4; ++I)
    {
        Projected[I] = normalize(mul(Transform, Corners[I]));
    }

    float3 FormFactor = LtcEdge(Projected[0], Projected[1]) + LtcEdge(Projected[1], Projected[2]) + LtcEdge(Projected[2], Projected[3]) + LtcEdge(Projected[3], Projected[0]);
    // A sphere with the same vector form factor approximates clipping the quad at the horizon.
    float Length = length(FormFactor);
    return max((Length * Length + FormFactor.z) / (Length + 1.0), 0.0);
}

void RectLighting(inout FLightSum Sum, FVisualLight Light, float3 Position, float3 N, float3 V, float Roughness)
{
    float3 FromCenter = Position - Light.PositionType.xyz;
    float3 Right = Light.RightWidth.xyz * (0.5 * Light.RightWidth.w);
    float3 Up = Light.UpHeight.xyz * (0.5 * Light.UpHeight.w);
    // Range is measured from the emitter's surface, so long panels light along their whole length.
    float3 Nearest = Light.RightWidth.xyz * clamp(dot(FromCenter, Light.RightWidth.xyz), -0.5 * Light.RightWidth.w, 0.5 * Light.RightWidth.w) + Light.UpHeight.xyz * clamp(dot(FromCenter, Light.UpHeight.xyz), -0.5 * Light.UpHeight.w, 0.5 * Light.UpHeight.w);
    float Distance = length(FromCenter - Nearest);
    if (dot(FromCenter, Light.DirectionRange.xyz) <= 0.0 || Distance >= Light.DirectionRange.w) return;

    // The corners must wind so their normal faces the lit side; mirroring Right keeps the same quad.
    if (dot(cross(-Right, Up), Light.DirectionRange.xyz) < 0.0) Right = -Right;
    float3 Corners[4] = { Right - Up - FromCenter, -Right - Up - FromCenter, -Right + Up - FromCenter, Right + Up - FromCenter };

    // Isotropic GGX only needs some tangent when V lies along N.
    float3 Tangent = V - N * dot(V, N);
    Tangent = dot(Tangent, Tangent) > 1e-8 ? normalize(Tangent) : normalize(cross(N, abs(N.x) < 0.9 ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0)));
    float3x3 Basis = float3x3(Tangent, -cross(N, Tangent), N);
    float2 UV = float2(Roughness, sqrt(1.0 - saturate(dot(N, V)))) * (63.0 / 64.0) + 0.5 / 64.0;
    float4 Inverse = LtcMatrix.SampleLevel(LinearSampler, UV, 0);
    float2 Amplitude = LtcAmplitude.SampleLevel(LinearSampler, UV, 0).xy;
    float3x3 InverseMatrix = float3x3(Inverse.x, 0.0, Inverse.z, 0.0, 1.0, 0.0, Inverse.y, 0.0, Inverse.w);

    // The LTCs integrate cosine and GGX lobes, so rough diffuse keeps only EON's single-scattering scale and the edge tint is left out.
    float3 Radiance = Light.ColorIntensity.rgb * Light.ColorIntensity.w / (PI * max(0.0001, Light.RightWidth.w * Light.UpHeight.w)) * pow(saturate(1.0 - pow(Distance / Light.DirectionRange.w, 4.0)), 2.0) * LightVisibility(ShadowAtlas, Light, Position, N);
    Sum.Diffuse += Radiance * (PI * LtcQuad(Basis, Corners));
    float3 Specular = Radiance * LtcQuad(mul(InverseMatrix, Basis), Corners);
    Sum.Specular0 += Specular * Amplitude.x;
    Sum.Specular90 += Specular * Amplitude.y;
}

void ShadeLight(inout FLightSum Sum, uint Index, float3 Position, float3 N, float3 V, float Roughness, float DiffuseRoughness)
{
    FVisualLight Light = Lights[Index];
    int Type = int(Light.PositionType.w);
    if (Type == 1) return;
    if (Type == 4)
    {
        RectLighting(Sum, Light, Position, N, V, Roughness);
        return;
    }

    float3 L;
    float Attenuation = LightAttenuation(Light, Position, L);
    // Shadow filtering costs more than the lighting, so pixels out of range, outside the cone, or facing away skip it.
    if (Attenuation <= 0.0 || dot(N, L) <= 0.0) return;
    AddLight(Sum, Light.ColorIntensity.rgb * Light.ColorIntensity.w * Attenuation * LightVisibility(ShadowAtlas, Light, Position, N), N, V, L, Roughness, DiffuseRoughness);
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
    float3 DiffuseNormal = N;
    if (Visual.Occlusion.x > 0.5)
    {
        float4 Screen = ScreenOcclusion.Load(int3(int2(Input.Position.xy), 0));
        AO *= Screen.a;
        // The bent normal comes from depth and lacks normal-map detail, so it bends the shading normal by its offset from the surface normal.
        float3 Bent = mul((float3x3)Visual.ViewToWorld, Screen.xyz * 2.0 - 1.0);
        if (dot(Bent, Bent) > 0.01)
            DiffuseNormal = normalize(N + normalize(Bent) - normalize(Input.Normal));
    }
    float3 Emissive = Visual.Material.Emissive.rgb * Visual.Material.Emissive.w;
    if ((Flags & 32u) != 0u) Emissive *= EmissiveTexture.Sample(LinearSampler, Input.UV).rgb;
    float3 V = normalize(mul(Visual.ViewToWorld, float4(0.0, 0.0, 0.0, 1.0)).xyz - Input.WorldPosition);
    float3 Result = Emissive;
    if (Visual.Controls.x == 0.0 && Visual.Sky.x == 0.0 && (Visual.Sky.w < 0.5 || Visual.Sky.w > 1.5))
        Result += Base.rgb * (0.35 + 0.65 * saturate(dot(N, normalize(mul((float3x3)Visual.ViewToWorld, float3(0.0, 0.5, -1.0))))));
    // Lights add up without the material, which applies once afterwards; see FLightSum.
    float DiffuseRoughness = Visual.Material.Specular.w;
    FLightSum Sum = (FLightSum)0;
    uint ClusterBase = LightClusterBase(Input.Position.xy, Input.WorldPosition);
    // The first 32 lights get their own loop, so scenes within one mask word never step through words.
    uint WordCount = LightWordCount();
    for (uint Mask = WordCount != 0 ? LightMaskWord(ClusterBase, 0) : 0; Mask != 0; Mask &= Mask - 1)
        ShadeLight(Sum, firstbitlow(Mask), Input.WorldPosition, N, V, Roughness, DiffuseRoughness);
    for (uint Word = 1; Word < WordCount; ++Word)
    {
        for (uint Mask = LightMaskWord(ClusterBase, Word); Mask != 0; Mask &= Mask - 1)
            ShadeLight(Sum, Word * 32 + firstbitlow(Mask), Input.WorldPosition, N, V, Roughness, DiffuseRoughness);
    }

    float NoV = saturate(dot(N, V));
    FSurface Surface = MakeSurface(Base.rgb, Metallic, Roughness, DiffuseRoughness, Visual.Material.Specular.rgb, NoV);
    Result += Surface.DiffuseSingle * Sum.Diffuse + Surface.DiffuseMultiple * Sum.DiffuseMultiple + max(Surface.Specular0 * Sum.Specular0 + Surface.Specular90 * Sum.Specular90 - Surface.SpecularF82 * Sum.SpecularF82, 0.0);
    if (Visual.Sky.x > 0.0)
    {
        uint DiffuseWidth, DiffuseHeight;
        DiffuseEnvironment.GetDimensions(DiffuseWidth, DiffuseHeight);
        float2 DiffuseUV = DirectionUV(DiffuseNormal);
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
        // Jimenez et al.'s multi-bounce fit returns light that bright surfaces would reflect back into their own creases.
        float3 Albedo = (1.0 - Metallic) * Base.rgb;
        float3 BounceA = 2.0404 * Albedo - 0.3324;
        float3 BounceB = -4.7951 * Albedo + 0.6417;
        float3 BounceC = 2.7552 * Albedo + 0.6903;
        float3 DiffuseOcclusion = max(AO, ((AO * BounceA + BounceB) * AO + BounceC) * AO);
        // Lagarde's specular occlusion: grazing and rough reflections see more of the occluded hemisphere.
        float SpecularOcclusion = saturate(pow(NoV + AO, exp2(-16.0 * Roughness - 1.0)) - 1.0 + AO);
        Result += (Surface.DiffuseReflectance * Diffuse / PI * DiffuseOcclusion + Specular * Surface.SpecularReflectance * SpecularOcclusion) * Visual.Sky.x * Visual.SkyColor.rgb;
    }
    if (Visual.Sky.w < 0.5) return float4(Result / (1.0 + Result), 1.0);
    return float4(clamp(Result, 0.0, 65504.0), 1.0);
}
