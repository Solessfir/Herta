#include "ContentBrowser.h"

#include "Herta/Assets/AssetSearch.h"
#include "Herta/Platform/FileDialog.h"
#include "Herta/ToolUI/ToolUI.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace Herta
{
namespace
{
bool IsInside(const std::string_view Path, const std::string_view Folder)
{
	return Path.size() > Folder.size() && Path.starts_with(Folder) && Path[Folder.size()] == '/';
}

std::string_view Leaf(const std::string_view Path)
{
	const auto Separator = Path.rfind('/');
	return Separator == std::string_view::npos ? Path : Path.substr(Separator + 1);
}

bool FinishFolderRename(FContentBrowserState& State, FPreviewAssets& Assets)
{
	if (State.RenamingFolder.empty())
	{
		return true;
	}

	const auto Renamed = Assets.RenameFolder(State.RenamingFolder, State.FolderName.data());
	if (!Renamed)
	{
		State.FolderError = Renamed.error().Message;
		State.bFocusFolderName = true;
		return false;
	}

	State.SelectedFolder = *Renamed;
	State.RenamingFolder.clear();
	State.bFocusFolderName = false;
	State.FolderError.clear();
	return true;
}

void NavigateContentFolder(FContentBrowserState& State, const std::string& Folder, FPreviewAssets* const Assets)
{
	const bool bRenamedTarget = !State.RenamingFolder.empty() && Folder == State.RenamingFolder;
	if (Assets && !FinishFolderRename(State, *Assets))
	{
		return;
	}

	State.Folder = bRenamedTarget ? State.SelectedFolder : Folder;
	State.SelectedFolder.clear();
	State.Filter();
}

void DrawFolderRename(FContentBrowserState& State, FPreviewAssets& Assets)
{
	const bool bStart = State.bFocusFolderName;
	if (bStart)
	{
		ImGui::SetKeyboardFocusHere();
	}

	const bool bCommit = ImGui::InputText("##FolderName", State.FolderName.data(), State.FolderName.size(), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
	if (bStart && ImGui::IsItemActive())
	{
		State.bFocusFolderName = false;
	}

	if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
	{
		State.RenamingFolder.clear();
		State.bFocusFolderName = false;
		State.FolderError.clear();
	}
	else if (bCommit || (!bStart && ImGui::IsItemDeactivated()))
	{
		FinishFolderRename(State, Assets);
	}
}

bool DrawFolderTile(const std::string& Path, FContentBrowserState& State, FPreviewAssets* const Assets, const ImVec2 Size, const float Scale, const bool bList)
{
	ImGui::PushID(Path.c_str());
	ImGui::BeginGroup();
	const ImVec2 Start = ImGui::GetCursorScreenPos();
	const bool bRenaming = Assets && State.RenamingFolder == Path;
	bool bOpen = false;
	const ImGuiSelectableFlags Flags = ImGuiSelectableFlags_AllowDoubleClick | static_cast<ImGuiSelectableFlags>(ImGuiSelectableFlags_NoPadWithHalfSpacing) | (bRenaming ? ImGuiSelectableFlags_AllowOverlap : 0);
	if (ImGui::Selectable("##Folder", State.SelectedFolder == Path, Flags, Size))
	{
		State.SelectedFolder = Path;
		State.Selected = {};
		bOpen = !bRenaming && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
	}

	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup) && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
	{
		State.ContextFolder = Path;
	}

	const float Font = ImGui::GetFontSize();
	const float IconSize = bList ? Font : Size.y - Font * 2.7f;
	const float IconBottom = Start.y + Font * 0.35f + IconSize;
	const ImVec2 LabelMinimum = bList ? ImVec2{Start.x + Font + 16.f * Scale, Start.y + (Size.y - Font) * 0.5f} : ImVec2{Start.x + 4.f * Scale, IconBottom + Font * 0.3f};
	ToolUIIcon(EToolUIMenuIcon::ContentBrowser, bList ? Start.x + Font * 0.5f + 6.f * Scale : Start.x + Size.x * 0.5f, bList ? Start.y + Size.y * 0.5f : Start.y + Font * 0.35f + IconSize * 0.5f, bList ? Scale : IconSize / 20.f);
	if (bRenaming)
	{
		const bool bScrollToRow = State.bFocusFolderName;
		ImGui::SetCursorScreenPos(LabelMinimum);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {2.f * Scale, Scale});
		ImGui::SetNextItemWidth(std::max(1.f, Start.x + Size.x - LabelMinimum.x - 4.f * Scale));
		DrawFolderRename(State, *Assets);
		ImGui::PopStyleVar();
		if (bScrollToRow)
		{
			ImGui::SetScrollHereY();
		}
	}
	else
	{
		const auto Name = Leaf(Path);
		ImGui::RenderTextClipped(LabelMinimum, {Start.x + Size.x - 4.f * Scale, LabelMinimum.y + Font}, Name.data(), Name.data() + Name.size(), nullptr, {bList ? 0.f : 0.5f, 0.f});
	}

	ImGui::EndGroup();
	ImGui::PopID();
	return bOpen;
}

void DrawAssetTooltip(const FPreviewAssetOption& Asset, const FPreviewAssets* const Assets, const float Scale)
{
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10.f * Scale, 8.f * Scale});
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {6.f * Scale, 4.f * Scale});
	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {4.f * Scale, 2.f * Scale});

	if (ImGui::BeginTooltip())
	{
		const float Width = std::min(380.f * Scale, ImGui::GetMainViewport()->WorkSize.x - 32.f * Scale);
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Width);
		const auto Name = Leaf(Asset.Label);
		ImGui::TextUnformatted(Name.data(), Name.data() + Name.size());
		ImGui::TextDisabled("%s", IsPlaceableContentAsset(Asset) ? "Static Mesh" : Asset.Importer == "Material" ? "Material"
		                                                                       : Asset.Importer == "Texture"    ? "Texture"
		                                                                                                        : "Asset");
		ImGui::Separator();
		const FPreviewMeshSlot* const Cached = Assets ? Assets->GetCachedAsset(Asset.Id) : nullptr;
		if (ImGui::BeginTable("##AssetMetadata", 2, ImGuiTableFlags_SizingStretchProp, {Width, 0.f}))
		{
			ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 92.f * Scale);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

			const auto Row = [](const char* Label, const std::string_view Value)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextDisabled("%s", Label);
				ImGui::TableSetColumnIndex(1);
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
				ImGui::TextUnformatted(Value.data(), Value.data() + Value.size());
				ImGui::PopTextWrapPos();
			};

			Row("Source", Asset.Label);
			Row("Asset ID", Asset.Id.ToString());

			if (Cached && Cached->Metadata)
			{
				if (const auto* Model = std::get_if<FPreviewModelMetadata>(&*Cached->Metadata))
				{
					Row("Vertices", std::format("{}", Model->Vertices));
					Row("Triangles", std::format("{}", Model->Triangles));
					Row("Materials", std::format("{}", Model->Materials));
					const FVector3 Size = Model->BoundsMaximum - Model->BoundsMinimum;
					Row("Bounds size", std::format("{:.3g} x {:.3g} x {:.3g} m", Size.X, Size.Y, Size.Z));
				}
				else if (const auto* Texture = std::get_if<FPreviewTextureMetadata>(&*Cached->Metadata))
				{
					Row("Dimensions", std::format("{} x {}", Texture->Width, Texture->Height));
					Row("Format", Texture->PixelFormat == ETexturePixelFormat::Rgba32Float ? "RGBA32F (HDR)" : "RGBA8");
					Row("Color space", Texture->ColorSpace == ETextureColorSpace::Srgb ? "sRGB" : "Linear");
					Row("Mip levels", std::format("{}", Texture->Mips));
				}
				else if (const auto* Material = std::get_if<FPreviewMaterialMetadata>(&*Cached->Metadata))
				{
					Row("Name", Material->Name);
					Row("Texture maps", std::format("{}", Material->TextureMaps));
					Row("Blend", Material->BlendMode == EMaterialBlendMode::Masked ? "Masked" : "Opaque");
					Row("Shader", Cached->MaterialSource && !Cached->MaterialSource->ShaderPath.empty() ? std::string_view(Cached->MaterialSource->ShaderPath) : "Engine PBR");
				}
			}

			if (!Cached || !Cached->Metadata || Cached->bLoading || !Cached->Error.empty())
			{
				std::string_view Status = Cached && !Cached->Error.empty() ? std::string_view{Cached->Error} : "Not loaded";
				if (Cached && Cached->bLoading)
				{
					Status = Cached->Metadata ? "Reimporting..." : "Loading...";
				}

				Row("Preview", Status);
			}

			ImGui::EndTable();
		}

		if (IsPlaceableContentAsset(Asset))
		{
			ImGui::Separator();
			ImGui::TextDisabled("Drag into the viewport to place");
		}
		else if (Asset.Importer == "Material")
		{
			ImGui::Separator();
			ImGui::TextDisabled("Double-click to edit. Drag onto a mesh to assign.");
		}

		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}

	ImGui::PopStyleVar(3);
}

