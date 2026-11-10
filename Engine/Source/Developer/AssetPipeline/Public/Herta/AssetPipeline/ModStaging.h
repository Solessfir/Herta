#pragma once

#include "Herta/Assets/AssetRegistry.h"
#include "Herta/Assets/DataMod.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace Herta
{
struct FModStageRequest
{
	// The mod's source folder: Mod.hmod and a Content folder of source assets with their metadata.
	std::filesystem::path ModRoot;
	// The game it modifies, whose staged layout the mod's files follow.
	std::filesystem::path ProjectPath;
	// Receives Mod.hmod and the cooked files under Files, laid out like the staged game. A previous stage there is replaced.
	std::filesystem::path OutputRoot;
	std::filesystem::path EngineContentRoot;
	std::filesystem::path EngineShaderRoot;
	std::filesystem::path DerivedDataRoot;
	std::filesystem::path WorkerPath;
	std::string TargetPlatform;
};

struct FModStageResult
{
	FModManifest Manifest{};
	// The package paths of the files it staged, in path order.
	std::vector<std::string> Files{};
	std::size_t Assets = 0;
	std::uint64_t Bytes = 0;
};

// Cooks every source asset in the mod's Content folder through HertaAssetWorker into OutputRoot/Files under the game's content root, so an
// asset with one of the game's IDs replaces it and a new ID adds one. Textures are staged in both color spaces, since the mod cannot know how
// the game samples them. Materials with their own shaders are refused until mods can carry compiled shaders.
[[nodiscard]] std::expected<FModStageResult, FAssetError> StageModContent(const FModStageRequest& Request);
}
