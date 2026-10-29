#include "VisualShared.slangh"
#include "VisualLights.slangh"
#include "Brdf.slangh"

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
[[vk::binding(13, 0)]] Texture2D<float4> LocalShadowAtlas;
// Screen-space shadows toward the sun that casts the cascades, written when Visual.Occlusion.y is set.
[[vk::binding(14, 0)]] Texture2D<float4> ContactShadow;
// Diffuse irradiance from global illumination at 1/16 scale, read when Visual.Occlusion.w is set; alpha marks pixels its probes covered.
[[vk::binding(15, 0)]] Texture2D<float4> GlobalIllumination;

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
};

// The previous frame's object-to-clip, from the second instance stream that only the velocity prepass binds.
struct FInstanceMotionInput
{
    float4 Previous0 : TEXCOORD9; float4 Previous1 : TEXCOORD10; float4 Previous2 : TEXCOORD11; float4 Previous3 : TEXCOORD12;
};

FVertexOutput TransformInstance(float3 Position, float2 UV, float3 Normal, float4 Tangent, FInstanceInput Instance)
{
    return TransformVertex(Position, UV, Normal, Tangent, transpose(float4x4(Instance.Clip0, Instance.Clip1, Instance.Clip2, Instance.Clip3)), transpose(float4x4(Instance.View0, Instance.View1, Instance.View2, Instance.View3)));
}

[shader("vertex")]
FVertexOutput instancedVertexMain(float3 Position : POSITION, float2 UV : TEXCOORD0, float3 Normal : NORMAL, float4 Tangent : TANGENT, FInstanceInput Instance)
{
    return TransformInstance(Position, UV, Normal, Tangent, Instance);
}

[shader("vertex")]
FVertexOutput instancedVelocityVertexMain(float3 Position : POSITION, float2 UV : TEXCOORD0, float3 Normal : NORMAL, float4 Tangent : TANGENT, FInstanceInput Instance, FInstanceMotionInput Motion)
{
    FVertexOutput Output = TransformInstance(Position, UV, Normal, Tangent, Instance);
    Output.PreviousClip = mul(transpose(float4x4(Motion.Previous0, Motion.Previous1, Motion.Previous2, Motion.Previous3)), float4(Position, 1.0));
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

    // The LTCs integrate cosine and GGX lobes, so rough diffuse loses only EON's s / t shaping and the edge tint is left out.
    float3 Radiance = Light.ColorIntensity.rgb * Light.ColorIntensity.w / (PI * max(0.0001, Light.RightWidth.w * Light.UpHeight.w)) * pow(saturate(1.0 - pow(Distance / Light.DirectionRange.w, 4.0)), 2.0) * LightVisibility(ShadowAtlas, LocalShadowAtlas, Light, Position, N);
    float3 Irradiance = Radiance * (PI * LtcQuad(Basis, Corners));
    Sum.Diffuse += Irradiance;
    Sum.DiffuseMultiple += Irradiance;
    float3 Specular = Radiance * LtcQuad(mul(InverseMatrix, Basis), Corners);
    Sum.Specular0 += Specular * Amplitude.x;
    Sum.Specular90 += Specular * Amplitude.y;
}

void ShadeLight(inout FLightSum Sum, uint Index, float3 Position, float3 N, float3 V, float Roughness, float DiffuseRoughness, float Contact)
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
    float Visibility = LightVisibility(ShadowAtlas, LocalShadowAtlas, Light, Position, N);
    // Only the cascaded sun has contact shadows.
    if (Type == 0 && Light.ConeShadow.w == 4.0) Visibility *= Contact;
    float3 DiffuseBefore = Sum.Diffuse;
    AddLight(Sum, Light.ColorIntensity.rgb * Light.ColorIntensity.w * Attenuation * Visibility, N, V, L, Roughness, DiffuseRoughness);
    // 4 V (1 - V) peaks halfway through the penumbra and vanishes in full light and full shadow.
    if (Visual.Occlusion.z > 1.0) Sum.Penumbra += (Sum.Diffuse - DiffuseBefore) * 4.0 * Visibility * (1.0 - Visibility);
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
    float Contact = Visual.Occlusion.y > 0.5 ? ContactShadow.Load(int3(int2(Input.Position.xy), 0)).r : 1.0;
    // The first 32 lights get their own loop, so scenes within one mask word never step through words.
    uint WordCount = LightWordCount();
    for (uint Mask = WordCount != 0 ? LightMaskWord(ClusterBase, 0) : 0; Mask != 0; Mask &= Mask - 1)
        ShadeLight(Sum, firstbitlow(Mask), Input.WorldPosition, N, V, Roughness, DiffuseRoughness, Contact);
    for (uint Word = 1; Word < WordCount; ++Word)
    {
        for (uint Mask = LightMaskWord(ClusterBase, Word); Mask != 0; Mask &= Mask - 1)
            ShadeLight(Sum, Word * 32 + firstbitlow(Mask), Input.WorldPosition, N, V, Roughness, DiffuseRoughness, Contact);
    }

    float NoV = saturate(dot(N, V));
    FSurface Surface = MakeSurface(SpecularAlbedo, Base.rgb, Metallic, Roughness, DiffuseRoughness, Visual.Material.Specular.rgb, NoV);
    Result += Surface.DiffuseSingle * Sum.Diffuse + Surface.DiffuseMultiple * Sum.DiffuseMultiple + max(Surface.Specular0 * Sum.Specular0 + Surface.Specular90 * Sum.Specular90 - Surface.SpecularF82 * Sum.SpecularF82, 0.0);
    if (Visual.Occlusion.z > 1.0)
    {
        // Saturation grows around the penumbra light's own luminance, and no channel drops below zero.
        float3 Penumbra = Surface.DiffuseSingle * Sum.Penumbra;
        float Luminance = dot(Penumbra, float3(0.2126, 0.7152, 0.0722));
        Result += max(Luminance + (Penumbra - Luminance) * Visual.Occlusion.z, 0.0) - Penumbra;
    }
    float4 Indirect = Visual.Occlusion.w > 0.5 ? GlobalIllumination.Load(int3(int2(Input.Position.xy), 0)) : 0.0;
    if (Visual.Sky.x > 0.0 || Indirect.a > 0.5)
    {
        // Global illumination already carries the sky's intensity, occluded by the scene, and light bounced off it.
        float3 Diffuse = Indirect.rgb * 16.0;
        if (Indirect.a <= 0.5)
        {
            uint DiffuseWidth, DiffuseHeight;
            DiffuseEnvironment.GetDimensions(DiffuseWidth, DiffuseHeight);
            float2 DiffuseUV = DirectionUV(DiffuseNormal);
            // Material maps repeat, but environment latitude must not blend the opposite poles.
            DiffuseUV.y = clamp(DiffuseUV.y, 0.5 / DiffuseHeight, 1.0 - 0.5 / DiffuseHeight);
            Diffuse = DiffuseEnvironment.SampleLevel(LinearSampler, DiffuseUV, 0).rgb * Visual.Sky.x * Visual.SkyColor.rgb;
        }

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
        Result += Surface.DiffuseReflectance * Diffuse / PI * DiffuseOcclusion + Specular * Surface.SpecularReflectance * SpecularOcclusion * Visual.Sky.x * Visual.SkyColor.rgb;
    }
    if (Visual.Sky.w < 0.5) return float4(Result / (1.0 + Result), 1.0);
    return float4(clamp(Result, 0.0, 65504.0), 1.0);
}
