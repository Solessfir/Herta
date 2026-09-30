#include "OutlinerPanel.h"

#include <cstdio>
#include <doctest/doctest.h>
#include <initializer_list>

namespace Herta
{
TEST_CASE("Outliner search matches preview labels and types without case sensitivity")
{
	FOutlinerPanelState State;
	CHECK(State.IsPreviewCubeVisible());
	for (const char* const Query : {"preview", "CUBE", "static mesh"})
	{
		std::snprintf(State.Search.InputBuf, sizeof(State.Search.InputBuf), "%s", Query);
		State.Search.Build();
		CHECK(State.IsPreviewCubeVisible());
	}
	for (const char* const Query : {"missing", "-Cube"})
	{
		std::snprintf(State.Search.InputBuf, sizeof(State.Search.InputBuf), "%s", Query);
		State.Search.Build();
		CHECK_FALSE(State.IsPreviewCubeVisible());
	}
	State.Search.Clear();
	CHECK(State.IsPreviewCubeVisible());
}
}
