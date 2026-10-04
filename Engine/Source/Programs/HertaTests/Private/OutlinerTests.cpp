#include "OutlinerPanel.h"
#include "PreviewScene.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdio>
#include <initializer_list>
#include <numeric>
#include <vector>

namespace Herta
{
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
