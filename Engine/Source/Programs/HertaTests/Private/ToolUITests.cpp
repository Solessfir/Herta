#include "../../../Editor/EditorFramework/Private/ConsoleInput.h"
#include "../../../Runtime/ToolUI/Private/ImmersiveViewport.h"
#include "Herta/ToolUI/ToolUI.h"
#include "TestFiles.h"

#include <doctest/doctest.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

namespace Herta
{
struct FToolUITestAccess
{
	static void MigrateContentDockHeight(std::uint32_t DockspaceId);
	static bool OrderContentDockTabs();
};

void FToolUITestAccess::MigrateContentDockHeight(const std::uint32_t DockspaceId)
{
	FToolUIContext::MigrateContentDockHeight(DockspaceId);
}

bool FToolUITestAccess::OrderContentDockTabs()
{
	return FToolUIContext::OrderContentDockTabs();
}

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
	ImGui::Begin("      Content Browser###Content Browser");
	ImGui::SetWindowFocus();
	GImGui->HoveredWindow = ImGui::GetCurrentWindow();
	CHECK(IsToolUIPanelFocused("Content Browser", false));
	CHECK(IsToolUIPanelHovered("Content Browser", false));
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

TEST_CASE("Shared icon buttons retain compact and toolbar dimensions")
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
	ImGui::Begin("IconButtons", nullptr, ImGuiWindowFlags_NoSavedSettings);
	constexpr std::string_view Label = "Content Browser";
	const float ExpectedWidth = ImGui::CalcTextSize(Label.data(), Label.data() + Label.size()).x + 36.f * ImGui::GetFontSize() / ImGui::GetStyle().FontSizeBase;
	ToolUIButton(Label, EToolUIMenuIcon::ContentBrowser, 26.f);
	CHECK(ImGui::GetItemRectSize().x == doctest::Approx(ExpectedWidth));
	CHECK(ImGui::GetItemRectSize().y == 26.f);
	ToolUIButton("Import", EToolUIMenuIcon::Open, 32.f);
	CHECK(ImGui::GetItemRectSize().y == 32.f);
	ToolUIButton("Default", EToolUIMenuIcon::ContentBrowser);
	CHECK(ImGui::GetItemRectSize().y == ImGui::GetFrameHeight());
	ImGui::End();
	ImGui::Render();
	ImGui::DestroyContext(Context);
	ImGui::SetCurrentContext(PreviousContext);
}

TEST_CASE("Project modal width constraints prevent fill-width inputs from shrinking the dialog")
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
	constexpr float Width = 440.f;
	std::array<char, 256> Name{};
	std::array<char, 256> Module{};
	std::array<char, 256> Destination{};
	std::array<float, 8> Widths{};
	std::array<float, 8> Heights{};
	bool bConstrainWidth = false;

	SUBCASE("Appearing size alone reproduces the shrink")
	{
		bConstrainWidth = false;
	}

	SUBCASE("Fixed width retains automatic height")
	{
		bConstrainWidth = true;
	}

	for (std::size_t Frame = 0; Frame < Widths.size(); ++Frame)
	{
		ImGui::NewFrame();

		if (Frame == 0)
		{
			ImGui::OpenPopup("New project");
		}

		ImGui::SetNextWindowSize({Width, 0.f}, ImGuiCond_Appearing);

		if (bConstrainWidth)
		{
			ImGui::SetNextWindowSizeConstraints({Width, 0.f}, {Width, FLT_MAX});
		}

		if (ImGui::BeginPopupModal("New project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			Widths[Frame] = ImGui::GetWindowWidth();
			Heights[Frame] = ImGui::GetWindowHeight();
			ImGui::TextUnformatted("Game project");
			ImGui::TextDisabled("C++ module, content folder, and an empty starting level.");
			ImGui::Separator();
			ImGui::TextUnformatted("Name");
			ImGui::SetNextItemWidth(-1.f);
			ImGui::InputTextWithHint("##ProjectName", "My Game", Name.data(), Name.size());
			ImGui::TextUnformatted("C++ module");
			ImGui::SetNextItemWidth(-1.f);
			ImGui::InputTextWithHint("##ProjectModule", "MyGame", Module.data(), Module.size());
			ImGui::TextUnformatted("Location (new directory)");
			ImGui::SetNextItemWidth(-1.f);
			ImGui::InputText("##ProjectDestination", Destination.data(), Destination.size());

			if (Frame >= 4)
			{
				ImGui::TextWrapped("The destination directory already exists. Choose a new directory before creating the project.");
			}

			ImGui::Separator();
			ImGui::Button("Create and open", {150.f, 0.f});
			ImGui::SameLine();
			ImGui::Button("Cancel");
			ImGui::EndPopup();
		}

		ImGui::Render();
	}

	CHECK(Widths.front() == Width);

	if (bConstrainWidth)
	{
		for (const float ActualWidth : Widths)
		{
			CHECK(ActualWidth == Width);
		}

		CHECK(Heights.back() > Heights[3]);
	}
	else
	{
		CHECK(Widths.back() < Widths.front());
	}

	ImGui::DestroyContext(Context);
	ImGui::SetCurrentContext(PreviousContext);
}

