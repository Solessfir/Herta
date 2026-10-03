#pragma once

#include "Herta/EditorCore/CommandRegistry.h"

namespace Herta
{
[[nodiscard]] std::expected<void, FEditorCommandError> RegisterSceneFileCommands(FEditorCommandRegistry& Commands);
}
