#include "Herta/ToolUI/ToolUI.h"

#include "Herta/Application/Application.h"
#include "Herta/Core/Build.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <backends/imgui_impl_glfw.h>
#include <cfloat>
#include <cmath>
#include <format>
#include <fstream>
#include <imgui.h>
#include <imgui_internal.h>
#include <limits>
#include <misc/freetype/imgui_freetype.h>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Herta
{
namespace
{
[[nodiscard]] std::string PathToUtf8(const std::filesystem::path& Path)
{
	const std::u8string Text = Path.generic_u8string();
	return {reinterpret_cast<const char*>(Text.data()), Text.size()};
}

[[nodiscard]] ImVec4 ToImGuiColor(const FToolUIColor Color) noexcept
{
	constexpr float InverseByte = 1.0f / 255.0f;
	return {static_cast<float>(Color.Red) * InverseByte, static_cast<float>(Color.Green) * InverseByte, static_cast<float>(Color.Blue) * InverseByte, static_cast<float>(Color.Alpha) * InverseByte};
}

[[nodiscard]] ImU32 ToImGuiPackedColor(const FToolUIColor Color) noexcept
{
	return IM_COL32(Color.Red, Color.Green, Color.Blue, Color.Alpha);
}

[[nodiscard]] ImVec4 WithAlpha(const FToolUIColor Color, const float Alpha) noexcept
{
	ImVec4 Result = ToImGuiColor(Color);
	Result.w = Alpha;
	return Result;
}

void DrawWindowControls(ImDrawList& DrawList, const ImVec2 Origin, const FTitleBarLayout& Layout)
{
	const ImVec2 MousePosition = ImGui::GetIO().MousePos;
	const float Height = static_cast<float>(Layout.TitleBarHeight);
	const float Scale = Height / static_cast<float>(DefaultTitleBarHeight);
	const float CenterY = Origin.y + Height * 0.5f;
	const ImU32 GlyphColor = ToImGuiPackedColor(ToolUITheme::TextPrimary);
	const auto DrawBackground = [&](const FTitleBarControlBounds Bounds, const bool bClose)
	{
		if (!Bounds.bVisible || MousePosition.x < Origin.x + static_cast<float>(Bounds.MinimumX) || MousePosition.x >= Origin.x + static_cast<float>(Bounds.MaximumX) || MousePosition.y < Origin.y || MousePosition.y >= Origin.y + Height)
		{
			return;
		}

		DrawList.AddRectFilled(
		    {Origin.x + static_cast<float>(Bounds.MinimumX), Origin.y},
		    {Origin.x + static_cast<float>(Bounds.MaximumX), Origin.y + Height},
		    ToImGuiPackedColor(bClose ? ToolUITheme::CloseHover : ToolUITheme::TitleBarControlHover),
		    4.0f * Scale);
	};
	const FTitleBarControlBounds Minimize = GetTitleBarControlBounds(Layout, ETitleBarHitRegion::MinimizeButton);
	const FTitleBarControlBounds Maximize = GetTitleBarControlBounds(Layout, ETitleBarHitRegion::MaximizeButton);
	const FTitleBarControlBounds Close = GetTitleBarControlBounds(Layout, ETitleBarHitRegion::CloseButton);
	DrawBackground(Minimize, false);
	DrawBackground(Maximize, false);
	DrawBackground(Close, true);

	if (Minimize.bVisible)
	{
		const float CenterX = Origin.x + static_cast<float>(Minimize.MinimumX + Minimize.MaximumX) * 0.5f;
		DrawList.AddLine({CenterX - 5.0f * Scale, CenterY + 3.0f * Scale}, {CenterX + 5.0f * Scale, CenterY + 3.0f * Scale}, GlyphColor, Scale);
	}

	if (Maximize.bVisible)
	{
		const float CenterX = Origin.x + static_cast<float>(Maximize.MinimumX + Maximize.MaximumX) * 0.5f;
		if (Layout.bMaximized)
		{
			DrawList.AddRect({CenterX - 4.0f * Scale, CenterY - 3.0f * Scale}, {CenterX + 4.0f * Scale, CenterY + 5.0f * Scale}, GlyphColor, 0.0f, 0, Scale);
			DrawList.AddRect({CenterX - 2.0f * Scale, CenterY - 5.0f * Scale}, {CenterX + 6.0f * Scale, CenterY + 3.0f * Scale}, GlyphColor, 0.0f, 0, Scale);
		}
		else
		{
			DrawList.AddRect({CenterX - 5.0f * Scale, CenterY - 5.0f * Scale}, {CenterX + 5.0f * Scale, CenterY + 5.0f * Scale}, GlyphColor, 0.0f, 0, Scale);
		}
	}

	if (Close.bVisible)
	{
		const float CenterX = Origin.x + static_cast<float>(Close.MinimumX + Close.MaximumX) * 0.5f;
		DrawList.AddLine({CenterX - 5.0f * Scale, CenterY - 5.0f * Scale}, {CenterX + 5.0f * Scale, CenterY + 5.0f * Scale}, GlyphColor, Scale);
		DrawList.AddLine({CenterX + 5.0f * Scale, CenterY - 5.0f * Scale}, {CenterX - 5.0f * Scale, CenterY + 5.0f * Scale}, GlyphColor, Scale);
	}
}

[[nodiscard]] std::expected<std::vector<std::byte>, FToolUIError> ReadFile(const std::filesystem::path& Path)
{
	std::ifstream Stream(Path, std::ios::binary | std::ios::ate);
	if (!Stream)
	{
		return std::unexpected(FToolUIError{"Could not open editor resource: " + PathToUtf8(Path)});
	}

	const std::streamsize Size = Stream.tellg();
	if (Size <= 0)
	{
		return std::unexpected(FToolUIError{"Editor resource is empty: " + PathToUtf8(Path)});
	}

	try
	{
		std::vector<std::byte> Bytes(static_cast<std::size_t>(Size));
		Stream.seekg(0, std::ios::beg);
		if (!Stream.read(reinterpret_cast<char*>(Bytes.data()), Size))
		{
			return std::unexpected(FToolUIError{"Could not read editor resource: " + PathToUtf8(Path)});
		}
		return Bytes;
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FToolUIError{Exception.what()});
	}
}

[[nodiscard]] std::optional<FEditorAppearance> LoadAppearance(const std::filesystem::path& Path)
{
	std::ifstream Stream(Path);
	std::string Header;
	int Version = 0;
	std::string AccentLabel;
	int Red = 0;
	int Green = 0;
	int Blue = 0;
	std::string GradientLabel;
	FEditorAppearance Appearance;
	std::string PanelLabel;
	int PanelMode = 0;
	if (!(Stream >> Header >> Version) || Header != "HertaEditorAppearance" || (Version != 1 && Version != 2) ||
	    !(Stream >> AccentLabel >> Red >> Green >> Blue) || AccentLabel != "Accent" ||
	    !(Stream >> GradientLabel >> Appearance.GradientHeight >> Appearance.Saturation >> Appearance.Intensity) || GradientLabel != "Gradient" ||
	    !(Stream >> PanelLabel >> PanelMode) || PanelLabel != "Panel")
	{
		return std::nullopt;
	}
	if (Version == 2)
	{
		std::string GlassLabel;
		std::string MotionLabel;
		int ReducedMotion = 0;
		if (!(Stream >> GlassLabel >> Appearance.PanelOpacity >> Appearance.BlurRadius) || GlassLabel != "Glass" ||
		    !(Stream >> MotionLabel >> ReducedMotion) || MotionLabel != "Motion" || (ReducedMotion != 0 && ReducedMotion != 1))
		{
			return std::nullopt;
		}
		Appearance.bReducedMotion = ReducedMotion != 0;
	}

	if (Red < 0 || Red > 255 || Green < 0 || Green > 255 || Blue < 0 || Blue > 255 ||
	    !std::isfinite(Appearance.GradientHeight) || Appearance.GradientHeight < 0.0f || Appearance.GradientHeight > 1.0f ||
	    !std::isfinite(Appearance.Saturation) || Appearance.Saturation < 0.0f || Appearance.Saturation > 1.0f ||
	    !std::isfinite(Appearance.Intensity) || Appearance.Intensity < 0.0f || Appearance.Intensity > 1.0f ||
	    !std::isfinite(Appearance.PanelOpacity) || Appearance.PanelOpacity < 0.0f || Appearance.PanelOpacity > 1.0f ||
	    !std::isfinite(Appearance.BlurRadius) || Appearance.BlurRadius < 0.0f || Appearance.BlurRadius > 40.0f ||
	    PanelMode < static_cast<int>(EPanelTransparency::AllPanels) || PanelMode > static_cast<int>(EPanelTransparency::Disabled))
	{
		return std::nullopt;
	}

	Appearance.Accent = {static_cast<std::uint8_t>(Red), static_cast<std::uint8_t>(Green), static_cast<std::uint8_t>(Blue), 255};
	if (Version == 1 && Appearance.Accent == FToolUIColor{84, 108, 232, 255})
		Appearance.Accent = FEditorAppearance{}.Accent;
	Appearance.PanelTransparency = static_cast<EPanelTransparency>(PanelMode);
	return Appearance;
}

void SaveAppearance(const std::filesystem::path& Path, const FEditorAppearance& Appearance) noexcept
{
	try
	{
		std::ofstream Stream(Path, std::ios::trunc);
		if (!Stream)
		{
			return;
		}
		Stream << "HertaEditorAppearance 2\n";
		Stream << "Accent " << static_cast<int>(Appearance.Accent.Red) << ' ' << static_cast<int>(Appearance.Accent.Green) << ' ' << static_cast<int>(Appearance.Accent.Blue) << '\n';
		Stream << "Gradient " << Appearance.GradientHeight << ' ' << Appearance.Saturation << ' ' << Appearance.Intensity << '\n';
		Stream << "Panel " << static_cast<int>(Appearance.PanelTransparency) << '\n';
		Stream << "Glass " << Appearance.PanelOpacity << ' ' << Appearance.BlurRadius << '\n';
		Stream << "Motion " << static_cast<int>(Appearance.bReducedMotion) << '\n';
	}
	catch (...) // NOLINT(bugprone-empty-catch)
	{
		// Appearance persistence is best-effort and must not break editor teardown.
	}
}

void ResetRendererTextureState() noexcept
{
	for (ImTextureData* const Texture : ImGui::GetPlatformIO().Textures)
	{
		if (Texture->GetTexID() != ImTextureID_Invalid)
		{
			Texture->SetTexID(ImTextureID_Invalid);
			Texture->BackendUserData = nullptr;
			Texture->SetStatus(ImTextureStatus_Destroyed);
		}
	}
	ImGui::GetIO().BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;
}

void ApplyBaseStyle(ImGuiStyle& Style, const FToolUIThemeMetrics& Metrics)
{
	Style.FontSizeBase = Metrics.BaseFontSize;
	Style.WindowPadding = {Metrics.WindowPadding, Metrics.WindowPadding};
	Style.WindowRounding = Metrics.WindowRounding;
	Style.WindowBorderSize = 0.0f;
	Style.ChildRounding = Metrics.ChildRounding;
	Style.ChildBorderSize = 0.0f;
	Style.PopupRounding = Metrics.PopupRounding;
	Style.PopupBorderSize = 1.0f;
	Style.FramePadding = {12.0f, 8.0f};
	Style.FrameRounding = Metrics.FrameRounding;
	Style.FrameBorderSize = 1.0f;
	Style.ItemSpacing = {8.0f, 8.0f};
	Style.ItemInnerSpacing = {8.0f, 6.0f};
	Style.CellPadding = {10.0f, 8.0f};
	Style.IndentSpacing = 20.0f;
	Style.ScrollbarSize = Metrics.ScrollbarSize;
	Style.ScrollbarRounding = Metrics.ScrollbarRounding;
	Style.GrabMinSize = 10.0f;
	Style.GrabRounding = Metrics.FrameRounding;
	Style.ImageRounding = Metrics.FrameRounding;
	Style.TabRounding = 0.0f;
	Style.MenuItemRounding = Metrics.FrameRounding;
	Style.DragDropTargetRounding = Metrics.FrameRounding;
	Style.TabBorderSize = 0.0f;
	Style.TabBarBorderSize = 0.0f;
	Style.TabBarOverlineSize = 0.0f;
	Style.DockingSeparatorSize = 1.0f;
	Style.SeparatorSize = 1.0f;
	Style.DisabledAlpha = 0.55f;

	ImVec4* const Palette = Style.Colors;
	Palette[ImGuiCol_Text] = ToImGuiColor(ToolUITheme::TextPrimary);
	Palette[ImGuiCol_TextDisabled] = ToImGuiColor(ToolUITheme::TextMuted);
	Palette[ImGuiCol_WindowBg] = WithAlpha(ToolUITheme::Surface0, 0.94f);
	Palette[ImGuiCol_ChildBg] = WithAlpha(ToolUITheme::Surface1, 0.90f);
	Palette[ImGuiCol_PopupBg] = ToImGuiColor(ToolUITheme::Surface1);
	Palette[ImGuiCol_Border] = ToImGuiColor(ToolUITheme::Border);
	Palette[ImGuiCol_BorderShadow] = WithAlpha(ToolUITheme::Canvas, 0.0f);
	Palette[ImGuiCol_FrameBg] = ToImGuiColor(ToolUITheme::Surface1);
	Palette[ImGuiCol_TitleBg] = ToImGuiColor(ToolUITheme::Surface0);
	Palette[ImGuiCol_TitleBgActive] = ToImGuiColor(ToolUITheme::Surface0);
	Palette[ImGuiCol_TitleBgCollapsed] = ToImGuiColor(ToolUITheme::Surface0);
	Palette[ImGuiCol_MenuBarBg] = ToImGuiColor(ToolUITheme::Surface0);
	Palette[ImGuiCol_ScrollbarBg] = ToImGuiColor(ToolUITheme::Surface0);
	Palette[ImGuiCol_ScrollbarGrab] = ToImGuiColor(ToolUITheme::Border);
	Palette[ImGuiCol_Button] = ToImGuiColor(ToolUITheme::Surface1);
	Palette[ImGuiCol_Separator] = ToImGuiColor(ToolUITheme::BorderSoft);
	Palette[ImGuiCol_ResizeGrip] = WithAlpha(ToolUITheme::Canvas, 0.0f);
	Palette[ImGuiCol_Tab] = WithAlpha(ToolUITheme::Surface0, 0.0f);
	Palette[ImGuiCol_TabSelected] = WithAlpha(ToolUITheme::Surface0, 0.0f);
	Palette[ImGuiCol_TabDimmed] = WithAlpha(ToolUITheme::Surface0, 0.0f);
	Palette[ImGuiCol_TabDimmedSelected] = WithAlpha(ToolUITheme::Surface0, 0.0f);
	Palette[ImGuiCol_DockingPreview] = WithAlpha(ToolUITheme::TextPrimary, 0.15f);
	Palette[ImGuiCol_DockingEmptyBg] = WithAlpha(ToolUITheme::Canvas, 0.0f);
	Palette[ImGuiCol_TableHeaderBg] = ToImGuiColor(ToolUITheme::Surface2);
	Palette[ImGuiCol_TableBorderStrong] = ToImGuiColor(ToolUITheme::Border);
	Palette[ImGuiCol_TableBorderLight] = ToImGuiColor(ToolUITheme::BorderSoft);
	Palette[ImGuiCol_TableRowBg] = WithAlpha(ToolUITheme::Canvas, 0.0f);
	Palette[ImGuiCol_TableRowBgAlt] = WithAlpha(ToolUITheme::Surface2, 0.45f);
	Palette[ImGuiCol_NavWindowingHighlight] = ToImGuiColor(ToolUITheme::TextPrimary);
	Palette[ImGuiCol_NavWindowingDimBg] = WithAlpha(ToolUITheme::Canvas, 0.72f);
	Palette[ImGuiCol_ModalWindowDimBg] = WithAlpha(ToolUITheme::Canvas, 0.82f);
}

void ApplyInteractiveColors(ImGuiStyle& Style, const FToolUIColor Accent)
{
	ImVec4* const Palette = Style.Colors;
	const auto Tint = [Accent](const FToolUIColor Base, const float Strength)
	{
		return ToImGuiColor(AdditiveToolUITint(Base, Accent, Strength));
	};
	Palette[ImGuiCol_FrameBgHovered] = Tint(ToolUITheme::Surface1, ToolUITheme::HoverTint);
	Palette[ImGuiCol_FrameBgActive] = Tint(ToolUITheme::Surface1, ToolUITheme::ActiveTint);
	Palette[ImGuiCol_ScrollbarGrabHovered] = Tint(ToolUITheme::Border, ToolUITheme::HoverTint);
	Palette[ImGuiCol_ScrollbarGrabActive] = Tint(ToolUITheme::Border, ToolUITheme::ActiveTint);
	Palette[ImGuiCol_CheckMark] = ToImGuiColor(Accent);
	Palette[ImGuiCol_CheckboxSelectedBg] = Tint(ToolUITheme::Surface2, ToolUITheme::ActiveTint);
	Palette[ImGuiCol_SliderGrab] = Tint(ToolUITheme::Border, ToolUITheme::ActiveTint);
	Palette[ImGuiCol_SliderGrabActive] = Tint(ToolUITheme::Border, ToolUITheme::StrongTint);
	Palette[ImGuiCol_ButtonHovered] = Tint(ToolUITheme::Surface1, ToolUITheme::HoverTint);
	Palette[ImGuiCol_ButtonActive] = Tint(ToolUITheme::Surface1, ToolUITheme::ActiveTint);
	Palette[ImGuiCol_Header] = Tint(ToolUITheme::Surface2, ToolUITheme::SubtleTint);
	Palette[ImGuiCol_HeaderHovered] = Tint(ToolUITheme::Surface1, ToolUITheme::HoverTint);
	Palette[ImGuiCol_HeaderActive] = Tint(ToolUITheme::Surface1, ToolUITheme::ActiveTint);
	Palette[ImGuiCol_SeparatorHovered] = Tint(ToolUITheme::Border, ToolUITheme::ActiveTint);
	Palette[ImGuiCol_SeparatorActive] = Tint(ToolUITheme::Border, ToolUITheme::StrongTint);
	Palette[ImGuiCol_ResizeGripHovered] = Tint(ToolUITheme::Surface1, ToolUITheme::ActiveTint);
	Palette[ImGuiCol_ResizeGripActive] = Tint(ToolUITheme::Surface1, ToolUITheme::StrongTint);
	Palette[ImGuiCol_InputTextCursor] = ToImGuiColor(Accent);
	Palette[ImGuiCol_TabHovered] = WithAlpha(ToolUITheme::SurfaceHover, 0.35f);
	Palette[ImGuiCol_TabSelectedOverline] = ToImGuiColor(ToolUITheme::NeutralAccent);
	Palette[ImGuiCol_PlotLinesHovered] = ToImGuiColor(Accent);
	Palette[ImGuiCol_PlotHistogram] = ToImGuiColor(Accent);
	Palette[ImGuiCol_PlotHistogramHovered] = Tint(Accent, ToolUITheme::ActiveTint);
	Palette[ImGuiCol_TextLink] = ToImGuiColor(Accent);
	Palette[ImGuiCol_TextSelectedBg] = WithAlpha(Accent, 0.35f);
	Palette[ImGuiCol_DragDropTarget] = ToImGuiColor(Accent);
	Palette[ImGuiCol_DragDropTargetBg] = WithAlpha(Accent, 0.15f);
	Palette[ImGuiCol_UnsavedMarker] = ToImGuiColor(Accent);
	Palette[ImGuiCol_NavCursor] = ToImGuiColor(Accent);

	// Docking is a structural action, so it stays neutral instead of inheriting content color.
	Palette[ImGuiCol_DockingPreview] = WithAlpha(ToolUITheme::TextPrimary, 0.15f);
}

void ApplyAppearanceStyle(ImGuiStyle& Style, const FEditorAppearance& Appearance)
{
	const float Alpha = ClampToolUIUnit(Appearance.PanelOpacity);
	ImVec4* const Palette = Style.Colors;
	Palette[ImGuiCol_WindowBg] = WithAlpha(ToolUITheme::Surface0, Alpha);
	Palette[ImGuiCol_ChildBg] = WithAlpha(ToolUITheme::Surface0, 0.0f);
	Palette[ImGuiCol_FrameBg] = WithAlpha(ToolUITheme::Surface1, Alpha * 0.5f);
	Palette[ImGuiCol_Button] = WithAlpha(ToolUITheme::Surface1, Alpha * 0.5f);
	Palette[ImGuiCol_PopupBg] = WithAlpha(ToolUITheme::Surface1, 0.0f);
	Palette[ImGuiCol_TitleBg] = WithAlpha(ToolUITheme::Surface0, 0.0f);
	Palette[ImGuiCol_TitleBgActive] = WithAlpha(ToolUITheme::Surface0, 0.0f);
	Palette[ImGuiCol_TitleBgCollapsed] = WithAlpha(ToolUITheme::Surface0, 0.0f);
	Palette[ImGuiCol_MenuBarBg] = WithAlpha(ToolUITheme::Surface0, 0.0f);
	Palette[ImGuiCol_ScrollbarBg] = WithAlpha(ToolUITheme::Surface0, 0.0f);
	Palette[ImGuiCol_TableHeaderBg] = WithAlpha(ToolUITheme::Surface2, Alpha);
	Palette[ImGuiCol_TableRowBgAlt] = WithAlpha(ToolUITheme::Surface2, Alpha * 0.5f);
	Palette[ImGuiCol_TabHovered] = WithAlpha(ToolUITheme::SurfaceHover, Alpha * 0.5f);
	Palette[ImGuiCol_Border] = WithAlpha(ToolUITheme::Border, Alpha);
	Palette[ImGuiCol_BorderShadow] = WithAlpha(ToolUITheme::Canvas, 0.0f);
}

}

