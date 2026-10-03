#include "ViewportBoxSelection.h"

#include <doctest/doctest.h>

#include <array>
#include <vector>

namespace Herta
{
namespace
{
FPreviewSelection MakeBoxInitialSelection()
{
	return {.Indices = {1, 3}, .Active = 3, .Anchor = 1};
}
}

TEST_CASE("Viewport box selection latches its drag threshold and ignores inactive updates")
{
	FViewportBoxSelectionState State;
	State.Update({8.f, 9.f}, true);
	CHECK_FALSE(State.bActive);
	CHECK_FALSE(State.bDragging);
	CHECK(State.Current == FVector2{});
	State.Begin({1.f, 2.f}, MakeBoxInitialSelection(), false, false);
	CHECK(State.bActive);
	CHECK_FALSE(State.bDragging);
	CHECK((State.Start == FVector2{1.f, 2.f}));
	CHECK(State.Current == State.Start);
	State.Update({2.f, 3.f}, false);
	CHECK_FALSE(State.bDragging);
	CHECK((State.Current == FVector2{2.f, 3.f}));
	State.Update({9.f, 12.f}, true);
	CHECK(State.bDragging);
	State.Update(State.Start, false);
	CHECK(State.bDragging);
	State.Cancel();
	CHECK_FALSE(State.bActive);
	CHECK_FALSE(State.bDragging);
	State.Update({99.f, 99.f}, true);
	CHECK(State.Current == State.Start);
	State.Begin({4.f, 5.f}, MakeBoxInitialSelection(), true, false);
	CHECK(State.bActive);
	CHECK_FALSE(State.bDragging);
	CHECK(State.Current == State.Start);
}

TEST_CASE("Viewport box selection replaces with unique hits in deterministic order")
{
	FViewportBoxSelectionState State;
	State.Begin({}, MakeBoxInitialSelection(), false, false);
	constexpr std::array Hits{4, 2, 4, 2, 6, -1};
	const FPreviewSelection Selection = State.MakeSelection(Hits);
	CHECK(Selection.Indices == std::vector<int>{4, 2, 6});
	CHECK(Selection.Active == 6);
	CHECK(Selection.Anchor == 4);
	CHECK(State.InitialSelection == MakeBoxInitialSelection());
	const FPreviewSelection Empty = State.MakeSelection({});
	CHECK(Empty.Indices.empty());
	CHECK(Empty.Active == -1);
	CHECK(Empty.Anchor == -1);
}

TEST_CASE("Viewport Shift box selection adds unique hits while retaining its initial selection")
{
	FViewportBoxSelectionState State;
	State.Begin({}, MakeBoxInitialSelection(), true, false);
	constexpr std::array Hits{3, 2, 2, 1, 4};
	const FPreviewSelection Selection = State.MakeSelection(Hits);
	CHECK(Selection.Indices == std::vector<int>{1, 3, 2, 4});
	CHECK(Selection.Active == 3);
	CHECK(Selection.Anchor == 1);
	CHECK(State.MakeSelection({}) == MakeBoxInitialSelection());
	CHECK(State.MakeSelection(Hits) == Selection);
	CHECK(State.InitialSelection == MakeBoxInitialSelection());
}

TEST_CASE("Viewport Ctrl box selection toggles each hit once against the immutable initial snapshot")
{
	for (const bool bAdd : {false, true})
	{
		FViewportBoxSelectionState State;
		FPreviewSelection Initial = MakeBoxInitialSelection();
		State.Begin({}, Initial, bAdd, true);
		Initial.Select(-1);
		constexpr std::array Hits{3, 2, 3, 4, 2};
		const FPreviewSelection Selection = State.MakeSelection(Hits);
		CHECK(Selection.Indices == std::vector<int>{1, 2, 4});
		CHECK(Selection.Active == 4);
		CHECK(Selection.Anchor == 1);
		CHECK(State.MakeSelection(Hits) == Selection);
		CHECK(State.InitialSelection == MakeBoxInitialSelection());
		CHECK(State.MakeSelection({}) == MakeBoxInitialSelection());
		constexpr std::array AllInitial{1, 3, 1};
		const FPreviewSelection Empty = State.MakeSelection(AllInitial);
		CHECK(Empty.Indices.empty());
		CHECK(Empty.Active == -1);
		CHECK(Empty.Anchor == -1);
	}
}

TEST_CASE("Viewport box selection keeps active and anchor valid after modifier composition")
{
	FViewportBoxSelectionState State;
	State.Begin({}, MakeBoxInitialSelection(), false, true);
	constexpr std::array Hits{1, 4};
	const FPreviewSelection Selection = State.MakeSelection(Hits);
	CHECK(Selection.Indices == std::vector<int>{3, 4});
	CHECK(Selection.Active == 3);
	CHECK(Selection.Anchor == 3);
	FPreviewSelection Empty;
	Empty.Select(-1);
	State.Begin({}, Empty, true, false);
	constexpr std::array Added{7, 5, 7};
	const FPreviewSelection AddedSelection = State.MakeSelection(Added);
	CHECK(AddedSelection.Active == 5);
	CHECK(AddedSelection.Anchor == 7);
	CHECK(AddedSelection.Contains(AddedSelection.Active));
	CHECK(AddedSelection.Contains(AddedSelection.Anchor));
}
}
