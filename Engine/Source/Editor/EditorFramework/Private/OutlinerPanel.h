#pragma once

#include "Herta/Level/LevelSerialization.h"

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
	int FolderIndex = -1;
	int Depth = 0;
	bool bHasChildren = false;
};

struct FOutlinerReparentRequest
{
	std::vector<FObjectId> Objects{};
	std::optional<FObjectId> Parent{};
};

enum class EOutlinerFolderAction
{
	Create,
	Delete,
	MoveFolder,
	MoveEntities,
};

struct FOutlinerFolderRequest
{
	EOutlinerFolderAction Action = EOutlinerFolderAction::Create;
	FObjectId Folder{};
	std::optional<FObjectId> Parent{};
	std::vector<FObjectId> Objects{};
	std::string Name{};
};

struct FOutlinerPanelState
{
	ImGuiTextFilter Search;
	std::array<char, 1025> RenameBuffer{};
	std::vector<int> VisibleIndices;
	std::vector<FOutlinerRow> VisibleRows;
	// Measured by last frame's clipper; pinning the folders above the top row needs it before this frame's rows.
	float RowHeight = 0.f;
	std::set<FObjectId> CollapsedObjects;
	std::set<FObjectId> CollapsedFolders;
	std::optional<FOutlinerReparentRequest> ReparentRequest;
	std::optional<FOutlinerFolderRequest> FolderRequest;
	std::vector<bool> SelectedMask;
	std::vector<std::pair<FObjectId, std::optional<FObjectId>>> CachedHierarchy;
	std::vector<FLevelFolder> CachedFolders;
	std::vector<FOutlinerRow> HierarchyRows;
	std::vector<int> ParentIndices;
	FObjectId RenameObject{};
	FObjectId SelectedFolder{};
	FObjectId RenameFolder{};
	// The type chip in effect, as an EPreviewObjectCategory, or -1 for all.
	int CategoryFilter = -1;
	// Editor-only and for this session: hidden entities do not draw or pick in the viewport, and locked ones do not pick.
	std::set<FObjectId> HiddenObjects;
	std::set<FObjectId> LockedObjects;
	bool bRenameRequested = false;
	bool bRevealSelection = false;
	bool bRenaming = false;
	bool bRenameCommitted = false;

	// While searching or filtering by type, rows show expanded and keep the ancestors of every match.
	[[nodiscard]] bool IsFiltering() const
	{
		return Search.IsActive() || CategoryFilter >= 0;
	}

	[[nodiscard]] bool IsObjectVisible(const std::string_view Label, const bool bStaticMesh = true) const
	{
		const std::string SearchText = std::string(Label) + (bStaticMesh ? " Static Mesh" : " Entity");
		return Search.PassFilter(SearchText.c_str());
	}
};

void BuildOutlinerVisibleRows(FOutlinerPanelState& State, std::span<const FPreviewObject> Objects, std::span<const FLevelFolder> Folders = {});

bool ExpandOutlinerAncestors(FOutlinerPanelState& State, std::span<const FPreviewObject> Objects, std::span<const FLevelFolder> Folders, int ObjectIndex);

std::optional<FOutlinerReparentRequest> MakeOutlinerReparentRequest(std::span<const FPreviewObject> Objects, const FPreviewSelection& Selection, int SourceIndex, std::optional<FObjectId> Parent);

bool DrawOutlinerRenameField(FOutlinerPanelState& State, ImVec2 Position, float Width, bool bStartRename);

bool DrawPreviewOutlinerContents(FPreviewSelection& Selection, std::span<const FPreviewObject> Objects, bool bDragging, FOutlinerPanelState& State, std::span<const FLevelFolder> Folders = {});

[[nodiscard]] bool DrawPreviewOutlinerPanel(FToolUIContext& ToolUI, bool& bOpen, FPreviewSelection& Selection, std::span<const FPreviewObject> Objects, bool bDragging, FOutlinerPanelState& State, std::span<const FLevelFolder> Folders = {});
}
