#include "Fullscreen.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> DepthTexture;
[[vk::binding(1, 0)]] Texture2D<float4> ShadowAtlas;
[[vk::binding(2, 0)]] Texture2D<float4> DiffuseEnvironment;

[shader("fragment")]
float4 fragmentMain(FFullscreenOutput Input) : SV_Target
{
    float Depth = DepthTexture.SampleLevel(LinearSampler, Input.UV, 0).r;
    float3 Eye = mul(Visual.ViewToWorld, float4(0.0, 0.0, 0.0, 1.0)).xyz;
    float3 Direction = CameraRay(Input.UV);
    float Distance = Depth > 0.000001 ? min(Visual.Fog.w, length(WorldPosition(Input.UV, Depth) - Eye)) : Visual.Fog.w;
    int Steps = Visual.Controls.w < 0.0 ? 8 : int(Visual.Controls.w);
    float StepLength = Distance / max(1, Steps);
    float Transmittance = 1.0;
    float3 Scattering = 0.0;
    // Isotropic in-scattering needs the sky's mean radiance; the zenith and nadir irradiance pair recovers it for a smooth sky.
    float3 SkyIrradiance = DiffuseEnvironment.SampleLevel(LinearSampler, DirectionUV(float3(0.0, 1.0, 0.0)), 0).rgb + DiffuseEnvironment.SampleLevel(LinearSampler, DirectionUV(float3(0.0, -1.0, 0.0)), 0).rgb;
    float3 Ambient = SkyIrradiance * Visual.Sky.x * Visual.SkyColor.rgb / (2.0 * PI);
    // With a few metres between samples, every pixel would test shadows at the same depths and shafts would show as slabs.
    // Interleaved gradient noise (Jimenez 2014) staggers the samples per pixel; the composite's 3x3 upsample averages the dither out.
    float Jitter = frac(52.9829189 * frac(dot(Input.Position.xy, float2(0.06711056, 0.00583715))));
    for (int S = 0; S < Steps; ++S)
    {
        float3 Position = Eye + Direction * (float(S) + Jitter) * StepLength;
        // Constant below the fog height and exponential above it, so an open void beneath the level is not filled with opaque fog.
        float Density = Visual.Fog.x * exp(clamp(-Visual.Fog.y * (Position.y - Visual.FogColor.w), -8.0, 0.0));
        float SegmentT = exp(-Density * StepLength);
        float3 Incoming = Ambient;
        if (Visual.Controls.w > 0.0)
        {
            for (int I = 0; I < int(Visual.Controls.x); ++I)
            {
                FVisualLight Light = Visual.Lights[I];
                if (int(Light.PositionType.w) == 1) continue;
                float3 L;
                float Attenuation = LightAttenuation(Light, Position, L);
                float G = Visual.Fog.z;
                float Mu = dot(-Direction, L);
                float Phase = (1.0 - G * G) / (4.0 * PI * pow(max(0.002, 1.0 + G * G - 2.0 * G * Mu), 1.5));
                Incoming += Light.ColorIntensity.rgb * Light.ColorIntensity.w * Attenuation * Phase * LightVisibility(ShadowAtlas, Light, Position, float3(0.0, 0.0, 0.0));
            }
        }
        Scattering += Transmittance * (1.0 - SegmentT) * Visual.FogColor.rgb * Incoming;
        Transmittance *= SegmentT;
        if (Transmittance < 0.001) break;
    }
    return float4(clamp(Scattering, 0.0, 65504.0), Transmittance);
}
