#pragma once

#include <imgui.h>
#include <span>
#include <string>
#include <string_view>

namespace Herta
{
class FToolUIContext;
struct FPreviewObject;

struct FOutlinerPanelState
{
	ImGuiTextFilter Search;

	[[nodiscard]] bool IsObjectVisible(const std::string_view Label) const
	{
		const std::string SearchText = std::string(Label) + " Static Mesh";
		return Search.PassFilter(SearchText.c_str());
	}
};

[[nodiscard]] bool DrawPreviewOutlinerPanel(FToolUIContext& ToolUI, bool& bOpen, int& SelectedObject, std::span<const FPreviewObject> Objects, bool bDragging, FOutlinerPanelState& State);
}
