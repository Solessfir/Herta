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
    float DensityFade = 1.0 - smoothstep(Spacing * 0.5, Spacing, max(Footprint.x, Footprint.y));
    return max(Line.x, Line.y) * DensityFade;
}

[shader("fragment")]
float4 fragmentMain(FVertexOutput Input) : SV_Target
{
    float2 Footprint = max(fwidth(Input.WorldXZ), 0.0001);
    float Minor = gridCoverage(Input.WorldXZ, Footprint, 1.0);
    float Major = gridCoverage(Input.WorldXZ, Footprint, 5.0);
    float Fade = 1.0 - smoothstep(50.0, 200.0, length(Input.WorldXZ - Input.CenterXZ));
    float Alpha = max(Minor * (32.0 / 255.0), Major * (60.0 / 255.0)) * Fade;
    // Linearized #8c929a for the sRGB scene target.
    return float4(0.26225, 0.28744, 0.32314, Alpha);
}
