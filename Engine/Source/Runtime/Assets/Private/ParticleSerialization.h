#pragma once

#include "Herta/Assets/ParticleSystem.h"
#include "Herta/Core/BinaryStream.h"

namespace Herta
{
// Version 10 added module stacks and events; older versions write and read emitters without them.
void WriteParticleSystem(FBinaryWriter& Writer, const FCookedParticleSystem& System, std::uint32_t Version);
[[nodiscard]] bool NeedsParticleStageFormat(const FCookedParticleSystem& System);
// Leaves the reader invalid on truncated or out-of-range data; callers validate the result.
[[nodiscard]] FCookedParticleSystem ReadParticleSystem(FBinaryReader& Reader, std::uint32_t Version);
}
