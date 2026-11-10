#pragma once

#include "Herta/AssetPipeline/AssetMetadata.h"
#include "Herta/AssetPipeline/BuildKey.h"
#include "Herta/Assets/Sound.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace Herta
{
struct FSoundSettings
{
	FSoundPlayback Playback{};
	bool bLoop = false;
	bool bStream = false;
};

// Bus, Volume, MaximumInstances, Concurrency, MinimumDistance, and MaximumDistance for both importers, and Loop and Stream for waves.
[[nodiscard]] std::expected<FSoundSettings, FAssetError> ParseSoundSettings(const FAssetImportSettings& Settings, bool bWave);
[[nodiscard]] std::expected<FCookedSound, FAssetError> CookSound(std::span<const std::byte> Source, const FSoundSettings& Settings);
// Compiles the graph and appends each wave's metadata as a dependency, since that is where the graph's wave IDs come from.
[[nodiscard]] std::expected<FCookedSoundGraph, FAssetError> CookSoundGraph(const std::filesystem::path& ContentRoot, std::string_view SourcePath, std::span<const std::byte> Source, const FSoundSettings& Settings, std::vector<FAssetBuildDependency>& Dependencies);
}
