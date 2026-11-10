#pragma once

#include "Herta/Assets/StateTree.h"
#include "Herta/Core/BinaryStream.h"

namespace Herta
{
void WriteStateTree(FBinaryWriter& Writer, const FCookedStateTree& Tree);
// Leaves the reader invalid on truncated or oversized data; callers validate the result.
[[nodiscard]] FCookedStateTree ReadStateTree(FBinaryReader& Reader);
}
