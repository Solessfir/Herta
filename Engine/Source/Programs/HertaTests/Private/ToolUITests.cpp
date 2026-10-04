#include "../../../Editor/EditorFramework/Private/ConsoleInput.h"
#include "../../../Runtime/ToolUI/Private/ImmersiveViewport.h"
#include "Herta/ToolUI/ToolUI.h"

#include <doctest/doctest.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <string>

namespace Herta
{
TEST_CASE("Panel focus and hover use stable IDs and the immersive viewport alias")
{
	ImGuiContext* const PreviousContext = ImGui::GetCurrentContext();
	ImGuiContext* const Context = ImGui::CreateContext();
	ImGui::SetCurrentContext(Context);
	ImGuiIO& IO = ImGui::GetIO();
	IO.DisplaySize = {1280, 720};
	IO.DeltaTime = 1.f / 60.f;
	IO.IniFilename = nullptr;
	IO.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	IO.Fonts->AddFontDefault();
	ImGui::NewFrame();
	ImGui::Begin("      Outliner###Outliner");
	ImGui::SetWindowFocus();
	CHECK(IsToolUIPanelFocused("Outliner", false));
	CHECK_FALSE(IsToolUIPanelFocused("Details", false));
	ImGui::BeginChild("Rows");
	ImGui::SetWindowFocus();
	CHECK(IsToolUIPanelFocused("Outliner", false));
	GImGui->HoveredWindow = ImGui::GetCurrentWindow();
	CHECK(IsToolUIPanelHovered("Outliner", false));
	CHECK_FALSE(IsToolUIPanelHovered("Details", false));
	ImGui::EndChild();
	ImGui::End();
	ImGui::Begin("Details");
	ImGui::SetWindowFocus("Outliner");
	GImGui->HoveredWindow = ImGui::GetCurrentWindow();
	CHECK(IsToolUIPanelFocused("Outliner", false));
	CHECK(IsToolUIPanelHovered("Details", false));
	CHECK_FALSE(IsToolUIPanelHovered("Outliner", false));
	ImGui::BeginChild("Properties");
	GImGui->HoveredWindow = ImGui::GetCurrentWindow();
	CHECK(IsToolUIPanelHovered("Details", false));
	ImGui::EndChild();
	ImGui::End();
	ImGui::Begin(ImmersiveViewportName);
	ImGui::SetWindowFocus();
	CHECK(IsToolUIPanelFocused("Viewport", true));
	CHECK_FALSE(IsToolUIPanelFocused("Viewport", false));
	CHECK_FALSE(IsToolUIPanelFocused("Outliner", true));
	GImGui->HoveredWindow = ImGui::GetCurrentWindow();
	CHECK(IsToolUIPanelHovered("Viewport", true));
	CHECK_FALSE(IsToolUIPanelHovered("Viewport", false));
	CHECK_FALSE(IsToolUIPanelHovered("Details", true));
	GImGui->HoveredWindow = nullptr;
	CHECK_FALSE(IsToolUIPanelHovered("Viewport", true));
	ImGui::End();
	ImGui::Render();
	ImGui::DestroyContext(Context);
	ImGui::SetCurrentContext(PreviousContext);
}

TEST_CASE("Icon menu sizing reserves long authoring labels and shortcut columns")
{
	ImGuiContext* const PreviousContext = ImGui::GetCurrentContext();
	ImGuiContext* const Context = ImGui::CreateContext();
	ImGui::SetCurrentContext(Context);
	ImGuiIO& IO = ImGui::GetIO();
	IO.DisplaySize = {1280, 720};
	IO.DeltaTime = 1.f / 60.f;
	IO.IniFilename = nullptr;
	IO.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	IO.Fonts->AddFontDefault();
	constexpr std::string_view Label = "Undo Duplicate selected scene objects";

	for (int Frame = 0; Frame < 4; ++Frame)
	{
		ImGui::NewFrame();
		ImGui::Begin("AuthoringMenuMeasurement", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
		const float LabelWidth = ImGui::CalcTextSize(Label.data(), Label.data() + Label.size()).x + 36.f * ImGui::GetFontSize() / ImGui::GetStyle().FontSizeBase;
		ToolUIMenuItem(Label, EToolUIMenuIcon::Undo, nullptr, "Ctrl+Z");
		const ImGuiMenuColumns& Columns = ImGui::GetCurrentWindow()->DC.MenuColumns;
		CHECK(Columns.Widths[1] >= static_cast<ImU16>(LabelWidth));
		if (Frame > 0)
		{
			CHECK(Columns.OffsetShortcut >= Columns.OffsetLabel + static_cast<ImU16>(LabelWidth));
		}

		ImGui::End();
		ImGui::Render();
	}

	ImGui::DestroyContext(Context);
	ImGui::SetCurrentContext(PreviousContext);
}

TEST_CASE("Immersive viewport follows workspace bounds without changing dock membership")
{
	ImGuiContext* const PreviousContext = ImGui::GetCurrentContext();
	ImGuiContext* const Context = ImGui::CreateContext();
	ImGui::SetCurrentContext(Context);
	ImGuiIO& IO = ImGui::GetIO();
	IO.DisplaySize = {1280, 720};
	IO.DeltaTime = 1.f / 60.f;
	IO.IniFilename = nullptr;
	IO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	IO.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	IO.Fonts->AddFontDefault();
	const ImGuiID DockId = ImHashStr("ImmersiveTestDock");
	for (int Frame = 0; Frame < 5; ++Frame)
	{
		const bool bImmersive = Frame > 0 && Frame < 4;
		ImGui::NewFrame();
		if (Frame == 0)
		{
			ImGui::DockBuilderAddNode(DockId, ImGuiDockNodeFlags_DockSpace);
			ImGui::DockBuilderSetNodeSize(DockId, {1280, 640});
			ImGui::DockBuilderDockWindow("Viewport", DockId);
			ImGui::DockBuilderFinish(DockId);
		}

		ImGui::SetNextWindowPos({0, 36});
		ImGui::SetNextWindowSize({1280, 640});
		ImGui::Begin("DockHost", nullptr, ImGuiWindowFlags_NoSavedSettings);
		ImGui::DockSpace(DockId, {1280, 640}, bImmersive ? ImGuiDockNodeFlags_KeepAliveOnly : ImGuiDockNodeFlags_None);
		ImGui::End();
		ImGui::Begin("Viewport", nullptr, bImmersive ? ImGuiWindowFlags_NoInputs : ImGuiWindowFlags_None);
		CHECK(ImGui::GetCurrentWindow()->DockId == DockId);
		ImGui::End();
		if (bImmersive)
		{
			const FToolUICanvasBounds Canvas{.X = 0, .Y = 36, .Width = Frame == 2 ? 1000.f : 1280.f, .Height = 640};
			BeginImmersiveViewport(Canvas, Frame == 1);
			const ImGuiWindow* const Window = ImGui::GetCurrentWindow();
			CHECK(Window->DockId == 0);
			CHECK(Window->Viewport == ImGui::GetMainViewport());
			CHECK(Window->Pos.y == Canvas.Y);
			CHECK(Window->Size.x == Canvas.Width);
			CHECK(Window->Size.y == Canvas.Height);
			CHECK((Window->Flags & ImGuiWindowFlags_NoSavedSettings) != 0);
			CHECK((Window->Flags & ImGuiWindowFlags_NoDocking) != 0);
			ImGui::End();
		}

		ImGui::Render();
	}

	ImGui::DestroyContext(Context);
	ImGui::SetCurrentContext(PreviousContext);
}

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
	CHECK(Appearance.PanelOpacity == doctest::Approx(0.9f));
	CHECK(Appearance.BlurRadius == doctest::Approx(24.f));
	CHECK_FALSE(Appearance.bReducedMotion);
	CHECK(Metrics.BaseFontSize == 15.625f);
	CHECK(Metrics.TitleBarHeight == 36.f);
	CHECK(FocusedGradient.BottomLeft == ToolUITheme::Canvas);
	CHECK(FocusedGradient.BottomRight == ToolUITheme::Canvas);
	CHECK(FocusedGradient.TopLeft == UnfocusedGradient.TopLeft);
	constexpr FEditorAppearance Colored{.Accent = {.Red = 84, .Green = 108, .Blue = 232, .Alpha = 255}};
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
	    .Accent = {.Red = 255, .Green = 0, .Blue = 0, .Alpha = 255},
	    .GradientHeight = 0.5f,
	    .Saturation = 1.f,
	    .Intensity = 1.f,
	};