void DrawAssetTile(const FPreviewAssetOption& Asset, FContentBrowserState& State, const ImVec2 Size, const float Scale, const bool bList, FPreviewAssets* const Assets)
{
	const std::string Id = Asset.Id.ToString();
	ImGui::PushID(Id.c_str());
	const ImVec2 Start = ImGui::GetCursorScreenPos();
	if (ImGui::Selectable("##Asset", State.Selected == Asset.Id, static_cast<ImGuiSelectableFlags>(ImGuiSelectableFlags_NoPadWithHalfSpacing) | ImGuiSelectableFlags_AllowDoubleClick, Size))
	{
		State.Selected = Asset.Id;
		State.SelectedFolder.clear();
		if (Asset.Importer == "Material" && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			State.OpenMaterialRequested = Asset.Id;
		}
	}

	if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
	{
		State.ContextAsset = State.Selected = Asset.Id;
		State.SelectedFolder.clear();
	}

	if ((IsPlaceableContentAsset(Asset) || Asset.Importer == "Material") && ImGui::BeginDragDropSource())
	{
		ImGui::SetDragDropPayload(ContentAssetPayload, &Asset.Id, sizeof(Asset.Id));
		ImGui::TextUnformatted(Asset.Label.c_str());
		ImGui::TextDisabled("%s", Asset.Importer == "Material" ? "Assign material" : "Place static mesh");
		ImGui::EndDragDropSource();
	}

	if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
	{
		DrawAssetTooltip(Asset, Assets, Scale);
	}

	ImDrawList* const Draw = ImGui::GetWindowDrawList();
	const float Font = ImGui::GetFontSize();
	if (bList)
	{
		ToolUIIcon(IsPlaceableContentAsset(Asset) ? EToolUIMenuIcon::Cube : Asset.Importer == "Material" ? EToolUIMenuIcon::Material
		                                                                                                 : EToolUIMenuIcon::Panel,
		    Start.x + Font * 0.5f + 6.f * Scale, Start.y + Size.y * 0.5f, Scale);
		const auto Name = Leaf(Asset.Label);
		ImGui::RenderTextClipped({Start.x + Font + 16.f * Scale, Start.y + (Size.y - Font) * 0.5f}, {Start.x + Size.x - 4.f * Scale, Start.y + Size.y}, Name.data(), Name.data() + Name.size(), nullptr);
		ImGui::PopID();
		return;
	}

	const float IconSize = Size.y - Font * 2.7f;
	const ImVec2 Minimum{Start.x + (Size.x - IconSize) * 0.5f, Start.y + Font * 0.35f};
	const ImVec2 Maximum{Minimum.x + IconSize, Minimum.y + IconSize};
	Draw->AddRectFilled(Minimum, Maximum, ImGui::GetColorU32(ImVec4{0.28f, 0.31f, 0.34f, 0.42f}), 6.f);
	const auto Thumbnail = Assets ? Assets->GetThumbnail(Asset.Id) : FEditorAssetThumbnail{};
	if (Thumbnail)
	{
		Draw->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(*Thumbnail)), Minimum, Maximum, {0.f, 0.f}, {1.f, 1.f}, IM_COL32_WHITE, ImGui::GetStyle().FrameRounding);
	}
	else
	{
		ToolUIIcon(IsPlaceableContentAsset(Asset) ? EToolUIMenuIcon::Cube : Asset.Importer == "Material" ? EToolUIMenuIcon::Material
		                                                                                                 : EToolUIMenuIcon::Panel,
		    (Minimum.x + Maximum.x) * 0.5f, (Minimum.y + Maximum.y) * 0.5f, std::max(Scale, IconSize / 32.f));
	}

	const auto Name = Leaf(Asset.Label);
	ImGui::RenderTextClipped({Start.x + 4.f, Maximum.y + Font * 0.3f}, {Start.x + Size.x - 4.f, Maximum.y + Font * 1.3f}, Name.data(), Name.data() + Name.size(), nullptr, {0.5f, 0.f});
	ImGui::PopID();
}
}

