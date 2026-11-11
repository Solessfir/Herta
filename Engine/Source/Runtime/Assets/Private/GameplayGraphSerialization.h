#pragma once

#include "Herta/Assets/GameplayGraph.h"
#include "Herta/Core/BinaryStream.h"

namespace Herta
{
void WriteGameplayGraph(FBinaryWriter& Writer, const FCookedGameplayGraph& Graph);
// Leaves the reader invalid on truncated or oversized data; callers validate the result.
[[nodiscard]] FCookedGameplayGraph ReadGameplayGraph(FBinaryReader& Reader);
}
