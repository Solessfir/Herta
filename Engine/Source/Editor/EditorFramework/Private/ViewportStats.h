#pragma once

#include "Herta/EditorCore/CommandRegistry.h"

#include <memory>
#include <string>
#include <utility>

namespace Herta
{
struct FViewportStats
{
	bool bUnitVisible = false;
	bool bFpsVisible = false;
};

[[nodiscard]] inline std::expected<void, FEditorCommandError> RegisterViewportStatsCommand(FEditorCommandRegistry& Registry, std::shared_ptr<FViewportStats> Stats)
{
	if (!Stats)
	{
		return std::unexpected(FEditorCommandError{.Code = EEditorCommandErrorCode::InvalidDescriptor, .Message = "Viewport stats state is unavailable"});
	}

	return Registry.Register({"stat", "Toggle viewport statistics: stat unit or stat fps", [Stats = std::move(Stats)](const std::span<const std::string_view> Arguments) -> std::expected<FEditorCommandResult, FEditorCommandError>
	{
		if (Arguments.size() != 1 || (Arguments[0] != "unit" && Arguments[0] != "fps"))
		{
			return std::unexpected(FEditorCommandError{.Code = EEditorCommandErrorCode::ExecutionFailed, .Message = "Usage: stat unit | stat fps"});
		}

		bool& bVisible = Arguments[0] == "unit" ? Stats->bUnitVisible : Stats->bFpsVisible;
		bVisible = !bVisible;
		return FEditorCommandResult{0, std::string(Arguments[0]) + (bVisible ? " enabled" : " disabled")};
	}});
}
}