bool IsPlaceableContentAsset(const FPreviewAssetOption& Asset) noexcept
{
	return Asset.Id.IsValid() && (Asset.Importer == "Gltf" || Asset.Importer == "Blender");
}

void FContentBrowserState::Refresh(const std::span<const FPreviewAssetOption> Options, const std::uint64_t NewGeneration, const std::span<const std::string> Directories)
{
	if (Generation == NewGeneration)
	{
		return;
	}

	Generation = NewGeneration;
	Assets.assign(Options.begin(), Options.end());
	std::set<std::string> Paths{"Engine", "Game"};
	Paths.insert(Directories.begin(), Directories.end());
	for (const auto& Asset : Assets)
	{
		for (auto Slash = Asset.Label.find('/'); Slash != std::string::npos; Slash = Asset.Label.find('/', Slash + 1))
		{
			Paths.insert(Asset.Label.substr(0, Slash));
		}
	}

	Folders.assign(Paths.begin(), Paths.end());
	const auto HierarchyCharacter = [](const unsigned char Character)
	{
		return Character == '/' ? 0 : Character;
	};

	std::ranges::sort(Folders, [HierarchyCharacter](const auto& First, const auto& Second)
	{
		const bool bFirstGame = First == "Game" || First.starts_with("Game/");
		const bool bSecondGame = Second == "Game" || Second.starts_with("Game/");
		if (bFirstGame != bSecondGame)
		{
			return bFirstGame;
		}

		return std::ranges::lexicographical_compare(First, Second, {}, HierarchyCharacter, HierarchyCharacter);
	});

	if (!Paths.contains(Folder))
	{
		Folder = "Game";
	}

	if (!Paths.contains(SelectedFolder))
	{
		SelectedFolder.clear();
	}

	if (!RenamingFolder.empty() && !Paths.contains(RenamingFolder))
	{
		RenamingFolder.clear();
		bFocusFolderName = false;
	}

	if (!std::ranges::any_of(Assets, [this](const auto& Asset)
	{
		return Asset.Id == Selected;
	}))
	{
		Selected = {};
	}

	RefreshFolders();
	Filter();
}

