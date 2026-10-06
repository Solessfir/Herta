#include "OutlinerPanel.h"
#include "PreviewLevel.h"

#include <doctest/doctest.h>
#include <imgui_internal.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <numeric>
#include <ostream>
#include <vector>

namespace Herta
{
namespace
{
struct FOutlinerRenameTestContext
{
	FOutlinerRenameTestContext();
	~FOutlinerRenameTestContext();
	FOutlinerRenameTestContext(const FOutlinerRenameTestContext&) = delete;
	FOutlinerRenameTestContext& operator=(const FOutlinerRenameTestContext&) = delete;
	FOutlinerRenameTestContext(FOutlinerRenameTestContext&&) = delete;
	FOutlinerRenameTestContext& operator=(FOutlinerRenameTestContext&&) = delete;

	void Frame(int RenameRow, bool bStartRename = false);
	void PanelFrame(std::span<const FPreviewObject> Objects, FPreviewSelection& Selection, ImVec2 Size = {500.f, 360.f}, bool bDragging = false, std::span<const FObjectId> DraggedObjects = {}, std::span<const FLevelFolder> Folders = {}, std::optional<FObjectId> DraggedFolder = {});

	ImGuiContext* Previous = ImGui::GetCurrentContext();
	ImGuiContext* Context = ImGui::CreateContext();
	FOutlinerPanelState State;
	ImGuiWindow* Entries = nullptr;
	ImGuiTable* Table = nullptr;
	bool bRenameActive = false;
};

FOutlinerRenameTestContext::FOutlinerRenameTestContext()
{
	ImGui::SetCurrentContext(Context);
	ImGuiIO& IO = ImGui::GetIO();
	IO.DisplaySize = {640.f, 480.f};
	IO.DeltaTime = 1.f / 60.f;
	IO.IniFilename = nullptr;
	IO.ConfigInputTrickleEventQueue = false;
	IO.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	IO.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	IO.Fonts->AddFontDefault();
	std::snprintf(State.RenameBuffer.data(), State.RenameBuffer.size(), "%s", "Cube");
}

FOutlinerRenameTestContext::~FOutlinerRenameTestContext()
{
	ImGui::DestroyContext(Context);
	ImGui::SetCurrentContext(Previous);
}

void FOutlinerRenameTestContext::Frame(const int RenameRow, const bool bStartRename)
{
	if (bStartRename)
	{
		State.bRenaming = true;
	}

	bRenameActive = false;
	ImGui::NewFrame();
	ImGui::SetNextWindowPos({20.f, 20.f});
	ImGui::SetNextWindowSize({500.f, 360.f});
	ImGui::Begin("Outliner rename test host", nullptr, ImGuiWindowFlags_NoSavedSettings);
	if (ImGui::BeginChild("Entries"))
	{
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.f, 2.f});
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {8.f, 6.f});
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {6.f, 8.f});
		if (ImGui::BeginTable("Objects", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX))
		{
			ImGui::TableSetupColumn("Item Label", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 105.f);
			ImGui::TableHeadersRow();

			for (int Row = 0; Row < 3; ++Row)
			{
				ImGui::PushID(Row);
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				const bool bRenamingRow = Row == RenameRow && State.bRenaming;
				if (bRenamingRow)
				{
					ImGui::SetNextItemAllowOverlap();
				}

				ImGui::Selectable("##Object", Row == RenameRow, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
				if (bRenamingRow)
				{
					const ImVec2 Minimum = ImGui::GetItemRectMin();
					const ImVec2 Maximum = ImGui::GetItemRectMax();
					const float LabelX = Minimum.x + 24.f;
					const float CenterY = (Minimum.y + Maximum.y) * 0.5f;
					const float Width = ImGui::GetCurrentTable()->Columns[0].WorkMaxX - LabelX;
					DrawOutlinerRenameField(State, {LabelX, CenterY - ImGui::GetFrameHeight() * 0.5f}, Width, bStartRename);
					bRenameActive = ImGui::IsItemActive();
					CHECK_FALSE(ImGui::GetCurrentWindow()->DC.IsSetPos);
				}

				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted("Static Mesh");
				ImGui::PopID();
			}

			ImGui::EndTable();
		}

		ImGui::PopStyleVar(3);
	}

	ImGui::EndChild();
	ImGui::End();
	ImGui::Render();
	CHECK(Context->ErrorCountCurrentFrame == 0);
}

void FOutlinerRenameTestContext::PanelFrame(const std::span<const FPreviewObject> Objects, FPreviewSelection& Selection, const ImVec2 Size, const bool bDragging, const std::span<const FObjectId> DraggedObjects, const std::span<const FLevelFolder> Folders, const std::optional<FObjectId> DraggedFolder)
{
	ImGui::NewFrame();
	ImGui::SetNextWindowPos({20.f, 20.f});
	ImGui::SetNextWindowSize(Size);
	ImGui::Begin("Outliner panel test host", nullptr, ImGuiWindowFlags_NoSavedSettings);
	if (ImGui::IsWindowAppearing())
	{
		ImGui::SetWindowFocus();
	}

	ImGuiWindow* const Host = ImGui::GetCurrentWindow();
	if (!DraggedObjects.empty() && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern))
	{
		ImGui::SetDragDropPayload("Herta.OutlinerObjects", DraggedObjects.data(), DraggedObjects.size_bytes(), ImGuiCond_Once);
		ImGui::TextUnformatted("External test drag");
		ImGui::EndDragDropSource();
	}

	if (DraggedFolder && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern))
	{
		ImGui::SetDragDropPayload("Herta.OutlinerFolder", &*DraggedFolder, sizeof(FObjectId), ImGuiCond_Once);
		ImGui::TextUnformatted("External folder drag");
		ImGui::EndDragDropSource();
	}

	DrawPreviewOutlinerContents(Selection, Objects, bDragging, State, Folders);
	for (ImGuiWindow* const Window : Context->Windows)
	{
		if (Window->ParentWindow == Host && Window->ChildId == Host->GetID("##OutlinerEntries"))
		{
			Entries = Window;
			Table = Context->Tables.GetByKey(Window->GetID("##OutlinerObjects"));
		}
	}

	ImGui::End();
	ImGui::Render();
	CHECK(Context->ErrorCountCurrentFrame == 0);
}
}

