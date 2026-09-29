struct FVertexOutput
{
    float4 Position : SV_Position;
    float4 Color : COLOR0;
};

[shader("vertex")]
FVertexOutput vertexMain(float4 Position : POSITION, float4 Color : COLOR0)
{
    FVertexOutput Output;
    Output.Position = Position;
    Output.Color = Color;
    return Output;
}

[shader("fragment")]
float4 fragmentMain(FVertexOutput Input) : SV_Target
{
    return Input.Color;
}
