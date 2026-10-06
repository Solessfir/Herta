#pragma once

#include "Herta/EditorCore/CommandRegistry.h"

#include <filesystem>

namespace Herta
{
[[nodiscard]] std::expected<void, FEditorCommandError> RegisterProjectCommands(FEditorCommandRegistry& Registry, const std::filesystem::path& EngineRoot);
}
