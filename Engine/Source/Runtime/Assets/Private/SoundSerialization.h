#pragma once

#include "Herta/Assets/Sound.h"
#include "Herta/Core/BinaryStream.h"

namespace Herta
{
void WriteCookedSound(FBinaryWriter& Writer, const FCookedSound& Sound);
void WriteCookedSoundGraph(FBinaryWriter& Writer, const FCookedSoundGraph& Graph);
// Reads everything before the samples into Sound and returns the sample count.
[[nodiscard]] std::uint64_t ReadCookedSoundHeader(FBinaryReader& Reader, FCookedSound& Sound);
// Both leave the reader invalid on truncated or out-of-range data; callers validate the result.
[[nodiscard]] FCookedSound ReadCookedSound(FBinaryReader& Reader);
[[nodiscard]] FCookedSoundGraph ReadCookedSoundGraph(FBinaryReader& Reader);
}
