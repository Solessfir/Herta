#include "RadianceCascades.slangh"

// Frees probes the clipmap left behind, gives probes to the cells readers asked for last frame, and chooses which probes trace their near rays
// and which their shells this frame. Visual.Material.BaseColor.x is the stage; see EStage in RadianceCascades.cpp.
[[vk::binding(384, 0)]] RWStructuredBuffer<uint> Cells;
// Each probe's cell, level, and update count; see FieldProbeLevel.
[[vk::binding(385, 0)]] RWStructuredBuffer<int> ProbeCells;
// Each probe's offset and state as four floats; see FieldProbes.
[[vk::binding(386, 0)]] RWStructuredBuffer<float> Probes;
// Four words per probe, one after another: the frames the camera, rays, and a probe below last read it, and its shell's frame.
[[vk::binding(387, 0)]] RWStructuredBuffer<uint> Used;
[[vk::binding(388, 0)]] RWStructuredBuffer<uint> Requests;
// Each level's free probes, as a stack in the level's share of the pool.
[[vk::binding(389, 0)]] RWStructuredBuffer<uint> FreeList;
// The near list, then each level's shell list; see CascadeListBase.
[[vk::binding(390, 0)]] RWStructuredBuffer<uint> Lists;
// Each list's dispatch group counts, four words apart, then the counters below, which the renderer also reads back.
[[vk::binding(391, 0)]] RWStructuredBuffer<uint> Counters;

static const uint StageReset = 0;
static const uint StageClear = 1;
static const uint StageSelect = 2;
static const uint StageAllocate = 3;
static const uint StageRays = 4;
static const uint StageFinish = 5;

// Keep in sync with RadianceCascades.cpp. The first of each group of four is the near list or level 0.
static const uint CounterListed = 16;
static const uint CounterRead = 20;
static const uint CounterInside = 21;
static const uint CounterUnallocated = 22;
static const uint CounterFree = 24;
static const uint CounterTaken = 28;
static const uint CounterFreeBefore = 32;
static const uint CounterWords = 36;

// Probes the camera read within this many frames trace their near rays every frame.
static const uint CameraFrames = 2;
static const uint RayFrames = 8;
// Shells trace this rarely, the further ones less often: what they see is far away and moves slowly across their directions. A probe below
// that merges with a shell marks it within the longest of these. Level 0 has no shell; its turn is for the near rays of probes only rays read.
static const uint ShellFrames[FieldLevels] = { 8, 4, 8, 16 };
static const uint ParentFrames = 40;
// Probes left inside geometry once relocation gives up only look again this rarely, in case what they are inside moves.
static const uint InsideFrames = 64;
// Once fewer than an eighth of a level's probes are free, probes nothing has read for this many frames give up their cells.
static const uint EvictFrames = 60;

bool AddToList(uint List, uint Probe)
{
    uint Index;
    InterlockedAdd(Counters[CounterListed + List], 1u, Index);
    if (Index >= CascadeListCount[List])
        return false;

    Lists[CascadeListBase[List] + Index] = Probe;
    return true;
}

