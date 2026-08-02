#include "Herta/RHI/Presentation.h"

#include <doctest/doctest.h>
#include <ostream>

TEST_CASE("Swapchain image count respects surface limits")
{
	CHECK(Herta::ChooseSwapchainImageCount(2, 0, 3) == 3);
	CHECK(Herta::ChooseSwapchainImageCount(3, 0, 2) == 3);
	CHECK(Herta::ChooseSwapchainImageCount(2, 2, 3) == 2);
	CHECK(Herta::ChooseSwapchainImageCount(2, 4, 3) == 3);
}

TEST_CASE("Presentation extent is clamped to surface limits")
{
	constexpr Herta::FExtent2D Minimum{320, 200};
	constexpr Herta::FExtent2D Maximum{3840, 2160};

	CHECK(Herta::ClampPresentationExtent({1920, 1080}, Minimum, Maximum) == Herta::FExtent2D{1920, 1080});
	CHECK(Herta::ClampPresentationExtent({100, 100}, Minimum, Maximum) == Minimum);
	CHECK(Herta::ClampPresentationExtent({7680, 4320}, Minimum, Maximum) == Maximum);
}

TEST_CASE("Presentation errors have stable diagnostic names")
{
	CHECK(Herta::ToString(Herta::EPresentationErrorCode::DeviceLost) == "Device lost");
	CHECK(Herta::ToString(static_cast<Herta::EPresentationErrorCode>(255)) == "Unknown presentation error");
}

TEST_CASE("Presentation viewport handles reserve zero as invalid")
{
	constexpr Herta::FPresentationViewportHandle Invalid;
	constexpr Herta::FPresentationViewportHandle First{1};

	CHECK_FALSE(Invalid.IsValid());
	CHECK(First.IsValid());
	CHECK(First != Invalid);
}
