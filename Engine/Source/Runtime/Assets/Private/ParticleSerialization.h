#pragma once

#include "Herta/Assets/ParticleSystem.h"
#include "Herta/Core/BinaryStream.h"

namespace Herta
{
void WriteParticleSystem(FBinaryWriter& Writer, const FCookedParticleSystem& System);
// Leaves the reader invalid on truncated or out-of-range data; callers validate the result.
[[nodiscard]] FCookedParticleSystem ReadParticleSystem(FBinaryReader& Reader);
}