TEST_CASE("Content dock height migration changes only the original bottom split")
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
	ImGui::NewFrame();
	const ImGuiID DockId = ImHashStr("ContentHeightMigration");
	ImGui::DockBuilderAddNode(DockId, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(DockId, {1280, 640});
	ImGuiID BottomId = 0;
	ImGuiID CenterId = 0;
	ImGui::DockBuilderSplitNode(DockId, ImGuiDir_Down, 0.26f, &BottomId, &CenterId);
	ImGui::DockBuilderDockWindow("Viewport", CenterId);
	ImGui::DockBuilderDockWindow("Output Log", BottomId);
	ImGui::DockBuilderDockWindow("Content Browser", BottomId);
	ImGui::DockBuilderFinish(DockId);
	ImGuiDockNode* const Bottom = ImGui::DockBuilderGetNode(BottomId);
	ImGuiDockNode* const Center = ImGui::DockBuilderGetNode(CenterId);
	const float OriginalHeight = Bottom->Size.y;
	bool bShouldMigrate = true;
	bool bOtherDockspace = false;

	SUBCASE("Original default grows while preserving membership")
	{
		CHECK(Center->IsCentralNode());
	}

	SUBCASE("Customized bottom height is preserved")
	{
		ImGui::DockBuilderSetNodeSize(BottomId, {Bottom->Size.x, 240.f});
		bShouldMigrate = false;
	}

	SUBCASE("An extra user docked tab is preserved")
	{
		ImGui::DockBuilderDockWindow("Custom Panel", BottomId);
		bShouldMigrate = false;
	}

	SUBCASE("Unrelated dockspace is preserved")
	{
		bShouldMigrate = false;
		bOtherDockspace = true;
	}

	const float HeightBefore = Bottom->Size.y;
	FToolUITestAccess::MigrateContentDockHeight(bOtherDockspace ? ImHashStr("AnotherDockspace") : DockId);
	if (bShouldMigrate)
	{
		const float AvailableHeight = Bottom->ParentNode->Size.y - ImGui::GetStyle().DockingSeparatorSize;
		CHECK(Bottom->Size.y == doctest::Approx(AvailableHeight * 0.3f));
		CHECK(Center->Size.y + Bottom->Size.y == doctest::Approx(AvailableHeight));
		CHECK(Bottom->Size.y > OriginalHeight);
	}
	else
	{
		CHECK(Bottom->Size.y == HeightBefore);
	}

	CHECK(ImGui::FindWindowSettingsByID(ImHashStr("Output Log"))->DockId == BottomId);
	CHECK(ImGui::FindWindowSettingsByID(ImHashStr("Content Browser"))->DockId == BottomId);
	CHECK(ImGui::FindWindowSettingsByID(ImHashStr("Viewport"))->DockId == CenterId);
	ImGui::Render();
	ImGui::DestroyContext(Context);
	ImGui::SetCurrentContext(PreviousContext);
}

