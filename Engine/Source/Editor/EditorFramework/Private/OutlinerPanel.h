#pragma once

#include "Herta/Scene/World.h"

#include <imgui.h>

#include <array>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Herta
{
class FToolUIContext;
struct FPreviewObject;
struct FPreviewSelection;

struct FOutlinerRow
{
	int ObjectIndex = -1;
	int Depth = 0;
	bool bHasChildren = false;
};

struct FOutlinerReparentRequest
{
	std::vector<FObjectId> Objects{};
	std::optional<FObjectId> Parent{};
};

struct FOutlinerPanelState
{
	ImGuiTextFilter Search;
	std::array<char, 1025> RenameBuffer{};
	std::vector<int> VisibleIndices;
	std::vector<FOutlinerRow> VisibleRows;
	std::set<FObjectId> CollapsedObjects;
	std::optional<FOutlinerReparentRequest> ReparentRequest;
	std::vector<bool> SelectedMask;
	std::vector<std::pair<FObjectId, std::optional<FObjectId>>> CachedHierarchy;
	std::vector<FOutlinerRow> HierarchyRows;
	std::vector<int> ParentIndices;
	FObjectId RenameObject{};
	bool bRenameRequested = false;
	bool bRenaming = false;
	bool bRenameCommitted = false;

	[[nodiscard]] bool IsObjectVisible(const std::string_view Label, const bool bStaticMesh = true) const
	{
		const std::string SearchText = std::string(Label) + (bStaticMesh ? " Static Mesh" : " Entity");
		return Search.PassFilter(SearchText.c_str());
	}
};

void BuildOutlinerVisibleRows(FOutlinerPanelState& State, std::span<const FPreviewObject> Objects);

std::optional<FOutlinerReparentRequest> MakeOutlinerReparentRequest(std::span<const FPreviewObject> Objects, const FPreviewSelection& Selection, int SourceIndex, std::optional<FObjectId> Parent);

bool DrawOutlinerRenameField(FOutlinerPanelState& State, ImVec2 Position, float Width, bool bStartRename);

bool DrawPreviewOutlinerContents(FPreviewSelection& Selection, std::span<const FPreviewObject> Objects, bool bDragging, FOutlinerPanelState& State);

[[nodiscard]] bool DrawPreviewOutlinerPanel(FToolUIContext& ToolUI, bool& bOpen, FPreviewSelection& Selection, std::span<const FPreviewObject> Objects, bool bDragging, FOutlinerPanelState& State);
}
