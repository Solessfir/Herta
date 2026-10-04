#pragma once

#include "Herta/Scene/World.h"

#include <imgui.h>

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
class FToolUIContext;
struct FPreviewObject;
struct FPreviewSelection;

struct FOutlinerPanelState
{
	ImGuiTextFilter Search;
	std::array<char, 1025> RenameBuffer{};
	std::vector<int> VisibleIndices;
	std::vector<bool> SelectedMask;
	FObjectId RenameObject{};
	bool bRenameRequested = false;
	bool bRenaming = false;
	bool bRenameCommitted = false;

	[[nodiscard]] bool IsObjectVisible(const std::string_view Label) const
	{
		const std::string SearchText = std::string(Label) + " Static Mesh";
		return Search.PassFilter(SearchText.c_str());
	}
};

bool DrawOutlinerRenameField(FOutlinerPanelState& State, ImVec2 Position, float Width, bool bStartRename);

[[nodiscard]] bool DrawPreviewOutlinerPanel(FToolUIContext& ToolUI, bool& bOpen, FPreviewSelection& Selection, std::span<const FPreviewObject> Objects, bool bDragging, FOutlinerPanelState& State);
}
