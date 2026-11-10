#pragma once

#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/Level/LevelSerialization.h"
#include "Herta/NavigationBuilder/NavigationBuilder.h"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Herta
{
class FTaskSystem;

struct FNavigationCookRequest
{
	// Where the level is saved, which its regions' paths are relative to.
	std::filesystem::path LevelPath;
	std::filesystem::path EngineContentRoot;
	std::filesystem::path GameContentRoot;
	std::filesystem::path DerivedDataRoot;
	std::filesystem::path WorkerPath;
	std::string TargetPlatform;
	FTaskSystem* Tasks = nullptr;
};

// A level's cooked navigation sits beside it, named like it with the .hnav extension.
[[nodiscard]] std::filesystem::path GetLevelNavigationPath(const std::filesystem::path& LevelPath);
[[nodiscard]] FNavigationBox GetCookedModelBounds(const FCookedModel& Model);
// The levels a level's regions place, parallel to its regions; a region whose level does not load is empty.
[[nodiscard]] std::vector<std::optional<FLevelDocument>> LoadLevelRegions(const FLevelDocument& Level, const std::filesystem::path& LevelPath);
// Cooks the meshes of the level's and its regions' static bodies through the asset worker for their bounds, then builds the level's
// navigation. Empty when the level and its regions have no Navigation Bounds.
[[nodiscard]] std::expected<std::optional<FNavigationFile>, FAssetError> CookLevelNavigation(const FLevelDocument& Level, const FNavigationCookRequest& Request);
}
