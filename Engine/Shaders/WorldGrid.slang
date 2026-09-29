struct FVertexOutput
{
    float4 Position : SV_Position;
    float2 WorldXZ : TEXCOORD0;
    float2 CenterXZ : TEXCOORD1;
};

[shader("vertex")]
FVertexOutput vertexMain(float4 Position : POSITION, float4 Color : COLOR0)
{
    FVertexOutput Output;
    Output.Position = Position;
    Output.WorldXZ = Color.xy;
    Output.CenterXZ = Color.zw;
    return Output;
}

float gridCoverage(float2 WorldXZ, float2 Footprint, float Spacing)
{
    float2 Distance = abs(frac(WorldXZ / Spacing + 0.5) - 0.5) * Spacing;
    float2 Line = 1.0 - smoothstep(0.0, Footprint, Distance);
    float Density = max(Footprint.x, Footprint.y) / Spacing;
    float DensityFade = 1.0 - smoothstep(0.25, 0.9, Density);
    return max(Line.x, Line.y) * DensityFade;
}

[shader("fragment")]
float4 fragmentMain(FVertexOutput Input) : SV_Target
{
    float2 Footprint = max(fwidth(Input.WorldXZ), 0.0001);
    float MinorDensityFade = 1.0 - smoothstep(0.15, 0.6, max(Footprint.x, Footprint.y));
    float Minor = gridCoverage(Input.WorldXZ, Footprint, 1.0) * MinorDensityFade;
    float Major = gridCoverage(Input.WorldXZ, Footprint, 5.0);
    float Fade = 1.0 - smoothstep(50.0, 200.0, length(Input.WorldXZ - Input.CenterXZ));
    float Alpha = max(Minor * (18.0 / 255.0), Major * (42.0 / 255.0)) * Fade;
    // Linearized #8c929a for the sRGB scene target.
    return float4(0.26225, 0.28744, 0.32314, Alpha);
}
