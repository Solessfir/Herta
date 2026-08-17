#include "Herta/ToolUI/ToolUI.h"

#include <doctest/doctest.h>
#include <fstream>
#include <iterator>
#include <string>

namespace Herta
{
TEST_CASE("ToolUI theme defaults preserve the editor visual contract")
{
	constexpr FEditorAppearance Appearance;
	constexpr FToolUIThemeMetrics Metrics;
	constexpr FToolUIGradient FocusedGradient = ResolveToolUIGradient(Appearance, true);
	constexpr FToolUIGradient UnfocusedGradient = ResolveToolUIGradient(Appearance, false);

	CHECK(Appearance.Accent == ToolUITheme::Presets[6].Color);
	CHECK(Appearance.PanelTransparency == EPanelTransparency::AllPanels);
	CHECK(Metrics.BaseFontSize == 15.0f);
	CHECK(Metrics.TitleBarHeight == 36.0f);
	CHECK(Metrics.TitleBarButtonWidth == 46.0f);
	CHECK(FocusedGradient.BottomLeft == ToolUITheme::Canvas);
	CHECK(FocusedGradient.BottomRight == ToolUITheme::Canvas);
	CHECK(FocusedGradient.TopLeft != UnfocusedGradient.TopLeft);
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