struct FToolUIContext::FImplementation
{
	FApplication* Application = nullptr;
	FWindow* Window = nullptr;
	GLFWwindow* BackendWindow = nullptr;
	FEditorAppearance Appearance;
	FToolUIThemeMetrics Metrics;
	FToolUIRendererBridge Renderer;
	std::function<void(bool)> VSyncChanged;
	std::vector<std::byte> RegularFontBytes;
	std::vector<std::byte> MediumFontBytes;
	std::string LayoutPath;
	std::filesystem::path AppearancePath;
	std::unordered_map<ImGuiID, bool> PreviousDockState;
	std::unordered_map<ImGuiID, bool> PresentedPanels;
	std::optional<FToolUIError> PendingError;
	ImGuiContext* Context = nullptr;
	ImFont* RegularFont = nullptr;
	ImFont* MediumFont = nullptr;
	ImGuiStyle BaseStyle;
	void (*BackendPlatformDestroyWindow)(ImGuiViewport*) = nullptr;
	ImGuiID DockspaceId = 0;
	FToolUICanvasBounds WorkspaceCanvas;
	float CurrentStyleScale = 1.0f;
	float MainMenuRight = 0.0f;
	double MainCursorX = 0.0;
	double MainCursorY = 0.0;
	EWindowSystem WindowSystem = EWindowSystem::Unknown;
	bool bMainCursorPositionValid = false;
	bool bProgrammaticWindowPosition = false;
	bool bVSync = true;
	bool bBuildDefaultLayout = false;
	bool bDetailsDockMigrationComplete = false;
	bool bFrameActive = false;
	bool bAppearanceDirty = false;
	bool bPlatformWindowsRendered = true;
	bool bDestroying = false;
	bool bGlfwInitialized = false;
	bool bHertaPlatformInterfaceInstalled = false;
	bool bCallbacksInstalled = false;
	bool bRendererInitialized = false;
};