TEST_CASE("Outliner rename stays inside table layout and Enter commits an edited label")
{
	for (const int Row : {0, 2})
	{
		CAPTURE(Row);
		FOutlinerRenameTestContext Test;
		Test.Frame(Row, true);
		Test.Frame(Row);
		Test.Frame(Row);
		REQUIRE(Test.bRenameActive);
		ImGuiIO& IO = ImGui::GetIO();
		IO.AddInputCharactersUTF8("Renamed Cube");
		Test.Frame(Row);
		CHECK(std::string_view(Test.State.RenameBuffer.data()) == "Renamed Cube");
		CHECK(Test.State.bRenaming);
		CHECK_FALSE(Test.State.bRenameCommitted);
		IO.AddKeyEvent(ImGuiKey_Enter, true);
		Test.Frame(Row);
		CHECK_FALSE(Test.State.bRenaming);
		CHECK(Test.State.bRenameCommitted);
	}
}

TEST_CASE("Outliner rename Escape cancels without committing at either table boundary")
{
	for (const int Row : {0, 2})
	{
		CAPTURE(Row);
		FOutlinerRenameTestContext Test;
		Test.Frame(Row, true);
		Test.Frame(Row);
		Test.Frame(Row);
		REQUIRE(Test.bRenameActive);
		ImGuiIO& IO = ImGui::GetIO();
		IO.AddInputCharactersUTF8("Canceled edit");
		Test.Frame(Row);
		IO.AddKeyEvent(ImGuiKey_Escape, true);
		Test.Frame(Row);
		CHECK_FALSE(Test.State.bRenaming);
		CHECK_FALSE(Test.State.bRenameCommitted);
	}
}

TEST_CASE("Preview object rename trims edges and rejects blank labels")
{
	std::string Label = "Preview Cube";
	CHECK_FALSE(RenamePreviewObject(Label, " \t\r\n"));
	CHECK(Label == "Preview Cube");
	CHECK(RenamePreviewObject(Label, " \tLanding Floor \r\n"));
	CHECK(Label == "Landing Floor");
	CHECK(RenamePreviewObject(Label, "Landing Floor"));
	CHECK(RenamePreviewObject(Label, "Cube##2###stable"));
	CHECK(Label == "Cube##2###stable");
}

TEST_CASE("Multi-selection translation changes only selected secondary objects")
{
	std::array<FPreviewObject, 3> Objects{{{.Label = "Active", .Translation = {1, 2, 3}}, {.Label = "Selected", .Translation = {3, 2, 3}}, {.Label = "Unselected", .Translation = {9, 8, 7}}}};
	FPreviewSelection Selection;
	Selection.Select(1, true);
	Selection.Active = 0;
	const FPreviewObject Previous = Objects[0];
	Objects[0].Translation = {2, 4, 6};
	ApplyPreviewTransformDelta(Objects, Selection, Previous);
	CHECK(Objects[0].Translation.x == doctest::Approx(2));
	CHECK(Objects[0].Translation.y == doctest::Approx(4));
	CHECK(Objects[0].Translation.z == doctest::Approx(6));
	CHECK(Objects[1].Translation.x == doctest::Approx(4));
	CHECK(Objects[1].Translation.y == doctest::Approx(4));
	CHECK(Objects[1].Translation.z == doctest::Approx(6));
	CHECK(Objects[2].Translation.x == doctest::Approx(9));
	CHECK(Objects[2].Translation.y == doctest::Approx(8));
	CHECK(Objects[2].Translation.z == doctest::Approx(7));
}

TEST_CASE("Multi-selection rotation orbits secondary objects around the active pivot")
{
	std::array<FPreviewObject, 2> Objects{{{.Label = "Active", .Translation = {1, 2, 3}}, {.Label = "Selected", .Translation = {3, 2, 3}}}};
	FPreviewSelection Selection;
	Selection.Select(1, true);
	Selection.Active = 0;
	const FPreviewObject Previous = Objects[0];
	Im3d::Mat3& Rotation = Objects[0].Rotation;
	Rotation(0, 0) = Rotation(1, 1) = 0.f;
	Rotation(0, 1) = -1.f;
	Rotation(1, 0) = 1.f;
	ApplyPreviewTransformDelta(Objects, Selection, Previous);
	CHECK(Objects[1].Translation.x == doctest::Approx(1));
	CHECK(Objects[1].Translation.y == doctest::Approx(4));
	CHECK(Objects[1].Translation.z == doctest::Approx(3));
	for (int Row = 0; Row < 3; ++Row)
	{
		for (int Column = 0; Column < 3; ++Column)
		{
			CHECK(Objects[1].Rotation(Row, Column) == doctest::Approx(Rotation(Row, Column)));
		}
	}
}

TEST_CASE("Multi-selection uniform scale adjusts object sizes and pivot distances")
{
	std::array<FPreviewObject, 2> Objects{{{.Label = "Active", .Translation = {1, 2, 3}, .Rotation = Im3d::Mat3(1.f), .Scale = {2, 2, 2}}, {.Label = "Selected", .Translation = {3, 2, 3}, .Rotation = Im3d::Mat3(1.f), .Scale = {1, 2, 3}}}};
	FPreviewSelection Selection;
	Selection.Select(1, true);
	Selection.Active = 0;
	const FPreviewObject Previous = Objects[0];
	Objects[0].Scale = {4, 4, 4};
	ApplyPreviewTransformDelta(Objects, Selection, Previous);
	CHECK(Objects[1].Translation.x == doctest::Approx(5));
	CHECK(Objects[1].Translation.y == doctest::Approx(2));
	CHECK(Objects[1].Translation.z == doctest::Approx(3));
	CHECK(Objects[1].Scale.x == doctest::Approx(2));
	CHECK(Objects[1].Scale.y == doctest::Approx(4));
	CHECK(Objects[1].Scale.z == doctest::Approx(6));
	CHECK(Objects[0].Scale.x == doctest::Approx(4));
}

