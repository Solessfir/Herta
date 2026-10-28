// Matches FOcclusionCullUniforms in MeshRenderer.cpp.
struct FOcclusionCullUniforms
{
    column_major float4x4 WorldToClip;
    // Candidates, draws, phase (0 early, 1 late), and depth hierarchy mips.
    uint4 Counts;
    // Depth hierarchy mip 0 width and height.
    float4 Hierarchy;
};

[[vk::binding(64, 0)]] ConstantBuffer<FOcclusionCullUniforms> Cull;
[[vk::binding(0, 0)]] Texture2D<float> DepthHierarchy;
[[vk::binding(384, 0)]] RWStructuredBuffer<uint> Culled;
// Five uints per draw for the early, late, and final lists: index count, instance count, first index, vertex offset, and first instance.
[[vk::binding(385, 0)]] RWStructuredBuffer<uint> Arguments;
// One uint per model: whether it passed the late test last frame.
[[vk::binding(386, 0)]] RWStructuredBuffer<uint> History;
// Per candidate: world bounds center, model, extent, first draw, and section count, matching FOcclusionCandidate.
[[vk::binding(16, 0)]] StructuredBuffer<uint> Candidates;
// The camera's frustum-culled instances, one per candidate.
[[vk::binding(17, 0)]] StructuredBuffer<uint> Instances;

static const uint CandidateWords = 9;
static const uint ArgumentWords = 5;
// Matches sizeof(FMeshInstance).
static const uint InstanceWords = 48;
static const uint EarlyList = 0;
static const uint LateList = 1;
static const uint FinalList = 2;

// Every section of the candidate's mesh draws it, so each section's instance count grows; the first section's count picks its slot.
void Append(uint List, uint Candidate, uint FirstDraw, uint Sections)
{
    uint Base = (List * Cull.Counts.y + FirstDraw) * ArgumentWords;
    uint Slot;
    InterlockedAdd(Arguments[Base + 1], 1, Slot);
    for (uint Section = 1; Section < Sections; ++Section)
        InterlockedAdd(Arguments[Base + Section * ArgumentWords + 1], 1);

    uint Target = (Arguments[Base + 4] + Slot) * InstanceWords;
    uint Source = Candidate * InstanceWords;
    for (uint Word = 0; Word < InstanceWords; ++Word)
        Culled[Target + Word] = Instances[Source + Word];
}

float LoadHierarchy(uint2 Texel, uint Mip)
{
    return DepthHierarchy.Load(int3(Texel, Mip));
}

// The box is hidden when its nearest reversed-Z depth lies behind the farthest depth in the hierarchy over its screen rectangle, read at the level where that rectangle spans at most 2x2 texels.
bool IsVisible(float3 Center, float3 Extent)
{
    float2 Minimum = 1e30;
    float2 Maximum = -1e30;
    float Nearest = 0.0;
    for (uint Corner = 0; Corner < 8; ++Corner)
    {
        float3 Sign = float3(Corner & 1 ? 1.0 : -1.0, Corner & 2 ? 1.0 : -1.0, Corner & 4 ? 1.0 : -1.0);
        float4 Clip = mul(Cull.WorldToClip, float4(Center + Extent * Sign, 1.0));
        if (Clip.w <= 1e-4)
            return true;

        float3 Ndc = Clip.xyz / Clip.w;
        Minimum = min(Minimum, Ndc.xy);
        Maximum = max(Maximum, Ndc.xy);
        Nearest = max(Nearest, Ndc.z);
    }

    // Clip y points up and texture rows down.
    float2 PixelMinimum = saturate(float2(Minimum.x, -Maximum.y) * 0.5 + 0.5) * Cull.Hierarchy.xy;
    float2 PixelMaximum = saturate(float2(Maximum.x, -Minimum.y) * 0.5 + 0.5) * Cull.Hierarchy.xy;
    float Size = max(PixelMaximum.x - PixelMinimum.x, PixelMaximum.y - PixelMinimum.y);
    uint Mip = min(uint(ceil(log2(max(Size, 1.0)))), Cull.Counts.w - 1);
    uint2 MipSize = max(uint2(Cull.Hierarchy.xy) >> Mip, 1u);
    uint2 A = min(uint2(PixelMinimum) >> Mip, MipSize - 1);
    uint2 B = min(uint2(PixelMaximum) >> Mip, MipSize - 1);
    float Farthest = min(min(LoadHierarchy(A, Mip), LoadHierarchy(uint2(B.x, A.y), Mip)), min(LoadHierarchy(uint2(A.x, B.y), Mip), LoadHierarchy(B, Mip)));
    return Nearest >= Farthest;
}

// Two-phase occlusion culling. The early phase redraws what was visible last frame; the late phase tests every candidate against the hierarchy built from that depth, draws what the early phase missed, and lists everything visible for shading.
[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID)
{
    uint Candidate = Id.x;
    if (Candidate >= Cull.Counts.x)
        return;

    uint Base = Candidate * CandidateWords;
    float3 Center = asfloat(uint3(Candidates[Base], Candidates[Base + 1], Candidates[Base + 2]));
    uint Model = Candidates[Base + 3];
    float3 Extent = asfloat(uint3(Candidates[Base + 4], Candidates[Base + 5], Candidates[Base + 6]));
    uint FirstDraw = Candidates[Base + 7];
    uint Sections = Candidates[Base + 8];
    bool bWasVisible = History[Model] != 0;
    if (Cull.Counts.z == 0)
    {
        if (bWasVisible)
            Append(EarlyList, Candidate, FirstDraw, Sections);

        return;
    }

    bool bVisible = IsVisible(Center, Extent);
    History[Model] = bVisible ? 1 : 0;
    if (bVisible)
    {
        Append(FinalList, Candidate, FirstDraw, Sections);
        if (!bWasVisible)
            Append(LateList, Candidate, FirstDraw, Sections);
    }
}
