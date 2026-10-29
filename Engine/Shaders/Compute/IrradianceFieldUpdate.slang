#include "IrradianceField.slangh"

// Frees probes the clipmap left behind, gives probes to the cells readers asked for last frame, and chooses which probes trace this frame.
// Visual.Material.BaseColor.x is the stage; see EStage in IrradianceField.cpp.
[[vk::binding(384, 0)]] RWStructuredBuffer<uint> Cells;
// Each probe's cell, level, and update count; see FieldProbeLevel.
[[vk::binding(385, 0)]] RWStructuredBuffer<int> ProbeCells;
// Each probe's offset and state as four floats; see FieldProbes.
[[vk::binding(386, 0)]] RWStructuredBuffer<float> Probes;
[[vk::binding(387, 0)]] RWStructuredBuffer<uint> Used;
[[vk::binding(388, 0)]] RWStructuredBuffer<uint> Requests;
// The free probes, as a stack.
[[vk::binding(389, 0)]] RWStructuredBuffer<uint> FreeList;
[[vk::binding(390, 0)]] RWStructuredBuffer<uint> TraceList;
// The trace and blend dispatches' group counts in the first three words, then the counters below, which the renderer also reads back.
[[vk::binding(391, 0)]] RWStructuredBuffer<uint> Counters;

static const uint StageReset = 0;
static const uint StageClear = 1;
static const uint StageSelect = 2;
static const uint StageAllocate = 3;
static const uint StageSettle = 4;
static const uint StageFinish = 5;

// Keep in sync with IrradianceField.cpp.
static const uint CounterTraced = 4;
static const uint CounterRead = 5;
static const uint CounterInside = 6;
static const uint CounterUnallocated = 7;
static const uint CounterFree = 8;
static const uint CounterTaken = 9;
static const uint CounterFreeBefore = 10;
static const uint CounterWords = 12;

// Probes the camera read within this many frames trace every frame; probes only rays read trace every fourth frame while they stay in use.
static const uint CameraFrames = 2;
static const uint RayFrames = 8;
// Probes still settling after they appear, relocating and averaging their first bounces, trace every fourth frame in the room the others leave,
// so places the camera walks into are already lit instead of changing as it arrives.
static const uint SettledUpdates = 16;
// Once fewer than an eighth of the probes are free, probes nothing has read for this many frames give up their cells.
static const uint EvictFrames = 30;