namespace
{
void DrawGlassSurface(ImDrawList& DrawList, const ImGuiViewport& Viewport, const FEditorAppearance& Appearance, const ImVec2 Minimum, const ImVec2 Maximum, const float Radius)
{
	const float BlurRadius = std::clamp(Appearance.BlurRadius, 0.0f, 40.0f);
	if (BlurRadius > 0.0f && Viewport.Size.x > 0.0f && Viewport.Size.y > 0.0f)
	{
		// Reserved renderer IDs encode the shared backdrop radius in framebuffer pixels.
		const auto PixelRadius = static_cast<ImTextureID>(std::clamp(std::round(BlurRadius * Viewport.DpiScale), 1.0f, 160.0f));
		const ImTextureID BackdropId = std::numeric_limits<ImTextureID>::max() - PixelRadius;
		const ImVec2 UVMinimum{(Minimum.x - Viewport.Pos.x) / Viewport.Size.x, (Minimum.y - Viewport.Pos.y) / Viewport.Size.y};
		const ImVec2 UVMaximum{(Maximum.x - Viewport.Pos.x) / Viewport.Size.x, (Maximum.y - Viewport.Pos.y) / Viewport.Size.y};
		DrawList.AddImageRounded(ImTextureRef(BackdropId), Minimum, Maximum, UVMinimum, UVMaximum, IM_COL32_WHITE, Radius);
	}
	DrawList.AddRectFilled(Minimum, Maximum, ImGui::ColorConvertFloat4ToU32(WithAlpha(ToolUITheme::Surface0, ClampToolUIUnit(Appearance.PanelOpacity))), Radius);
	if (Radius > 0.0f)
		DrawList.AddRect(Minimum, Maximum, ImGui::ColorConvertFloat4ToU32(WithAlpha(ToolUITheme::NeutralAccent, ClampToolUIUnit(Appearance.PanelOpacity) * 0.28f)), Radius, 0, Viewport.DpiScale);
}

void DrawWindowSurfaces(FToolUIContext::FImplementation& Owner)
{
	// ImGui owns dock tabs and popup creation. Keep their surface/underline adapter here, without changing docking internals.
	for (ImGuiWindow* const Window : Owner.Context->Windows)
	{
		if (!Window->Active || Window->Hidden)
			continue;
		const bool bPopup = (Window->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip)) != 0;
		const auto Panel = Owner.PresentedPanels.find(Window->ID);
		const bool bPanel = Panel != Owner.PresentedPanels.end();
		if (!bPopup && !bPanel)
			continue;
		ImGuiDockNode* const Node = Window->DockNode;
		if (Node && Node->VisibleWindow != Window)
			continue;
		ImDrawList* const DrawList = Node && Node->HostWindow ? Node->HostWindow->DrawList : Window->DrawList;
		const ImVec2 Minimum = Node ? Node->Pos : Window->Pos;
		const ImVec2 Size = Node ? Node->Size : Window->Size;
		const ImVec2 Maximum{Minimum.x + Size.x, Minimum.y + Size.y};
		const float Radius = Node ? 0.0f : Window->WindowRounding;
		if (bPopup || (bPanel && !Panel->second && IsToolUIPanelTransparent(Owner.Appearance.PanelTransparency, Node != nullptr)))
		{
			ImDrawListSplitter Splitter;
			Splitter.Split(DrawList, 2);
			Splitter.SetCurrentChannel(DrawList, 1);
			DrawList->PushClipRect(Minimum, Maximum, false);
			DrawGlassSurface(*DrawList, *Window->Viewport, Owner.Appearance, Minimum, Maximum, Radius);
			DrawList->PopClipRect();
			Splitter.SetCurrentChannel(DrawList, 0);
			DrawList->CmdBuffer.swap(Splitter._Channels[1]._CmdBuffer);
			DrawList->IdxBuffer.swap(Splitter._Channels[1]._IdxBuffer);
			Splitter.Merge(DrawList);
		}
		if (Node && Node->TabBar && !Node->IsHiddenTabBar())
		{
			const ImGuiTabBar& Bar = *Node->TabBar;
			for (const ImGuiTabItem& Tab : Bar.Tabs)
			{
				if (Tab.ID != Bar.VisibleTabId)
					continue;
				const float Left = std::max(Bar.ScrollingRectMinX, Bar.BarRect.Min.x + Tab.Offset - Bar.ScrollingAnim);
				const float Right = std::min(Bar.ScrollingRectMaxX, Bar.BarRect.Min.x + Tab.Offset - Bar.ScrollingAnim + Tab.Width);
				DrawList->PushClipRect(Bar.BarRect.Min, Bar.BarRect.Max, false);
				if (Right > Left)
					DrawList->AddLine({Left, Bar.BarRect.Max.y - 1.0f}, {Right, Bar.BarRect.Max.y - 1.0f}, ToImGuiPackedColor(ToolUITheme::NeutralAccent), Window->Viewport->DpiScale);
				DrawList->PopClipRect();
			}
		}
	}
}

struct FToolUIViewportData
{
	FToolUIContext::FImplementation* Owner = nullptr;
	ImGuiViewport* Viewport = nullptr;
	FWindow* Window = nullptr;
	std::uint64_t RendererHandle = 0;
	FToolUIViewportPosition CachedPosition;
	double CursorX = 0.0;
	double CursorY = 0.0;
	int IgnoreMoveEventFrame = -1;
	int IgnoreResizeEventFrame = -1;
	bool bCursorInside = false;
	bool bCursorPositionValid = false;
	bool bFrameReady = false;
};

[[nodiscard]] FToolUIContext::FImplementation* GetToolUIImplementation() noexcept
{
	return static_cast<FToolUIContext::FImplementation*>(ImGui::GetIO().UserData);
}

[[nodiscard]] FToolUIViewportData* GetViewportData(ImGuiViewport* const Viewport) noexcept
{
	if (Viewport == nullptr || Viewport == ImGui::GetMainViewport())
	{
		return nullptr;
	}
	return static_cast<FToolUIViewportData*>(Viewport->PlatformUserData);
}

[[nodiscard]] FWindow* GetViewportWindow(ImGuiViewport* const Viewport) noexcept
{
	if (Viewport == ImGui::GetMainViewport())
	{
		return GetToolUIImplementation()->Window;
	}
	const FToolUIViewportData* const Data = GetViewportData(Viewport);
	return Data != nullptr ? Data->Window : nullptr;
}

void UpdateTitleBarUiCaptureRegions(FWindow& Window, const ImGuiViewport& Viewport, const ImGuiWindow* const IgnoredWindow, const float MainMenuRight = 0.0f)
{
	FTitleBarHitTestState HitTestState = Window.GetTitleBarHitTestState();
	HitTestState.UiCaptureRegionCount = 0;
	HitTestState.bUiCapturesEntireTitleBar = false;

	const FTitleBarLayout& Layout = HitTestState.Layout;
	const ImRect TitleBarRect{
	    Viewport.Pos,
	    {Viewport.Pos.x + static_cast<float>(Layout.WindowWidth), Viewport.Pos.y + static_cast<float>(Layout.TitleBarHeight)}};
	const ImGuiContext& Context = *ImGui::GetCurrentContext();
	// A detached viewport owner spans Herta's native chrome internally, but the chrome must retain native dragging.
	const ImGuiWindow* const ViewportOwnerWindow = static_cast<const ImGuiViewportP&>(Viewport).Window;
	for (const ImGuiWindow* const UiWindow : Context.Windows)
	{
		if (UiWindow == IgnoredWindow || UiWindow == ViewportOwnerWindow || !UiWindow->Active || UiWindow->Hidden || UiWindow->Viewport != &Viewport || (UiWindow->Flags & ImGuiWindowFlags_NoMouseInputs) != 0)
		{
			continue;
		}

		const ImRect Intersection{
		    {std::max(UiWindow->OuterRectClipped.Min.x, TitleBarRect.Min.x), std::max(UiWindow->OuterRectClipped.Min.y, TitleBarRect.Min.y)},
		    {std::min(UiWindow->OuterRectClipped.Max.x, TitleBarRect.Max.x), std::min(UiWindow->OuterRectClipped.Max.y, TitleBarRect.Max.y)}};
		if (Intersection.Min.x >= Intersection.Max.x || Intersection.Min.y >= Intersection.Max.y)
		{
			continue;
		}

		if (HitTestState.UiCaptureRegionCount >= HitTestState.UiCaptureRegions.size())
		{
			// Losing caption dragging is safer than sending a UI click to native window chrome.
			HitTestState.UiCaptureRegionCount = 0;
			HitTestState.bUiCapturesEntireTitleBar = true;
			break;
		}

		HitTestState.UiCaptureRegions[HitTestState.UiCaptureRegionCount++] = {
		    .MinimumX = std::clamp(static_cast<int>(std::floor(Intersection.Min.x - Viewport.Pos.x)), 0, Layout.WindowWidth),
		    .MinimumY = std::clamp(static_cast<int>(std::floor(Intersection.Min.y - Viewport.Pos.y)), 0, Layout.TitleBarHeight),
		    .MaximumX = std::clamp(static_cast<int>(std::ceil(Intersection.Max.x - Viewport.Pos.x)), 0, Layout.WindowWidth),
		    .MaximumY = std::clamp(static_cast<int>(std::ceil(Intersection.Max.y - Viewport.Pos.y)), 0, Layout.TitleBarHeight)};
	}

	if (MainMenuRight > Viewport.Pos.x && HitTestState.UiCaptureRegionCount < HitTestState.UiCaptureRegions.size())
	{
		HitTestState.UiCaptureRegions[HitTestState.UiCaptureRegionCount++] = {
		    .MinimumX = Layout.TitleBarHeight,
		    .MinimumY = 0,
		    .MaximumX = std::clamp(static_cast<int>(std::ceil(MainMenuRight - Viewport.Pos.x)), 0, Layout.WindowWidth),
		    .MaximumY = Layout.TitleBarHeight};
	}
	Window.SetTitleBarHitTestState(HitTestState);
}

void RecordViewportError(FToolUIContext::FImplementation& Implementation, FToolUIError Error) noexcept
{
	if (!Implementation.PendingError)
	{
		Implementation.PendingError = std::move(Error);
	}
}

[[nodiscard]] ImGuiKey ToImGuiKey(const EKey Key) noexcept
{
	switch (Key)
	{
		case EKey::Tab:
			return ImGuiKey_Tab;
		case EKey::Left:
			return ImGuiKey_LeftArrow;
		case EKey::Right:
			return ImGuiKey_RightArrow;
		case EKey::Up:
			return ImGuiKey_UpArrow;
		case EKey::Down:
			return ImGuiKey_DownArrow;
		case EKey::PageUp:
			return ImGuiKey_PageUp;
		case EKey::PageDown:
			return ImGuiKey_PageDown;
		case EKey::Home:
			return ImGuiKey_Home;
		case EKey::End:
			return ImGuiKey_End;
		case EKey::Insert:
			return ImGuiKey_Insert;
		case EKey::Delete:
			return ImGuiKey_Delete;
		case EKey::Backspace:
			return ImGuiKey_Backspace;
		case EKey::Space:
			return ImGuiKey_Space;
		case EKey::Enter:
			return ImGuiKey_Enter;
		case EKey::Escape:
			return ImGuiKey_Escape;
		case EKey::Apostrophe:
			return ImGuiKey_Apostrophe;
		case EKey::Comma:
			return ImGuiKey_Comma;
		case EKey::Minus:
			return ImGuiKey_Minus;
		case EKey::Period:
			return ImGuiKey_Period;
		case EKey::Slash:
			return ImGuiKey_Slash;
		case EKey::Semicolon:
			return ImGuiKey_Semicolon;
		case EKey::Equal:
			return ImGuiKey_Equal;
		case EKey::LeftBracket:
			return ImGuiKey_LeftBracket;
		case EKey::Backslash:
			return ImGuiKey_Backslash;
		case EKey::World1:
		case EKey::World2:
			return ImGuiKey_Oem102;
		case EKey::RightBracket:
			return ImGuiKey_RightBracket;
		case EKey::GraveAccent:
			return ImGuiKey_GraveAccent;
		case EKey::CapsLock:
			return ImGuiKey_CapsLock;
		case EKey::ScrollLock:
			return ImGuiKey_ScrollLock;
		case EKey::NumLock:
			return ImGuiKey_NumLock;
		case EKey::PrintScreen:
			return ImGuiKey_PrintScreen;
		case EKey::Pause:
			return ImGuiKey_Pause;
		case EKey::KeypadZero:
			return ImGuiKey_Keypad0;
		case EKey::KeypadOne:
			return ImGuiKey_Keypad1;
		case EKey::KeypadTwo:
			return ImGuiKey_Keypad2;
		case EKey::KeypadThree:
			return ImGuiKey_Keypad3;
		case EKey::KeypadFour:
			return ImGuiKey_Keypad4;
		case EKey::KeypadFive:
			return ImGuiKey_Keypad5;
		case EKey::KeypadSix:
			return ImGuiKey_Keypad6;
		case EKey::KeypadSeven:
			return ImGuiKey_Keypad7;
		case EKey::KeypadEight:
			return ImGuiKey_Keypad8;
		case EKey::KeypadNine:
			return ImGuiKey_Keypad9;
		case EKey::KeypadDecimal:
			return ImGuiKey_KeypadDecimal;
		case EKey::KeypadDivide:
			return ImGuiKey_KeypadDivide;
		case EKey::KeypadMultiply:
			return ImGuiKey_KeypadMultiply;
		case EKey::KeypadSubtract:
			return ImGuiKey_KeypadSubtract;
		case EKey::KeypadAdd:
			return ImGuiKey_KeypadAdd;
		case EKey::KeypadEnter:
			return ImGuiKey_KeypadEnter;
		case EKey::KeypadEqual:
			return ImGuiKey_KeypadEqual;
		case EKey::LeftShift:
			return ImGuiKey_LeftShift;
		case EKey::LeftControl:
			return ImGuiKey_LeftCtrl;
		case EKey::LeftAlt:
			return ImGuiKey_LeftAlt;
		case EKey::LeftSuper:
			return ImGuiKey_LeftSuper;
		case EKey::RightShift:
			return ImGuiKey_RightShift;
		case EKey::RightControl:
			return ImGuiKey_RightCtrl;
		case EKey::RightAlt:
			return ImGuiKey_RightAlt;
		case EKey::RightSuper:
			return ImGuiKey_RightSuper;
		case EKey::Menu:
			return ImGuiKey_Menu;
		case EKey::Zero:
			return ImGuiKey_0;
		case EKey::One:
			return ImGuiKey_1;
		case EKey::Two:
			return ImGuiKey_2;
		case EKey::Three:
			return ImGuiKey_3;
		case EKey::Four:
			return ImGuiKey_4;
		case EKey::Five:
			return ImGuiKey_5;
		case EKey::Six:
			return ImGuiKey_6;
		case EKey::Seven:
			return ImGuiKey_7;
		case EKey::Eight:
			return ImGuiKey_8;
		case EKey::Nine:
			return ImGuiKey_9;
		case EKey::A:
			return ImGuiKey_A;
		case EKey::B:
			return ImGuiKey_B;
		case EKey::C:
			return ImGuiKey_C;
		case EKey::D:
			return ImGuiKey_D;
		case EKey::E:
			return ImGuiKey_E;
		case EKey::F:
			return ImGuiKey_F;
		case EKey::G:
			return ImGuiKey_G;
		case EKey::H:
			return ImGuiKey_H;
		case EKey::I:
			return ImGuiKey_I;
		case EKey::J:
			return ImGuiKey_J;
		case EKey::K:
			return ImGuiKey_K;
		case EKey::L:
			return ImGuiKey_L;
		case EKey::M:
			return ImGuiKey_M;
		case EKey::N:
			return ImGuiKey_N;
		case EKey::O:
			return ImGuiKey_O;
		case EKey::P:
			return ImGuiKey_P;
		case EKey::Q:
			return ImGuiKey_Q;
		case EKey::R:
			return ImGuiKey_R;
		case EKey::S:
			return ImGuiKey_S;
		case EKey::T:
			return ImGuiKey_T;
		case EKey::U:
			return ImGuiKey_U;
		case EKey::V:
			return ImGuiKey_V;
		case EKey::W:
			return ImGuiKey_W;
		case EKey::X:
			return ImGuiKey_X;
		case EKey::Y:
			return ImGuiKey_Y;
		case EKey::Z:
			return ImGuiKey_Z;
		case EKey::F1:
			return ImGuiKey_F1;
		case EKey::F2:
			return ImGuiKey_F2;
		case EKey::F3:
			return ImGuiKey_F3;
		case EKey::F4:
			return ImGuiKey_F4;
		case EKey::F5:
			return ImGuiKey_F5;
		case EKey::F6:
			return ImGuiKey_F6;
		case EKey::F7:
			return ImGuiKey_F7;
		case EKey::F8:
			return ImGuiKey_F8;
		case EKey::F9:
			return ImGuiKey_F9;
		case EKey::F10:
			return ImGuiKey_F10;
		case EKey::F11:
			return ImGuiKey_F11;
		case EKey::F12:
			return ImGuiKey_F12;
		case EKey::F13:
			return ImGuiKey_F13;
		case EKey::F14:
			return ImGuiKey_F14;
		case EKey::F15:
			return ImGuiKey_F15;
		case EKey::F16:
			return ImGuiKey_F16;
		case EKey::F17:
			return ImGuiKey_F17;
		case EKey::F18:
			return ImGuiKey_F18;
		case EKey::F19:
			return ImGuiKey_F19;
		case EKey::F20:
			return ImGuiKey_F20;
		case EKey::F21:
			return ImGuiKey_F21;
		case EKey::F22:
			return ImGuiKey_F22;
		case EKey::F23:
			return ImGuiKey_F23;
		case EKey::F24:
			return ImGuiKey_F24;
		case EKey::F25:
		case EKey::Unknown:
			return ImGuiKey_None;
	}
	return ImGuiKey_None;
}