void FContentBrowserState::RefreshFolders()
{
	VisibleFolders.clear();
	std::string Hidden;
	for (std::size_t Index = 0; Index < Folders.size(); ++Index)
	{
		const auto& Path = Folders[Index];
		if (!Hidden.empty() && IsInside(Path, Hidden))
		{
			continue;
		}

		Hidden.clear();
		VisibleFolders.push_back(Index);
		if (Collapsed.contains(Path))
		{
			Hidden = Path;
		}
	}
}

void FContentBrowserState::Filter()
{
	const bool bSearching = Search[0] != '\0';
	std::vector<std::string_view> FolderCandidates;
	std::vector<std::size_t> FolderIndices;
	for (std::size_t Index = 0; Index < Folders.size(); ++Index)
	{
		if (IsInside(Folders[Index], Folder) && (bSearching || Folders[Index].find('/', Folder.size() + 1) == std::string::npos))
		{
			FolderCandidates.emplace_back(Folders[Index]);
			FolderIndices.push_back(Index);
		}
	}

	FolderMatches.clear();
	if (const auto Results = SearchAssets(FolderCandidates, Search.data()))
	{
		for (const auto& Match : *Results)
		{
			FolderMatches.push_back(FolderIndices[Match.Index]);
		}
	}

	std::vector<std::string_view> Candidates;
	std::vector<std::size_t> Indices;
	for (std::size_t Index = 0; Index < Assets.size(); ++Index)
	{
		if (IsInside(Assets[Index].Label, Folder) && (bSearching || Assets[Index].Label.find('/', Folder.size() + 1) == std::string::npos))
		{
			Candidates.emplace_back(Assets[Index].Label);
			Indices.push_back(Index);
		}
	}

	Matches.clear();
	if (const auto Results = SearchAssets(Candidates, Search.data()))
	{
		for (const auto& Match : *Results)
		{
			Matches.push_back(Indices[Match.Index]);
		}
	}
}

std::string FContentBrowserState::GetImportDestination() const
{
	return Folder.starts_with("Game/") ? Folder.substr(5) : std::string{};
}

std::string FContentBrowserState::GetFileManagerFolder() const
{
	if (!SelectedFolder.empty())
	{
		return SelectedFolder;
	}

	for (const std::size_t Index : Matches)
	{
		const auto& Asset = Assets[Index];
		if (Asset.Id == Selected)
		{
			return Asset.Label.substr(0, Asset.Label.rfind('/'));
		}
	}

	return Folder;
}

void FContentBrowserState::AdjustZoom(const float Wheel)
{
	Zoom = std::clamp(Zoom + Wheel, 0.f, 9.f);
}

void ToggleContentBrowser(bool& bOpen, FContentBrowserState& State)
{
	const ImGuiWindow* const Window = ImGui::FindWindowByID(ImHashStr("Content Browser"));
	bOpen = !(bOpen && Window && Window->WasActive && !Window->Hidden);
	State.bFocusRequested = bOpen;
	if (!bOpen)
	{
		State.RenamingFolder.clear();
		State.bFocusFolderName = false;
		State.FolderError.clear();
	}
}

