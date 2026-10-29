// Indirect group counts for the probe stages, from the probe counts compaction has reached so far. It binds the arguments only here,
// so the stages that read them as indirect parameters never also bind them for writing. Keep the capacities in sync with RadianceCascades.slangh.
[[vk::binding(16, 0)]] StructuredBuffer<uint> Counters;
[[vk::binding(384, 0)]] RWStructuredBuffer<uint> Arguments;

static const uint CascadeCount = 4;
static const uint ProbeCapacity[CascadeCount] = { 65536u, 16384u, 4096u, 2048u };

void Write(uint Slot, uint3 Groups)
{
    Arguments[Slot * 3u] = Groups.x;
    Arguments[Slot * 3u + 1u] = Groups.y;
    Arguments[Slot * 3u + 2u] = Groups.z;
}

// Slots: parent insertion into cascades 1 to 3, links from cascades 0 to 2, merging of cascades 0 to 3, and irradiance.
[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain()
{
    for (uint Cascade = 0; Cascade < CascadeCount; ++Cascade)
    {
        uint Probes = min(Counters[Cascade], ProbeCapacity[Cascade]);
        uint ProbeGroups = (Probes + 63u) / 64u;
        if (Cascade + 1u < CascadeCount)
        {
            Write(Cascade, uint3(ProbeGroups, 1u, 1u));
            Write(3u + Cascade, uint3(ProbeGroups, 1u, 1u));
        }

        // Merging runs in rows of 32768 groups, which the top cascade's directions exceed.
        uint MergeGroups = (Probes * (32u << (2u * Cascade)) + 63u) / 64u;
        Write(6u + Cascade, uint3(min(MergeGroups, 32768u), max((MergeGroups + 32767u) / 32768u, 1u), 1u));
    }

    uint BaseProbes = min(Counters[0], ProbeCapacity[0]);
    Write(10u, uint3(1024u, max((BaseProbes + 1023u) / 1024u, 1u), 1u));
}
