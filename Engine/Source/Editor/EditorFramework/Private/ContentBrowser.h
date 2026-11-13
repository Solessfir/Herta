#pragma once

#include "PreviewAssets.h"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Herta
{
inline constexpr const char* ContentAssetPayload = "Herta.ContentAsset";

struct FContentBrowserState
{
	void Refresh(std::span<const FPreviewAssetOption> Options, std::uint64_t Generation, std::span<const std::string> Directories = {});
	void Filter();
	void RefreshFolders();
	void AdjustZoom(float Wheel);
	std::string GetImportDestination() const;
	std::string GetFileManagerFolder() const;
	// A click on the grid's tile at Index, counting folders before assets: alone it selects only that tile, bToggle adds or removes it, and bRange selects every tile from the previous click.
	void ClickTile(std::size_t Index, bool bToggle, bool bRange);
	void SelectAllTiles();
	void SelectAsset(FAssetId Asset);
	void SelectFolder(const std::string& Path);
	void ClearSelection();
	bool IsSelected(FAssetId Asset) const;
	bool IsSelected(const std::string& Path) const;
	// The selected project assets, which can move and be deleted; Engine content is read-only.
	std::vector<FAssetId> GetSelectedGameAssets() const;

	std::array<char, 256> Search{};
	bool bFocusRequested = false;
	std::string Folder = "Game";
	std::string SelectedFolder;
	std::string ContextFolder;
	FAssetId Selected{};
	// Every selected tile beside Selected and SelectedFolder, which name the tile clicked last.
	std::vector<FAssetId> SelectedAssets;
	std::vector<std::string> SelectedFolders;
	std::size_t SelectionAnchor = 0;
	FAssetId ContextAsset{};
	FAssetId OpenMaterialRequested{};
	FAssetId OpenAudioGraphRequested{};
	FAssetId OpenVfxGraphRequested{};
	FAssetId OpenGameplayGraphRequested{};
	FAssetId OpenStateTreeRequested{};
	std::vector<FPreviewAssetOption> Assets;
	std::vector<std::string> Folders;
	std::vector<std::size_t> VisibleFolders;
	std::set<std::string> Collapsed;
	std::vector<std::size_t> Matches;
	std::vector<std::size_t> FolderMatches;
	std::uint64_t Generation = std::numeric_limits<std::uint64_t>::max();
	float Zoom = 3.f;
	float SidebarWidth = 190.f;
	// The folder tree beside the grid; the button before the path hides it.
	bool bSidebarOpen = true;
	// The type chip in effect, as an index into the chips, or -1 for every type.
	int TypeFilter = -1;
	// Assets directly inside each of Folders, shown beside the tree rows.
	std::vector<int> FolderAssetCounts;

	std::array<char, 256> FolderName{};
	std::string RenamingFolder;
	bool bFocusFolderName = false;
	std::string FolderError;

	// Asset rename and delete, each confirmed in its own modal.
	FAssetId RenamingAsset{};
	std::vector<FAssetId> DeletingAssets;
	std::array<char, 128> AssetName{};
	std::string AssetError;
	bool bOpenAssetRename = false;
	bool bOpenAssetDelete = false;
	// Assets dropped on a folder, moved once the frame's drawing ends.
	std::optional<std::pair<std::vector<FAssetId>, std::string>> MoveRequested;
};

bool IsPlaceableContentAsset(const FPreviewAssetOption& Asset) noexcept;
bool IsSoundContentAsset(const FPreviewAssetOption& Asset) noexcept;
bool IsEffectContentAsset(const FPreviewAssetOption& Asset) noexcept;
void ToggleContentBrowser(bool& bOpen, FContentBrowserState& State);
// Shows a folder by its mount-prefixed path, finishing any folder rename in progress first.
void NavigateContentFolder(FContentBrowserState& State, const std::string& Folder, FPreviewAssets* Assets);
// The drawer variant floats over a document tab and closes once focus leaves it.
bool DrawContentBrowser(FToolUIContext& ToolUI, bool& bOpen, FContentBrowserState& State, FPreviewAssets* Assets, bool bDrawer = false);
}