bool DrawContentBrowser(FToolUIContext& ToolUI, bool& bOpen, FContentBrowserState& State, FPreviewAssets* const Assets)
{
	State.Refresh(Assets ? Assets->GetOptions() : std::span<const FPreviewAssetOption>{}, Assets ? Assets->GetOptionsGeneration() : 0, Assets ? Assets->GetFolders() : std::span<const std::string>{});
	const bool bBusy = Assets && (Assets->IsScanning() || Assets->IsImporting());
	bool bImport = false;
	bool bNewMaterial = false;
	std::string CreationFolder = State.Folder;
	std::vector<FAssetId> ThumbnailAssets;
	if (State.bFocusRequested)
	{
		ImGui::SetNextWindowFocus();
	}

	if (ToolUI.BeginPanel("Content Browser", &bOpen))
	{
		State.bFocusRequested = false;
		const float Scale = ImGui::GetFontSize() / ToolUI.GetMetrics().BaseFontSize;
		const bool bCanCreateFolder = Assets && !bBusy && State.RenamingFolder.empty() && !(GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) && (State.Folder == "Game" || State.Folder.starts_with("Game/"));
		const bool bFolderShortcutsAvailable = !ImGui::GetIO().AppFocusLost && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetIO().KeyMods == (ImGuiMod_Ctrl | ImGuiMod_Shift) && !(GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) && State.RenamingFolder.empty() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
		bool bNewFolder = bCanCreateFolder && bFolderShortcutsAvailable && ImGui::IsKeyPressed(ImGuiKey_N, false);
		bool bOpenDirectory = Assets && bFolderShortcutsAvailable && ImGui::IsKeyPressed(ImGuiKey_O, false);
		std::string OpenDirectoryFolder = bOpenDirectory ? State.GetFileManagerFolder() : std::string{};
		ImGui::PushStyleColor(ImGuiCol_NavCursor, {0.f, 0.f, 0.f, 0.f});
		ImGui::PushStyleVar(ImGuiStyleVar_SelectableRounding, ImGui::GetStyle().FrameRounding);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.f * Scale, 5.f * Scale});
		if (ToolUIButton("Import...", EToolUIMenuIcon::Import, ImGui::GetFontSize() + 10.f * Scale))
		{
			bImport = true;
		}

		const float SearchWidth = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - ImGui::GetItemRectMax().x - ImGui::GetStyle().ItemSpacing.x;
		if (SearchWidth >= 180.f * Scale)
		{
			ImGui::SameLine();
		}

		ImGui::SetNextItemWidth(-1.f);

		if (ToolUI.DrawSearchField("##ContentSearch", "Search assets", State.Search.data(), State.Search.size()))
		{
			if (Assets && !FinishFolderRename(State, *Assets))
			{
				State.Search.fill('\0');
			}

			State.Filter();
		}

		ImGui::PopStyleVar();
		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			State.ContextAsset = {};
		}

		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
		{
			State.ContextFolder = State.Folder;
			ImGui::OpenPopup("Content actions");
		}

		ImGui::SetNextWindowSizeConstraints({220.f * Scale, 0.f}, {std::numeric_limits<float>::max(), std::numeric_limits<float>::max()});
		ImGui::SetNextWindowBgAlpha(0.f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12.f * Scale, 8.f * Scale});
		if (ImGui::BeginPopup("Content actions", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
		{
			const ImVec2 Position = ImGui::GetWindowPos();
			const ImVec2 Size = ImGui::GetWindowSize();
			ToolUI.DrawGlassSurface(Position.x, Position.y, Size.x, Size.y, ToolUI.GetMetrics().PopupRounding * Scale);
			ImGui::BeginDisabled(!Assets || bBusy);
			bImport |= ToolUIMenuItem("Import...", EToolUIMenuIcon::Import, nullptr, "Ctrl+I");
			ImGui::EndDisabled();
			const bool bContextWritable = Assets && !bBusy && State.RenamingFolder.empty() && (State.ContextFolder == "Game" || State.ContextFolder.starts_with("Game/"));
			ImGui::BeginDisabled(!bContextWritable);
			if (ToolUIMenuItem("New folder", EToolUIMenuIcon::ContentBrowser, nullptr, "Ctrl+Shift+N"))
			{
				bNewFolder = true;
				CreationFolder = State.ContextFolder;
			}

			bNewMaterial |= ToolUIMenuItem("New material", EToolUIMenuIcon::Material);
			if (bNewMaterial)
			{
				CreationFolder = State.ContextFolder;
			}

			ImGui::EndDisabled();
			const auto Context = std::ranges::find(State.Assets, State.ContextAsset, &FPreviewAssetOption::Id);
			if (Context != State.Assets.end() && Context->Importer == "Material")
			{
				ImGui::Separator();
				if (ToolUIMenuItem("Edit material", EToolUIMenuIcon::Material))
				{
					State.OpenMaterialRequested = Context->Id;
				}
			}

			ImGui::Separator();
			ImGui::BeginDisabled(!Assets);
#ifdef HERTA_PLATFORM_WINDOWS
			const bool bOpenContextDirectory = ToolUIMenuItem("Open in Explorer", EToolUIMenuIcon::Open, nullptr, "Ctrl+Shift+O");
#else
			const bool bOpenContextDirectory = ToolUIMenuItem("Open in file manager", EToolUIMenuIcon::Open, nullptr, "Ctrl+Shift+O");
#endif
			if (bOpenContextDirectory)
			{
				bOpenDirectory = true;
				OpenDirectoryFolder = State.ContextFolder;
			}

			ImGui::EndDisabled();
			ImGui::EndPopup();
		}

		ImGui::PopStyleVar();
		if (bOpenDirectory)
		{
			const auto Directory = Assets->GetFolderPath(OpenDirectoryFolder);
			if (!Directory)
			{
				State.FolderError = Directory.error().Message;
			}
			else if (const auto Opened = OpenDirectoryInFileManager(*Directory); !Opened)
			{
				State.FolderError = Opened.error().Message;
			}
			else
			{
				State.FolderError.clear();
			}
		}

		if (bNewFolder)
		{
			const auto Created = Assets->CreateFolder(CreationFolder, "New Folder", true);
			if (Created)
			{
				State.SelectedFolder = *Created;
				State.Selected = {};
				State.Search.fill('\0');
				for (auto Slash = Created->find('/'); Slash != std::string::npos; Slash = Created->find('/', Slash + 1))
				{
					State.Collapsed.erase(Created->substr(0, Slash));
				}

				State.Refresh(Assets->GetOptions(), Assets->GetOptionsGeneration(), Assets->GetFolders());
				State.RenamingFolder = *Created;
				State.bFocusFolderName = true;
				State.FolderName.fill('\0');
				std::ranges::copy(Leaf(*Created), State.FolderName.begin());
				State.FolderError.clear();
			}
			else
			{
				State.FolderError = Created.error().Message;
			}
		}

		if (bNewMaterial)
		{
			const auto Created = Assets->CreateMaterial(CreationFolder, "New Material");
			if (Created)
			{
				State.Selected = State.OpenMaterialRequested = *Created;
				State.SelectedFolder.clear();
				State.Search.fill('\0');
				State.Filter();
				State.FolderError.clear();
			}
			else
			{
				State.FolderError = Created.error().Message;
			}
		}

		ImGui::Separator();
		const float Footer = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
		const ImVec2 Available = ImGui::GetContentRegionAvail();
		const bool bShowFolders = Available.x > 250.f * Scale;
		bool bFoldersChanged = false;
		if (bShowFolders)
		{
			const float DividerWidth = 5.f * Scale;
			float FolderWidth = std::clamp(State.SidebarWidth * Scale, 100.f * Scale, Available.x - 128.f * Scale - DividerWidth);
			float BrowserWidth = Available.x - FolderWidth - DividerWidth;
			const ImVec2 Start = ImGui::GetCursorScreenPos();
			const ImRect Divider{{Start.x + FolderWidth, Start.y}, {Start.x + FolderWidth + DividerWidth, Start.y + std::max(1.f, Available.y - Footer)}};
			const ImU32 DividerHovered = ImGui::GetColorU32(ImGuiCol_SeparatorHovered);
			const ImU32 DividerActive = ImGui::GetColorU32(ImGuiCol_SeparatorActive);
			const ImU32 DividerIdle = ImGui::GetColorU32(ImGuiCol_Separator);
			ImGui::PushStyleColor(ImGuiCol_Separator, {0.f, 0.f, 0.f, 0.f});
			ImGui::PushStyleColor(ImGuiCol_SeparatorHovered, {0.f, 0.f, 0.f, 0.f});
			ImGui::PushStyleColor(ImGuiCol_SeparatorActive, {0.f, 0.f, 0.f, 0.f});
			if (ImGui::SplitterBehavior(Divider, ImGui::GetID("##FolderDivider"), ImGuiAxis_X, &FolderWidth, &BrowserWidth, 100.f * Scale, 128.f * Scale, 4.f * Scale))
			{
				State.SidebarWidth = FolderWidth / Scale;
			}

			ImGui::PopStyleColor(3);
			ImGui::GetWindowDrawList()->AddRectFilled({Start.x + FolderWidth, Start.y + 4.f * Scale}, {Start.x + FolderWidth + DividerWidth, Divider.Max.y - 4.f * Scale}, ImGui::IsItemActive() ? DividerActive : ImGui::IsItemHovered() ? DividerHovered
			                                                                                                                                                                                                                              : DividerIdle,
			    DividerWidth * 0.5f);

			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {2.f * Scale, 2.f * Scale});
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {ImGui::GetStyle().ItemSpacing.x, 3.f * Scale});
			ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
			ImGui::BeginChild("##Folders", {FolderWidth, -Footer}, ImGuiChildFlags_Borders);
			ImGuiListClipper Clipper;
			Clipper.Begin(static_cast<int>(State.VisibleFolders.size()), ImGui::GetFrameHeightWithSpacing());
			while (Clipper.Step())
			{
				for (int Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; ++Row)
				{
					const std::size_t Index = State.VisibleFolders[static_cast<std::size_t>(Row)];
					const std::string& Path = State.Folders[Index];
					const bool bChildren = Index + 1 < State.Folders.size() && IsInside(State.Folders[Index + 1], Path);
					const float Indent = std::min(static_cast<float>(std::ranges::count(Path, '/')) * 8.f * Scale, ImGui::GetContentRegionAvail().x * 0.4f);
					ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Indent);
					const ImVec2 RowStart = ImGui::GetCursorScreenPos();
					ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_Framed;
					Flags |= bChildren ? 0 : ImGuiTreeNodeFlags_Leaf;
					Flags |= Path == State.Folder ? ImGuiTreeNodeFlags_Selected : 0;
					ImGui::SetNextItemOpen(!State.Collapsed.contains(Path), ImGuiCond_Always);
					const auto Label = Leaf(Path);
					ImGui::PushStyleColor(ImGuiCol_Header, Path == State.Folder ? ImGui::GetStyle().Colors[ImGuiCol_Header] : ImVec4{0.f, 0.f, 0.f, 0.f});
					const bool bExpanded = ImGui::TreeNodeEx(Path.c_str(), Flags, " ");
					ImGui::PopStyleColor();
					const ImVec2 Minimum = ImGui::GetItemRectMin();
					const float IconX = RowStart.x + ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.x * 3.f + 6.f * Scale;
					ToolUIIcon(EToolUIMenuIcon::ContentBrowser, IconX, (Minimum.y + ImGui::GetItemRectMax().y) * 0.5f, Scale);
					ImGui::RenderTextClipped({IconX + 12.f * Scale, RowStart.y + ImGui::GetStyle().FramePadding.y}, ImGui::GetItemRectMax(), Label.data(), Label.data() + Label.size(), nullptr);
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup) && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
					{
						State.ContextFolder = Path;
					}

					if (ImGui::IsItemToggledOpen())
					{
						if (bExpanded)
						{
							State.Collapsed.erase(Path);
						}
						else
						{
							State.Collapsed.insert(Path);
						}

						bFoldersChanged = true;
					}

					if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
					{
						NavigateContentFolder(State, Path, Assets);
					}
				}
			}

			ImGui::EndChild();
			ImGui::PopStyleVar(3);
			ImGui::SameLine(0.f, 0.f);
			ImGui::Dummy({DividerWidth, std::max(1.f, Available.y - Footer)});
			ImGui::SameLine(0.f, 0.f);
		}

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.f * Scale, 4.f * Scale});
		ImGui::BeginChild("##AssetGrid", {0.f, -Footer}, ImGuiChildFlags_AlwaysUseWindowPadding);
		ImGui::PopStyleVar();
		if (ImGui::IsWindowHovered() && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0.f)
		{
			ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, ImGui::GetID("##ContentZoom"));
			State.AdjustZoom(ImGui::GetIO().MouseWheel);
		}

		if (!bShowFolders && ImGui::BeginCombo("##Folder", State.Folder.c_str()))
		{
			for (const std::string& Path : State.Folders)
			{
				ImGui::PushID(Path.c_str());
				if (ImGui::Selectable(std::format("      {}", Path).c_str(), Path == State.Folder))
				{
					NavigateContentFolder(State, Path, Assets);
				}

				const ImVec2 Minimum = ImGui::GetItemRectMin();
				ToolUIIcon(EToolUIMenuIcon::ContentBrowser, Minimum.x + 8.f * Scale, (Minimum.y + ImGui::GetItemRectMax().y) * 0.5f, Scale);
				ImGui::PopID();
			}

			ImGui::EndCombo();
		}

		const float Width = ImGui::GetContentRegionAvail().x;
		const float Gap = ImGui::GetStyle().ItemSpacing.x;
		const bool bList = State.Zoom < 0.5f;
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {Gap, bList ? 3.f * Scale : ImGui::GetStyle().ItemSpacing.y});
		const float TileSize = (48.f + State.Zoom * 16.f) * Scale;
		const int Columns = bList ? 1 : std::max(1, static_cast<int>((Width + Gap) / (TileSize + 8.f * Scale + Gap)));
		const ImVec2 Tile{std::max(1.f, (Width - static_cast<float>(Columns - 1) * Gap) / static_cast<float>(Columns)), bList ? ImGui::GetFontSize() + 4.f * Scale : TileSize};
		const std::size_t TileCount = State.FolderMatches.size() + State.Matches.size();
		const int Rows = static_cast<int>((TileCount + static_cast<std::size_t>(Columns) - 1) / static_cast<std::size_t>(Columns));
		std::string OpenFolder;
		ImGuiListClipper Clipper;
		Clipper.Begin(Rows, Tile.y + ImGui::GetStyle().ItemSpacing.y);
		if (!State.RenamingFolder.empty())
		{
			for (std::size_t Index = 0; Index < State.FolderMatches.size(); ++Index)
			{
				if (State.Folders[State.FolderMatches[Index]] == State.RenamingFolder)
				{
					Clipper.IncludeItemByIndex(static_cast<int>(Index / static_cast<std::size_t>(Columns)));
					break;
				}
			}
		}

		while (Clipper.Step())
		{
			for (int Row = Clipper.DisplayStart; Row < Clipper.DisplayEnd; ++Row)
			{
				for (int Column = 0; Column < Columns; ++Column)
				{
					const std::size_t Match = static_cast<std::size_t>(Row * Columns + Column);
					if (Match >= TileCount)
					{
						break;
					}

					if (Column > 0)
					{
						ImGui::SameLine();
					}

					if (Match < State.FolderMatches.size())
					{
						const std::string& Path = State.Folders[State.FolderMatches[Match]];
						if (DrawFolderTile(Path, State, Assets, Tile, Scale, bList))
						{
							OpenFolder = Path;
						}
					}
					else
					{
						const auto& Asset = State.Assets[State.Matches[Match - State.FolderMatches.size()]];
						if (!bList && ThumbnailAssets.size() < 64)
						{
							ThumbnailAssets.push_back(Asset.Id);
						}

						DrawAssetTile(Asset, State, Tile, Scale, bList, Assets);
					}
				}
			}
		}

		if (TileCount == 0)
		{
			ImGui::PushTextWrapPos(0.f);
			if (bBusy)
			{
				ImGui::TextDisabled("Scanning content...");
			}
			else if (State.Search[0] != '\0')
			{
				ImGui::TextUnformatted("No search results.");
				ImGui::TextDisabled("Try a different search or clear the filter.");
			}
			else
			{
				const char* const Hint = State.Folder == "Game" || State.Folder.starts_with("Game/") ? "Drop files here or right-click to import or create a folder." : "Engine content is read-only.";
				const ImVec2 HintAvailable = ImGui::GetContentRegionAvail();
				const ImVec2 TextSize = ImGui::CalcTextSize(Hint, nullptr, false, std::max(1.f, HintAvailable.x));
				const ImVec2 Cursor = ImGui::GetCursorPos();
				ImGui::SetCursorPos({Cursor.x + std::max(0.f, (HintAvailable.x - TextSize.x) * 0.5f), Cursor.y + std::max(0.f, (HintAvailable.y - TextSize.y) * 0.5f)});
				ImGui::PushTextWrapPos(Cursor.x + HintAvailable.x);
				ImGui::TextDisabled("%s", Hint);
				ImGui::PopTextWrapPos();
			}

			ImGui::PopTextWrapPos();
		}

		ImGui::PopStyleVar();
		ImGui::EndChild();
		if (!OpenFolder.empty())
		{
			NavigateContentFolder(State, OpenFolder, Assets);
		}

		if (bFoldersChanged)
		{
			State.RefreshFolders();
		}

		if (!State.FolderError.empty())
		{
			ImGui::TextColored({0.92f, 0.45f, 0.42f, 1.f}, "%s", State.FolderError.c_str());
		}
		else
		{
			ImGui::TextDisabled("%s  -  %zu folders, %zu assets%s", State.Folder.c_str(), State.FolderMatches.size(), State.Matches.size(), bBusy ? "  -  Working..." : "");
		}

		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Drop files from your file manager to import. Engine content is read-only; imports always go to Game.");
		}

		if (!State.FolderError.empty())
		{
			ImGui::SetItemTooltip("%s", State.FolderError.c_str());
		}

		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
	}

	ToolUI.EndPanel();
	if (Assets)
	{
		Assets->SetThumbnailAssets(ThumbnailAssets);
	}

	return bImport;
}
}
