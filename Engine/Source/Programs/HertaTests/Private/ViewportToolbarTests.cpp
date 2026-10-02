#include "ViewportIsland.h"

#include <doctest/doctest.h>

namespace Herta
{
TEST_CASE("Viewport play controls stay on the top row when the toolbar gap fits on both sides")
{
	CHECK(CanFitViewportToolbarIsland(100.0f, 64.0f, 96.0f, 168.0f, 4.0f));
	CHECK_FALSE(CanFitViewportToolbarIsland(100.0f, 64.0f, 96.5f, 168.0f, 4.0f));
	CHECK_FALSE(CanFitViewportToolbarIsland(100.0f, 64.0f, 96.0f, 167.5f, 4.0f));
	CHECK_FALSE(CanFitViewportToolbarIsland(100.0f, 64.0f, 110.0f, 168.0f, 4.0f));
	CHECK_FALSE(CanFitViewportToolbarIsland(100.0f, 64.0f, 96.0f, 160.0f, 4.0f));
}

TEST_CASE("Viewport play control placement follows changing neighbor widths and physical scale")
{
	CHECK(CanFitViewportToolbarIsland(304.5f, 63.0f, 0.0f, 460.0f, 4.0f));
	CHECK_FALSE(CanFitViewportToolbarIsland(304.5f, 63.0f, 0.0f, 370.0f, 4.0f));
	CHECK(CanFitViewportToolbarIsland(225.0f, 80.0f, 220.0f, 310.0f, 5.0f));
	CHECK_FALSE(CanFitViewportToolbarIsland(225.0f, 80.0f, 220.5f, 310.0f, 5.0f));
}
}
