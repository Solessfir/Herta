#pragma once

#include "Herta/EditorCore/CommandRegistry.h"

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace Herta
{
struct FAssetCommandOptions
{
	// Used when a command omits --content-root.
	std::filesystem::path DefaultContentRoot;
	std::filesystem::path DerivedDataRoot;
	std::filesystem::path WorkerPath;
	std::string TargetPlatform;
	std::vector<std::filesystem::path> DependencyContentRoots{};
	std::filesystem::path EngineContentRoot{};
	std::filesystem::path GameContentRoot{};
};

// Registers asset.validate, asset.list, asset.import, and asset.reimport for both the editor console and HertaEditorCmd.
// Cooking always runs in HertaAssetWorker.
[[nodiscard]] std::expected<void, FEditorCommandError> RegisterAssetCommands(FEditorCommandRegistry& Registry, const FAssetCommandOptions& Options);
}
