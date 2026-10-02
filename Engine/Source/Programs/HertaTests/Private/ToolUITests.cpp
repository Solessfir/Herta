#include "Herta/ToolUI/ToolUI.h"

#include <doctest/doctest.h>
#include <fstream>
#include <iterator>
#include <string>

namespace Herta
{
TEST_CASE("ToolUI scene canvas excludes chrome but not overlay panels")
{
	CHECK(ResolveToolUIWorkspaceCanvas({0, 0, 1920, 1080}, 36, 32) == FToolUICanvasBounds{0, 36, 1920, 1012});
	CHECK(ResolveToolUIWorkspaceCanvas({-1920, 80, 1920, 1080}, 54, 48) == FToolUICanvasBounds{-1920, 134, 1920, 978});
	CHECK(ResolveToolUIWorkspaceCanvas({0, 0, 10, 20}, 36, 32) == FToolUICanvasBounds{0, 20, 10, 0});
	CHECK(ResolveToolUIWorkspaceCanvas({0, 0, 0, 0}, 36, 32) == FToolUICanvasBounds{});
}

TEST_CASE("ToolUI theme defaults preserve the editor visual contract")
{
	constexpr FEditorAppearance Appearance;
	constexpr FToolUIThemeMetrics Metrics;
	constexpr FToolUIGradient FocusedGradient = ResolveToolUIGradient(Appearance, true);
	constexpr FToolUIGradient UnfocusedGradient = ResolveToolUIGradient(Appearance, false);

	CHECK(Appearance.Accent == FToolUIColor{184, 184, 184, 255});
	CHECK(Appearance.PanelTransparency == EPanelTransparency::AllPanels);
	CHECK(Appearance.PanelOpacity == doctest::Approx(0.90f));
	CHECK(Appearance.BlurRadius == doctest::Approx(24.0f));
	CHECK_FALSE(Appearance.bReducedMotion);
	CHECK(Metrics.BaseFontSize == 15.625f);
	CHECK(Metrics.TitleBarHeight == 36.0f);
	CHECK(FocusedGradient.BottomLeft == ToolUITheme::Canvas);
	CHECK(FocusedGradient.BottomRight == ToolUITheme::Canvas);
	CHECK(FocusedGradient.TopLeft == UnfocusedGradient.TopLeft);
	constexpr FEditorAppearance Colored{.Accent = {84, 108, 232, 255}};
	CHECK(ResolveToolUIGradient(Colored, true).TopLeft != ResolveToolUIGradient(Colored, false).TopLeft);
}

TEST_CASE("ToolUI panel transparency policies distinguish docked panels")
{
	CHECK(IsToolUIPanelTransparent(EPanelTransparency::AllPanels, false));
	CHECK(IsToolUIPanelTransparent(EPanelTransparency::AllPanels, true));
	CHECK(IsToolUIPanelTransparent(EPanelTransparency::FloatingOnly, false));
	CHECK_FALSE(IsToolUIPanelTransparent(EPanelTransparency::FloatingOnly, true));
	CHECK_FALSE(IsToolUIPanelTransparent(EPanelTransparency::DockedOnly, false));
	CHECK(IsToolUIPanelTransparent(EPanelTransparency::DockedOnly, true));
	CHECK_FALSE(IsToolUIPanelTransparent(EPanelTransparency::Disabled, false));
	CHECK_FALSE(IsToolUIPanelTransparent(EPanelTransparency::Disabled, true));
}

TEST_CASE("ToolUI intensity reaches the selected color at full strength")
{
	constexpr FEditorAppearance Appearance{
	    .Accent = {255, 0, 0, 255},
	    .GradientHeight = 0.5f,
	    .Saturation = 1.0f,
	    .Intensity = 1.0f};
	constexpr FToolUIGradient Gradient = ResolveToolUIGradient(Appearance, true);
	CHECK(Gradient.TopLeft == Appearance.Accent);
	CHECK(Gradient.TopRight == MixToolUIColor(ToolUITheme::Canvas, Appearance.Accent, ToolUITheme::TrailingIntensityRatio));
}

TEST_CASE("Scene viewport backgrounds never tint over the scene or erase island contrast")
{
	for (const EPanelTransparency Mode : {EPanelTransparency::AllPanels, EPanelTransparency::FloatingOnly, EPanelTransparency::DockedOnly, EPanelTransparency::Disabled})
	{
		CHECK(ResolveToolUIPanelBackgroundAlpha(Mode, true, true) == 0.0f);
		CHECK(ResolveToolUIPanelBackgroundAlpha(Mode, false, true) == 0.0f);
	}
	CHECK(ResolveToolUIPanelBackgroundAlpha(EPanelTransparency::AllPanels, true, false) == 0.0f);
	CHECK(ResolveToolUIPanelBackgroundAlpha(EPanelTransparency::Disabled, true, false) == 1.0f);
}

TEST_CASE("ToolUI viewport positioning respects compositor ownership")
{
	constexpr FToolUIViewportPosition Cached{120.0f, 240.0f};
	constexpr FToolUIViewportPosition Platform{640.0f, 360.0f};
	CHECK(ResolveToolUIViewportPosition(true, Cached, Platform) == Platform);
	CHECK(ResolveToolUIViewportPosition(false, Cached, Platform) == Cached);
}

TEST_CASE("ToolUI maps platform viewport behavior without backend details")
{
	CHECK(ResolveToolUIViewportWindowPolicy(false, false, false) == FToolUIViewportWindowPolicy{true, false, true});
	CHECK(ResolveToolUIViewportWindowPolicy(true, true, true) == FToolUIViewportWindowPolicy{false, true, false});
}

TEST_CASE("Roboto editor resources retain their redistribution license")
{
	std::ifstream License("Engine/Content/Editor/Fonts/Roboto/OFL.txt", std::ios::binary);
	REQUIRE(License.good());
	const std::string Text{std::istreambuf_iterator<char>(License), std::istreambuf_iterator<char>()};
	CHECK(Text.find("SIL OPEN FONT LICENSE") != std::string::npos);
	CHECK(Text.find("Copyright") != std::string::npos);

	std::ifstream Regular("Engine/Content/Editor/Fonts/Roboto/Roboto-Regular.ttf", std::ios::binary | std::ios::ate);
	std::ifstream Medium("Engine/Content/Editor/Fonts/Roboto/Roboto-Medium.ttf", std::ios::binary | std::ios::ate);
	CHECK(Regular.tellg() > 0);
	CHECK(Medium.tellg() > 0);
}
}
