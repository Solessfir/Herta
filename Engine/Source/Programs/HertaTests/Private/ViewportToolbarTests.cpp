#include "ViewportIsland.h"

#include <doctest/doctest.h>

namespace Herta
{
TEST_CASE("Viewport play controls stay on the top row when the toolbar gap fits on both sides")
{
	CHECK(CanFitViewportToolbarIsland(100.f, 64.f, 96.f, 168.f, 4.f));
	CHECK_FALSE(CanFitViewportToolbarIsland(100.f, 64.f, 96.5f, 168.f, 4.f));
	CHECK_FALSE(CanFitViewportToolbarIsland(100.f, 64.f, 96.f, 167.5f, 4.f));
	CHECK_FALSE(CanFitViewportToolbarIsland(100.f, 64.f, 110.f, 168.f, 4.f));
	CHECK_FALSE(CanFitViewportToolbarIsland(100.f, 64.f, 96.f, 160.f, 4.f));
}

TEST_CASE("Viewport play control placement follows changing neighbor widths and physical scale")
{
	CHECK(CanFitViewportToolbarIsland(304.5f, 63.f, 0.f, 460.f, 4.f));
	CHECK_FALSE(CanFitViewportToolbarIsland(304.5f, 63.f, 0.f, 370.f, 4.f));
	CHECK(CanFitViewportToolbarIsland(225.f, 80.f, 220.f, 310.f, 5.f));
	CHECK_FALSE(CanFitViewportToolbarIsland(225.f, 80.f, 220.5f, 310.f, 5.f));
}
}
