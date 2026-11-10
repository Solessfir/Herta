#pragma once

#include "Herta/Assets/CookedContent.h"
#include "Herta/Audio/AudioEngine.h"

#include <expected>
#include <span>

namespace Herta
{
// Adds sounds and sound graphs from staged content, with every graph's waves. Streamed sounds stay on disk and play from their staged files.
[[nodiscard]] std::expected<void, FAudioError> AddStagedSounds(FAudioEngine& Engine, const FCookedContent& Content, std::span<const FAssetId> Sounds);
}
