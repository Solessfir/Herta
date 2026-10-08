#include "Herta/RHI/Graphics.h"
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
	constexpr Herta::FExtent2D Minimum{.Width = 320, .Height = 200};
	constexpr Herta::FExtent2D Maximum{.Width = 3840, .Height = 2160};

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

TEST_CASE("sRGB palette colors normalize without changing transfer function")
{
	constexpr Herta::FSrgbColor Black = Herta::ConvertSrgb8ToSrgbColor(0, 0, 0, 0);
	constexpr Herta::FSrgbColor White = Herta::ConvertSrgb8ToSrgbColor(255, 255, 255, 255);
	constexpr Herta::FSrgbColor Canvas = Herta::ConvertSrgb8ToSrgbColor(18, 18, 19);

	CHECK(Black.Red == doctest::Approx(0.f));
	CHECK(Black.Alpha == doctest::Approx(0.f));
	CHECK(White.Red == doctest::Approx(1.f));
	CHECK(White.Alpha == doctest::Approx(1.f));
	CHECK(Canvas.Red == doctest::Approx(18.f / 255.f));
	CHECK(Canvas.Green == doctest::Approx(18.f / 255.f));
	CHECK(Canvas.Blue == doctest::Approx(19.f / 255.f));
	CHECK(Canvas.Alpha == doctest::Approx(1.f));
}

TEST_CASE("RHI visual authoring texture formats preserve HDR storage sizes")
{
	CHECK(Herta::GetTextureTexelBytes(Herta::ETextureFormat::Rgba8) == 4);
	CHECK(Herta::GetTextureTexelBytes(Herta::ETextureFormat::Rgba8Srgb) == 4);
	CHECK(Herta::GetTextureTexelBytes(Herta::ETextureFormat::Depth32) == 4);
	CHECK(Herta::GetTextureTexelBytes(Herta::ETextureFormat::Rgba16Float) == 8);
	CHECK(Herta::GetTextureTexelBytes(Herta::ETextureFormat::Rgba32Float) == 16);
	CHECK(Herta::GetTextureTexelBytes(static_cast<Herta::ETextureFormat>(255)) == 0);
	CHECK(sizeof(Herta::FMeshVertex) == 48);
	CHECK(offsetof(Herta::FMeshVertex, Normal) == 20);
	CHECK(offsetof(Herta::FMeshVertex, Tangent) == 32);
}

TEST_CASE("HDR10 encoding follows SMPTE ST 2084")
{
	CHECK(Herta::EncodePqLuminance(0.f) < 1e-5f);
	CHECK(Herta::EncodePqLuminance(100.f) == doctest::Approx(0.50808f).epsilon(1e-4));
	CHECK(Herta::EncodePqLuminance(1000.f) == doctest::Approx(0.75183f).epsilon(1e-4));
	CHECK(Herta::EncodePqLuminance(10000.f) == doctest::Approx(1.f));
	CHECK(Herta::EncodePqLuminance(20000.f) == doctest::Approx(1.f));

	// The Rec.709 to Rec.2020 rows each sum to one, so white stays neutral at paper white.
	const Herta::FSrgbColor White = Herta::EncodeHdr10Color({.Red = 1.f, .Green = 1.f, .Blue = 1.f, .Alpha = 0.5f}, 203.f);
	CHECK(White.Red == doctest::Approx(0.58069f).epsilon(1e-3));
	CHECK(White.Green == doctest::Approx(White.Red).epsilon(1e-4));
	CHECK(White.Blue == doctest::Approx(White.Red).epsilon(1e-4));
	CHECK(White.Alpha == 0.5f);

	const Herta::FSrgbColor Red = Herta::EncodeHdr10Color({.Red = 1.f, .Green = 0.f, .Blue = 0.f, .Alpha = 1.f}, 203.f);
	CHECK(Red.Red > Red.Green);
	CHECK(Red.Green > Red.Blue);
	CHECK(Red.Blue > 0.f);
}
