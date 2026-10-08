#pragma once

#include "PreviewAssets.h"

#include <array>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
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
	std::vector<FPreviewAssetOption> Assets;
	std::vector<std::string> Folders;
	std::vector<std::size_t> VisibleFolders;
	std::set<std::string> Collapsed;
	std::vector<std::size_t> Matches;
	std::vector<std::size_t> FolderMatches;
	std::uint64_t Generation = std::numeric_limits<std::uint64_t>::max();
	float Zoom = 3.f;
	// The zoom slider appears only while hovered, dragged, or briefly after a change, so the footer stays quiet.
	float ShownZoom = 3.f;
	double ZoomVisibleUntil = 0.;
	bool bZoomHovered = false;
	float SidebarWidth = 190.f;

	std::array<char, 256> FolderName{};
	std::string RenamingFolder;
	bool bFocusFolderName = false;
	std::string FolderError;
};

bool IsPlaceableContentAsset(const FPreviewAssetOption& Asset) noexcept;
void ToggleContentBrowser(bool& bOpen, FContentBrowserState& State);
bool DrawContentBrowser(FToolUIContext& ToolUI, bool& bOpen, FContentBrowserState& State, FPreviewAssets* Assets);
}