TEST_CASE("Overflowing group edits restore the active transform without changing other objects")
{
	std::array<FPreviewObject, 3> Objects{{{.Label = "Active", .Translation = {-3.0e38f, 0, 0}}, {.Label = "Near", .Translation = {-3.0e38f, 1, 0}}, {.Label = "Far", .Translation = {3.0e38f, 0, 0}}}};
	FPreviewSelection Selection;
	Selection.Select(1, true);
	Selection.Select(2, true);
	Selection.Active = 0;
	const FPreviewObject Previous = Objects[0];
	Objects[0].Translation.y = 1;
	ApplyPreviewTransformDelta(Objects, Selection, Previous);
	CHECK(Objects[0].Translation.y == doctest::Approx(0));
	CHECK(Objects[1].Translation.y == doctest::Approx(1));
	CHECK(Objects[2].Translation.x == 3.0e38f);
}

TEST_CASE("Single and empty preview selections do not change secondary objects")
{
	auto Objects = CreatePreviewObjects();
	FPreviewSelection Selection;
	const FPreviewObject Previous = Objects[0];
	const FPreviewObject OriginalFloor = Objects[1];
	Objects[0].Translation = {1, 2, 3};
	ApplyPreviewTransformDelta(Objects, Selection, Previous);
	CHECK(Objects[1].Translation.y == doctest::Approx(OriginalFloor.Translation.y));
	CHECK(Objects[1].Scale.x == doctest::Approx(OriginalFloor.Scale.x));
	Selection.Select(-1);
	ApplyPreviewTransformDelta(Objects, Selection, Previous);
	CHECK(Objects[0].Translation.x == doctest::Approx(1));
	CHECK(Objects[1].Translation.y == doctest::Approx(OriginalFloor.Translation.y));
}

TEST_CASE("Preview selection replaces, toggles, and clears individual objects")
{
	FPreviewSelection Selection;
	CHECK(Selection.Contains(PreviewCubeIndex));
	Selection.Select(PreviewFloorIndex);
	CHECK_FALSE(Selection.Contains(PreviewCubeIndex));
	CHECK(Selection.Contains(PreviewFloorIndex));
	CHECK(Selection.Active == PreviewFloorIndex);
	Selection.Select(PreviewCubeIndex, true);
	CHECK(Selection.Indices.size() == 2);
	CHECK(Selection.Active == PreviewCubeIndex);
	Selection.Select(PreviewCubeIndex, true);
	CHECK_FALSE(Selection.Contains(PreviewCubeIndex));
	CHECK(Selection.Active == PreviewFloorIndex);
	Selection.Select(PreviewFloorIndex, true);
	CHECK(Selection.Indices.empty());
	CHECK(Selection.Active == -1);
	Selection.Select(PreviewCubeIndex);
	Selection.Select(-1);
	CHECK(Selection.Indices.empty());
}

TEST_CASE("Outliner ranges follow visible order and preserve their anchor")
{
	FPreviewSelection Selection;
	const std::array Visible{3, 1, 4, 0};
	Selection.Select(1);
	Selection.SelectRange(0, Visible);
	CHECK(Selection.Indices == std::vector<int>{1, 4, 0});
	CHECK(Selection.Anchor == 1);
	CHECK(Selection.Active == 0);
	Selection.SelectRange(3, Visible);
	CHECK(Selection.Indices == std::vector<int>{3, 1});
	CHECK(Selection.Anchor == 1);
	Selection.Select(7, true);
	Selection.SelectRange(4, Visible, true);
	CHECK(Selection.Contains(7));
	CHECK(Selection.Contains(4));
	Selection.Select(7, true);
	Selection.Select(7, true);
	Selection.SelectRange(4, Visible, true);
	CHECK(Selection.Contains(7));
	CHECK(Selection.Contains(4));
	Selection.SelectAll(Visible);
	CHECK(Selection.Indices == std::vector<int>{3, 1, 4, 0});
	const std::array Filtered{4, 0};
	Selection.SelectAll(Filtered);
	CHECK(Selection.Indices == std::vector<int>{4, 0});
	Selection.SelectAll({});
	CHECK(Selection.Indices.empty());
	CHECK(Selection.Active == -1);
}