void AddModifierEvents(ImGuiIO& Input, const EKey Key, const EInputAction Action, const EModifierFlags Modifiers) noexcept
{
	const bool bDown = Action != EInputAction::Released;
	const bool bControl = Key == EKey::LeftControl || Key == EKey::RightControl ? bDown : HasModifier(Modifiers, EModifierFlags::Control);
	const bool bShift = Key == EKey::LeftShift || Key == EKey::RightShift ? bDown : HasModifier(Modifiers, EModifierFlags::Shift);
	const bool bAlt = Key == EKey::LeftAlt || Key == EKey::RightAlt ? bDown : HasModifier(Modifiers, EModifierFlags::Alt);
	const bool bSuper = Key == EKey::LeftSuper || Key == EKey::RightSuper ? bDown : HasModifier(Modifiers, EModifierFlags::Super);
	Input.AddKeyEvent(ImGuiMod_Ctrl, bControl);
	Input.AddKeyEvent(ImGuiMod_Shift, bShift);
	Input.AddKeyEvent(ImGuiMod_Alt, bAlt);
	Input.AddKeyEvent(ImGuiMod_Super, bSuper);
}

[[nodiscard]] int ToImGuiMouseButton(const EMouseButton Button) noexcept
{
	switch (Button)
	{
		case EMouseButton::Left:
			return ImGuiMouseButton_Left;
		case EMouseButton::Right:
			return ImGuiMouseButton_Right;
		case EMouseButton::Middle:
			return ImGuiMouseButton_Middle;
		case EMouseButton::Four:
			return 3;
		case EMouseButton::Five:
			return 4;
		case EMouseButton::Six:
		case EMouseButton::Seven:
		case EMouseButton::Eight:
			return -1;
	}
	return -1;
}

[[nodiscard]] ImVec2 ResolveMousePosition(const FToolUIContext::FImplementation& Owner, const ImGuiViewport& Viewport, const FToolUIViewportData* const Data, const FWindow& Window, const double X, const double Y)
{
	if (Data == nullptr)
	{
		return {Viewport.Pos.x + static_cast<float>(X), Viewport.Pos.y + static_cast<float>(Y)};
	}

	const FWindowPosition PlatformPosition = Window.GetPosition();
	const FToolUIViewportPosition Position = ResolveToolUIViewportPosition(Owner.bProgrammaticWindowPosition, Data->CachedPosition, {static_cast<float>(PlatformPosition.X), static_cast<float>(PlatformPosition.Y)});
	return {Position.X + static_cast<float>(X), Position.Y + static_cast<float>(Y)};
}

void SetViewportCallbacks(FToolUIContext::FImplementation& Owner, ImGuiViewport& Viewport, FWindow& Window, FToolUIViewportData* const Data, std::function<void()> RefreshRequested = {})
{
	FWindowCallbacks Callbacks;
	Callbacks.CloseRequested = [&Owner, &Viewport](FWindow& EventWindow)
	{
		ImGui::SetCurrentContext(Owner.Context);
		if (&Viewport == ImGui::GetMainViewport())
		{
			EventWindow.RequestClose();
		}
		else
		{
			Viewport.PlatformRequestClose = true;
		}
	};
	Callbacks.Moved = [&Owner, &Viewport, Data](FWindow&, const int X, const int Y)
	{
		ImGui::SetCurrentContext(Owner.Context);
		if (Data != nullptr)
		{
			Data->CachedPosition = {static_cast<float>(X), static_cast<float>(Y)};
			if (ImGui::GetFrameCount() <= Data->IgnoreMoveEventFrame + 1)
			{
				return;
			}
		}
		Viewport.PlatformRequestMove = true;
	};
	Callbacks.Resized = [&Owner, &Viewport, Data](FWindow&, const int, const int)
	{
		ImGui::SetCurrentContext(Owner.Context);
		if (Data != nullptr && ImGui::GetFrameCount() <= Data->IgnoreResizeEventFrame + 1)
		{
			return;
		}
		Viewport.PlatformRequestResize = true;
	};
	Callbacks.FocusChanged = [&Owner](FWindow&, const bool bFocused)
	{
		ImGui::SetCurrentContext(Owner.Context);
		ImGui::GetIO().AddFocusEvent(bFocused);
	};
	Callbacks.RefreshRequested = [&Owner, RefreshRequested = std::move(RefreshRequested)](FWindow&)
	{
		if (RefreshRequested)
		{
			RefreshRequested();
		}
		else
		{
			Owner.Application->PostEmptyEvent();
		}
	};
	Callbacks.KeyChanged = [&Owner](FWindow&, const EKey Key, const EInputAction Action, const EModifierFlags Modifiers)
	{
		ImGui::SetCurrentContext(Owner.Context);
		ImGuiIO& Input = ImGui::GetIO();
		AddModifierEvents(Input, Key, Action, Modifiers);
		if (Action == EInputAction::Repeated)
		{
			return;
		}
		const ImGuiKey ImGuiKeyValue = ToImGuiKey(Key);
		if (ImGuiKeyValue != ImGuiKey_None)
		{
			Input.AddKeyEvent(ImGuiKeyValue, Action == EInputAction::Pressed);
		}
	};
	Callbacks.TextInput = [&Owner](FWindow&, const char32_t Character)
	{
		ImGui::SetCurrentContext(Owner.Context);
		ImGui::GetIO().AddInputCharacter(static_cast<unsigned int>(Character));
	};
	Callbacks.MouseButtonChanged = [&Owner, &Viewport, Data](FWindow& EventWindow, const EMouseButton Button, const EInputAction Action, const EModifierFlags)
	{
		ImGui::SetCurrentContext(Owner.Context);
		if (Action == EInputAction::Repeated)
		{
			return;
		}
		const int ImGuiButton = ToImGuiMouseButton(Button);
		if (ImGuiButton >= 0)
		{
			ImGuiIO& Input = ImGui::GetIO();
			const bool bCursorPositionValid = Data != nullptr ? Data->bCursorPositionValid : Owner.bMainCursorPositionValid;
			if (bCursorPositionValid)
			{
				const double CursorX = Data != nullptr ? Data->CursorX : Owner.MainCursorX;
				const double CursorY = Data != nullptr ? Data->CursorY : Owner.MainCursorY;
				const ImVec2 MousePosition = ResolveMousePosition(Owner, Viewport, Data, EventWindow, CursorX, CursorY);
				// Keep the press paired with the native cursor sample that produced it. ImGui may otherwise consume a stale position when events arrive between frames.
				Input.AddMousePosEvent(MousePosition.x, MousePosition.y);
			}
			Input.AddMouseButtonEvent(ImGuiButton, Action == EInputAction::Pressed);
		}
	};
	Callbacks.CursorMoved = [&Owner, &Viewport, Data](FWindow& EventWindow, const double X, const double Y)
	{
		ImGui::SetCurrentContext(Owner.Context);
		if (Data != nullptr)
		{
			Data->CursorX = X;
			Data->CursorY = Y;
			Data->bCursorPositionValid = true;
		}
		else
		{
			Owner.MainCursorX = X;
			Owner.MainCursorY = Y;
			Owner.bMainCursorPositionValid = true;
		}
		const ImVec2 MousePosition = ResolveMousePosition(Owner, Viewport, Data, EventWindow, X, Y);
		ImGui::GetIO().AddMousePosEvent(MousePosition.x, MousePosition.y);
	};
	Callbacks.CursorEntered = [&Owner, &Viewport, Data](FWindow&, const bool bEntered)
	{
		ImGui::SetCurrentContext(Owner.Context);
		if (Data != nullptr)
		{
			Data->bCursorInside = bEntered;
			if (!bEntered)
			{
				Data->bCursorPositionValid = false;
			}
		}
		else if (!bEntered)
		{
			Owner.bMainCursorPositionValid = false;
		}
		ImGuiIO& Input = ImGui::GetIO();
		Input.AddMouseViewportEvent(bEntered ? Viewport.ID : 0);
		if (!bEntered)
		{
			Input.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
		}
	};
	Callbacks.Scrolled = [&Owner](FWindow&, const double X, const double Y)
	{
		ImGui::SetCurrentContext(Owner.Context);
		ImGui::GetIO().AddMouseWheelEvent(static_cast<float>(X), static_cast<float>(Y));
	};
	Window.SetCallbacks(std::move(Callbacks));
}

