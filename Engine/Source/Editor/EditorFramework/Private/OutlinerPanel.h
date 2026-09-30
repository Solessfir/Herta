#pragma once

#include <imgui.h>

namespace Herta
{
class FToolUIContext;

struct FOutlinerPanelState
{
	ImGuiTextFilter Search;

	[[nodiscard]] bool IsPreviewCubeVisible() const
	{
		return Search.PassFilter("Preview Cube Static Mesh");
	}
};

[[nodiscard]] bool DrawPreviewOutlinerPanel(FToolUIContext& ToolUI, bool& bOpen, bool& bSelected, bool bDragging, FOutlinerPanelState& State);
}
