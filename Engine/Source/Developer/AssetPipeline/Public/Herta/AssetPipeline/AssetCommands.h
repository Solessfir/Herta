#pragma once

#include "Herta/EditorCore/CommandRegistry.h"

#include <expected>
#include <filesystem>
#include <string>

namespace Herta
{
struct FAssetCommandOptions
{
	// Used when a command omits --content-root.
	std::filesystem::path DefaultContentRoot;
	std::filesystem::path DerivedDataRoot;
	std::filesystem::path WorkerPath;
	std::string TargetPlatform;
};

// Registers asset.validate, asset.list, asset.import, and asset.reimport for both the editor console and HertaEditorCmd.
// Cooking always runs in HertaAssetWorker.
[[nodiscard]] std::expected<void, FEditorCommandError> RegisterAssetCommands(FEditorCommandRegistry& Registry, const FAssetCommandOptions& Options);
}