void PlatformCreateWindow(ImGuiViewport* const Viewport)
{
	FToolUIContext::FImplementation& Owner = *GetToolUIImplementation();
	FWindow* CreatedWindow = nullptr;
	try
	{
		auto Data = std::make_unique<FToolUIViewportData>();
		FWindowDescriptor Descriptor;
		Descriptor.Title = "Herta Panel";
		Descriptor.Width = std::max(1, static_cast<int>(std::lround(Viewport->Size.x)));
		Descriptor.Height = std::max(1, static_cast<int>(std::lround(Viewport->Size.y)));
		Descriptor.bVisible = false;
		Descriptor.bResizable = true;
		Descriptor.bCustomTitleBar = true;
		const FToolUIViewportWindowPolicy WindowPolicy = ResolveToolUIViewportWindowPolicy(
		    (Viewport->Flags & ImGuiViewportFlags_NoTaskBarIcon) != 0,
		    (Viewport->Flags & ImGuiViewportFlags_TopMost) != 0,
		    (Viewport->Flags & ImGuiViewportFlags_NoFocusOnAppearing) != 0);
		Descriptor.bShowInTaskbar = WindowPolicy.bShowInTaskbar;
		Descriptor.bTopMost = WindowPolicy.bTopMost;
		Descriptor.bFocusOnShow = WindowPolicy.bFocusOnShow;
		std::expected<FWindow*, FApplicationError> WindowResult = Owner.Application->CreateWindow(std::move(Descriptor));
		if (!WindowResult)
		{
			RecordViewportError(Owner, FToolUIError{std::move(WindowResult.error().Message)});
			Viewport->PlatformRequestClose = true;
			return;
		}

		Data->Owner = &Owner;
		Data->Viewport = Viewport;
		Data->Window = CreatedWindow = *WindowResult;
		Data->CachedPosition = {Viewport->Pos.x, Viewport->Pos.y};
		Viewport->PlatformUserData = Data.get();
		Viewport->PlatformHandle = Data->Window->GetBackendHandle().Value;
		SetViewportCallbacks(Owner, *Viewport, *Data->Window, Data.get());
		Data->IgnoreMoveEventFrame = ImGui::GetFrameCount();
		if (Owner.bProgrammaticWindowPosition)
		{
			std::expected<void, FApplicationError> PositionResult = Data->Window->SetPosition(static_cast<int>(std::lround(Viewport->Pos.x)), static_cast<int>(std::lround(Viewport->Pos.y)));
			if (!PositionResult)
			{
				RecordViewportError(Owner, FToolUIError{std::move(PositionResult.error().Message)});
			}
		}
		Viewport->PlatformUserData = Data.release();
	}
	catch (const std::exception& Exception)
	{
		if (CreatedWindow != nullptr)
		{
			CreatedWindow->SetCallbacks({});
			Owner.Application->DestroyWindow(*CreatedWindow);
		}
		Viewport->PlatformUserData = nullptr;
		Viewport->PlatformHandle = nullptr;
		RecordViewportError(Owner, FToolUIError{Exception.what()});
		Viewport->PlatformRequestClose = true;
	}
	catch (...)
	{
		if (CreatedWindow != nullptr)
		{
			CreatedWindow->SetCallbacks({});
			Owner.Application->DestroyWindow(*CreatedWindow);
		}
		Viewport->PlatformUserData = nullptr;
		Viewport->PlatformHandle = nullptr;
		RecordViewportError(Owner, FToolUIError{"Could not create a ToolUI platform window"});
		Viewport->PlatformRequestClose = true;
	}
}

void PlatformDestroyWindow(ImGuiViewport* const Viewport)
{
	FToolUIViewportData* const Data = GetViewportData(Viewport);
	if (Data == nullptr)
	{
		return;
	}
	Data->Window->SetCallbacks({});
	Data->Owner->Application->DestroyWindow(*Data->Window);
	delete Data;
	Viewport->PlatformUserData = nullptr;
	Viewport->PlatformHandle = nullptr;
	Viewport->PlatformHandleRaw = nullptr;
}

void PlatformShowWindow(ImGuiViewport* const Viewport)
{
	if (FWindow* const Window = GetViewportWindow(Viewport))
	{
		Window->Show();
	}
}

void PlatformSetWindowPosition(ImGuiViewport* const Viewport, const ImVec2 Position)
{
	FToolUIViewportData* const Data = GetViewportData(Viewport);
	if (Data == nullptr)
	{
		return;
	}
	Data->CachedPosition = {Position.x, Position.y};
	Data->IgnoreMoveEventFrame = ImGui::GetFrameCount();
	if (!Data->Owner->bProgrammaticWindowPosition)
	{
		return;
	}
	std::expected<void, FApplicationError> Result = Data->Window->SetPosition(static_cast<int>(std::lround(Position.x)), static_cast<int>(std::lround(Position.y)));
	if (!Result)
	{
		RecordViewportError(*Data->Owner, FToolUIError{std::move(Result.error().Message)});
	}
}

[[nodiscard]] ImVec2 PlatformGetWindowPosition(ImGuiViewport* const Viewport)
{
	FWindow* const Window = GetViewportWindow(Viewport);
	if (Window == nullptr)
	{
		return Viewport->Pos;
	}
	const FWindowPosition PlatformPosition = Window->GetPosition();
	const FToolUIViewportData* const Data = GetViewportData(Viewport);
	const FToolUIViewportPosition CachedPosition = Data != nullptr ? Data->CachedPosition : FToolUIViewportPosition{Viewport->Pos.x, Viewport->Pos.y};
	const FToolUIViewportPosition Position = ResolveToolUIViewportPosition(GetToolUIImplementation()->bProgrammaticWindowPosition, CachedPosition, {static_cast<float>(PlatformPosition.X), static_cast<float>(PlatformPosition.Y)});
	return {Position.X, Position.Y};
}

void PlatformSetWindowSize(ImGuiViewport* const Viewport, const ImVec2 Size)
{
	FToolUIViewportData* const Data = GetViewportData(Viewport);
	if (Data == nullptr)
	{
		return;
	}
	Data->IgnoreResizeEventFrame = ImGui::GetFrameCount();
	std::expected<void, FApplicationError> Result = Data->Window->SetSize(std::max(1, static_cast<int>(std::lround(Size.x))), std::max(1, static_cast<int>(std::lround(Size.y))));
	if (!Result)
	{
		RecordViewportError(*Data->Owner, FToolUIError{std::move(Result.error().Message)});
	}
}

[[nodiscard]] ImVec2 PlatformGetWindowSize(ImGuiViewport* const Viewport)
{
	FWindow* const Window = GetViewportWindow(Viewport);
	return Window != nullptr ? ImVec2{static_cast<float>(Window->GetWidth()), static_cast<float>(Window->GetHeight())} : Viewport->Size;
}

[[nodiscard]] ImVec2 PlatformGetWindowFramebufferScale(ImGuiViewport* const Viewport)
{
	FWindow* const Window = GetViewportWindow(Viewport);
	if (Window == nullptr || Window->GetWidth() <= 0 || Window->GetHeight() <= 0)
	{
		return {1.0f, 1.0f};
	}
	return {
	    static_cast<float>(Window->GetFramebufferWidth()) / static_cast<float>(Window->GetWidth()),
	    static_cast<float>(Window->GetFramebufferHeight()) / static_cast<float>(Window->GetHeight())};
}

void PlatformSetWindowFocus(ImGuiViewport* const Viewport)
{
	if (FWindow* const Window = GetViewportWindow(Viewport))
	{
		Window->Focus();
	}
}

[[nodiscard]] bool PlatformGetWindowFocus(ImGuiViewport* const Viewport)
{
	FWindow* const Window = GetViewportWindow(Viewport);
	return Window != nullptr && Window->IsFocused();
}

[[nodiscard]] bool PlatformGetWindowMinimized(ImGuiViewport* const Viewport)
{
	FWindow* const Window = GetViewportWindow(Viewport);
	return Window != nullptr && Window->IsMinimized();
}

void PlatformSetWindowTitle(ImGuiViewport* const Viewport, const char* const Title)
{
	if (FWindow* const Window = GetViewportWindow(Viewport))
	{
		Window->SetTitle(Title != nullptr ? Title : "Herta Panel");
	}
}

[[nodiscard]] float PlatformGetWindowDpiScale(ImGuiViewport* const Viewport)
{
	FToolUIContext::FImplementation& Owner = *GetToolUIImplementation();
	FWindow* const Window = GetViewportWindow(Viewport);
	return Window != nullptr && Owner.WindowSystem != EWindowSystem::Wayland ? std::max(0.01f, Window->GetContentScale()) : 1.0f;
}

[[nodiscard]] ImVec4 PlatformGetWindowWorkAreaInsets(ImGuiViewport* const Viewport)
{
	if (Viewport == ImGui::GetMainViewport())
	{
		return {};
	}
	FWindow* const Window = GetViewportWindow(Viewport);
	return Window != nullptr ? ImVec4{0.0f, static_cast<float>(Window->GetTitleBarHitTestState().Layout.TitleBarHeight), 0.0f, 0.0f} : ImVec4{};
}

void PlatformOnChangedViewport(ImGuiViewport* const Viewport)
{
	FToolUIContext::FImplementation& Owner = *GetToolUIImplementation();
	const float DpiScale = std::max(0.01f, Viewport->DpiScale);
	if (std::abs(Owner.CurrentStyleScale - DpiScale) < 0.001f)
	{
		return;
	}
	ImGuiStyle& Style = ImGui::GetStyle();
	Style = Owner.BaseStyle;
	Style.ScaleAllSizes(DpiScale);
	Style.FontScaleDpi = DpiScale;
	Owner.CurrentStyleScale = DpiScale;
}

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> GetFramebufferSize(const FWindow& Window) noexcept
{
	return {
	    static_cast<std::uint32_t>(std::max(0, Window.GetFramebufferWidth())),
	    static_cast<std::uint32_t>(std::max(0, Window.GetFramebufferHeight()))};
}

void ResizeViewportRenderer(FToolUIViewportData& Data)
{
	if (Data.RendererHandle == 0 || !Data.Owner->Renderer.ResizeViewport)
	{
		return;
	}
	const auto [Width, Height] = GetFramebufferSize(*Data.Window);
	if (Width == 0 || Height == 0)
	{
		return;
	}
	std::expected<void, FToolUIError> Result = Data.Owner->Renderer.ResizeViewport(Data.RendererHandle, Width, Height);
	if (!Result)
	{
		RecordViewportError(*Data.Owner, std::move(Result.error()));
	}
}

void RendererCreateWindow(ImGuiViewport* const Viewport)
{
	FToolUIViewportData* const Data = GetViewportData(Viewport);
	if (Data == nullptr || !Data->Owner->Renderer.CreateViewport)
	{
		return;
	}
	const auto [Width, Height] = GetFramebufferSize(*Data->Window);
	std::expected<std::uint64_t, FToolUIError> Result = Data->Owner->Renderer.CreateViewport(Data->Window->GetBackendHandle().Value, Width, Height);
	if (!Result)
	{
		RecordViewportError(*Data->Owner, std::move(Result.error()));
		return;
	}
	Data->RendererHandle = *Result;
	Viewport->RendererUserData = Data;
}

void RendererDestroyWindow(ImGuiViewport* const Viewport)
{
	FToolUIViewportData* const Data = GetViewportData(Viewport);
	if (Data != nullptr && Data->RendererHandle != 0 && Data->Owner->Renderer.DestroyViewport)
	{
		std::expected<void, FToolUIError> Result = Data->Owner->Renderer.DestroyViewport(Data->RendererHandle);
		if (!Result && !Data->Owner->bDestroying)
		{
			RecordViewportError(*Data->Owner, std::move(Result.error()));
		}
		Data->RendererHandle = 0;
	}
	Viewport->RendererUserData = nullptr;
}

void RendererSetWindowSize(ImGuiViewport* const Viewport, const ImVec2)
{
	if (FToolUIViewportData* const Data = GetViewportData(Viewport))
	{
		ResizeViewportRenderer(*Data);
	}
}

void RendererRenderWindow(ImGuiViewport* const Viewport, void*)
{
	FToolUIViewportData* const Data = GetViewportData(Viewport);
	if (Data == nullptr || Data->RendererHandle == 0 || !Data->Owner->Renderer.BeginViewportFrame || !Data->Owner->Renderer.RenderViewport)
	{
		return;
	}
	Data->bFrameReady = false;
	std::expected<EToolUIViewportFrameStatus, FToolUIError> BeginResult = Data->Owner->Renderer.BeginViewportFrame(Data->RendererHandle);
	if (!BeginResult)
	{
		RecordViewportError(*Data->Owner, std::move(BeginResult.error()));
		return;
	}
	if (*BeginResult == EToolUIViewportFrameStatus::NeedsResize)
	{
		ResizeViewportRenderer(*Data);
		return;
	}
	if (*BeginResult == EToolUIViewportFrameStatus::Skipped)
	{
		return;
	}
	Data->bFrameReady = true;
	std::expected<void, FToolUIError> RenderResult = Data->Owner->Renderer.RenderViewport(Data->RendererHandle, Viewport->DrawData);
	if (!RenderResult)
	{
		RecordViewportError(*Data->Owner, std::move(RenderResult.error()));
		return;
	}
}

