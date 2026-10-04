#include "OutlinerPanel.h"
#include "PreviewScene.h"

#include <doctest/doctest.h>
#include <imgui_internal.h>

#include <array>
#include <cstdio>
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

	ImGuiContext* Previous = ImGui::GetCurrentContext();
	ImGuiContext* Context = ImGui::CreateContext();
	FOutlinerPanelState State;
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
}
