#include "Herta/GameUI/GameUI.h"
#include "TestGraphicsDevice.h"

#include <doctest/doctest.h>

#include <array>
#include <string_view>

namespace Herta
{
namespace
{
[[nodiscard]] std::unique_ptr<FGameUI> CreateTestUI(Tests::FTestGraphicsDevice& Device, std::uint64_t& Registered)
{
	auto UI = FGameUI::Create({
	    .Device = &Device,
	    .FontPath = "Engine/Content/Editor/Fonts/Roboto/Roboto-Medium.ttf",
	    .RegisterTexture = [&Registered](const FTextureHandle&) -> std::expected<std::uint64_t, FGameUIError>
	{
		return ++Registered;
	},
	    .UnregisterTexture = [](std::uint64_t) {
	},
	});

	REQUIRE_MESSAGE(UI.has_value(), (UI ? "" : UI.error().Message));
	return std::move(*UI);
}
}

TEST_CASE("Game UI lays out text at the screen scale and batches it with rectangles")
{
	Tests::FTestGraphicsDevice Device;
	std::uint64_t Registered = 0;
	const auto UI = CreateTestUI(Device, Registered);
	REQUIRE(UI->BeginFrame({1920, 1080}));
	CHECK(Registered == 1);
	const FVector2 Size = UI->MeasureText("Time 12.34", EGameUIFont::Body);
	CHECK(Size.X > 80.f);
	CHECK(Size.Y > 20.f);
	CHECK(UI->MeasureText("Paused", EGameUIFont::Title).Y > Size.Y);

	UI->DrawRect({.Offset = {24.f, 24.f}}, {200.f, 60.f}, MakeGameUIColor(0, 0, 0, 128));
	UI->DrawText("Time 12.34", {.Offset = {40.f, 34.f}}, EGameUIFont::Body, MakeGameUIColor(255, 255, 255));
	const FScreenDrawList Draws = UI->GetDrawList();
	// One rectangle and nine visible glyphs share the glyph texture.
	CHECK(Draws.Commands.size() == 1);
	CHECK(Draws.Indices.size() == 10 * 6);
	CHECK(Draws.Vertices.front().X == doctest::Approx(24.f));

	// Layout units follow the screen height, so the same placement lands proportionally at 4K.
	REQUIRE(UI->BeginFrame({3840, 2160}));
	CHECK(Registered == 2);
	UI->DrawRect({.Anchor = {1.f, 1.f}, .Pivot = {1.f, 1.f}, .Offset = {-24.f, -24.f}}, {100.f, 50.f}, MakeGameUIColor(255, 0, 0));
	CHECK(UI->GetDrawList().Vertices[2].X == doctest::Approx(3840.f - 48.f));
	CHECK(UI->GetDrawList().Vertices[2].Y == doctest::Approx(2160.f - 48.f));
}

TEST_CASE("Game UI menus share focus between keys and the pointer")
{
	Tests::FTestGraphicsDevice Device;
	std::uint64_t Registered = 0;
	const auto UI = CreateTestUI(Device, Registered);
	constexpr std::array<std::string_view, 3> Items{"Resume", "Restart", "Quit"};
	std::size_t Focus = 0;

	REQUIRE(UI->BeginFrame({1920, 1080}));
	CHECK_FALSE(UI->DrawMenu(Items, Focus, {.bDown = true}));
	CHECK(Focus == 1);
	REQUIRE(UI->BeginFrame({1920, 1080}));
	CHECK_FALSE(UI->DrawMenu(Items, Focus, {.bUp = true}));
	REQUIRE(UI->BeginFrame({1920, 1080}));
	CHECK_FALSE(UI->DrawMenu(Items, Focus, {.bUp = true}));
	CHECK(Focus == 2);
	REQUIRE(UI->BeginFrame({1920, 1080}));
	CHECK(UI->DrawMenu(Items, Focus, {.bActivate = true}) == std::optional<std::size_t>{2});

	// The first item is centered horizontally, 0.5 * (3 * 64 + 2 * 12) units above the middle.
	REQUIRE(UI->BeginFrame({1920, 1080}));
	CHECK_FALSE(UI->DrawMenu(Items, Focus, {.Pointer = FVector2{960.f, 540.f - 100.f}}));
	CHECK(Focus == 0);
	REQUIRE(UI->BeginFrame({1920, 1080}));
	CHECK(UI->DrawMenu(Items, Focus, {.Pointer = FVector2{960.f, 540.f + 100.f}, .bClicked = true}) == std::optional<std::size_t>{2});
	CHECK(Focus == 2);
}
}
