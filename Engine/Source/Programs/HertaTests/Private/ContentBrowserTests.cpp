#include "ContentBrowser.h"
#include "EditorLevel.h"

#include <doctest/doctest.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <vector>

namespace Herta
{
namespace
{
std::vector<FPreviewAssetOption> MakeContentOptions()
{
	return {
	    FPreviewAssetOption{.Id = FAssetId{1, 1}, .Label = "Engine/Shapes/Cube.gltf", .Importer = "Gltf"},
	    FPreviewAssetOption{.Id = FAssetId{1, 2}, .Label = "Game/Models/CharacterCube.gltf", .Importer = "Gltf"},
	    FPreviewAssetOption{.Id = FAssetId{1, 3}, .Label = "Game/Models/Sub/Chair.blend", .Importer = "Blender"},
	    FPreviewAssetOption{.Id = FAssetId{1, 4}, .Label = "Game/Models-Extra/Stone.gltf", .Importer = "Gltf"},
	    FPreviewAssetOption{.Id = FAssetId{1, 5}, .Label = "Game/Textures/Albedo.png", .Importer = "Texture"},
	};
}

std::vector<std::string> VisibleContentFolders(const FContentBrowserState& State)
{
	std::vector<std::string> Folders;

	for (const std::size_t Index : State.VisibleFolders)
	{
		REQUIRE(Index < State.Folders.size());
		Folders.push_back(State.Folders[Index]);
	}

	return Folders;
}

std::vector<FObjectId> ContentLevelSelection(const FEditorLevel& Level)
{
	const auto Selection = Level.GetSelection();
	return {Selection.begin(), Selection.end()};
}

std::vector<std::string> MatchedContentFolders(const FContentBrowserState& State)
{
	std::vector<std::string> Folders;

	for (const std::size_t Index : State.FolderMatches)
	{
		REQUIRE(Index < State.Folders.size());
		Folders.push_back(State.Folders[Index]);
	}

	return Folders;
}
}

TEST_CASE("Content browser generates mount folders and collapses complete descendant subtrees")
{
	FContentBrowserState State;
	State.Refresh(MakeContentOptions(), 1);
	const std::array<std::string_view, 7> Expected{"Game", "Game/Models", "Game/Models/Sub", "Game/Models-Extra", "Game/Textures", "Engine", "Engine/Shapes"};
	REQUIRE(State.Folders.size() == Expected.size());
	CHECK(std::ranges::equal(State.Folders, Expected));

	for (const std::string_view Folder : Expected)
	{
		CHECK(std::ranges::count(State.Folders, Folder) == 1);
	}

	CHECK(State.VisibleFolders.size() == State.Folders.size());
	State.Collapsed.insert("Game/Models");
	State.RefreshFolders();
	const auto Visible = VisibleContentFolders(State);
	CHECK(std::ranges::find(Visible, "Game/Models") != Visible.end());
	CHECK(std::ranges::find(Visible, "Game/Models-Extra") != Visible.end());
	CHECK(std::ranges::find(Visible, "Game/Models/Sub") == Visible.end());
	State.Collapsed.insert("Game");
	State.RefreshFolders();
	CHECK(VisibleContentFolders(State) == std::vector<std::string>{"Game", "Engine", "Engine/Shapes"});
	State.Collapsed.erase("Game");
	State.Collapsed.erase("Game/Models");
	State.RefreshFolders();
	CHECK(State.VisibleFolders.size() == State.Folders.size());
}

TEST_CASE("Content browser zoom is bounded and reversible down to compact list view")
{
	FContentBrowserState State;
	CHECK(State.Zoom == 3.f);
	State.AdjustZoom(-1.f);
	CHECK(State.Zoom == 2.f);
	State.AdjustZoom(1.f);
	CHECK(State.Zoom == 3.f);
	State.AdjustZoom(-100.f);
	CHECK(State.Zoom == 0.f);
	State.AdjustZoom(0.25f);
	CHECK(State.Zoom == 0.25f);
	State.AdjustZoom(100.f);
	CHECK(State.Zoom == 9.f);
	State.AdjustZoom(-1.f);
	CHECK(State.Zoom == 8.f);
}

TEST_CASE("Content browser browsing shows immediate child folders and assets within each mount")
{
	FContentBrowserState State;
	const auto Options = MakeContentOptions();
	State.Refresh(Options, 1);
	CHECK(State.Matches.empty());
	CHECK(MatchedContentFolders(State) == std::vector<std::string>{"Game/Models", "Game/Models-Extra", "Game/Textures"});
	State.Folder = "Engine";
	State.Filter();
	CHECK(State.Matches.empty());
	CHECK(MatchedContentFolders(State) == std::vector<std::string>{"Engine/Shapes"});
	State.Folder = "Engine/Shapes";
	State.Filter();
	CHECK(State.Matches == std::vector<std::size_t>{0});
	CHECK(State.FolderMatches.empty());
	State.Folder = "Game/Models";
	State.Filter();
	CHECK(State.Matches == std::vector<std::size_t>{1});
	CHECK(MatchedContentFolders(State) == std::vector<std::string>{"Game/Models/Sub"});
	State.Folder = "Game/Models-Extra";
	State.Filter();
	CHECK(State.Matches == std::vector<std::size_t>{3});
	CHECK(State.FolderMatches.empty());
}

TEST_CASE("Content browser fuzzy search includes recursive assets and folders without crossing mounts")
{
	FContentBrowserState State;
	const auto Options = MakeContentOptions();
	State.Refresh(Options, 1);
	State.Folder = "Game/Models";
	constexpr std::string_view Query = "cHcB";
	std::ranges::copy(Query, State.Search.begin());
	State.Filter();
	CHECK(State.Matches == std::vector<std::size_t>{1});
	CHECK(State.FolderMatches.empty());
	State.Search.fill('\0');
	std::ranges::copy(std::string_view{"chair"}, State.Search.begin());
	State.Filter();
	CHECK(State.Matches == std::vector<std::size_t>{2});
	CHECK(State.FolderMatches.empty());
	State.Search.fill('\0');
	std::ranges::copy(std::string_view{"Models/sUb"}, State.Search.begin());
	State.Folder = "Game";
	State.Filter();
	CHECK(State.Matches == std::vector<std::size_t>{2});
	CHECK(MatchedContentFolders(State) == std::vector<std::string>{"Game/Models/Sub"});
	State.Search.fill('\0');
	std::ranges::copy(std::string_view{"cube"}, State.Search.begin());
	State.Filter();
	CHECK(State.Matches == std::vector<std::size_t>{1});
	State.Folder = "Engine";
	State.Filter();
	CHECK(State.Matches == std::vector<std::size_t>{0});
	State.Search.fill('\0');
	State.Search[0] = 'z';
	State.Filter();
	CHECK(State.Matches.empty());
	CHECK(State.FolderMatches.empty());
	State.Search.fill('\0');
	State.Folder = "Game";
	State.Filter();
	CHECK(State.Matches.empty());
	CHECK(MatchedContentFolders(State) == std::vector<std::string>{"Game/Models", "Game/Models-Extra", "Game/Textures"});
}

TEST_CASE("Content browser retains empty folders and selects newly created paths")
{
	FContentBrowserState State;
	const std::array<std::string, 3> Directories{"Game/Empty", "Game/Empty/Nested", "Game/Other"};
	State.Refresh({}, 1, Directories);
	CHECK(State.Folders == std::vector<std::string>{"Game", "Game/Empty", "Game/Empty/Nested", "Game/Other", "Engine"});
	State.Folder = "Game/Empty/Nested";
	State.SelectedFolder = State.Folder;
	State.Refresh({}, 2, Directories);
	CHECK(State.Folder == "Game/Empty/Nested");
	CHECK(State.SelectedFolder == State.Folder);
	CHECK(State.Matches.empty());
	CHECK(State.FolderMatches.empty());
	State.Refresh({}, 3);
	CHECK(State.Folder == "Game");
	CHECK(State.SelectedFolder.empty());
}

TEST_CASE("Content browser file manager target follows visible selection or the current folder")
{
	FContentBrowserState State;
	State.Refresh(MakeContentOptions(), 1);
	CHECK(State.GetFileManagerFolder() == "Game");
	State.SelectedFolder = "Game/Models";
	CHECK(State.GetFileManagerFolder() == "Game/Models");
	State.SelectedFolder.clear();
	std::ranges::copy(std::string_view{"chair"}, State.Search.begin());
	State.Filter();
	State.Selected = FAssetId{1, 3};
	CHECK(State.GetFileManagerFolder() == "Game/Models/Sub");
	State.Folder = "Engine";
	State.Search.fill('\0');
	State.Filter();
	CHECK(State.GetFileManagerFolder() == "Engine");
	State.Folder = "Engine/Shapes";
	State.Filter();
	State.Selected = FAssetId{1, 1};
	CHECK(State.GetFileManagerFolder() == "Engine/Shapes");
	State.Refresh({}, 2);
	CHECK(State.GetFileManagerFolder() == "Game");
}

TEST_CASE("Content browser shortcut toggles visible panels and activates hidden tabs")
{
	ImGuiContext* const PreviousContext = ImGui::GetCurrentContext();
	ImGuiContext* const Context = ImGui::CreateContext();
	ImGui::SetCurrentContext(Context);
	ImGuiIO& IO = ImGui::GetIO();
	IO.DisplaySize = {1280, 720};
	IO.DeltaTime = 1.f / 60.f;
	IO.IniFilename = nullptr;
	IO.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	IO.Fonts->AddFontDefault();
	FContentBrowserState State;
	bool bOpen = false;
	ToggleContentBrowser(bOpen, State);
	CHECK(bOpen);
	CHECK(State.bFocusRequested);
	ImGui::NewFrame();
	ImGui::Begin("      Content Browser###Content Browser");
	ImGui::TextUnformatted("Content");
	ImGui::End();
	ImGui::Render();
	ImGui::NewFrame();
	ImGui::Begin("      Content Browser###Content Browser");
	ImGuiWindow* const Window = ImGui::GetCurrentWindow();
	ImGui::TextUnformatted("Content");
	ImGui::End();
	ToggleContentBrowser(bOpen, State);
	CHECK_FALSE(bOpen);
	CHECK_FALSE(State.bFocusRequested);
	ToggleContentBrowser(bOpen, State);
	CHECK(bOpen);
	CHECK(State.bFocusRequested);
	Window->Hidden = true;
	ToggleContentBrowser(bOpen, State);
	CHECK(bOpen);
	CHECK(State.bFocusRequested);
	Window->Hidden = false;
	ImGui::Render();
	ImGui::DestroyContext(Context);
	ImGui::SetCurrentContext(PreviousContext);
}

TEST_CASE("Content browser refreshes only new generations and clears stale folder and asset selection")
{
	FContentBrowserState State;
	auto Options = MakeContentOptions();
	State.Refresh(Options, 1);
	State.Folder = "Game/Models/Sub";
	State.SelectedFolder = State.Folder;
	State.Selected = Options[2].Id;
	State.Filter();
	CHECK(State.Matches == std::vector<std::size_t>{2});
	Options[2].Label = "Game/Imported/Chair.blend";
	State.Refresh(Options, 1);
	CHECK(State.Assets[2].Label == "Game/Models/Sub/Chair.blend");
	CHECK(State.Folder == "Game/Models/Sub");
	CHECK(State.SelectedFolder == "Game/Models/Sub");
	State.Refresh(Options, 2);
	CHECK(State.Assets[2].Label == "Game/Imported/Chair.blend");
	CHECK(State.Generation == 2);
	CHECK(State.Folder == "Game");
	CHECK(State.SelectedFolder.empty());
	CHECK(State.Selected == Options[2].Id);
	State.SelectedFolder = "Game/Models";
	Options.erase(Options.begin() + 2);
	State.Refresh(Options, 3);
	CHECK_FALSE(State.Selected.IsValid());
	CHECK(State.SelectedFolder == "Game/Models");
	State.Refresh({}, 4);
	CHECK(State.Assets.empty());
	CHECK(State.Matches.empty());
	CHECK(State.FolderMatches.empty());
	CHECK(State.SelectedFolder.empty());
	CHECK(State.Folders == std::vector<std::string>{"Game", "Engine"});
}

TEST_CASE("Content browser places only known model importers with valid asset identities")
{
	FPreviewAssetOption Asset{.Id = FAssetId{1, 1}, .Label = "Game/Model.gltf", .Importer = "Gltf"};
	CHECK(IsPlaceableContentAsset(Asset));
	Asset.Importer = "Blender";
	CHECK(IsPlaceableContentAsset(Asset));

	for (const std::string_view Importer : {"Texture", "", "Unknown", "gltf"})
	{
		Asset.Importer = Importer;
		CHECK_FALSE(IsPlaceableContentAsset(Asset));
	}

	Asset.Importer = "Gltf";
	Asset.Id = {};
	CHECK_FALSE(IsPlaceableContentAsset(Asset));
}

TEST_CASE("Content browser import destinations never target the read-only Engine mount")
{
	FContentBrowserState State;
	CHECK(State.GetImportDestination().empty());
	State.Folder = "Game/Models/Props";
	CHECK(State.GetImportDestination() == "Models/Props");
	State.Folder = "Engine";
	CHECK(State.GetImportDestination().empty());
	State.Folder = "Engine/Shapes";
	CHECK(State.GetImportDestination().empty());
}

TEST_CASE("Content browser mesh placement is one undoable transaction with stable selection")
{
	FEditorLevel Level;
	const auto Before = Level.GetWorld().SnapshotEntities();
	const std::array PreviousSelection{Before[0].Id, Before[1].Id};
	Level.SetSelection(PreviousSelection, Before[0].Id);
	const FAssetId Asset{7, 9};
	const FWorldPosition Position{3.25, 4.5, -2.};
	const auto Created = Level.CreateMeshEntity(Asset, "Imported mesh", Position);
	REQUIRE(Created);
	const auto Handle = Level.GetWorld().FindEntity(*Created);
	REQUIRE(Handle);
	const auto Entity = Level.GetWorld().GetEntity(*Handle);
	REQUIRE(Entity);
	REQUIRE(Entity->Mesh);
	CHECK(Entity->Mesh->Asset == Asset);
	CHECK(Entity->Name == "Imported mesh");
	CHECK(Entity->Transform.Translation == Position);
	CHECK(Entity->BodyType == ELevelBodyType::None);
	CHECK(ContentLevelSelection(Level) == std::vector<FObjectId>{*Created});
	CHECK(Level.GetActiveObject() == *Created);
	CHECK(Level.CanUndo());
	REQUIRE(Level.Undo());
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK(ContentLevelSelection(Level) == std::vector<FObjectId>(PreviousSelection.begin(), PreviousSelection.end()));
	CHECK(Level.GetActiveObject() == Before[0].Id);
	CHECK_FALSE(Level.CanUndo());
	REQUIRE(Level.Redo());
	CHECK(Level.GetWorld().FindEntity(*Created));
	CHECK(ContentLevelSelection(Level) == std::vector<FObjectId>{*Created});
	const auto After = Level.GetWorld().SnapshotEntities();
	CHECK_FALSE(Level.CreateMeshEntity({}, "Invalid asset"));
	CHECK(Level.GetWorld().SnapshotEntities() == After);
}

TEST_CASE("Failed in-memory level loading preserves document identity world selection path and history")
{
	FEditorLevel Level;
	const auto Created = Level.CreateMeshEntity(FAssetId{2, 2}, "Keep me");
	REQUIRE(Created);
	Level.SetPath("Current.hlevel");
	const auto Before = Level.GetWorld().SnapshotEntities();
	const auto Selected = ContentLevelSelection(Level);
	const auto Active = Level.GetActiveObject();
	const auto Generation = Level.GetGeneration();
	const std::string Name{Level.GetName()};
	FLevelDocument Invalid{.Id = FObjectId{3, 3}, .Name = "Replacement", .Entities = Before};

	SUBCASE("Duplicate entity identity")
	{
		Invalid.Entities[1].Id = Invalid.Entities[0].Id;
	}

	SUBCASE("Non-finite transform")
	{
		Invalid.Entities[0].Transform.Translation.Meters.X = std::numeric_limits<double>::infinity();
	}

	SUBCASE("Invalid level document identity")
	{
		Invalid.Id = {};
	}

	SUBCASE("Invalid level document name")
	{
		Invalid.Name = "Invalid\nname";
	}

	CHECK_FALSE(Level.LoadDocument(std::move(Invalid), "Replacement.hlevel"));
	CHECK(Level.GetWorld().SnapshotEntities() == Before);
	CHECK(ContentLevelSelection(Level) == Selected);
	CHECK(Level.GetActiveObject() == Active);
	CHECK(Level.GetGeneration() == Generation);
	CHECK(Level.GetName() == Name);
	CHECK(Level.GetPath() == std::filesystem::path("Current.hlevel"));
	CHECK(Level.CanUndo());
	CHECK(Level.IsDirty());
	REQUIRE(Level.Undo());
	CHECK_FALSE(Level.GetWorld().FindEntity(*Created));
}
}