void RendererSwapBuffers(ImGuiViewport* const Viewport, void*)
{
	FToolUIViewportData* const Data = GetViewportData(Viewport);
	if (Data == nullptr || !Data->bFrameReady || !Data->Owner->Renderer.PresentViewport)
	{
		return;
	}
	Data->bFrameReady = false;
	std::expected<EToolUIViewportFrameStatus, FToolUIError> PresentResult = Data->Owner->Renderer.PresentViewport(Data->RendererHandle);
	if (!PresentResult)
	{
		RecordViewportError(*Data->Owner, std::move(PresentResult.error()));
		return;
	}
	if (*PresentResult == EToolUIViewportFrameStatus::NeedsResize)
	{
		ResizeViewportRenderer(*Data);
	}
}

void DrawDetachedViewportChrome(FToolUIContext::FImplementation& Owner)
{
	ImGuiPlatformIO& Platform = ImGui::GetPlatformIO();
	for (int ViewportIndex = 1; ViewportIndex < Platform.Viewports.Size; ++ViewportIndex)
	{
		ImGuiViewport* const Viewport = Platform.Viewports[ViewportIndex];
		FToolUIViewportData* const Data = GetViewportData(Viewport);
		if (Data == nullptr || Data->Window == nullptr || (Viewport->Flags & ImGuiViewportFlags_IsMinimized) != 0)
		{
			continue;
		}

		const FTitleBarLayout& Layout = Data->Window->GetTitleBarHitTestState().Layout;
		const float Scale = static_cast<float>(Layout.TitleBarHeight) / static_cast<float>(DefaultTitleBarHeight);
		const ImVec2 Minimum = Viewport->Pos;
		const ImVec2 ViewportMaximum{Viewport->Pos.x + Viewport->Size.x, Viewport->Pos.y + Viewport->Size.y};
		const ImVec2 TitleBarMaximum{Viewport->Pos.x + static_cast<float>(Layout.WindowWidth), Viewport->Pos.y + static_cast<float>(Layout.TitleBarHeight)};
		const FToolUIGradient Gradient = ResolveToolUIGradient(Owner.Appearance, Data->Window->IsFocused());
		const float GradientBottom = Minimum.y + Viewport->Size.y * ClampToolUIUnit(Owner.Appearance.GradientHeight);
		ImDrawList* const Background = ImGui::GetBackgroundDrawList(Viewport);
		Background->AddRectFilled(Minimum, ViewportMaximum, ToImGuiPackedColor(ToolUITheme::Canvas));
		Background->AddRectFilledMultiColor(Minimum, {ViewportMaximum.x, GradientBottom}, ToImGuiPackedColor(Gradient.TopLeft), ToImGuiPackedColor(Gradient.TopRight), ToImGuiPackedColor(Gradient.BottomRight), ToImGuiPackedColor(Gradient.BottomLeft));
		ImDrawList* const DrawList = ImGui::GetForegroundDrawList(Viewport);
		DrawList->AddRectFilled(Minimum, TitleBarMaximum, ToImGuiPackedColor(ToolUITheme::TitleBar));
		DrawList->AddLine({Minimum.x, TitleBarMaximum.y}, {TitleBarMaximum.x, TitleBarMaximum.y}, ToImGuiPackedColor(ToolUITheme::BorderSoft));
		DrawList->AddCircle({Minimum.x + 18.0f * Scale, Minimum.y + 18.0f * Scale}, 7.0f * Scale, ToImGuiPackedColor(ToolUITheme::NeutralAccent), 24, 2.0f * Scale);
		DrawList->AddCircleFilled({Minimum.x + 18.0f * Scale, Minimum.y + 18.0f * Scale}, 2.0f * Scale, ToImGuiPackedColor(ToolUITheme::NeutralAccent));
		DrawWindowControls(*DrawList, Minimum, Layout);
		const std::string_view Title = Data->Window->GetTitle();
		DrawList->AddText(Owner.MediumFont, Owner.Metrics.BaseFontSize * std::max(1.0f, Viewport->DpiScale), {Minimum.x + 50.0f * Scale, Minimum.y + 10.0f * Scale}, ToImGuiPackedColor(ToolUITheme::TextPrimary), Title.data(), Title.data() + Title.size());
		UpdateTitleBarUiCaptureRegions(*Data->Window, *Viewport, nullptr);
	}
}

void DestroyHertaPlatformWindows(FToolUIContext::FImplementation& Owner) noexcept
{
	ImGuiPlatformIO& Platform = ImGui::GetPlatformIO();
	for (int ViewportIndex = Platform.Viewports.Size - 1; ViewportIndex >= 1; --ViewportIndex)
	{
		ImGui::DestroyPlatformWindow(static_cast<ImGuiViewportP*>(Platform.Viewports[ViewportIndex]));
	}
	Platform.Platform_DestroyWindow = Owner.BackendPlatformDestroyWindow;
	Platform.Renderer_DestroyWindow = nullptr;
}

void InstallHertaPlatformInterface(FToolUIContext::FImplementation& Owner)
{
	ImGuiIO& Input = ImGui::GetIO();
	Input.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports | ImGuiBackendFlags_HasMouseHoveredViewport;
	ImGuiPlatformIO& Platform = ImGui::GetPlatformIO();
	Owner.BackendPlatformDestroyWindow = Platform.Platform_DestroyWindow;
	Platform.Platform_CreateWindow = PlatformCreateWindow;
	Platform.Platform_DestroyWindow = PlatformDestroyWindow;
	Platform.Platform_ShowWindow = PlatformShowWindow;
	Platform.Platform_SetWindowPos = PlatformSetWindowPosition;
	Platform.Platform_GetWindowPos = PlatformGetWindowPosition;
	Platform.Platform_SetWindowSize = PlatformSetWindowSize;
	Platform.Platform_GetWindowSize = PlatformGetWindowSize;
	Platform.Platform_GetWindowFramebufferScale = PlatformGetWindowFramebufferScale;
	Platform.Platform_SetWindowFocus = PlatformSetWindowFocus;
	Platform.Platform_GetWindowFocus = PlatformGetWindowFocus;
	Platform.Platform_GetWindowMinimized = PlatformGetWindowMinimized;
	Platform.Platform_SetWindowTitle = PlatformSetWindowTitle;
	Platform.Platform_GetWindowDpiScale = PlatformGetWindowDpiScale;
	Platform.Platform_GetWindowWorkAreaInsets = PlatformGetWindowWorkAreaInsets;
	Platform.Platform_OnChangedViewport = PlatformOnChangedViewport;
	Platform.Platform_RenderWindow = nullptr;
	Platform.Platform_SwapBuffers = nullptr;
	Platform.Platform_SetWindowAlpha = nullptr;
	Platform.Renderer_CreateWindow = RendererCreateWindow;
	Platform.Renderer_DestroyWindow = RendererDestroyWindow;
	Platform.Renderer_SetWindowSize = RendererSetWindowSize;
	Platform.Renderer_RenderWindow = RendererRenderWindow;
	Platform.Renderer_SwapBuffers = RendererSwapBuffers;
}
}

std::expected<std::unique_ptr<FToolUIContext>, FToolUIError> FToolUIContext::Create(FToolUIDescriptor Descriptor)
{
	if (Descriptor.Application == nullptr || Descriptor.Window == nullptr || Descriptor.Window->GetBackendHandle().Value == nullptr)
	{
		return std::unexpected(FToolUIError{"ToolUI requires a valid Application and native window"});
	}

	std::expected<std::vector<std::byte>, FToolUIError> RegularFontBytes = ReadFile(Descriptor.RegularFontPath);
	if (!RegularFontBytes)
	{
		return std::unexpected(std::move(RegularFontBytes.error()));
	}

	std::expected<std::vector<std::byte>, FToolUIError> MediumFontBytes = ReadFile(Descriptor.MediumFontPath);
	if (!MediumFontBytes)
	{
		return std::unexpected(std::move(MediumFontBytes.error()));
	}

	std::error_code DirectoryError;
	if (!Descriptor.LayoutPath.parent_path().empty())
	{
		std::filesystem::create_directories(Descriptor.LayoutPath.parent_path(), DirectoryError);
		if (DirectoryError)
		{
			return std::unexpected(FToolUIError{"Could not create the editor layout directory: " + DirectoryError.message()});
		}
	}
	if (!Descriptor.AppearancePath.parent_path().empty())
	{
		std::filesystem::create_directories(Descriptor.AppearancePath.parent_path(), DirectoryError);
		if (DirectoryError)
		{
			return std::unexpected(FToolUIError{"Could not create the editor appearance directory: " + DirectoryError.message()});
		}
	}

	std::unique_ptr<FImplementation> Implementation;
	const auto CleanupFailedInitialization = [&Implementation]() noexcept
	{
		if (!Implementation || Implementation->Context == nullptr)
		{
			return;
		}
		ImGui::SetCurrentContext(Implementation->Context);
		Implementation->bDestroying = true;
		if (Implementation->bHertaPlatformInterfaceInstalled)
		{
			DestroyHertaPlatformWindows(*Implementation);
		}
		if (Implementation->bCallbacksInstalled)
		{
			Implementation->Window->SetCallbacks({});
		}
		if (Implementation->bRendererInitialized && Implementation->Renderer.Shutdown)
		{
			Implementation->Renderer.Shutdown();
			ResetRendererTextureState();
		}
		if (Implementation->bGlfwInitialized)
		{
			ImGui_ImplGlfw_Shutdown();
		}
		ImGui::DestroyContext(Implementation->Context);
		Implementation->Context = nullptr;
	};

	try
	{
		Implementation = std::make_unique<FImplementation>();
		Implementation->Application = Descriptor.Application;
		Implementation->Window = Descriptor.Window;
		Implementation->BackendWindow = static_cast<GLFWwindow*>(Descriptor.Window->GetBackendHandle().Value);
		Implementation->WindowSystem = Descriptor.Application->GetCapabilities().WindowSystem;
		Implementation->bProgrammaticWindowPosition = Descriptor.Application->GetCapabilities().bProgrammaticWindowPosition;
		Implementation->Appearance = Descriptor.Appearance;
		if (const std::optional<FEditorAppearance> SavedAppearance = LoadAppearance(Descriptor.AppearancePath))
		{
			Implementation->Appearance = *SavedAppearance;
		}
		Implementation->Renderer = std::move(Descriptor.Renderer);
		Implementation->bVSync = Descriptor.bVSync;
		Implementation->VSyncChanged = std::move(Descriptor.VSyncChanged);
		Implementation->RegularFontBytes = std::move(*RegularFontBytes);
		Implementation->MediumFontBytes = std::move(*MediumFontBytes);
		Implementation->LayoutPath = PathToUtf8(Descriptor.LayoutPath);
		Implementation->AppearancePath = std::move(Descriptor.AppearancePath);
		Implementation->bBuildDefaultLayout = !std::filesystem::exists(Descriptor.LayoutPath);

		IMGUI_CHECKVERSION();
		Implementation->Context = ImGui::CreateContext();
		if (Implementation->Context == nullptr)
		{
			return std::unexpected(FToolUIError{"Could not create the Dear ImGui context"});
		}

		ImGui::SetCurrentContext(Implementation->Context);
		ImGuiIO& Input = ImGui::GetIO();
		Input.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
		const bool bMultiViewportRendererAvailable = Implementation->Renderer.CreateViewport && Implementation->Renderer.DestroyViewport && Implementation->Renderer.ResizeViewport && Implementation->Renderer.BeginViewportFrame && Implementation->Renderer.RenderViewport && Implementation->Renderer.PresentViewport;
		if (bMultiViewportRendererAvailable)
		{
			Input.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
		}
		Input.ConfigDpiScaleFonts = true;
		Input.ConfigDpiScaleViewports = true;
		if (Implementation->Renderer.Initialize)
		{
			Input.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
		}
		Input.UserData = Implementation.get();
		Input.IniFilename = Implementation->LayoutPath.c_str();
		Input.ConfigWindowsMoveFromTitleBarOnly = true;
		Input.Fonts->SetFontLoader(ImGuiFreeType::GetFontLoader());

		ImFontConfig FontConfiguration;
		FontConfiguration.FontDataOwnedByAtlas = false;
		Implementation->RegularFont = Input.Fonts->AddFontFromMemoryTTF(Implementation->RegularFontBytes.data(), static_cast<int>(Implementation->RegularFontBytes.size()), 0.0f, &FontConfiguration);
		ImFontConfig MediumConfiguration = FontConfiguration;
		Implementation->MediumFont = Input.Fonts->AddFontFromMemoryTTF(Implementation->MediumFontBytes.data(), static_cast<int>(Implementation->MediumFontBytes.size()), 0.0f, &MediumConfiguration);
		if (Implementation->RegularFont == nullptr || Implementation->MediumFont == nullptr)
		{
			CleanupFailedInitialization();
			return std::unexpected(FToolUIError{"Could not register the editor fonts"});
		}
		Input.FontDefault = Implementation->RegularFont;

		ApplyBaseStyle(ImGui::GetStyle(), Implementation->Metrics);
		ApplyInteractiveColors(ImGui::GetStyle(), Implementation->Appearance.Accent);
		ApplyAppearanceStyle(ImGui::GetStyle(), Implementation->Appearance);
		Implementation->BaseStyle = ImGui::GetStyle();
		if (!ImGui_ImplGlfw_InitForVulkan(Implementation->BackendWindow, false))
		{
			CleanupFailedInitialization();
			return std::unexpected(FToolUIError{"Could not initialize the Dear ImGui GLFW platform adapter"});
		}
		Implementation->bGlfwInitialized = true;
		if (bMultiViewportRendererAvailable)
		{
			InstallHertaPlatformInterface(*Implementation);
			Implementation->bHertaPlatformInterfaceInstalled = true;
		}
		SetViewportCallbacks(*Implementation, *ImGui::GetMainViewport(), *Descriptor.Window, nullptr, std::move(Descriptor.RefreshRequested));
		Implementation->bCallbacksInstalled = true;

		if (Implementation->Renderer.Initialize)
		{
			Implementation->bRendererInitialized = true;
			std::expected<void, FToolUIError> InitializeResult = Implementation->Renderer.Initialize();
			if (!InitializeResult)
			{
				CleanupFailedInitialization();
				return std::unexpected(std::move(InitializeResult.error()));
			}
		}

		return std::unique_ptr<FToolUIContext>(new FToolUIContext(std::move(Implementation)));
	}
	catch (const std::exception& Exception)
	{
		CleanupFailedInitialization();
		return std::unexpected(FToolUIError{Exception.what()});
	}
	catch (...)
	{
		CleanupFailedInitialization();
		return std::unexpected(FToolUIError{"ToolUI initialization failed due to an unknown error"});
	}
}

