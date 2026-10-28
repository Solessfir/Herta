// Matches FDepthHierarchyUniforms in MeshRenderer.cpp.
struct FDepthHierarchyUniforms
{
    // 1 when the first target is level 0, read from scene depth; the number of levels to write; and the scene depth size.
    uint4 Settings;
};

[[vk::binding(64, 0)]] ConstantBuffer<FDepthHierarchyUniforms> Hierarchy;
[[vk::binding(0, 0)]] Texture2D<float> SceneDepth;
// The level the targets reduce from, then up to five consecutive levels.
[[vk::binding(384, 0)]] [format("r32f")] RWTexture2D<float> Source;
[[vk::binding(385, 0)]] [format("r32f")] RWTexture2D<float> Target0;
[[vk::binding(386, 0)]] [format("r32f")] RWTexture2D<float> Target1;
[[vk::binding(387, 0)]] [format("r32f")] RWTexture2D<float> Target2;
[[vk::binding(388, 0)]] [format("r32f")] RWTexture2D<float> Target3;
[[vk::binding(389, 0)]] [format("r32f")] RWTexture2D<float> Target4;

groupshared float Reduced[16][16];

// Farther texels have smaller reversed-Z depth, and a texel outside a level holds 1, which never wins.
float LoadSource(int2 Texel)
{
    uint2 Size;
    Source.GetDimensions(Size.x, Size.y);
    return all(uint2(Texel) < Size) ? Source[Texel] : 1.0;
}

// Level 0 is the largest power of two within half the view, so each of its texels covers up to 5x5 scene pixels, rounded outward.
float LoadSceneFootprint(uint2 Texel, uint2 LevelSize)
{
    uint2 DepthSize = Hierarchy.Settings.zw;
    uint2 Begin = Texel * DepthSize / LevelSize;
    uint2 End = min(((Texel + 1) * DepthSize + LevelSize - 1) / LevelSize, DepthSize);
    float Farthest = 1.0;
    for (uint Y = Begin.y; Y < End.y; ++Y)
    {
        for (uint X = Begin.x; X < End.x; ++X)
            Farthest = min(Farthest, SceneDepth.Load(int3(X, Y, 0)));
    }

    return Farthest;
}

void Store(RWTexture2D<float> Target, uint2 Texel, float Value)
{
    uint2 Size;
    Target.GetDimensions(Size.x, Size.y);
    if (all(Texel < Size))
        Target[Texel] = Value;
}

// Each 16x16 group writes a 16x16 tile of its first level and reduces it in shared memory down to one texel four levels later.
[shader("compute")]
[numthreads(16, 16, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID, uint3 Local : SV_GroupThreadID, uint3 Group : SV_GroupID)
{
    uint2 Size;
    Target0.GetDimensions(Size.x, Size.y);
    float Farthest = 1.0;
    if (all(Id.xy < Size))
    {
        if (Hierarchy.Settings.x != 0)
        {
            Farthest = LoadSceneFootprint(Id.xy, Size);
        }
        else
        {
            int2 Child = int2(Id.xy) * 2;
            Farthest = min(min(LoadSource(Child), LoadSource(Child + int2(1, 0))), min(LoadSource(Child + int2(0, 1)), LoadSource(Child + int2(1, 1))));
        }

        Target0[Id.xy] = Farthest;
    }

    Reduced[Local.y][Local.x] = Farthest;
    for (uint Level = 1; Level < 5; ++Level)
    {
        GroupMemoryBarrierWithGroupSync();
        uint Width = 16 >> Level;
        if (Level < Hierarchy.Settings.y && all(Local.xy < Width))
        {
            uint2 Child = Local.xy * 2;
            Farthest = min(min(Reduced[Child.y][Child.x], Reduced[Child.y][Child.x + 1]), min(Reduced[Child.y + 1][Child.x], Reduced[Child.y + 1][Child.x + 1]));
            uint2 Texel = Group.xy * Width + Local.xy;
            if (Level == 1)
                Store(Target1, Texel, Farthest);
            else if (Level == 2)
                Store(Target2, Texel, Farthest);
            else if (Level == 3)
                Store(Target3, Texel, Farthest);
            else
                Store(Target4, Texel, Farthest);
        }

        // Every thread reads before any writes the next level's values in place.
        GroupMemoryBarrierWithGroupSync();
        if (all(Local.xy < Width))
            Reduced[Local.y][Local.x] = Farthest;
    }
}
