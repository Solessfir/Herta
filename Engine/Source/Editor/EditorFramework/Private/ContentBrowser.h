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

	std::array<char, 256> Search{};
	bool bFocusRequested = false;
	std::string Folder = "Game";
	std::string SelectedFolder;
	std::string ContextFolder;
	FAssetId Selected{};
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

	std::array<char, 256> FolderName{};
	std::string RenamingFolder;
	bool bFocusFolderName = false;
	std::string FolderError;

	// Asset rename and delete, each confirmed in its own modal.
	FAssetId RenamingAsset{};
	FAssetId DeletingAsset{};
	std::array<char, 128> AssetName{};
	std::string AssetError;
	bool bOpenAssetRename = false;
	bool bOpenAssetDelete = false;
	// An asset dropped on a folder, moved once the frame's drawing ends.
	std::optional<std::pair<FAssetId, std::string>> MoveRequested;
};

bool IsPlaceableContentAsset(const FPreviewAssetOption& Asset) noexcept;
bool IsSoundContentAsset(const FPreviewAssetOption& Asset) noexcept;
bool IsEffectContentAsset(const FPreviewAssetOption& Asset) noexcept;
void ToggleContentBrowser(bool& bOpen, FContentBrowserState& State);
// The drawer variant floats over a document tab and closes once focus leaves it.
bool DrawContentBrowser(FToolUIContext& ToolUI, bool& bOpen, FContentBrowserState& State, FPreviewAssets* Assets, bool bDrawer = false);
}