	constexpr FToolUIGradient Gradient = ResolveToolUIGradient(Appearance, true);
	CHECK(Gradient.TopLeft == Appearance.Accent);
	CHECK(Gradient.TopRight == MixToolUIColor(ToolUITheme::Canvas, Appearance.Accent, ToolUITheme::TrailingIntensityRatio));
}

TEST_CASE("Scene viewport backgrounds never tint over the scene or erase island contrast")
{
	for (const EPanelTransparency Mode : {EPanelTransparency::AllPanels, EPanelTransparency::FloatingOnly, EPanelTransparency::DockedOnly, EPanelTransparency::Disabled})
	{
		CHECK(ResolveToolUIPanelBackgroundAlpha(Mode, true, true) == 0.f);
		CHECK(ResolveToolUIPanelBackgroundAlpha(Mode, false, true) == 0.f);
	}

	CHECK(ResolveToolUIPanelBackgroundAlpha(EPanelTransparency::AllPanels, true, false) == 0.f);
	CHECK(ResolveToolUIPanelBackgroundAlpha(EPanelTransparency::Disabled, true, false) == 1.f);
}

TEST_CASE("ToolUI viewport positioning respects compositor ownership")
{
	constexpr FToolUIViewportPosition Cached{.X = 120.f, .Y = 240.f};
	constexpr FToolUIViewportPosition Platform{.X = 640.f, .Y = 360.f};
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

TEST_CASE("Console shortcuts operate on the active ImGui text editor")
{
	ImGuiContext* const PreviousContext = ImGui::GetCurrentContext();
	ImGuiContext* const Context = ImGui::CreateContext();
	ImGui::SetCurrentContext(Context);
	ImGuiIO& IO = ImGui::GetIO();
	IO.DisplaySize = {1280, 720};
	IO.DeltaTime = 1.f / 60.f;
	IO.IniFilename = nullptr;
	IO.ConfigInputTrickleEventQueue = false;
	IO.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
	IO.Fonts->AddFontDefault();

	struct FInputProbe
	{
		std::array<char, 256> Buffer{};
		int Cursor = 0;
		int SelectionStart = 0;
		int SelectionEnd = 0;
		bool bSeedCaret = true;
	} Probe;

	const auto DrawFrame = [&](const bool bFocus = false)
	{
		ImGui::NewFrame();
		ImGui::Begin("ConsoleShortcutTest", nullptr, ImGuiWindowFlags_NoSavedSettings);

		if (bFocus)
		{
			ImGui::SetKeyboardFocusHere();
		}

		ImGui::InputText("Console", Probe.Buffer.data(), Probe.Buffer.size(), ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_CallbackCharFilter, [](ImGuiInputTextCallbackData* const Data)
		{
			FInputProbe& Input = *static_cast<FInputProbe*>(Data->UserData);
			if (Input.bSeedCaret && Data->EventFlag == ImGuiInputTextFlags_CallbackAlways)
			{
				Data->CursorPos = Input.Cursor;
				Data->SelectionStart = Input.SelectionStart;
				Data->SelectionEnd = Input.SelectionEnd;
				Input.bSeedCaret = false;
			}

			const int Result = HandleConsoleInputShortcuts(*Data);
			if (Data->EventFlag != ImGuiInputTextFlags_CallbackCharFilter)
			{
				Input.Cursor = Data->CursorPos;
				Input.SelectionStart = Data->SelectionStart;
				Input.SelectionEnd = Data->SelectionEnd;
			}

			return Result;
		}, &Probe);

		ImGui::End();
		ImGui::Render();
	};
	const auto Focus = [&](const std::string_view Text, const int Cursor, const int SelectionStart = -1, const int SelectionEnd = -1)
	{
		std::ranges::copy(Text, Probe.Buffer.begin());
		Probe.Cursor = Cursor;
		Probe.SelectionStart = SelectionStart < 0 ? Cursor : SelectionStart;
		Probe.SelectionEnd = SelectionEnd < 0 ? Cursor : SelectionEnd;
		DrawFrame(true);
		DrawFrame();
		DrawFrame();
		CHECK_FALSE(Probe.bSeedCaret);
		CHECK(Probe.Cursor == Cursor);
	};
	const auto Press = [&](const ImGuiKey Key, const bool bCtrl = false, const bool bAlt = false, const bool bShift = false, const char* const Text = nullptr)
	{
		IO.AddKeyEvent(ImGuiMod_Ctrl, bCtrl);
		IO.AddKeyEvent(ImGuiMod_Alt, bAlt);
		IO.AddKeyEvent(ImGuiMod_Shift, bShift);
		IO.AddKeyEvent(Key, true);

		if (Text != nullptr)
		{
			IO.AddInputCharactersUTF8(Text);
		}

		DrawFrame();
		IO.AddKeyEvent(Key, false);
		IO.AddKeyEvent(ImGuiMod_Ctrl, false);
		IO.AddKeyEvent(ImGuiMod_Alt, false);
		IO.AddKeyEvent(ImGuiMod_Shift, false);
		DrawFrame();
	};
	const auto CheckUnselected = [&](const int Cursor)
	{
		CHECK(Probe.Cursor == Cursor);
		CHECK(Probe.SelectionStart == Cursor);
		CHECK(Probe.SelectionEnd == Cursor);
	};

	SUBCASE("Ctrl A overrides built-in select-all and Ctrl Shift A selects all")
	{
		Focus("alpha beta", 6);
		Press(ImGuiKey_A, true);
		CheckUnselected(0);
		Press(ImGuiKey_E, true);
		CheckUnselected(10);
		Press(ImGuiKey_A, true, false, true);
		CHECK(std::min(Probe.SelectionStart, Probe.SelectionEnd) == 0);
		CHECK(std::max(Probe.SelectionStart, Probe.SelectionEnd) == 10);
		CHECK(std::string_view(Probe.Buffer.data()) == "alpha beta");
	}

	SUBCASE("Ctrl U deletes the prefix and Ctrl Z restores it")
	{
		Focus("alpha beta", 6);
		Press(ImGuiKey_U, true);
		CHECK(std::string_view(Probe.Buffer.data()) == "beta");
		CheckUnselected(0);
		Press(ImGuiKey_Z, true);
		CHECK(std::string_view(Probe.Buffer.data()) == "alpha beta");
	}

	SUBCASE("Ctrl K deletes the suffix from a nonzero caret")
	{
		Focus("alpha beta", 6);
		Press(ImGuiKey_K, true);
		CHECK(std::string_view(Probe.Buffer.data()) == "alpha ");
		CheckUnselected(6);
	}

	SUBCASE("Ctrl U from the end removes the command suggestion prefix")
	{
		Focus("scene.load", 10);
		CHECK(HasConsoleCommandText(Probe.Buffer.data()));
		Press(ImGuiKey_U, true);
		CHECK(std::string_view(Probe.Buffer.data()).empty());
		CHECK_FALSE(HasConsoleCommandText(Probe.Buffer.data()));
		CheckUnselected(0);
	}

	SUBCASE("Ctrl W removes the previous whitespace-delimited word")
	{
		Focus("alpha beta.gamma tail", 17);
		Press(ImGuiKey_W, true);
		CHECK(std::string_view(Probe.Buffer.data()) == "alpha tail");
		CheckUnselected(6);
	}

	SUBCASE("A deletion shortcut prefers the selected range")
	{
		Focus("alpha beta tail", 10, 6, 10);
		Press(ImGuiKey_U, true);
		CHECK(std::string_view(Probe.Buffer.data()) == "alpha  tail");
		CheckUnselected(6);
	}

	SUBCASE("Alt word movement skips punctuation and filters shortcut characters")
	{
		Focus("alpha.beta_gamma tail", 5);
		Press(ImGuiKey_F, false, true, false, "f");
		CheckUnselected(16);
		Press(ImGuiKey_B, false, true, false, "b");
		CheckUnselected(6);
		Press(ImGuiKey_B, false, true);
		CheckUnselected(0);
		CHECK(std::string_view(Probe.Buffer.data()) == "alpha.beta_gamma tail");
	}

	SUBCASE("Alt word movement remains on UTF8 character boundaries")
	{
		Focus("one caf\xC3\xA9.foo", 4);
		Press(ImGuiKey_F, false, true);
		CheckUnselected(9);
		Press(ImGuiKey_B, false, true);
		CheckUnselected(4);
		CHECK(std::string_view(Probe.Buffer.data()) == "one caf\xC3\xA9.foo");
	}

	SUBCASE("Ordinary input is not intercepted")
	{
		Focus("alpha", 5);
		Press(ImGuiKey_B, false, false, false, "b");
		Press(ImGuiKey_F, false, false, false, "f");
		CHECK(std::string_view(Probe.Buffer.data()) == "alphabf");
		CHECK(Probe.Cursor == 7);
		CHECK(Probe.SelectionStart == Probe.SelectionEnd);
	}

	ImGui::DestroyContext(Context);
	ImGui::SetCurrentContext(PreviousContext);
}

TEST_CASE("Console suggestions require command text and dismiss when interaction ends")
{
	CHECK_FALSE(HasConsoleCommandText(""));
	CHECK_FALSE(HasConsoleCommandText(" \t\r\n\v\f"));
	CHECK(HasConsoleCommandText("scene"));
	CHECK(HasConsoleCommandText(" \tscene.load \n"));

	CHECK_FALSE(ShouldDismissConsoleSuggestions(true, false, false));
	CHECK_FALSE(ShouldDismissConsoleSuggestions(true, true, false));
	CHECK_FALSE(ShouldDismissConsoleSuggestions(false, true, false));
	CHECK(ShouldDismissConsoleSuggestions(false, false, false));
	CHECK(ShouldDismissConsoleSuggestions(true, false, true));
	CHECK(ShouldDismissConsoleSuggestions(true, true, true));
	CHECK(ShouldDismissConsoleSuggestions(false, true, true));
	CHECK(ShouldDismissConsoleSuggestions(false, false, true));
}
}