TEST_CASE("Content Browser stays first across saved ordering and close reopen without stealing focus")
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
	const ImGuiID DockId = ImHashStr("ContentTabMigration");
	ImGuiID BottomId = 0;
	ImGuiID CenterId = 0;
	ImGuiID LogTabId = 0;
	ImGuiID BrowserTabId = 0;
	float BottomHeight = 0.f;
	float CenterHeight = 0.f;
	bool bHideWholeBottom = false;
	bool bRestoreBrowserFocus = false;

	SUBCASE("Closing only Content Browser preserves Output Log")
	{
		bHideWholeBottom = false;
	}

	SUBCASE("Hiding the whole bottom group preserves its split and tabs")
	{
		bHideWholeBottom = true;
	}

	SUBCASE("Restoring Content Browser focus survives dock tab recreation")
	{
		bHideWholeBottom = true;
		bRestoreBrowserFocus = true;
	}

	for (int Frame = 0; Frame < 11; ++Frame)
	{
		const bool bBottomVisible = Frame < 5 || Frame > 7;
		ImGui::NewFrame();

		if (Frame == 0)
		{
			ImGui::DockBuilderAddNode(DockId, ImGuiDockNodeFlags_DockSpace);
			ImGui::DockBuilderSetNodeSize(DockId, {1280, 640});
			ImGui::DockBuilderSplitNode(DockId, ImGuiDir_Down, 0.3f, &BottomId, &CenterId);
			ImGui::DockBuilderDockWindow("Viewport", CenterId);
			ImGui::DockBuilderDockWindow("Output Log", BottomId);
			ImGui::DockBuilderDockWindow("Content Browser", BottomId);
			ImGui::DockBuilderFinish(DockId);
			ImGui::FindWindowSettingsByID(ImHashStr("Output Log"))->DockOrder = 0;
			ImGui::FindWindowSettingsByID(ImHashStr("Content Browser"))->DockOrder = 1;
		}

		ImGui::SetNextWindowPos({0, 36});
		ImGui::SetNextWindowSize({1280, 640});
		ImGui::Begin("ContentDockHost", nullptr, ImGuiWindowFlags_NoSavedSettings);
		ImGui::DockSpace(DockId, {1280, 640});
		ImGui::End();
		ImGui::Begin("Viewport");
		ImGui::End();

		if (bBottomVisible)
		{
			if (bRestoreBrowserFocus && Frame == 9)
			{
				ImGui::SetNextWindowFocus();
			}

			ImGui::Begin("      Content Browser###Content Browser");
			BrowserTabId = ImGui::GetCurrentWindow()->TabId;
			ImGui::End();
		}

		if (!bHideWholeBottom || bBottomVisible)
		{
			ImGui::Begin("Output Log");
			LogTabId = ImGui::GetCurrentWindow()->TabId;
			ImGui::End();
		}

		if (Frame == 1)
		{
			ImGui::SetWindowFocus("Output Log");
		}

		ImGuiDockNode* const Bottom = ImGui::DockBuilderGetNode(BottomId);
		ImGuiDockNode* const Center = ImGui::DockBuilderGetNode(CenterId);
		REQUIRE(Bottom != nullptr);
		REQUIRE(Center != nullptr);
		CHECK(Bottom->ParentNode == ImGui::DockBuilderGetNode(DockId));
		CHECK(Center->ParentNode == Bottom->ParentNode);
		CHECK(Bottom->ParentNode->SplitAxis == ImGuiAxis_Y);
		CHECK(ImGui::FindWindowByName("Output Log")->DockId == BottomId);
		CHECK(ImGui::FindWindowByID(ImHashStr("Content Browser"))->DockId == BottomId);
		CHECK(ImGui::FindWindowByName("Viewport")->DockId == CenterId);
		ImGuiTabBar* const Bar = Bottom->TabBar;

		if (Frame == 2)
		{
			REQUIRE(Bar != nullptr);
			CHECK(Bar->Tabs[0].ID == LogTabId);
			CHECK(Bar->SelectedTabId == LogTabId);
			CHECK(FToolUITestAccess::OrderContentDockTabs());
		}

		if (Frame > 2 && Frame < 5)
		{
			REQUIRE(Bar != nullptr);
			CHECK(Bar->Tabs[0].ID == BrowserTabId);
			CHECK(Bar->SelectedTabId == (bRestoreBrowserFocus && Frame == 4 ? BrowserTabId : LogTabId));
		}

		if (Frame == 3 && bRestoreBrowserFocus)
		{
			ImGui::SetWindowFocus("Content Browser");
		}

		if (Frame == 4)
		{
			BottomHeight = Bottom->Size.y;
			CenterHeight = Center->Size.y;
		}

		if (Frame == 7 && !bHideWholeBottom)
		{
			REQUIRE(Bar != nullptr);
			CHECK(ImGui::TabBarFindTabByID(Bar, BrowserTabId) == nullptr);
		}

		if (Frame == 7 && bHideWholeBottom)
		{
			CHECK_FALSE(Bottom->IsVisible);
			CHECK(Bottom->Windows.empty());
			CHECK(Center->Size.y > CenterHeight);
		}

		if (Frame == 10)
		{
			REQUIRE(Bar != nullptr);
			REQUIRE(Bar->Tabs.Size == 2);
			CHECK(Bar->Tabs[0].ID == BrowserTabId);
			CHECK(Bar->Tabs[1].ID == LogTabId);
			CHECK(Bottom->Size.y == doctest::Approx(BottomHeight));
			CHECK(Center->Size.y == doctest::Approx(CenterHeight));

			if (bRestoreBrowserFocus)
			{
				CHECK(Bar->SelectedTabId == BrowserTabId);
				CHECK(Bottom->SelectedTabId == BrowserTabId);
				CHECK(Bar->VisibleTabId == BrowserTabId);
			}
		}

		if (Frame > 2)
		{
			FToolUITestAccess::OrderContentDockTabs();
		}

		ImGui::Render();
	}

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
	constexpr std::string_view Label = "Undo Duplicate selected level objects";

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