TEST_CASE("Large additive Outliner ranges preserve unique selection")
{
	std::vector<int> Visible(10'000);
	std::iota(Visible.begin(), Visible.end(), 0);
	FPreviewSelection Selection;
	Selection.SelectAll(std::span<const int>(Visible).first(Visible.size() / 2));
	Selection.SelectRange(Visible.back(), Visible, true);
	CHECK(Selection.Indices.size() == Visible.size());
	CHECK(Selection.Indices == Visible);
}

TEST_CASE("Outliner search matches preview labels and types without case sensitivity")
{
	FOutlinerPanelState State;
	CHECK(State.IsObjectVisible("Preview Cube"));
	CHECK(State.IsObjectVisible("Floor"));
	for (const char* const Query : {"preview", "CUBE", "static mesh"})
	{
		std::snprintf(State.Search.InputBuf, sizeof(State.Search.InputBuf), "%s", Query);
		State.Search.Build();
		CHECK(State.IsObjectVisible("Preview Cube"));
	}

	for (const char* const Query : {"missing", "-Cube"})
	{
		std::snprintf(State.Search.InputBuf, sizeof(State.Search.InputBuf), "%s", Query);
		State.Search.Build();
		CHECK_FALSE(State.IsObjectVisible("Preview Cube"));
	}

	State.Search.Clear();
	CHECK(State.IsObjectVisible("Preview Cube"));
	for (const char* const Query : {"floor", "FLOOR", "static mesh", "-Cube"})
	{
		std::snprintf(State.Search.InputBuf, sizeof(State.Search.InputBuf), "%s", Query);
		State.Search.Build();
		CHECK(State.IsObjectVisible("Floor"));
	}

	std::snprintf(State.Search.InputBuf, sizeof(State.Search.InputBuf), "%s", "Floor,-Static");
	State.Search.Build();
	CHECK_FALSE(State.IsObjectVisible("Floor"));
}

TEST_CASE("Outliner hierarchy flattens sibling order and collapse excludes whole subtrees")
{
	const std::array<FPreviewObject, 5> Objects{{
	    {.Label = "Child", .Translation = {}, .Id = {1, 2}, .Parent = FObjectId{1, 1}},
	    {.Label = "Root", .Translation = {}, .Id = {1, 1}},
	    {.Label = "Grandchild", .Translation = {}, .Id = {1, 3}, .Parent = FObjectId{1, 2}},
	    {.Label = "Other root", .Translation = {}, .Id = {1, 4}},
	    {.Label = "Sibling", .Translation = {}, .Id = {1, 5}, .Parent = FObjectId{1, 1}},
	}};
	FOutlinerPanelState State;
	BuildOutlinerVisibleRows(State, Objects);
	CHECK(State.VisibleIndices == std::vector<int>{1, 0, 2, 4, 3});
	CHECK(State.VisibleRows[0].Depth == 0);
	CHECK(State.VisibleRows[1].Depth == 1);
	CHECK(State.VisibleRows[2].Depth == 2);
	CHECK(State.VisibleRows[3].Depth == 1);
	CHECK(State.VisibleRows[4].Depth == 0);
	CHECK(State.VisibleRows[0].bHasChildren);
	CHECK_FALSE(State.VisibleRows[2].bHasChildren);

	State.CollapsedObjects.insert(Objects[0].Id);
	BuildOutlinerVisibleRows(State, Objects);
	CHECK(State.VisibleIndices == std::vector<int>{1, 0, 4, 3});
	FPreviewSelection Selection;
	Selection.Select(0);
	Selection.SelectRange(3, State.VisibleIndices);
	CHECK(Selection.Indices == std::vector<int>{0, 4, 3});

	State.CollapsedObjects.insert(Objects[1].Id);
	BuildOutlinerVisibleRows(State, Objects);
	CHECK(State.VisibleIndices == std::vector<int>{1, 3});
	Selection.SelectAll(State.VisibleIndices);
	CHECK(Selection.Indices == State.VisibleIndices);
}

TEST_CASE("Outliner search reveals matching descendant ancestors without changing collapse")
{
	const std::array<FPreviewObject, 4> Objects{{
	    {.Label = "Assembly", .Translation = {}, .Id = {1, 1}},
	    {.Label = "Branch", .Translation = {}, .Id = {1, 2}, .Parent = FObjectId{1, 1}},
	    {.Label = "Needle", .Translation = {}, .Mesh = EngineCubeAsset, .Id = {1, 3}, .Parent = FObjectId{1, 2}},
	    {.Label = "Unrelated", .Translation = {}, .Id = {1, 4}, .Parent = FObjectId{1, 1}},
	}};
	FOutlinerPanelState State;
	State.CollapsedObjects.insert(Objects[0].Id);
	State.CollapsedObjects.insert(Objects[1].Id);
	for (const char* const Query : {"needle", "STATIC MESH"})
	{
		std::snprintf(State.Search.InputBuf, sizeof(State.Search.InputBuf), "%s", Query);
		State.Search.Build();
		BuildOutlinerVisibleRows(State, Objects);
		CHECK(State.VisibleIndices == std::vector<int>{0, 1, 2});
		CHECK(State.VisibleRows.back().Depth == 2);
	}

	CHECK(State.CollapsedObjects.size() == 2);
	State.Search.Clear();
	BuildOutlinerVisibleRows(State, Objects);
	CHECK(State.VisibleIndices == std::vector<int>{0});
	CHECK(State.IsObjectVisible("Assembly", false));
	std::snprintf(State.Search.InputBuf, sizeof(State.Search.InputBuf), "%s", "entity");
	State.Search.Build();
	CHECK(State.IsObjectVisible("Assembly", false));
	CHECK_FALSE(State.IsObjectVisible("Needle", true));
}

TEST_CASE("Outliner hierarchy refreshes metadata and bounds deep or invalid preview trees")
{
	std::vector<FPreviewObject> Objects(10'000);
	for (std::size_t Index = 0; Index < Objects.size(); ++Index)
	{
		Objects[Index].Id = {1, Index + 1};
		Objects[Index].Label = Index + 1 == Objects.size() ? "Needle" : "Node";
		if (Index > 0)
		{
			Objects[Index].Parent = Objects[Index - 1].Id;
		}
	}

	FOutlinerPanelState State;
	BuildOutlinerVisibleRows(State, Objects);
	REQUIRE(State.VisibleRows.size() == Objects.size());
	CHECK(State.VisibleRows.back().Depth == 9'999);
	State.CollapsedObjects.insert(Objects[0].Id);
	BuildOutlinerVisibleRows(State, Objects);
	CHECK(State.VisibleRows.size() == 1);
	std::snprintf(State.Search.InputBuf, sizeof(State.Search.InputBuf), "%s", "Needle");
	State.Search.Build();
	BuildOutlinerVisibleRows(State, Objects);
	CHECK(State.VisibleRows.size() == Objects.size());
	State.Search.Clear();
	Objects.back().Parent.reset();
	BuildOutlinerVisibleRows(State, Objects);
	CHECK(State.VisibleIndices == std::vector<int>{0, 9'999});

	Objects.resize(3);
	Objects[0].Parent = Objects[2].Id;
	State.CollapsedObjects.clear();
	BuildOutlinerVisibleRows(State, Objects);
	CHECK(State.VisibleRows.size() == Objects.size());
	Objects[0].Parent = FObjectId{9, 9};
	Objects[1].Parent = Objects[1].Id;
	BuildOutlinerVisibleRows(State, Objects);
	CHECK(State.VisibleIndices == std::vector<int>{0, 1, 2});
	CHECK(State.VisibleRows[0].Depth == 0);
	CHECK(State.VisibleRows[1].Depth == 0);
	Objects.clear();
	BuildOutlinerVisibleRows(State, Objects);
	CHECK(State.VisibleRows.empty());
	CHECK(State.CollapsedObjects.empty());
}

TEST_CASE("Outliner reparent requests carry selected stable IDs or just an unselected drag source")
{
	const std::array<FPreviewObject, 3> Objects{{
	    {.Label = "First", .Translation = {}, .Id = {1, 1}},
	    {.Label = "Second", .Translation = {}, .Id = {1, 2}, .Parent = FObjectId{1, 1}},
	    {.Label = "Third", .Translation = {}, .Id = {1, 3}},
	}};
	FPreviewSelection Selection;
	Selection.Indices = {1, 0, 1, -1, 99};
	const auto Multi = MakeOutlinerReparentRequest(Objects, Selection, 1, Objects[2].Id);
	REQUIRE(Multi);
	CHECK(Multi->Objects == std::vector<FObjectId>{Objects[1].Id, Objects[0].Id});
	CHECK(Multi->Parent == Objects[2].Id);
	const auto Single = MakeOutlinerReparentRequest(Objects, Selection, 2, Objects[1].Id);
	REQUIRE(Single);
	CHECK(Single->Objects == std::vector<FObjectId>{Objects[2].Id});
	const auto Root = MakeOutlinerReparentRequest(Objects, Selection, 1, std::nullopt);
	REQUIRE(Root);
	CHECK_FALSE(Root->Parent);
	CHECK(Root->Objects == Multi->Objects);
	CHECK_FALSE(MakeOutlinerReparentRequest(Objects, Selection, 1, Objects[0].Id));
	CHECK_FALSE(MakeOutlinerReparentRequest(Objects, Selection, -1, std::nullopt));
	CHECK_FALSE(MakeOutlinerReparentRequest(Objects, Selection, 99, std::nullopt));
}

TEST_CASE("Outliner panel clips large hierarchy rows and focuses an offscreen indented rename")
{
	std::vector<FPreviewObject> Objects(10'000);
	for (std::size_t Index = 0; Index < Objects.size(); ++Index)
	{
		Objects[Index].Id = {1, Index + 1};
		Objects[Index].Label = "Entity " + std::to_string(Index);
		if (Index + 1 == Objects.size())
		{
			Objects[Index].Parent = Objects[Index - 1].Id;
		}
	}

	for (const float Width : {500.f, 220.f})
	{
		CAPTURE(Width);
		FOutlinerRenameTestContext Test;
		FPreviewSelection Selection;
		Test.PanelFrame(Objects, Selection, {Width, 360.f});
		Test.PanelFrame(Objects, Selection, {Width, 360.f});
		REQUIRE(Test.Entries != nullptr);
		CHECK(ImGui::GetDrawData()->TotalVtxCount < 10'000);
		Selection.Select(9'999);
		Test.State.bRenameRequested = true;
		for (int Frame = 0; Frame < 4; ++Frame)
		{
			Test.PanelFrame(Objects, Selection, {Width, 360.f});
		}

		CHECK(Test.State.bRenaming);
		CHECK(Test.State.RenameObject == Objects.back().Id);
		CHECK(Test.Entries->Scroll.y > 0.f);
		CHECK(Test.Context->ActiveId != 0);
		CHECK(Test.Context->ActiveId == Test.Context->InputTextState.ID);
		ImGui::GetIO().AddInputCharactersUTF8("Renamed descendant");
		Test.PanelFrame(Objects, Selection, {Width, 360.f});
		CHECK(std::string_view(Test.State.RenameBuffer.data()) == "Renamed descendant");
		Test.PanelFrame(Objects, Selection, {Width, 360.f}, true);
		CHECK_FALSE(Test.State.bRenaming);
		CHECK_FALSE(Test.State.bRenameCommitted);
		CHECK_FALSE(Test.State.ReparentRequest);
	}
}

TEST_CASE("Outliner panel drop targets distinguish object rows from empty root space and disable during dragging")
{
	const std::array<FPreviewObject, 3> Objects{{
	    {.Label = "Root", .Translation = {}, .Id = {1, 1}},
	    {.Label = "Child", .Translation = {}, .Id = {1, 2}, .Parent = FObjectId{1, 1}},
	    {.Label = "Other root", .Translation = {}, .Id = {1, 3}},
	}};
	for (const bool bDropOnRow : {false, true})
	{
		for (const bool bDisabled : {false, true})
		{
			CAPTURE(bDropOnRow);
			CAPTURE(bDisabled);
			FOutlinerRenameTestContext Test;
			FPreviewSelection Selection;
			Test.PanelFrame(Objects, Selection);
			Test.PanelFrame(Objects, Selection);
			REQUIRE(Test.Entries != nullptr);
			REQUIRE(Test.Table != nullptr);
			const float MouseY = bDropOnRow ? (Test.Table->RowPosY1 + Test.Table->RowPosY2) * 0.5f : Test.Entries->InnerRect.Max.y - 20.f;
			ImGuiIO& IO = ImGui::GetIO();
			IO.AddMousePosEvent(Test.Entries->InnerRect.Min.x + 80.f, MouseY);
			IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			const std::array Dragged{Objects[1].Id};
			Test.PanelFrame(Objects, Selection, {500.f, 360.f}, bDisabled, Dragged);
			Test.PanelFrame(Objects, Selection, {500.f, 360.f}, bDisabled, Dragged);
			CHECK_FALSE(Test.State.ReparentRequest);
			IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			Test.PanelFrame(Objects, Selection, {500.f, 360.f}, bDisabled);
			if (bDisabled)
			{
				CHECK_FALSE(Test.State.ReparentRequest);
				CHECK_FALSE(Test.State.FolderRequest);
			}
			else if (bDropOnRow)
			{
				REQUIRE(Test.State.ReparentRequest);
				CHECK(Test.State.ReparentRequest->Objects == std::vector<FObjectId>{Objects[1].Id});
				CHECK(Test.State.ReparentRequest->Parent == Objects[2].Id);
				CHECK_FALSE(Test.State.FolderRequest);
			}
			else
			{
				REQUIRE(Test.State.FolderRequest);
				CHECK(Test.State.FolderRequest->Action == EOutlinerFolderAction::MoveEntities);
				CHECK(Test.State.FolderRequest->Objects == std::vector<FObjectId>{Objects[1].Id});
				CHECK_FALSE(Test.State.FolderRequest->Parent);
				CHECK_FALSE(Test.State.ReparentRequest);
			}
		}
	}
}

TEST_CASE("Outliner panel drag snapshots multi-selection and does not replace selection on release")
{
	const std::array<FPreviewObject, 3> Objects{{
	    {.Label = "Root", .Translation = {}, .Id = {1, 1}},
	    {.Label = "Child", .Translation = {}, .Id = {1, 2}, .Parent = FObjectId{1, 1}},
	    {.Label = "Other root", .Translation = {}, .Id = {1, 3}},
	}};
	for (const int SourceIndex : {0, 2})
	{
		CAPTURE(SourceIndex);
		FOutlinerRenameTestContext Test;
		FPreviewSelection Selection;
		Selection.Select(0);
		Selection.Select(1, true);
		const FPreviewSelection Original = Selection;
		Test.PanelFrame(Objects, Selection);
		Test.PanelFrame(Objects, Selection);
		REQUIRE(Test.Table != nullptr);
		REQUIRE(Test.Entries != nullptr);
		const float RowHeight = Test.Table->RowPosY2 - Test.Table->RowPosY1;
		const float MouseY = (Test.Table->RowPosY1 + Test.Table->RowPosY2) * 0.5f - static_cast<float>(2 - SourceIndex) * RowHeight;
		const float MouseX = Test.Entries->InnerRect.Min.x + 80.f;
		ImGuiIO& IO = ImGui::GetIO();
		IO.AddMousePosEvent(MouseX, MouseY);
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		Test.PanelFrame(Objects, Selection);
		IO.AddMousePosEvent(MouseX + 20.f, MouseY);
		Test.PanelFrame(Objects, Selection);
		Test.PanelFrame(Objects, Selection);
		const ImGuiPayload* const Payload = ImGui::GetDragDropPayload();
		REQUIRE(Payload != nullptr);
		REQUIRE(Payload->IsDataType("Herta.OutlinerObjects"));
		std::vector<FObjectId> Dragged(static_cast<std::size_t>(Payload->DataSize) / sizeof(FObjectId));
		std::memcpy(Dragged.data(), Payload->Data, static_cast<std::size_t>(Payload->DataSize));
		CHECK(Dragged == (SourceIndex == 0 ? std::vector<FObjectId>{Objects[0].Id, Objects[1].Id} : std::vector<FObjectId>{Objects[2].Id}));
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		Test.PanelFrame(Objects, Selection);
		CHECK(Selection == Original);
		CHECK_FALSE(Test.State.ReparentRequest);
	}
}

TEST_CASE("Outliner folders organize hierarchy roots without replacing transform parenting")
{
	const std::array<FPreviewObject, 4> Objects{{
	    {.Label = "Assembly", .Translation = {}, .Id = {1, 1}},
	    {.Label = "Child", .Translation = {}, .Id = {1, 2}, .Parent = FObjectId{1, 1}},
	    {.Label = "Other", .Translation = {}, .Id = {1, 3}},
	    {.Label = "Loose", .Translation = {}, .Id = {1, 4}},
	}};
	std::array<FLevelFolder, 3> Folders{{
	    {.Id = {2, 2}, .Name = "Nested", .Parent = {2, 1}, .Entities = {Objects[0].Id}},
	    {.Id = {2, 1}, .Name = "Group"},
	    {.Id = {2, 3}, .Name = "Other group", .Entities = {Objects[1].Id, Objects[2].Id}},
	}};
	FOutlinerPanelState State;
	BuildOutlinerVisibleRows(State, Objects, Folders);
	REQUIRE(State.VisibleRows.size() == 7);
	CHECK(State.VisibleIndices == std::vector<int>{0, 1, 2, 3});
	CHECK(State.VisibleRows[0].FolderIndex == 1);
	CHECK(State.VisibleRows[1].FolderIndex == 0);
	CHECK(State.VisibleRows[2].ObjectIndex == 0);
	CHECK(State.VisibleRows[2].Depth == 2);
	CHECK(State.VisibleRows[3].ObjectIndex == 1);
	CHECK(State.VisibleRows[3].Depth == 3);
	CHECK(State.VisibleRows[4].FolderIndex == 2);
	CHECK(State.VisibleRows[5].ObjectIndex == 2);
	CHECK(State.VisibleRows[6].ObjectIndex == 3);
	State.CollapsedFolders.insert(Folders[1].Id);
	BuildOutlinerVisibleRows(State, Objects, Folders);
	CHECK(State.VisibleIndices == std::vector<int>{2, 3});
	CHECK(State.VisibleRows.size() == 4);
	Folders[0].Parent = {};
	BuildOutlinerVisibleRows(State, Objects, Folders);
	CHECK(State.VisibleIndices == std::vector<int>{0, 1, 2, 3});
	CHECK(State.VisibleRows[1].FolderIndex == 0);
	CHECK(State.VisibleRows[2].Depth == 1);
	State.SelectedFolder = Folders[1].Id;
	BuildOutlinerVisibleRows(State, Objects);
	CHECK(State.CollapsedFolders.empty());
	CHECK_FALSE(State.SelectedFolder.IsValid());
}

TEST_CASE("Outliner reveal expands collapsed parent entities and folders of a viewport pick")
{
	const std::array<FPreviewObject, 3> Objects{{
	    {.Label = "Assembly", .Translation = {}, .Id = {1, 1}},
	    {.Label = "Child", .Translation = {}, .Id = {1, 2}, .Parent = FObjectId{1, 1}},
	    {.Label = "Other", .Translation = {}, .Id = {1, 3}},
	}};
	const std::array<FLevelFolder, 2> Folders{{
	    {.Id = {2, 1}, .Name = "Group"},
	    {.Id = {2, 2}, .Name = "Nested", .Parent = {2, 1}, .Entities = {Objects[0].Id}},
	}};
	FOutlinerPanelState State;
	State.CollapsedFolders = {Folders[0].Id, Folders[1].Id};
	State.CollapsedObjects = {Objects[0].Id};
	BuildOutlinerVisibleRows(State, Objects, Folders);
	CHECK(State.VisibleIndices == std::vector<int>{2});

	CHECK(ExpandOutlinerAncestors(State, Objects, Folders, 1));
	BuildOutlinerVisibleRows(State, Objects, Folders);
	CHECK(State.VisibleIndices == std::vector<int>{0, 1, 2});
	CHECK(State.CollapsedFolders.empty());
	CHECK(State.CollapsedObjects.empty());

	CHECK_FALSE(ExpandOutlinerAncestors(State, Objects, Folders, 1));
	CHECK_FALSE(ExpandOutlinerAncestors(State, Objects, Folders, -1));
	CHECK_FALSE(ExpandOutlinerAncestors(State, Objects, Folders, 3));
}

TEST_CASE("Outliner folder search preserves matched ancestors and folder collapse")
{
	const std::array<FPreviewObject, 2> Objects{{
	    {.Label = "Assembly", .Translation = {}, .Id = {1, 1}},
	    {.Label = "Needle", .Translation = {}, .Id = {1, 2}, .Parent = FObjectId{1, 1}},
	}};
	const std::array<FLevelFolder, 3> Folders{{
	    {.Id = {2, 1}, .Name = "Group"},
	    {.Id = {2, 2}, .Name = "Nested", .Parent = {2, 1}, .Entities = {Objects[0].Id}},
	    {.Id = {2, 3}, .Name = "Empty"},
	}};
	FOutlinerPanelState State;
	State.CollapsedFolders.insert(Folders[0].Id);
	std::snprintf(State.Search.InputBuf, sizeof(State.Search.InputBuf), "%s", "needle");
	State.Search.Build();
	BuildOutlinerVisibleRows(State, Objects, Folders);
	REQUIRE(State.VisibleRows.size() == 4);
	CHECK(State.VisibleRows[0].FolderIndex == 0);
	CHECK(State.VisibleRows[1].FolderIndex == 1);
	CHECK(State.VisibleRows.back().Depth == 3);
	CHECK(State.VisibleIndices == std::vector<int>{0, 1});
	CHECK(State.CollapsedFolders.contains(Folders[0].Id));
	std::snprintf(State.Search.InputBuf, sizeof(State.Search.InputBuf), "%s", "folder");
	State.Search.Build();
	BuildOutlinerVisibleRows(State, Objects, Folders);
	CHECK(State.VisibleRows.size() == 3);
	CHECK(State.VisibleIndices.empty());
	State.Search.Clear();
	BuildOutlinerVisibleRows(State, Objects, Folders);
	CHECK(State.VisibleRows.size() == 2);
	CHECK(State.VisibleIndices.empty());
}

TEST_CASE("Outliner folder rename focuses the inline row at narrow widths")
{
	const std::array<FPreviewObject, 1> Objects{{{.Label = "Entity", .Translation = {}, .Id = {1, 1}}}};
	const std::array<FLevelFolder, 1> Folders{{{.Id = {2, 1}, .Name = "New Folder", .Entities = {Objects[0].Id}}}};
	for (const float Width : {500.f, 220.f})
	{
		CAPTURE(Width);
		FOutlinerRenameTestContext Test;
		FPreviewSelection Selection;
		Test.PanelFrame(Objects, Selection, {Width, 360.f}, false, {}, Folders);
		Test.PanelFrame(Objects, Selection, {Width, 360.f}, false, {}, Folders);
		REQUIRE(Test.Table != nullptr);
		const float RowHeight = Test.Table->RowPosY2 - Test.Table->RowPosY1;
		const float FolderY = (Test.Table->RowPosY1 + Test.Table->RowPosY2) * 0.5f - RowHeight;
		ImGuiIO& IO = ImGui::GetIO();
		IO.AddMousePosEvent(Test.Entries->InnerRect.Min.x + 70.f, FolderY);
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		Test.PanelFrame(Objects, Selection, {Width, 360.f}, false, {}, Folders);
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		Test.PanelFrame(Objects, Selection, {Width, 360.f}, false, {}, Folders);
		CHECK(Selection.Indices.empty());
		CHECK(Test.State.SelectedFolder == Folders[0].Id);
		IO.AddKeyEvent(ImGuiKey_F2, true);
		Test.PanelFrame(Objects, Selection, {Width, 360.f}, false, {}, Folders);
		IO.AddKeyEvent(ImGuiKey_F2, false);
		Test.PanelFrame(Objects, Selection, {Width, 360.f}, false, {}, Folders);
		CHECK(Test.State.bRenaming);
		CHECK(Test.State.RenameFolder == Folders[0].Id);
		CHECK_FALSE(Test.State.RenameObject.IsValid());
		CHECK(Test.Context->ActiveId == Test.Context->InputTextState.ID);
		IO.AddInputCharactersUTF8("Organized");
		Test.PanelFrame(Objects, Selection, {Width, 360.f}, false, {}, Folders);
		IO.AddKeyEvent(ImGuiKey_Enter, true);
		Test.PanelFrame(Objects, Selection, {Width, 360.f}, false, {}, Folders);
		CHECK(Test.State.bRenameCommitted);
		CHECK(Test.State.RenameFolder == Folders[0].Id);
		CHECK(std::string_view(Test.State.RenameBuffer.data()) == "Organized");
		CHECK_FALSE(Test.State.FolderRequest);
		CHECK_FALSE(Test.State.ReparentRequest);
	}
}

TEST_CASE("Outliner folder shortcuts request creation and non-destructive deletion")
{
	const std::array<FLevelFolder, 1> Folders{{{.Id = {2, 1}, .Name = "Group"}}};
	FOutlinerRenameTestContext Test;
	FPreviewSelection Selection;
	Selection.Select(-1);
	Test.State.SelectedFolder = Folders[0].Id;
	Test.PanelFrame({}, Selection, {500.f, 360.f}, false, {}, Folders);
	Test.PanelFrame({}, Selection, {500.f, 360.f}, false, {}, Folders);
	ImGuiIO& IO = ImGui::GetIO();
	IO.AddKeyEvent(ImGuiMod_Ctrl, true);
	IO.AddKeyEvent(ImGuiMod_Shift, true);
	IO.AddKeyEvent(ImGuiKey_N, true);
	Test.PanelFrame({}, Selection, {500.f, 360.f}, false, {}, Folders);
	REQUIRE(Test.State.FolderRequest);
	CHECK(Test.State.FolderRequest->Action == EOutlinerFolderAction::Create);
	CHECK(Test.State.FolderRequest->Parent == Folders[0].Id);
	Test.State.FolderRequest.reset();
	IO.AddKeyEvent(ImGuiMod_Ctrl, false);
	IO.AddKeyEvent(ImGuiMod_Shift, false);
	IO.AddKeyEvent(ImGuiKey_N, false);
	IO.AddKeyEvent(ImGuiKey_Delete, true);
	Test.PanelFrame({}, Selection, {500.f, 360.f}, false, {}, Folders);
	REQUIRE(Test.State.FolderRequest);
	CHECK(Test.State.FolderRequest->Action == EOutlinerFolderAction::Delete);
	CHECK(Test.State.FolderRequest->Folder == Folders[0].Id);
	CHECK(Test.State.FolderRequest->Objects.empty());
	CHECK_FALSE(Test.State.ReparentRequest);
}

TEST_CASE("Outliner folder drops never issue transform parenting requests")
{
	const std::array<FPreviewObject, 1> Objects{{{.Label = "Entity", .Translation = {}, .Id = {1, 1}}}};
	const std::array<FLevelFolder, 2> Folders{{
	    {.Id = {2, 1}, .Name = "First"},
	    {.Id = {2, 2}, .Name = "Second"},
	}};
	for (const bool bFolderSource : {false, true})
	{
		for (const int TargetRow : {0, 2, -1})
		{
			CAPTURE(bFolderSource);
			CAPTURE(TargetRow);
			FOutlinerRenameTestContext Test;
			FPreviewSelection Selection;
			Test.PanelFrame(Objects, Selection, {500.f, 360.f}, false, {}, Folders);
			Test.PanelFrame(Objects, Selection, {500.f, 360.f}, false, {}, Folders);
			REQUIRE(Test.Table != nullptr);
			const float RowHeight = Test.Table->RowPosY2 - Test.Table->RowPosY1;
			const float MouseY = TargetRow < 0 ? Test.Entries->InnerRect.Max.y - 20.f : (Test.Table->RowPosY1 + Test.Table->RowPosY2) * 0.5f - static_cast<float>(2 - TargetRow) * RowHeight;
			ImGuiIO& IO = ImGui::GetIO();
			IO.AddMousePosEvent(Test.Entries->InnerRect.Min.x + 70.f, MouseY);
			IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			const std::array Dragged{Objects[0].Id};
			for (int Frame = 0; Frame < 2; ++Frame)
			{
				Test.PanelFrame(Objects, Selection, {500.f, 360.f}, false, bFolderSource ? std::span<const FObjectId>{} : Dragged, Folders, bFolderSource ? std::optional{Folders[1].Id} : std::nullopt);
			}

			IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			Test.PanelFrame(Objects, Selection, {500.f, 360.f}, false, {}, Folders);
			CHECK_FALSE(Test.State.ReparentRequest);
			if (TargetRow == 2)
			{
				CHECK_FALSE(Test.State.FolderRequest);
			}
			else
			{
				REQUIRE(Test.State.FolderRequest);
				CHECK(Test.State.FolderRequest->Action == (bFolderSource ? EOutlinerFolderAction::MoveFolder : EOutlinerFolderAction::MoveEntities));
				CHECK(Test.State.FolderRequest->Parent == (TargetRow < 0 ? std::nullopt : std::optional{Folders[0].Id}));
				if (bFolderSource)
				{
					CHECK(Test.State.FolderRequest->Folder == Folders[1].Id);
					CHECK(Test.State.FolderRequest->Objects.empty());
				}
				else
				{
					CHECK(Test.State.FolderRequest->Objects == std::vector<FObjectId>{Objects[0].Id});
				}
			}
		}
	}
}
}
