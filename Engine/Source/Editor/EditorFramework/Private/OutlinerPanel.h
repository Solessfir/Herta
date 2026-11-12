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
	// How many folders were pinned last frame; the scroll carries their height, so the rest of the list holds still while they pin.
	int PinnedCount = 0;
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

	// Text the panel sets each frame in the editor's language; the list draws it without the context.
	struct FTexts
	{
		std::string Selected = "selected";
		std::string Lock = "Lock, so viewport clicks pass through";
		std::string Locked = "Locked: viewport clicks pass through. Click to unlock.";
		std::string Hide = "Hide in the editor viewport";
		std::string Hidden = "Hidden in the editor viewport. Click to show.";
	} Texts;

	// The type chip in effect, as an EPreviewObjectCategory, or -1 for all.
	int CategoryFilter = -1;
	// Editor-only and kept per user for each level: hidden entities do not draw, light, or pick in the viewport, and locked ones do not pick.
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

// The hidden and locked entities of every level, as text kept per user: a "level <path>" line, then "hidden <id>" and "locked <id>" lines.
struct FOutlinerFlags
{
	std::set<FObjectId> Hidden;
	std::set<FObjectId> Locked;

	[[nodiscard]] bool operator==(const FOutlinerFlags&) const = default;
};

// One level's flags out of the text; lines that do not parse are skipped.
[[nodiscard]] FOutlinerFlags ParseOutlinerFlags(std::string_view Text, std::string_view Level);
// The text with one level's section replaced, or dropped when the level has no flags left.
[[nodiscard]] std::string ReplaceOutlinerFlags(std::string_view Text, std::string_view Level, const FOutlinerFlags& Flags);

void BuildOutlinerVisibleRows(FOutlinerPanelState& State, std::span<const FPreviewObject> Objects, std::span<const FLevelFolder> Folders = {});

bool ExpandOutlinerAncestors(FOutlinerPanelState& State, std::span<const FPreviewObject> Objects, std::span<const FLevelFolder> Folders, int ObjectIndex);

std::optional<FOutlinerReparentRequest> MakeOutlinerReparentRequest(std::span<const FPreviewObject> Objects, const FPreviewSelection& Selection, int SourceIndex, std::optional<FObjectId> Parent);

bool DrawOutlinerRenameField(FOutlinerPanelState& State, ImVec2 Position, float Width, bool bStartRename);

bool DrawPreviewOutlinerContents(FPreviewSelection& Selection, std::span<const FPreviewObject> Objects, bool bDragging, FOutlinerPanelState& State, std::span<const FLevelFolder> Folders = {});

[[nodiscard]] bool DrawPreviewOutlinerPanel(FToolUIContext& ToolUI, bool& bOpen, FPreviewSelection& Selection, std::span<const FPreviewObject> Objects, bool bDragging, FOutlinerPanelState& State, std::span<const FLevelFolder> Folders = {});
}
