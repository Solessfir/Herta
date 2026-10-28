// Bend Studio's screen-space shadows, vendored in External/BendScreenSpaceShadows and converted for Slang there. Each wave marches one shared ray toward the sun through
// the depth buffer, so a group reads its depth once for 64 pixels. They catch contact and fine detail the shadow cascades are too coarse for.
#define WAVE_SIZE 64
#define SAMPLE_COUNT 60
#define HARD_SHADOW_SAMPLES 4
#define FADE_OUT_SAMPLES 8
#include "../../../External/BendScreenSpaceShadows/bend_sss_gpu.slangh"

struct FContactShadowUniforms
{
    float4 LightCoordinate;
    int4 WaveOffset;
    float4 InverseSize;
};

[[vk::binding(64, 0)]] ConstantBuffer<FContactShadowUniforms> Contact;
[[vk::binding(0, 0)]] Texture2D<float> SceneDepth;
// Point sampling with a zero border, which is the far plane in reversed depth, so nothing off screen casts a shadow.
[[vk::binding(128, 0)]] SamplerState PointBorderSampler;
[[vk::binding(384, 0)]] [format("r8")] RWTexture2D<float> ContactShadow;

[shader("compute")]
[numthreads(WAVE_SIZE, 1, 1)]
void computeMain(uint3 Group : SV_GroupID, uint Thread : SV_GroupThreadID)
{
    DispatchParameters Parameters;
    Parameters.SetDefaults();
    Parameters.LightCoordinate = Contact.LightCoordinate;
    Parameters.WaveOffset = Contact.WaveOffset.xy;
    // Reversed depth: the far plane is 0 and the near plane 1.
    Parameters.FarDepthValue = 0.0;
    Parameters.NearDepthValue = 1.0;
    Parameters.InvDepthTextureSize = Contact.InverseSize.xy;
    // Sky pixels sit at the far plane and receive no shadow, so waves that are all sky stop before marching.
    Parameters.UseEarlyOut = true;
    Parameters.DepthBounds = float2(1e-7, 1.0);
    Parameters.DepthTexture = SceneDepth;
    Parameters.OutputTexture = ContactShadow;
    Parameters.PointBorderSampler = PointBorderSampler;
    WriteScreenSpaceShadow(Parameters, int3(Group), int(Thread));
}
