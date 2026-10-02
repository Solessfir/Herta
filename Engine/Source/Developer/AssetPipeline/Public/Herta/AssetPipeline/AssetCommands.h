#pragma once

#include "Herta/EditorCore/CommandRegistry.h"

#include <expected>
#include <filesystem>

namespace Herta
{
struct FAssetCommandOptions
{
	// Used when a command omits --content-root.
	std::filesystem::path DefaultContentRoot;
};

// Registers asset.validate, asset.list, and asset.import for both the editor console and HertaEditorCmd.
[[nodiscard]] std::expected<void, FEditorCommandError> RegisterAssetCommands(FEditorCommandRegistry& Registry, const FAssetCommandOptions& Options);
}
