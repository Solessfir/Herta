#include "OutlinerPanel.h"

#include <cstdio>
#include <doctest/doctest.h>
#include <initializer_list>

namespace Herta
{
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