// A probe's shell traces, and with it its near rays unless the camera's pixels already listed them.
void AddShell(uint Level, uint Probe, uint Frame, bool bNear)
{
    if (bNear)
        AddToList(0, Probe);

    if (Level > 0 && AddToList(Level, Probe))
        Used[CascadeRadianceFrame + Probe] = Frame;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 Id : SV_DispatchThreadID)
{
    uint Stage = uint(Visual.Material.BaseColor.x);
    uint Frame = FieldFrame();
    if (Stage == StageReset)
    {
        // New storage holds garbage.
        uint Index = Id.x;
        if (Index < FieldLevels * FieldCellsPerLevel)
            Cells[Index] = FieldEmpty;

        if (Index < FieldProbeCount)
        {
            for (uint Word = 0; Word < 4; ++Word)
            {
                ProbeCells[Index * 4 + Word] = Word < 3 ? 0 : -1;
                Probes[Index * 4 + Word] = 0.0;
                Used[Index + FieldProbeCount * Word] = 0;
            }

            FreeList[Index] = Index;
        }

        if (Index < CounterWords)
            Counters[Index] = Index >= CounterFree && Index < CounterFree + FieldLevels ? CascadeLevelCount[Index - CounterFree] : 0;

        if (Index == 0)
            Requests[0] = 0;

        return;
    }

    if (Stage == StageClear)
    {
        if (Id.x < FieldLevels)
        {
            Counters[CounterListed + Id.x] = 0;
            Counters[CounterTaken + Id.x] = 0;
            Counters[CounterFreeBefore + Id.x] = Counters[CounterFree + Id.x];
        }

        if (Id.x == 0)
        {
            Counters[CounterRead] = 0;
            Counters[CounterInside] = 0;
            Counters[CounterUnallocated] = 0;
        }

        return;
    }

    if (Stage == StageFinish)
    {
        if (Id.x < FieldLevels)
        {
            // Near rays run one group per probe, and a shell one group per 64 directions.
            uint Texels = CascadeTexels(Id.x);
            Counters[Id.x * 4] = min(Counters[CounterListed + Id.x], CascadeListCount[Id.x]);
            Counters[Id.x * 4 + 1] = Id.x == 0 ? 1 : Texels * Texels / 64;
            Counters[Id.x * 4 + 2] = 1;
            Counters[CounterFree + Id.x] -= min(Counters[CounterTaken + Id.x], Counters[CounterFree + Id.x]);
        }

        // This frame's readers ask anew.
        if (Id.x == 0)
            Requests[0] = 0;

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

        // Requests take probes down from the top of their level's free stack, which shrinks only once they all have, so each takes its own.
        uint Free = Counters[CounterFree + Level];
        uint Taken;
        InterlockedAdd(Counters[CounterTaken + Level], 1u, Taken);
        if (Taken >= Free)
        {
            InterlockedAdd(Counters[CounterUnallocated], 1u);
            Cells[Slot] = FieldEmpty;
            return;
        }

        uint Probe = FreeList[CascadeLevelBase[Level] + Free - 1 - Taken];
        Cells[Slot] = Probe;
        ProbeCells[Probe * 4] = Cell.x;
        ProbeCells[Probe * 4 + 1] = Cell.y;
        ProbeCells[Probe * 4 + 2] = Cell.z;
        ProbeCells[Probe * 4 + 3] = FieldPackProbe(Level, 0);
        for (uint Word = 0; Word < 4; ++Word)
            Probes[Probe * 4 + Word] = FieldProbeNew;

        // Counting as read by a probe below keeps it tracing until its first readers find it.
        Used[Probe * 2] = 0;
        Used[Probe * 2 + 1] = 0;
        Used[CascadeParentUse + Probe] = Frame;
        Used[CascadeRadianceFrame + Probe] = 0;
        AddShell(Level, Probe, Frame, true);
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
    uint ParentFrame = Used[CascadeParentUse + Probe];
    bool bCamera = Frame - CameraFrame <= CameraFrames;
    bool bParent = Frame - ParentFrame <= ParentFrames;
    bool bInside = Probes[Probe * 4 + 3] == FieldProbeInside;
    bool bGivenUp = bInside && FieldProbeUpdates(Packed) >= FieldRelocationUpdates;
    // A probe waiting for the level above, or whose shell still lacks it, tries again every frame until it arrives.
    bool bWaiting = Probes[Probe * 4 + 3] == FieldProbeNew || (Level > 0 && Used[CascadeRadianceFrame + Probe] <= CascadeIncomplete);
    bool bTurn = bGivenUp ? ((Probe + Frame) & (InsideFrames - 1)) == 0u : bWaiting || (Probe + Frame) % ShellFrames[Level] == 0u;
    if (Stage == StageRays)
    {
        // Runs after every probe the camera reads, every probe those merge with, and every new probe has its place, so probes only rays read,
        // which can reach far past what the camera sees, never take one from them. These also light the places the camera walks into before
        // it arrives.
        bool bRays = Frame - RayFrame <= RayFrames;
        if (!bCamera && !bParent && bRays && bTurn)
            AddShell(Level, Probe, Frame, true);

        return;
    }

    // Probes the clipmap left behind give up their cells, and so do probes nothing reads any more once their level's pool runs low.
    bool bStale = Frame - max(max(CameraFrame, RayFrame), ParentFrame) > EvictFrames && Counters[CounterFreeBefore + Level] < CascadeLevelCount[Level] / 8;
    if (!FieldInGrid(Level, Cell) || bStale)
    {
        Cells[FieldSlot(Level, Cell)] = FieldEmpty;
        ProbeCells[Probe * 4 + 3] = -1;
        uint Free;
        InterlockedAdd(Counters[CounterFree + Level], 1u, Free);
        FreeList[CascadeLevelBase[Level] + Free] = Probe;
        return;
    }

    if (bCamera)
    {
        InterlockedAdd(Counters[CounterRead], 1u);
        if (bInside)
            InterlockedAdd(Counters[CounterInside], 1u);
    }

    // Near rays light the probe's own tiles, which only the camera's pixels and other rays read. The camera's probes trace them every other
    // frame once they have settled, since the shells behind them change more slowly still. A probe that only passes its shell on to the
    // level below needs them just to find its place and whether it is inside geometry, so it traces them while it settles and rarely after.
    bool bSettled = Probes[Probe * 4 + 3] != FieldProbeNew && FieldProbeUpdates(Packed) >= FieldRelocationUpdates;
    bool bRays = Frame - RayFrame <= RayFrames;
    bool bNear = bCamera && (bGivenUp ? bTurn : !bSettled || ((Probe + Frame) & 1u) == 0u);
    if (bNear)
        AddToList(0, Probe);

    if ((bCamera || bParent) && bTurn)
        AddShell(Level, Probe, Frame, !bNear && !bCamera && (!bSettled || (Probe + Frame) % (ShellFrames[Level] * (bRays ? 2 : 8)) == 0u));
}
