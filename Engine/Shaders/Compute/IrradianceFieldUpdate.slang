#include "IrradianceField.slangh"

// Chooses which probes trace this frame. Visual.Material.BaseColor.x is the stage; see EStage in IrradianceField.cpp.
[[vk::binding(384, 0)]] [format("rgba16f")] RWTexture2D<float4> IrradianceAtlas;
[[vk::binding(385, 0)]] [format("rg16f")] RWTexture2D<float2> DistanceAtlas;
// Each slot's cell, four ints per slot with the last unused; a slot whose cell no longer matches the clipmap starts over.
[[vk::binding(386, 0)]] RWStructuredBuffer<int> Cells;
// Each probe's offset and state as four floats; see FieldProbes.
[[vk::binding(387, 0)]] RWStructuredBuffer<float> Probes;
[[vk::binding(388, 0)]] RWStructuredBuffer<uint> Used;
[[vk::binding(389, 0)]] RWStructuredBuffer<uint> TraceList;
// The probes asked to trace, the probes the camera read, and those classified inside geometry, which the renderer reads back for statistics.
[[vk::binding(390, 0)]] RWStructuredBuffer<uint> Counters;
// Group counts for the trace and blend dispatches.
[[vk::binding(391, 0)]] RWStructuredBuffer<uint> Arguments;

static const uint StageReset = 0;
static const uint StageClear = 1;
static const uint StageSelect = 2;
static const uint StageArguments = 3;

// Probes the camera read within this many frames trace every frame; probes only rays read trace every fourth frame while they stay in use.
static const uint CameraFrames = 2;
static const uint RayFrames = 8;

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID)
{
    uint Stage = uint(Visual.Material.BaseColor.x);
    if (Stage == StageReset)
    {
        // New storage holds garbage, and the atlases must be written before anything samples them. This stage runs over the distance atlas's
        // texels, one row per group row.
        uint2 Texel = Id.xy;
        uint2 Size = uint2(FieldTilesPerRow * FieldDistanceTile, FieldProbeCount / FieldTilesPerRow * FieldDistanceTile);
        if (all(Texel < Size))
        {
            DistanceAtlas[Texel] = 0.0;
            if (all(Texel < Size / 2u))
                IrradianceAtlas[Texel] = 0.0;
        }

        uint Slot = Texel.y * Size.x + Texel.x;
        if (Slot < FieldProbeCount)
        {
            for (uint Word = 0; Word < 4; ++Word)
            {
                Cells[Slot * 4 + Word] = 0x7FFFFFFF;
                Probes[Slot * 4 + Word] = 0.0;
            }

            Used[Slot * 2] = 0;
            Used[Slot * 2 + 1] = 0;
        }

        return;
    }

    if (Stage == StageClear)
    {
        if (Id.x < 4)
            Counters[Id.x] = 0;
        return;
    }

    if (Stage == StageArguments)
    {
        if (Id.x == 0)
        {
            Arguments[0] = min(Counters[0], FieldMaxTraced);
            Arguments[1] = 1;
            Arguments[2] = 1;
        }

        return;
    }

    uint Slot = Id.x;
    if (Slot >= FieldProbeCount)
        return;

    uint Level;
    int3 Cell = FieldSlotCell(Slot, Level);
    uint Frame = FieldFrame();
    if (any(int3(Cells[Slot * 4], Cells[Slot * 4 + 1], Cells[Slot * 4 + 2]) != Cell))
    {
        // The clipmap moved this slot to a new cell, whose probe has neither light nor any use yet.
        for (uint Word = 0; Word < 4; ++Word)
        {
            Cells[Slot * 4 + Word] = Word < 3 ? Cell[Word] : 0;
            Probes[Slot * 4 + Word] = 0.0;
        }

        Used[Slot * 2] = 0;
        Used[Slot * 2 + 1] = 0;
        return;
    }

    bool bCamera = Frame - Used[Slot * 2] <= CameraFrames;
    bool bRays = Frame - Used[Slot * 2 + 1] <= RayFrames;
    if (bCamera)
    {
        InterlockedAdd(Counters[1], 1u);
        if (Probes[Slot * 4 + 3] == FieldProbeInside)
            InterlockedAdd(Counters[2], 1u);
    }

    if (bCamera || (bRays && ((Slot + Frame) & 3u) == 0u))
    {
        uint Index;
        InterlockedAdd(Counters[0], 1u, Index);
        if (Index < FieldMaxTraced)
            TraceList[Index] = Slot;
    }
}