FToolUIContext::FToolUIContext(std::unique_ptr<FImplementation> Implementation) noexcept
    : Implementation(std::move(Implementation))
{
}

FToolUIContext::~FToolUIContext()
{
	if (!Implementation)
	{
		return;
	}

	ImGui::SetCurrentContext(Implementation->Context);
	if (Implementation->bAppearanceDirty)
	{
		SaveAppearance(Implementation->AppearancePath, Implementation->Appearance);
	}
	Implementation->bDestroying = true;
	if (Implementation->bHertaPlatformInterfaceInstalled)
	{
		DestroyHertaPlatformWindows(*Implementation);
	}
	Implementation->Window->SetCallbacks({});
	if (Implementation->bRendererInitialized && Implementation->Renderer.Shutdown)
	{
		Implementation->Renderer.Shutdown();
		ResetRendererTextureState();
	}
	if (Implementation->bGlfwInitialized)
	{
		ImGui_ImplGlfw_Shutdown();
	}
	ImGui::DestroyContext(Implementation->Context);
}

void FToolUIContext::BeginFrame()
{
	ImGui::SetCurrentContext(Implementation->Context);
	IM_ASSERT(Implementation->bPlatformWindowsRendered && "RenderPlatformWindows must run after the main presentation is complete");
	ImGuiStyle& Style = ImGui::GetStyle();
	Style = Implementation->BaseStyle;
	const float MainDpiScale = PlatformGetWindowDpiScale(ImGui::GetMainViewport());
	Style.ScaleAllSizes(MainDpiScale);
	Style.FontScaleDpi = MainDpiScale;
	Implementation->CurrentStyleScale = MainDpiScale;
	ImGui_ImplGlfw_NewFrame();
	if (!Implementation->bProgrammaticWindowPosition)
	{
		ImGuiPlatformIO& Platform = ImGui::GetPlatformIO();
		for (int ViewportIndex = 1; ViewportIndex < Platform.Viewports.Size; ++ViewportIndex)
		{
			FToolUIViewportData* const Data = GetViewportData(Platform.Viewports[ViewportIndex]);
			if (Data != nullptr && Data->bCursorInside)
			{
				ImGui::GetIO().AddMousePosEvent(Data->CachedPosition.X + static_cast<float>(Data->CursorX), Data->CachedPosition.Y + static_cast<float>(Data->CursorY));
				ImGui::GetIO().AddMouseViewportEvent(Data->Viewport->ID);
			}
		}
	}
	ImGui::NewFrame();
	Implementation->PresentedPanels.clear();
	for (ImGuiWindow* const Window : Implementation->Context->Windows)
	{
		if (Window->DockNode && Window->DockNode->TabBar)
		{
			const ImGuiTabBar& Bar = *Window->DockNode->TabBar;
			const bool bSelected = (Bar.NextSelectedTabId != 0 ? Bar.NextSelectedTabId : Bar.VisibleTabId) == Window->TabId;
			Window->DockStyle.Colors[ImGuiWindowDockStyleCol_Text] = ToImGuiPackedColor(bSelected ? ToolUITheme::TextPrimary : ToolUITheme::TextMuted);
		}
	}
	Implementation->bFrameActive = true;
}

std::expected<void, FToolUIError> FToolUIContext::EndFrame(const bool bRenderMainViewport)
{
	if (!Implementation->bFrameActive)
	{
		return std::unexpected(FToolUIError{"ToolUI frame was not started"});
	}

	DrawWindowSurfaces(*Implementation);
	DrawDetachedViewportChrome(*Implementation);
	UpdateTitleBarUiCaptureRegions(*Implementation->Window, *ImGui::GetMainViewport(), ImGui::FindWindowByName("HertaWorkspaceHost"), Implementation->MainMenuRight);
	ImGui::Render();
	Implementation->bFrameActive = false;
	Implementation->bPlatformWindowsRendered = (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) == 0;
	if (bRenderMainViewport && Implementation->Renderer.Render)
	{
		return Implementation->Renderer.Render(ImGui::GetDrawData());
	}
	return {};
}

std::expected<void, FToolUIError> FToolUIContext::RenderPlatformWindows()
{
	ImGui::SetCurrentContext(Implementation->Context);
	if (Implementation->bFrameActive)
	{
		return std::unexpected(FToolUIError{"ToolUI platform windows cannot render before EndFrame"});
	}
	if (Implementation->bPlatformWindowsRendered)
	{
		return {};
	}

	ImGui::UpdatePlatformWindows();
	if (!Implementation->PendingError)
	{
		ImGuiPlatformIO& Platform = ImGui::GetPlatformIO();
		for (int ViewportIndex = 1; ViewportIndex < Platform.Viewports.Size; ++ViewportIndex)
		{
			ImGuiViewport* const Viewport = Platform.Viewports[ViewportIndex];
			if ((Viewport->Flags & ImGuiViewportFlags_IsMinimized) != 0)
			{
				continue;
			}
			if (Platform.Renderer_RenderWindow)
			{
				Platform.Renderer_RenderWindow(Viewport, nullptr);
			}
			if (Platform.Renderer_SwapBuffers)
			{
				Platform.Renderer_SwapBuffers(Viewport, nullptr);
			}
			if (Implementation->PendingError)
			{
				break;
			}
		}
	}
	Implementation->bPlatformWindowsRendered = true;
	if (std::optional<FToolUIError> Error = std::exchange(Implementation->PendingError, std::nullopt); Error.has_value())
	{
		return std::unexpected(std::move(Error).value());
	}
	return {};
}

bool ToolUIMenuItem(const std::string_view Label, const EToolUIMenuIcon Icon, bool* const bSelected)
{
	const std::string Display = std::format("##{}", Label);
	const bool bPressed = ImGui::MenuItem(Display.c_str(), nullptr, bSelected);
	const float Scale = ImGui::GetFontSize() / 15.0f;
	const ImVec2 Min = ImGui::GetItemRectMin();
	const ImVec2 Max = ImGui::GetItemRectMax();
	const ImVec2 Center{Min.x + 16.0f * Scale, (Min.y + Max.y) * 0.5f};
	ImDrawList* const Draw = ImGui::GetWindowDrawList();
	Draw->AddText({Min.x + 36.0f * Scale, Center.y - ImGui::GetFontSize() * 0.5f}, ImGui::GetColorU32(ImGuiCol_Text), Label.data(), Label.data() + Label.size());
	const ImU32 Color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
	const auto Line = [&](const float X, const float Y, const float EndX, const float EndY)
	{
		Draw->AddLine({Center.x + X * Scale, Center.y + Y * Scale}, {Center.x + EndX * Scale, Center.y + EndY * Scale}, Color, Scale);
	};
	if (Icon == EToolUIMenuIcon::Panel || Icon == EToolUIMenuIcon::Layout)
	{
		Draw->AddRect({Center.x - 6.0f * Scale, Center.y - 5.0f * Scale}, {Center.x + 6.0f * Scale, Center.y + 5.0f * Scale}, Color, Scale);
		Line(-6, -2, 6, -2);
		if (Icon == EToolUIMenuIcon::Layout)
			Line(2, -2, 2, 5);
	}
	else if (Icon == EToolUIMenuIcon::Log)
	{
		Line(-5, -4, -1, 0);
		Line(-1, 0, -5, 4);
		Line(1, 4, 6, 4);
	}
	else if (Icon == EToolUIMenuIcon::Sync)
	{
		Draw->AddCircle(Center, 5.5f * Scale, Color, 18, Scale);
		Line(0, -3, 0, 0);
		Line(0, 0, 3, 0);
	}
	else
	{
		Line(-5, -5, -5, 5);
		Line(-5, -5, 0, -5);
		Line(-5, 5, 0, 5);
		Line(-1, 0, 6, 0);
		Line(3, -3, 6, 0);
		Line(3, 3, 6, 0);
	}
	return bPressed;
}

