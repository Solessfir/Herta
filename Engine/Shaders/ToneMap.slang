#include "Fullscreen.slangh"
[[vk::binding(0, 0)]] Texture2D<float4> SceneColor;
[[vk::binding(1, 0)]] Texture2D<float4> AutoExposure;

// GT7 tone mapping, ported from Polyphony Digital's MIT-licensed reference in External/gt7.
// It works in linear Rec.2020 where 1.0 is 100 cd/m2. SDR output maps the 250 cd/m2 paper white to display 1.0, so HDR output only needs a different peak.
static const float PaperWhite = 2.5;
static const float CurveAlpha = 0.25;
static const float CurveMidPoint = 0.538;
static const float CurveLinearSection = 0.444;
static const float CurveToeStrength = 1.28;
static const float BlendRatio = 0.6;
static const float FadeStart = 0.98;
static const float FadeEnd = 1.16;

static const float3x3 Rec709ToRec2020 = float3x3(
    0.627404, 0.329283, 0.043313,
    0.069097, 0.919540, 0.011362,
    0.016391, 0.088013, 0.895595);
static const float3x3 Rec2020ToRec709 = float3x3(
    1.660491, -0.587641, -0.072850,
    -0.124550, 1.132900, -0.008349,
    -0.018151, -0.100579, 1.118730);

float GtSmoothStep(float X, float Edge0, float Edge1)
{
    float T = saturate((X - Edge0) / (Edge1 - Edge0));
    return T * T * (3.0 - 2.0 * T);
}

float GtCurve(float X)
{
    float K = (CurveLinearSection - 1.0) / (CurveAlpha - 1.0);
    float A = PaperWhite * CurveLinearSection + PaperWhite * K;
    float B = -PaperWhite * K * exp(CurveLinearSection / K);
    float C = -1.0 / (K * PaperWhite);
    if (X < 0.0)
    {
        return 0.0;
    }

    if (X >= CurveLinearSection * PaperWhite)
    {
        return A + B * exp(X * C);
    }

    float WeightLinear = GtSmoothStep(X, 0.0, CurveMidPoint);
    float Toe = CurveMidPoint * pow(X / CurveMidPoint, CurveToeStrength);
    return (1.0 - WeightLinear) * Toe + WeightLinear * X;
}

float3 PqEncode(float3 Value)
{
    float3 Y = pow(max(Value, 0.0) * (100.0 / 10000.0), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * Y) / (1.0 + 18.6875 * Y), 78.84375);
}

float3 PqDecode(float3 Signal)
{
    float3 Np = pow(saturate(Signal), 1.0 / 78.84375);
    float3 L = max(Np - 0.8359375, 0.0) / (18.8515625 - 18.6875 * Np);
    return pow(L, 1.0 / 0.1593017578125) * (10000.0 / 100.0);
}

float3 Rec2020ToICtCp(float3 Rgb)
{
    float3 Lms = float3(dot(Rgb, float3(1688.0, 2146.0, 262.0)), dot(Rgb, float3(683.0, 2951.0, 462.0)), dot(Rgb, float3(99.0, 309.0, 3688.0))) / 4096.0;
    float3 Pq = PqEncode(Lms);
    return float3(dot(Pq, float3(2048.0, 2048.0, 0.0)), dot(Pq, float3(6610.0, -13613.0, 7003.0)), dot(Pq, float3(17933.0, -17390.0, -543.0))) / 4096.0;
}

float3 ICtCpToRec2020(float3 ICtCp)
{
    float3 Pq = float3(dot(ICtCp, float3(1.0, 0.00860904, 0.11103)), dot(ICtCp, float3(1.0, -0.00860904, -0.11103)), dot(ICtCp, float3(1.0, 0.560031, -0.320627)));
    float3 Lms = PqDecode(Pq);
    return max(float3(dot(Lms, float3(3.43661, -2.50645, 0.0698454)), dot(Lms, float3(-0.79133, 1.9836, -0.192271)), dot(Lms, float3(-0.0259499, -0.0989137, 1.12486))), 0.0);
}

float3 GtToneMap(float3 Rgb)
{
    float3 Ucs = Rec2020ToICtCp(Rgb);
    float3 Skewed = float3(GtCurve(Rgb.r), GtCurve(Rgb.g), GtCurve(Rgb.b));
    float3 SkewedUcs = Rec2020ToICtCp(Skewed);

    // Highlights near the display peak fade towards white instead of clipping to a skewed hue.
    float TargetIntensity = Rec2020ToICtCp(PaperWhite.xxx).x;
    float ChromaScale = 1.0 - GtSmoothStep(Ucs.x / TargetIntensity, FadeStart, FadeEnd);
    float3 Scaled = ICtCpToRec2020(float3(SkewedUcs.x, Ucs.yz * ChromaScale));
    return min(lerp(Skewed, Scaled, BlendRatio), PaperWhite) / PaperWhite;
}

[shader("fragment")]
float4 fragmentMain(FFullscreenOutput Input) : SV_Target
{
    float Scale = Visual.Exposure.x > 0.5 ? 1.0 / (1.2 * exp2(AutoExposure.Load(int3(0, 0, 0)).r)) : Visual.Controls.z;
    float3 Exposed = max(0.0, SceneColor.SampleLevel(LinearSampler, Input.UV, 0).rgb * Scale);

    // Exposed 1.0 is display white, which GT7 places at paper white.
    float3 Mapped = GtToneMap(mul(Rec709ToRec2020, Exposed * PaperWhite));
    return float4(saturate(mul(Rec2020ToRec709, Mapped)), 1.0);
}
