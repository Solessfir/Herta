#include "../VisualLights.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> DepthTexture;
[[vk::binding(1, 0)]] Texture2D<float4> ShadowAtlas;
[[vk::binding(2, 0)]] Texture2D<float4> SpecularEnvironment;
[[vk::binding(3, 0)]] Texture2D<float4> LocalShadowAtlas;
[[vk::binding(384, 0)]] [format("rgba16f")] RWTexture2D<float4> Scattering;
// The scene depth at each texel centre, which the denoise and the full-resolution composite compare against.
[[vk::binding(385, 0)]] [format("r32f")] RWTexture2D<float> FogDepth;

// In-scattered light from one light at Position along the view Direction.
float3 FogLight(uint Index, float3 Position, float3 Direction)
{
    FVisualLight Light = Lights[Index];
    if (int(Light.PositionType.w) == 1) return 0.0;
    float3 L;
    float Attenuation = LightAttenuation(Light, Position, L);
    if (Attenuation <= 0.0) return 0.0;
    return Light.ColorIntensity.rgb * Light.ColorIntensity.w * Attenuation * FogPhase(-Direction, L) * LightVisibility(ShadowAtlas, LocalShadowAtlas, Light, Position, float3(0.0, 0.0, 0.0), false);
}

groupshared float3 AmbientShare[64];

// Sky light scattered toward the camera: the environment weighted by the phase function over a spherical Fibonacci set of 64 directions, from a mip
// blurred to about their spacing. It varies slowly with the view, so each thread of a group adds one direction for the group's central ray. Halving
// the zenith and nadir irradiance instead weighted the sky by |cos| and ignored anisotropy, which left fogged sky 7% brighter than the path tracer.
float3 GroupAmbient(uint Thread, uint2 Group, uint2 Size)
{
    float Y = 1.0 - (2.0 * Thread + 1.0) / 64.0;
    float Ring = sqrt(saturate(1.0 - Y * Y));
    float Phi = Thread * 2.39996323;
    float3 L = float3(Ring * cos(Phi), Y, Ring * sin(Phi));
    float3 Center = CameraRay((float2(Group * 8u) + 4.0) / float2(Size));
    AmbientShare[Thread] = Visual.Sky.x > 0.0 ? SpecularEnvironment.SampleLevel(LinearSampler, DirectionUV(L), 0.6 * Visual.Sky.z).rgb * FogPhase(-Center, L) : 0.0;
    for (uint Stride = 32; Stride > 0; Stride /= 2)
    {
        GroupMemoryBarrierWithGroupSync();
        if (Thread < Stride)
            AmbientShare[Thread] += AmbientShare[Thread + Stride];
    }

    GroupMemoryBarrierWithGroupSync();
    return AmbientShare[0] * (4.0 * PI / 64.0) * Visual.Sky.x * Visual.SkyColor.rgb;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID, uint3 Group : SV_GroupID, uint Thread : SV_GroupIndex)
{
    uint Width, Height;
    Scattering.GetDimensions(Width, Height);
    // Every thread joins the group's sum before any leaves.
    float3 Ambient = GroupAmbient(Thread, Group.xy, uint2(Width, Height));
    if (any(Id.xy >= uint2(Width, Height)))
        return;

    float2 Pixel = float2(Id.xy) + 0.5;
    float2 UV = Pixel / float2(Width, Height);
    FogDepth[Id.xy] = DepthTexture.Load(int3(UV * Visual.Viewport.zw, 0)).r;
    float Depth = DepthTexture.SampleLevel(LinearSampler, UV, 0).r;
    float3 Eye = mul(Visual.ViewToWorld, float4(0.0, 0.0, 0.0, 1.0)).xyz;
    float3 Direction = CameraRay(UV);
    float Distance = Depth > 0.000001 ? min(Visual.Fog.w, length(WorldPosition(UV, Depth) - Eye)) : Visual.Fog.w;
    int Steps = Visual.Controls.w < 0.0 ? 8 : int(Visual.Controls.w);
    float StepLength = Distance / max(1, Steps);
    float Transmittance = 1.0;
    float3 InScattering = 0.0;
    // With a few metres between samples, every pixel would test shadows at the same depths and shafts would show as slabs.
    // Interleaved gradient noise (Jimenez 2014) staggers the samples per pixel; the 3x3 denoise averages the dither out.
    float Jitter = frac(52.9829189 * frac(dot(Pixel, float2(0.06711056, 0.00583715))));
    for (int S = 0; S < Steps; ++S)
    {
        float3 Position = Eye + Direction * (float(S) + Jitter) * StepLength;
        float Density = FogDensity(Position);
        float SegmentT = exp(-Density * StepLength);
        float3 Incoming = Ambient;
        if (Visual.Controls.w > 0.0)
        {
            uint ClusterBase = LightClusterBase(UV * Visual.Viewport.zw, Position);
            // The first 32 lights get their own loop, as in the mesh shader.
            uint WordCount = LightWordCount();
            for (uint Mask = WordCount != 0 ? LightMaskWord(ClusterBase, 0) : 0; Mask != 0; Mask &= Mask - 1)
                Incoming += FogLight(firstbitlow(Mask), Position, Direction);
            for (uint Word = 1; Word < WordCount; ++Word)
            {
                for (uint Mask = LightMaskWord(ClusterBase, Word); Mask != 0; Mask &= Mask - 1)
                    Incoming += FogLight(Word * 32 + firstbitlow(Mask), Position, Direction);
            }
        }
        InScattering += Transmittance * (1.0 - SegmentT) * Visual.FogColor.rgb * Incoming;
        Transmittance *= SegmentT;
        if (Transmittance < 0.001) break;
    }
    Scattering[Id.xy] = float4(clamp(InScattering, 0.0, 65504.0), Transmittance);
}