TEST_CASE("Editor appearance keeps HDR display preferences and reads older files")
{
	const Tests::FScratchDirectory Directory("Appearance");
	const std::filesystem::path Path = Directory.GetPath() / "Appearance.ini";
	FEditorAppearance Appearance;
	Appearance.BlurRadius = 12.f;
	Appearance.Display = {.bHdrOutput = true, .PaperWhite = 240.f, .PeakLuminance = 1000.f};
	SaveEditorAppearance(Path, Appearance);
	CHECK(LoadEditorAppearance(Path) == Appearance);

	{
		std::ofstream Version3(Path, std::ios::trunc);
		Version3 << "HertaEditorAppearance 3\nAccent 1 2 3\nGradient 0.5 0.8 0.15\nPanel 0\nGlass 0.9 20\n";
	}

	const std::optional<FEditorAppearance> Older = LoadEditorAppearance(Path);
	REQUIRE(Older);
	CHECK(Older->BlurRadius == 20.f);
	CHECK(Older->Display == FEditorDisplay{});

	{
		std::ofstream Dim(Path, std::ios::trunc);
		Dim << "HertaEditorAppearance 4\nAccent 1 2 3\nGradient 0.5 0.8 0.15\nPanel 0\nGlass 0.9 20\nDisplay 1 40 1000\n";
	}

	CHECK_FALSE(LoadEditorAppearance(Path));
}

TEST_CASE("HDR display luminance follows the system unless overridden")
{
	constexpr FEditorDisplay FollowSystem;
	constexpr FEditorDisplayLuminance Reported = ResolveEditorDisplayLuminance(FollowSystem, 240.f, 1015.f);
	CHECK(Reported.PaperWhite == 240.f);
	CHECK(Reported.PeakLuminance == 1015.f);

	constexpr FEditorDisplayLuminance Unknown = ResolveEditorDisplayLuminance(FollowSystem, 0.f, 0.f);
	CHECK(Unknown.PaperWhite == 200.f);
	CHECK(Unknown.PeakLuminance == 1000.f);

	// A dim panel's report still has to fit the range the tone mapper is tuned for.
	CHECK(ResolveEditorDisplayLuminance(FollowSystem, 60.f, 120.f).PaperWhite == MinimumHdrPaperWhite);
	CHECK(ResolveEditorDisplayLuminance(FollowSystem, 60.f, 120.f).PeakLuminance == MinimumHdrPeakLuminance);

	constexpr FEditorDisplayLuminance Overridden = ResolveEditorDisplayLuminance({.bHdrOutput = true, .PaperWhite = 300.f, .PeakLuminance = 1600.f}, 240.f, 1015.f);
	CHECK(Overridden.PaperWhite == 300.f);
	CHECK(Overridden.PeakLuminance == 1600.f);

	// The peak never drops below paper white.
	CHECK(ResolveEditorDisplayLuminance({.bHdrOutput = true, .PaperWhite = 400.f, .PeakLuminance = 300.f}, 0.f, 0.f).PeakLuminance == 400.f);
}

TEST_CASE("ToolUI theme defaults preserve the editor visual contract")
{
	constexpr FEditorAppearance Appearance;
	constexpr FToolUIThemeMetrics Metrics;
	constexpr FToolUIGradient FocusedGradient = ResolveToolUIGradient(Appearance, true);
	constexpr FToolUIGradient UnfocusedGradient = ResolveToolUIGradient(Appearance, false);

	CHECK(Appearance.Accent == FToolUIColor{184, 184, 184, 255});
	CHECK(Appearance.PanelTransparency == EPanelTransparency::AllPanels);
	CHECK(Appearance.PanelOpacity == doctest::Approx(0.95f));
	CHECK(Appearance.BlurRadius == doctest::Approx(24.f));
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
		Focus("level.load", 10);
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
	CHECK(HasConsoleCommandText("level"));
	CHECK(HasConsoleCommandText(" \tlevel.load \n"));

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
