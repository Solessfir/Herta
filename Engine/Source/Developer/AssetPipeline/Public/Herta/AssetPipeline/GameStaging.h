#pragma once

#include "Herta/Assets/AssetRegistry.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace Herta
{
struct FGameStageRequest
{
	std::filesystem::path ProjectPath;
	// Receives the project, its starting level, and every cooked asset the level reaches. A previous stage there is replaced.
	std::filesystem::path OutputRoot;
	std::filesystem::path EngineContentRoot;
	// Shader includes that material cooking may need.
	std::filesystem::path EngineShaderRoot;
	std::filesystem::path DerivedDataRoot;
	std::filesystem::path WorkerPath;
	std::string TargetPlatform;
};

struct FGameStageResult
{
	std::filesystem::path ProjectPath;
	std::size_t Assets = 0;
	std::uint64_t Bytes = 0;
};

// Staged layout, relative to OutputRoot: Engine/Content holds engine assets, and <Project>/ mirrors the project with its content root holding game assets.
[[nodiscard]] std::filesystem::path GetStagedEngineContentRoot(const std::filesystem::path& OutputRoot);

// Cooks the starting level's dependency closure through HertaAssetWorker and writes it as loose, directory-mounted content.
[[nodiscard]] std::expected<FGameStageResult, FAssetError> StageGameContent(const FGameStageRequest& Request);
}
