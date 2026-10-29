#include "RayTracing.slangh"

// The reference path tracer: one sample per pixel per dispatch, accumulated until the view, scene, or settings change.
// It shares Brdf.slangh, light units, and Rect light shapes with raster shading, so converged images measure what raster approximates.
[[vk::binding(384, 0)]] [format("rgba32f")] RWTexture2D<float4> Accumulation;
[[vk::binding(385, 0)]] [format("rgba16f")] RWTexture2D<float4> Output;

// Visual.Material carries the dispatch: x is the sample index, y the bounce limit, and z the angle one pixel subtends.
[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID)
{
    if (any(Id.xy >= uint2(Visual.Viewport.zw)))
        return;

    uint SampleIndex = uint(Visual.Material.BaseColor.x);
    uint MaximumBounces = uint(Visual.Material.BaseColor.y);
    float PixelAngle = Visual.Material.BaseColor.z;
    FRandom Random;
    Random.State = PcgHash(Id.x + PcgHash(Id.y + PcgHash(SampleIndex)));

    float2 UV = (float2(Id.xy) + float2(Random.Next(), Random.Next())) * Visual.Viewport.xy;
    float3 Origin = mul(Visual.ViewToWorld, float4(0.0, 0.0, 0.0, 1.0)).xyz;
    float3 Direction = CameraRay(UV);
    float3 Radiance = 0.0;
    float3 Throughput = 1.0;
    float ConeWidth = 0.0;
    float ConeSpread = PixelAngle;
    float3 PreviousPosition = 0.0;
    float3 PreviousNormal = 0.0;
    float PreviousPdf = 0.0;
    float PreviousWeightSum = 0.0;

    for (uint Bounce = 0; Bounce <= MaximumBounces; ++Bounce)
    {
        FHit Hit;
        bool bHit = TraceScene(Origin, Direction, 1e30, Hit);
        float Limit = bHit ? Hit.Distance : 1e30;
        if (Bounce > 0)
            Radiance += Throughput * HitLights(PreviousPosition, Origin, PreviousNormal, Direction, Limit, PreviousPdf, PreviousWeightSum);

        if (!bHit)
        {
            Radiance += Throughput * (Bounce == 0 ? BackgroundRadiance(Direction) : EnvironmentRadiance(Direction));
            break;
        }

        ConeWidth += ConeSpread * Hit.Distance;
        FSurfacePoint Point = ShadeHit(Hit, Direction, ConeWidth);
        float3 V = -Direction;
        Radiance += Throughput * Point.Material.Emissive;

        // Unlit levels in the studio preview get the same fixed key light as raster shading.
        if (Bounce == 0 && Visual.Sky.w > 1.5 && LightCount() == 0 && Visual.Sky.x == 0.0)
        {
            float3 Key = normalize(mul((float3x3)Visual.ViewToWorld, float3(0.0, 0.5, -1.0)));
            Radiance += Point.Material.Base.rgb * (0.35 + 0.65 * saturate(dot(Point.Material.Normal, Key)));
            break;
        }

        if (Bounce == MaximumBounces)
            break;

        float3 N = Point.Material.Normal;
        float NoV = saturate(dot(N, V));
        FSurface Surface = MakeSurface(SpecularAlbedo, Point.Material.Base.rgb, Point.Material.Metallic, Point.Material.Roughness, Point.Material.DiffuseRoughness, Point.Material.EdgeColor, NoV);
        float SpecularChance = SpecularProbability(Surface);
        float WeightSum;
        Radiance += Throughput * SampleLights(Point, Surface, V, SpecularChance, true, Random, WeightSum);

        float Alpha = GgxAlpha(Point.Material.Roughness);
        bool bSpecular = Random.Next() < SpecularChance;
        float3 L;
        if (bSpecular)
        {
            L = SampleGgx(V, N, Alpha, float2(Random.Next(), Random.Next()));
        }
        else
        {
            float Phi = 2.0 * PI * Random.Next();
            float R = sqrt(Random.Next());
            L = TangentToWorld(float3(R * cos(Phi), R * sin(Phi), sqrt(saturate(1.0 - R * R))), N);
        }

        float Pdf = BrdfPdf(V, N, L, Alpha, SpecularChance);
        if (Pdf <= 0.0 || dot(Point.GeometricNormal, L) <= 0.0)
            break;

        Throughput *= EvaluateBrdf(Surface, Point.Material, N, V, L) / Pdf;
        if (Bounce >= 2)
        {
            float Survival = clamp(max(Throughput.r, max(Throughput.g, Throughput.b)), 0.05, 0.95);
            if (Random.Next() >= Survival)
                break;
            Throughput /= Survival;
        }

        ConeSpread += bSpecular ? 2.0 * Alpha : 1.0;
        PreviousPosition = Point.Position;
        PreviousNormal = N;
        PreviousPdf = Pdf;
        PreviousWeightSum = WeightSum;
        Origin = Point.Position + Point.GeometricNormal * RayOffset;
        Direction = L;
    }

    if (any(isnan(Radiance)) || any(isinf(Radiance)))
        Radiance = 0.0;

    float4 Sum = SampleIndex == 0 ? float4(Radiance, 1.0) : Accumulation[Id.xy] + float4(Radiance, 1.0);
    Accumulation[Id.xy] = Sum;
    Output[Id.xy] = float4(min(Sum.rgb / Sum.w, 65504.0), 1.0);
}