void FToolUIContext::DrawWorkspace(const std::string_view ApplicationTitle, const std::function<void()>& DrawMenuItems, const std::function<void()>& DrawStatusItems)
{
	const ImGuiViewport* const Viewport = ImGui::GetMainViewport();
	const ImVec2 ViewportMinimum = Viewport->Pos;
	const ImVec2 ViewportMaximum{Viewport->Pos.x + Viewport->Size.x, Viewport->Pos.y + Viewport->Size.y};
	const FToolUIGradient Gradient = ResolveToolUIGradient(Implementation->Appearance, Implementation->Window->IsFocused());
	const float GradientBottom = ViewportMinimum.y + Viewport->Size.y * ClampToolUIUnit(Implementation->Appearance.GradientHeight);
	ImDrawList* const Background = ImGui::GetBackgroundDrawList();
	Background->AddRectFilled(ViewportMinimum, ViewportMaximum, ToImGuiPackedColor(ToolUITheme::Canvas));
	Background->AddRectFilledMultiColor(
	    ViewportMinimum,
	    {ViewportMaximum.x, GradientBottom},
	    ToImGuiPackedColor(Gradient.TopLeft),
	    ToImGuiPackedColor(Gradient.TopRight),
	    ToImGuiPackedColor(Gradient.BottomRight),
	    ToImGuiPackedColor(Gradient.BottomLeft));

	constexpr ImGuiWindowFlags HostFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_MenuBar;
	ImGui::SetNextWindowPos(ViewportMinimum);
	ImGui::SetNextWindowSize(Viewport->Size);
	ImGui::SetNextWindowViewport(Viewport->ID);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
	ImGui::Begin("HertaWorkspaceHost", nullptr, HostFlags);
	ImGui::PopStyleVar();

	ImDrawList* const DrawList = ImGui::GetWindowDrawList();
	const FTitleBarLayout& TitleBarLayout = Implementation->Window->GetTitleBarHitTestState().Layout;
	DrawList->PushClipRect(ViewportMinimum, ViewportMaximum, false);
	const float TitleBarHeight = static_cast<float>(TitleBarLayout.TitleBarHeight);
	const float ChromeScale = TitleBarHeight / static_cast<float>(DefaultTitleBarHeight);
	const float TitleBarBottom = ViewportMinimum.y + TitleBarHeight;
	const float StatusBarHeight = Implementation->Metrics.StatusBarHeight * ChromeScale;
	DrawList->AddRectFilled(ViewportMinimum, {ViewportMaximum.x, TitleBarBottom}, ToImGuiPackedColor(ToolUITheme::TitleBar));

	const ImVec2 SystemMinimum = ViewportMinimum;
	const ImVec2 SystemMaximum{ViewportMinimum.x + TitleBarHeight, TitleBarBottom};
	const ImVec2 MousePosition = ImGui::GetIO().MousePos;
	const auto IsHovered = [MousePosition](const ImVec2 Minimum, const ImVec2 Maximum)
	{
		return MousePosition.x >= Minimum.x && MousePosition.x < Maximum.x && MousePosition.y >= Minimum.y && MousePosition.y < Maximum.y;
	};
	if (TitleBarLayout.bSystemMenuEnabled && IsHovered(SystemMinimum, SystemMaximum))
	{
		DrawList->AddRectFilled(SystemMinimum, SystemMaximum, IM_COL32(255, 255, 255, 24), Implementation->Metrics.TitleBarControlRounding * ChromeScale);
	}

	DrawList->AddCircle({SystemMinimum.x + 18.0f * ChromeScale, SystemMinimum.y + 18.0f * ChromeScale}, 7.0f * ChromeScale, ToImGuiPackedColor(ToolUITheme::NeutralAccent), 24, 2.0f * ChromeScale);
	DrawList->AddCircleFilled({SystemMinimum.x + 18.0f * ChromeScale, SystemMinimum.y + 18.0f * ChromeScale}, 2.0f * ChromeScale, ToImGuiPackedColor(ToolUITheme::NeutralAccent));
	DrawWindowControls(*DrawList, ViewportMinimum, TitleBarLayout);

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.0f * ChromeScale, 7.0f * ChromeScale});
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.0f * ChromeScale, 6.0f * ChromeScale});
	if (ImGui::BeginMenuBar())
	{
		const float MenuTop = ViewportMinimum.y + (TitleBarHeight - ImGui::GetFontSize()) * 0.5f - ImGui::GetStyle().FramePadding.y;
		ImGui::SetCursorScreenPos({SystemMaximum.x + 4.0f * ChromeScale, MenuTop});
		if (ImGui::BeginMenu("File"))
		{
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.0f * ChromeScale, 10.0f * ChromeScale});
			ImGui::Dummy({220.0f * ChromeScale, 0.0f});
			ImGui::TextDisabled("Workspace");
			if (DrawMenuItems)
			{
				DrawMenuItems();
			}
			ImGui::Separator();
			bool bVSync = Implementation->bVSync;
			if (ToolUIMenuItem("VSync", EToolUIMenuIcon::Sync, &bVSync))
			{
				Implementation->bVSync = !Implementation->bVSync;
				if (Implementation->VSyncChanged)
				{
					Implementation->VSyncChanged(Implementation->bVSync);
				}
			}
			ImGui::Separator();
			if (ToolUIMenuItem("Reset layout", EToolUIMenuIcon::Layout))
			{
				Implementation->bBuildDefaultLayout = true;
			}
			ImGui::Separator();
			if (ToolUIMenuItem("Exit", EToolUIMenuIcon::Exit))
			{
				Implementation->Window->RequestClose();
			}
			ImGui::PopStyleVar();
			ImGui::EndMenu();
		}
		for (const char* const Label : {"Edit", "Window", "Tools", "Help"})
		{
			if (ImGui::BeginMenu(Label, false))
				ImGui::EndMenu();
		}
		Implementation->MainMenuRight = ImGui::GetItemRectMax().x + 8.0f * ChromeScale;
		ImGui::EndMenuBar();
	}
	ImGui::PopStyleVar(2);

	const std::string Version = std::format("Herta {}", GetBuildRevision());
	const float RightEdge = [&]
	{
		float Edge = ViewportMinimum.x + static_cast<float>(TitleBarLayout.WindowWidth) - 12.0f * ChromeScale;
		for (const ETitleBarHitRegion Region : {ETitleBarHitRegion::MinimizeButton, ETitleBarHitRegion::MaximizeButton, ETitleBarHitRegion::CloseButton})
		{
			const FTitleBarControlBounds Bounds = GetTitleBarControlBounds(TitleBarLayout, Region);
			if (Bounds.bVisible)
			{
				Edge = std::min(Edge, ViewportMinimum.x + static_cast<float>(Bounds.MinimumX) - 12.0f * ChromeScale);
			}
		}
		return Edge;
	}();
	float TextRight = RightEdge;
	const auto DrawRightText = [&](const std::string_view Value, const ImU32 Color)
	{
		const float Width = ImGui::CalcTextSize(Value.data(), Value.data() + Value.size()).x;
		if (TextRight - Width > Implementation->MainMenuRight + 20.0f * ChromeScale)
		{
			DrawList->AddText({TextRight - Width, ViewportMinimum.y + 10.0f * ChromeScale}, Color, Value.data(), Value.data() + Value.size());
			TextRight -= Width + 24.0f * ChromeScale;
		}
	};
	DrawRightText(Version, ToImGuiPackedColor(ToolUITheme::TextMuted));
	DrawRightText(ApplicationTitle, ToImGuiPackedColor(ToolUITheme::TextSecondary));
	const float Fps = ImGui::GetIO().Framerate;
	if (Fps > 0.0f)
	{
		const std::string Values = std::format("{:.1f} / {:.1f} ms", Fps, 1000.0f / Fps);
		const float LabelWidth = ImGui::CalcTextSize("FPS: ").x;
		const float Width = LabelWidth + ImGui::CalcTextSize(Values.c_str()).x;
		if (TextRight - Width > Implementation->MainMenuRight + 20.0f * ChromeScale)
		{
			const ImVec2 Position{TextRight - Width, ViewportMinimum.y + 10.0f * ChromeScale};
			DrawList->AddText(Position, ToImGuiPackedColor(ToolUITheme::TextMuted), "FPS: ");
			DrawList->AddText({Position.x + LabelWidth, Position.y}, ToImGuiPackedColor(ToolUITheme::TextPrimary), Values.c_str());
		}
	}

	Implementation->WorkspaceCanvas = ResolveToolUIWorkspaceCanvas({ViewportMinimum.x, ViewportMinimum.y, Viewport->Size.x, Viewport->Size.y}, TitleBarHeight, StatusBarHeight);
	ImGui::SetCursorScreenPos({Implementation->WorkspaceCanvas.X, Implementation->WorkspaceCanvas.Y});
	const ImVec2 DockSize{std::max(1.0f, Implementation->WorkspaceCanvas.Width), std::max(1.0f, Implementation->WorkspaceCanvas.Height)};
	// Changing the ID migrates pre-Viewport layouts once while preserving user layouts from this version onward.
	Implementation->DockspaceId = ImGui::GetID("HertaEditorDockspaceV3");
	const bool bDockspaceMissing = ImGui::DockBuilderGetNode(Implementation->DockspaceId) == nullptr;
	ImVec4 DockspaceBackground = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
	// Panel surfaces supply their own opacity; the dock host must not cover the scene behind them.
	DockspaceBackground.w = 0.0f;
	ImGui::PushStyleColor(ImGuiCol_WindowBg, DockspaceBackground);
	ImGui::DockSpace(Implementation->DockspaceId, DockSize, ImGuiDockNodeFlags_PassthruCentralNode);
	ImGui::PopStyleColor();
	if (Implementation->bBuildDefaultLayout || bDockspaceMissing)
	{
		ImGui::DockBuilderRemoveNode(Implementation->DockspaceId);
		ImGui::DockBuilderAddNode(Implementation->DockspaceId, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(Implementation->DockspaceId, DockSize);
		ImGuiID CenterId = Implementation->DockspaceId;
		const ImGuiID BottomId = ImGui::DockBuilderSplitNode(CenterId, ImGuiDir_Down, 0.18f, nullptr, &CenterId);
		const float DetailsFraction = std::clamp(350.0f * ChromeScale / DockSize.x, 0.18f, 0.38f);
		const ImGuiID SideId = ImGui::DockBuilderSplitNode(CenterId, ImGuiDir_Right, DetailsFraction, nullptr, &CenterId);
		if (ImGuiDockNode* const CenterNode = ImGui::DockBuilderGetNode(CenterId))
		{
			CenterNode->LocalFlags |= ImGuiDockNodeFlags_AutoHideTabBar;
		}
		ImGui::DockBuilderDockWindow("Details", SideId);
		ImGui::DockBuilderDockWindow("Start", SideId);
		ImGui::DockBuilderDockWindow("Viewport", CenterId);
		ImGui::DockBuilderDockWindow("Output Log", BottomId);
		ImGui::DockBuilderFinish(Implementation->DockspaceId);
		Implementation->bBuildDefaultLayout = false;
	}
	if (!Implementation->bDetailsDockMigrationComplete)
	{
		Implementation->bDetailsDockMigrationComplete = true;
		if (ImGui::FindWindowSettingsByID(ImHashStr("Details")) == nullptr)
		{
			const ImGuiWindowSettings* const StartSettings = ImGui::FindWindowSettingsByID(ImHashStr("Start"));
			if (StartSettings != nullptr && StartSettings->DockId != 0)
			{
				const ImGuiDockNode* const StartNode = ImGui::DockBuilderGetNode(StartSettings->DockId);
				if (StartNode != nullptr && StartNode->IsLeafNode())
				{
					ImGui::DockBuilderDockWindow("Details", StartSettings->DockId);
				}
			}
		}
	}

	const ImVec2 StatusMinimum{ViewportMinimum.x, ViewportMaximum.y - StatusBarHeight};
	Herta::DrawGlassSurface(*DrawList, *Viewport, Implementation->Appearance, StatusMinimum, ViewportMaximum, 0.0f);
	DrawList->AddLine(StatusMinimum, {ViewportMaximum.x, StatusMinimum.y}, ToImGuiPackedColor(ToolUITheme::BorderSoft));
	ImGui::SetCursorScreenPos(StatusMinimum);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10.0f * ChromeScale, 3.0f * ChromeScale});
	if (ImGui::BeginChild("HertaStatusBar", {Viewport->Size.x, StatusBarHeight}, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{
		if (DrawStatusItems)
		{
			DrawStatusItems();
		}
	}
	ImGui::EndChild();
	ImGui::PopStyleVar();

	DrawList->PopClipRect();
	ImGui::End();
}

bool FToolUIContext::BeginPanel(const std::string_view Name, bool* const bOpen, const bool bViewport)
{
	const ImGuiID WindowId = ImHashStr(Name.data(), Name.size());
	const auto Previous = Implementation->PreviousDockState.find(WindowId);
	const bool bPreviouslyDocked = Previous != Implementation->PreviousDockState.end() && Previous->second;
	ImGui::SetNextWindowBgAlpha(ResolveToolUIPanelBackgroundAlpha(Implementation->Appearance.PanelTransparency, bPreviouslyDocked, bViewport));
	if (bViewport)
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
	const bool bVisible = ImGui::Begin(std::string(Name).c_str(), bOpen);
	if (bViewport)
		ImGui::PopStyleVar();
	Implementation->PreviousDockState[WindowId] = ImGui::IsWindowDocked();
	if (ImGuiDockNode* const Node = ImGui::GetCurrentWindow()->DockNode)
		Node->LocalFlags |= ImGuiDockNodeFlags_NoWindowMenuButton;
	if (bVisible)
	{
		Implementation->PresentedPanels[WindowId] = bViewport;
	}
	if (bViewport)
	{
		if (ImGuiDockNode* const Node = ImGui::GetCurrentWindow()->DockNode)
		{
			Node->LocalFlags |= ImGuiDockNodeFlags_AutoHideTabBar;
		}
	}
	return bVisible;
}

void FToolUIContext::EndPanel()
{
	ImGui::End();
}

std::optional<FToolUICanvasBounds> FToolUIContext::GetWorkspaceCanvasForCurrentPanel() const noexcept
{
	const ImGuiWindow* const Window = ImGui::GetCurrentWindow();
	if (Window->Viewport != ImGui::GetMainViewport() || Window->DockNode == nullptr ||
	    ImGui::DockNodeGetRootNode(Window->DockNode)->ID != Implementation->DockspaceId)
		return std::nullopt;
	return Implementation->WorkspaceCanvas;
}

void FToolUIContext::DrawGlassSurface(const float X, const float Y, const float Width, const float Height, const float Radius) const
{
	if (Width <= 0.0f || Height <= 0.0f)
	{
		return;
	}
	Herta::DrawGlassSurface(*ImGui::GetWindowDrawList(), *ImGui::GetWindowViewport(), Implementation->Appearance, {X, Y}, {X + Width, Y + Height}, Radius);
}

void FToolUIContext::SetAppearance(const FEditorAppearance Appearance) noexcept
{
	if (Implementation->Appearance != Appearance)
	{
		Implementation->Appearance = Appearance;
		ApplyInteractiveColors(Implementation->BaseStyle, Implementation->Appearance.Accent);
		ApplyAppearanceStyle(Implementation->BaseStyle, Implementation->Appearance);
		Implementation->bAppearanceDirty = true;
	}
}

const FEditorAppearance& FToolUIContext::GetAppearance() const noexcept
{
	return Implementation->Appearance;
}

const FToolUIThemeMetrics& FToolUIContext::GetMetrics() const noexcept
{
	return Implementation->Metrics;
}

bool FToolUIContext::WantsTextInput() const noexcept
{
	return ImGui::GetIO().WantTextInput;
}

bool FToolUIContext::WantsMouseInput() const noexcept
{
	return ImGui::GetIO().WantCaptureMouse;
}
}
