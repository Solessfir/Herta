#pragma once

#include "Herta/Assets/Sound.h"
#include "Herta/Core/BinaryStream.h"

namespace Herta
{
void WriteCookedSound(FBinaryWriter& Writer, const FCookedSound& Sound);
// Version 9 added oscillators, envelopes, and start delays; older versions write and read graphs without them.
void WriteCookedSoundGraph(FBinaryWriter& Writer, const FCookedSoundGraph& Graph, std::uint32_t Version);
[[nodiscard]] bool NeedsSynthesisFormat(const FSoundGraphDesc& Graph);
// Reads everything before the samples into Sound and returns the sample count.
[[nodiscard]] std::uint64_t ReadCookedSoundHeader(FBinaryReader& Reader, FCookedSound& Sound);
// Both leave the reader invalid on truncated or out-of-range data; callers validate the result.
[[nodiscard]] FCookedSound ReadCookedSound(FBinaryReader& Reader);
[[nodiscard]] FCookedSoundGraph ReadCookedSoundGraph(FBinaryReader& Reader, std::uint32_t Version);
}