bool AddToTraceList(uint Probe)
{
    uint Index;
    InterlockedAdd(Counters[CounterTraced], 1u, Index);
    if (Index >= FieldMaxTraced)
        return false;

    TraceList[Index] = Probe;
    return true;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID)
{
    uint Stage = uint(Visual.Material.BaseColor.x);
    uint Frame = FieldFrame();
    if (Stage == StageReset)
    {
        // New storage holds garbage. The atlases need no clearing: a probe's first blend writes its whole tiles without reading them.
        uint Index = Id.x;
        if (Index < FieldLevels * FieldCellsPerLevel)
            Cells[Index] = FieldEmpty;

        if (Index < FieldProbeCount)
        {
            for (uint Word = 0; Word < 4; ++Word)
            {
                ProbeCells[Index * 4 + Word] = Word < 3 ? 0 : -1;
                Probes[Index * 4 + Word] = 0.0;
            }

            Used[Index * 2] = 0;
            Used[Index * 2 + 1] = 0;
            FreeList[Index] = Index;
        }

        if (Index < CounterWords)
            Counters[Index] = Index == CounterFree ? FieldProbeCount : 0;

        if (Index == 0)
            Requests[0] = 0;

        return;
    }

    if (Stage == StageClear)
    {
        if (Id.x == 0)
        {
            Counters[CounterTraced] = 0;
            Counters[CounterRead] = 0;
            Counters[CounterInside] = 0;
            Counters[CounterUnallocated] = 0;
            Counters[CounterTaken] = 0;
            Counters[CounterFreeBefore] = Counters[CounterFree];
        }

        return;
    }

    if (Stage == StageFinish)
    {
        if (Id.x == 0)
        {
            Counters[0] = min(Counters[CounterTraced], FieldMaxTraced);
            Counters[1] = 1;
            Counters[2] = 1;
            Counters[CounterFree] -= min(Counters[CounterTaken], Counters[CounterFree]);
            // This frame's readers ask anew.
            Requests[0] = 0;
        }

        return;
    }

    if (Stage == StageAllocate)
    {
        uint Request = Id.x;
        if (Request >= min(Requests[0], FieldMaxRequests))
            return;

        uint Base = 4 + Request * 4;
        int3 Cell = int3(Requests[Base], Requests[Base + 1], Requests[Base + 2]);
        uint Level = Requests[Base + 3];
        uint Slot = FieldSlot(Level, Cell);
        if (!FieldInGrid(Level, Cell))
        {
            // The clipmap moved past the cell since it was asked for.
            Cells[Slot] = FieldEmpty;
            return;
        }

        // Requests take probes down from the top of the free stack, which shrinks only once they all have, so each takes its own.
        uint Free = Counters[CounterFree];
        uint Taken;
        InterlockedAdd(Counters[CounterTaken], 1u, Taken);
        if (Taken >= Free)
        {
            InterlockedAdd(Counters[CounterUnallocated], 1u);
            Cells[Slot] = FieldEmpty;
            return;
        }

        uint Probe = FreeList[Free - 1 - Taken];
        Cells[Slot] = Probe;
        ProbeCells[Probe * 4] = Cell.x;
        ProbeCells[Probe * 4 + 1] = Cell.y;
        ProbeCells[Probe * 4 + 2] = Cell.z;
        ProbeCells[Probe * 4 + 3] = FieldPackProbe(Level, 0);
        for (uint Word = 0; Word < 4; ++Word)
            Probes[Probe * 4 + Word] = FieldProbeNew;

        // Counting as read by the camera keeps settling from adding it to the list twice, and traces it again next frame, the first its
        // readers find it.
        Used[Probe * 2] = Frame;
        Used[Probe * 2 + 1] = 0;
        AddToTraceList(Probe);
        return;
    }

    uint Probe = Id.x;
    if (Probe >= FieldProbeCount)
        return;

    int Packed = ProbeCells[Probe * 4 + 3];
    if (Packed < 0)
        return;

    uint Level = FieldProbeLevel(Packed);
    int3 Cell = int3(ProbeCells[Probe * 4], ProbeCells[Probe * 4 + 1], ProbeCells[Probe * 4 + 2]);
    uint CameraFrame = Used[Probe * 2];
    uint RayFrame = Used[Probe * 2 + 1];
    bool bCamera = Frame - CameraFrame <= CameraFrames;
    bool bRays = Frame - RayFrame <= RayFrames;
    bool bSelected = bCamera || (bRays && ((Probe + Frame) & 3u) == 0u);
    if (Stage == StageSettle)
    {
        // Runs after every selected and new probe has its place, so settling never takes one from a probe in use, and settling past the
        // list's end does not count as overflow.
        bool bSettling = Probes[Probe * 4 + 3] != FieldProbeInside && FieldProbeUpdates(Packed) < SettledUpdates;
        if (!bSelected && bSettling && ((Probe + Frame) & 3u) == 0u && !AddToTraceList(Probe))
            InterlockedAdd(Counters[CounterTraced], 0xFFFFFFFFu);

        return;
    }

    // Probes the clipmap left behind give up their cells, and so do probes nothing reads any more once the pool runs low.
    bool bStale = Frame - max(CameraFrame, RayFrame) > EvictFrames && Counters[CounterFreeBefore] < FieldProbeCount / 8;
    if (!FieldInGrid(Level, Cell) || bStale)
    {
        Cells[FieldSlot(Level, Cell)] = FieldEmpty;
        ProbeCells[Probe * 4 + 3] = -1;
        uint Free;
        InterlockedAdd(Counters[CounterFree], 1u, Free);
        FreeList[Free] = Probe;
        return;
    }

    if (bCamera)
    {
        InterlockedAdd(Counters[CounterRead], 1u);
        if (Probes[Probe * 4 + 3] == FieldProbeInside)
            InterlockedAdd(Counters[CounterInside], 1u);
    }

    if (bSelected)
        AddToTraceList(Probe);
}
