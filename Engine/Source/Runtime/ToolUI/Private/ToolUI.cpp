#include "Herta/ToolUI/ToolUI.h"

#include "Herta/Application/Application.h"
#include "Herta/Core/Build.h"
#include "Herta/Localization/Localization.h"
#include "Herta/TextLayout/TextLayout.h"
#include "Herta/ToolUI/EditorText.h"
#include "ImmersiveViewport.h"
#include "ToolUIAutomation.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/freetype/imgui_freetype.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <deque>
#include <format>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Herta
{
namespace
{
namespace Editor = Text::Editor;

[[nodiscard]] std::string PathToUtf8(const std::filesystem::path& Path)
{
	const std::u8string Text = Path.generic_u8string();
	return {reinterpret_cast<const char*>(Text.data()), Text.size()};
}

[[nodiscard]] ImVec4 ToImGuiColor(const FToolUIColor Color) noexcept
{
	constexpr float InverseByte = 1.f / 255.f;
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
		    4.f * Scale);
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
		DrawList.AddLine({CenterX - 5.f * Scale, CenterY}, {CenterX + 5.f * Scale, CenterY}, GlyphColor, Scale);
	}

	if (Maximize.bVisible)
	{
		const float CenterX = Origin.x + static_cast<float>(Maximize.MinimumX + Maximize.MaximumX) * 0.5f;
		if (Layout.bMaximized)
		{
			DrawList.PathLineTo({CenterX - 3.f * Scale, CenterY - 3.f * Scale});
			DrawList.PathArcToFast({CenterX - 2.f * Scale, CenterY - 4.f * Scale}, Scale, 6, 9);
			DrawList.PathArcToFast({CenterX + 4.f * Scale, CenterY - 4.f * Scale}, Scale, 9, 12);
			DrawList.PathArcToFast({CenterX + 4.f * Scale, CenterY + 2.f * Scale}, Scale, 0, 3);
			DrawList.PathLineTo({CenterX + 3.f * Scale, CenterY + 3.f * Scale});
			DrawList.PathStroke(GlyphColor, 0, Scale);
			DrawList.AddRect({CenterX - 5.f * Scale, CenterY - 3.f * Scale}, {CenterX + 3.f * Scale, CenterY + 5.f * Scale}, GlyphColor, Scale, 0, Scale);
		}
		else
		{
			DrawList.AddRect({CenterX - 5.f * Scale, CenterY - 5.f * Scale}, {CenterX + 5.f * Scale, CenterY + 5.f * Scale}, GlyphColor, 0.f, 0, Scale);
		}
	}

	if (Close.bVisible)
	{
		const float CenterX = Origin.x + static_cast<float>(Close.MinimumX + Close.MaximumX) * 0.5f;
		DrawList.AddLine({CenterX - 5.f * Scale, CenterY - 5.f * Scale}, {CenterX + 5.f * Scale, CenterY + 5.f * Scale}, GlyphColor, Scale);
		DrawList.AddLine({CenterX + 5.f * Scale, CenterY - 5.f * Scale}, {CenterX - 5.f * Scale, CenterY + 5.f * Scale}, GlyphColor, Scale);
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

	std::vector<std::byte> Bytes(static_cast<std::size_t>(Size));
	Stream.seekg(0, std::ios::beg);
	if (!Stream.read(reinterpret_cast<char*>(Bytes.data()), Size))
	{
		return std::unexpected(FToolUIError{"Could not read editor resource: " + PathToUtf8(Path)});
	}

	return Bytes;
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
	Style.WindowTitleAlign = {0.f, 0.5f};
	Style.WindowRounding = Metrics.WindowRounding;
	Style.WindowBorderSize = 0.f;
	Style.ChildRounding = Metrics.ChildRounding;
	Style.ChildBorderSize = 0.f;
	Style.PopupRounding = Metrics.PopupRounding;
	Style.PopupBorderSize = 1.f;
	Style.FramePadding = {12.f, 8.f};
	Style.FrameRounding = Metrics.FrameRounding;
	Style.FrameBorderSize = 1.f;
	Style.ItemSpacing = {8.f, 8.f};
	Style.ItemInnerSpacing = {8.f, 6.f};
	Style.CellPadding = {10.f, 8.f};
	Style.IndentSpacing = 20.f;
	Style.ScrollbarSize = Metrics.ScrollbarSize;
	Style.ScrollbarRounding = Metrics.ScrollbarRounding;
	Style.GrabMinSize = 10.f;
	Style.GrabRounding = Metrics.FrameRounding;
	Style.ImageRounding = Metrics.FrameRounding;
	Style.TabRounding = Metrics.FrameRounding;
	Style.MenuItemRounding = Metrics.FrameRounding;
	Style.DragDropTargetRounding = Metrics.FrameRounding;
	Style.TabBorderSize = 0.f;
	Style.TabBarBorderSize = 0.f;
	Style.TabBarOverlineSize = 0.f;
	Style.TabCloseButtonMinWidthSelected = 0.f;
	Style.DockingSeparatorSize = 1.f;
	Style.SeparatorSize = 1.f;
	Style.DisabledAlpha = 0.55f;

	ImVec4* const Palette = Style.Colors;
	Palette[ImGuiCol_Text] = ToImGuiColor(ToolUITheme::TextPrimary);
	Palette[ImGuiCol_TextDisabled] = ToImGuiColor(ToolUITheme::TextMuted);
	Palette[ImGuiCol_WindowBg] = WithAlpha(ToolUITheme::Surface0, 0.94f);
	Palette[ImGuiCol_ChildBg] = WithAlpha(ToolUITheme::Surface1, 0.9f);
	Palette[ImGuiCol_PopupBg] = ToImGuiColor(ToolUITheme::Surface1);
	Palette[ImGuiCol_Border] = ToImGuiColor(ToolUITheme::Border);
	Palette[ImGuiCol_BorderShadow] = WithAlpha(ToolUITheme::Canvas, 0.f);
	Palette[ImGuiCol_FrameBg] = ToImGuiColor(ToolUITheme::Surface1);
	Palette[ImGuiCol_TitleBg] = ToImGuiColor(ToolUITheme::Surface0);
	Palette[ImGuiCol_TitleBgActive] = ToImGuiColor(ToolUITheme::Surface0);
	Palette[ImGuiCol_TitleBgCollapsed] = ToImGuiColor(ToolUITheme::Surface0);
	Palette[ImGuiCol_MenuBarBg] = ToImGuiColor(ToolUITheme::Surface0);
	Palette[ImGuiCol_ScrollbarBg] = ToImGuiColor(ToolUITheme::Surface0);
	Palette[ImGuiCol_ScrollbarGrab] = ToImGuiColor(ToolUITheme::Border);
	Palette[ImGuiCol_Button] = ToImGuiColor(ToolUITheme::Surface1);
	Palette[ImGuiCol_Separator] = ToImGuiColor(ToolUITheme::BorderSoft);
	Palette[ImGuiCol_ResizeGrip] = WithAlpha(ToolUITheme::Canvas, 0.f);
	Palette[ImGuiCol_Tab] = WithAlpha(ToolUITheme::Surface0, 0.f);
	Palette[ImGuiCol_TabSelected] = WithAlpha(ToolUITheme::Surface0, 0.f);
	Palette[ImGuiCol_TabDimmed] = WithAlpha(ToolUITheme::Surface0, 0.f);
	Palette[ImGuiCol_TabDimmedSelected] = WithAlpha(ToolUITheme::Surface0, 0.f);
	Palette[ImGuiCol_DockingPreview] = WithAlpha(ToolUITheme::TextPrimary, 0.15f);
	Palette[ImGuiCol_DockingEmptyBg] = WithAlpha(ToolUITheme::Canvas, 0.f);
	Palette[ImGuiCol_TableHeaderBg] = ToImGuiColor(ToolUITheme::Surface2);
	Palette[ImGuiCol_TableBorderStrong] = ToImGuiColor(ToolUITheme::Border);
	Palette[ImGuiCol_TableBorderLight] = ToImGuiColor(ToolUITheme::BorderSoft);
	Palette[ImGuiCol_TableRowBg] = WithAlpha(ToolUITheme::Canvas, 0.f);
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
	Palette[ImGuiCol_Header] = WithAlpha(Accent, ToolUITheme::SubtleTint);
	Palette[ImGuiCol_HeaderHovered] = WithAlpha(Accent, ToolUITheme::HoverTint);
	Palette[ImGuiCol_HeaderActive] = WithAlpha(Accent, ToolUITheme::ActiveTint);
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
	Palette[ImGuiCol_ChildBg] = WithAlpha(ToolUITheme::Surface0, 0.f);
	Palette[ImGuiCol_FrameBg] = WithAlpha(ToolUITheme::Surface1, Alpha * 0.5f);
	Palette[ImGuiCol_Button] = WithAlpha(ToolUITheme::Surface1, Alpha * 0.5f);
	Palette[ImGuiCol_PopupBg] = WithAlpha(ToolUITheme::Surface1, 0.f);
	Palette[ImGuiCol_TitleBg] = WithAlpha(ToolUITheme::Surface0, 0.f);
	Palette[ImGuiCol_TitleBgActive] = WithAlpha(ToolUITheme::Surface0, 0.f);
	Palette[ImGuiCol_TitleBgCollapsed] = WithAlpha(ToolUITheme::Surface0, 0.f);
	Palette[ImGuiCol_MenuBarBg] = WithAlpha(ToolUITheme::Surface0, 0.f);
	Palette[ImGuiCol_ScrollbarBg] = WithAlpha(ToolUITheme::Surface0, 0.f);
	Palette[ImGuiCol_TableHeaderBg] = WithAlpha(ToolUITheme::Surface2, Alpha);
	Palette[ImGuiCol_TableRowBgAlt] = WithAlpha(ToolUITheme::Surface2, Alpha * 0.5f);
	Palette[ImGuiCol_TabHovered] = WithAlpha(ToolUITheme::SurfaceHover, Alpha * 0.5f);
	Palette[ImGuiCol_Border] = WithAlpha(ToolUITheme::Border, Alpha);
	Palette[ImGuiCol_BorderShadow] = WithAlpha(ToolUITheme::Canvas, 0.f);
}

}

std::optional<FEditorAppearance> LoadEditorAppearance(const std::filesystem::path& Path)
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
	if (!(Stream >> Header >> Version) || Header != "HertaEditorAppearance" || (Version < 1 || Version > 5) || !(Stream >> AccentLabel >> Red >> Green >> Blue) || AccentLabel != "Accent" || !(Stream >> GradientLabel >> Appearance.GradientHeight >> Appearance.Saturation >> Appearance.Intensity) || GradientLabel != "Gradient" || !(Stream >> PanelLabel >> PanelMode) || PanelLabel != "Panel")
	{
		return std::nullopt;
	}

	if (Version >= 2)
	{
		std::string GlassLabel;
		if (!(Stream >> GlassLabel >> Appearance.PanelOpacity >> Appearance.BlurRadius) || GlassLabel != "Glass")
		{
			return std::nullopt;
		}
	}

	if (Version >= 4)
	{
		std::string DisplayLabel;
		int bHdrOutput = 0;
		if (!(Stream >> DisplayLabel >> bHdrOutput >> Appearance.Display.PaperWhite >> Appearance.Display.PeakLuminance) || DisplayLabel != "Display" || (bHdrOutput != 0 && bHdrOutput != 1))
		{
			return std::nullopt;
		}

		Appearance.Display.bHdrOutput = bHdrOutput == 1;
		const auto InRangeOrSystem = [](const float Value, const float Minimum, const float Maximum)
		{
			return Value == 0.f || (std::isfinite(Value) && Value >= Minimum && Value <= Maximum);
		};

		if (!InRangeOrSystem(Appearance.Display.PaperWhite, MinimumHdrPaperWhite, MaximumHdrPaperWhite) || !InRangeOrSystem(Appearance.Display.PeakLuminance, MinimumHdrPeakLuminance, MaximumHdrPeakLuminance))
		{
			return std::nullopt;
		}
	}

	if (Version >= 5)
	{
		std::string CultureLabel;
		if (!(Stream >> CultureLabel >> Appearance.Culture) || CultureLabel != "Culture" || Appearance.Culture.size() > 32 || !std::ranges::all_of(Appearance.Culture, [](const char Character)
		{
			return std::isalnum(static_cast<unsigned char>(Character)) || Character == '-';
		}))
		{
			return std::nullopt;
		}
	}

	// Version 2 stored a reduced-motion flag; the editor no longer animates, so it is read and ignored.
	if (Version == 2)
	{
		std::string MotionLabel;
		int ReducedMotion = 0;
		if (!(Stream >> MotionLabel >> ReducedMotion) || MotionLabel != "Motion" || (ReducedMotion != 0 && ReducedMotion != 1))
		{
			return std::nullopt;
		}
	}

	if (Red < 0 || Red > 255 || Green < 0 || Green > 255 || Blue < 0 || Blue > 255 || !std::isfinite(Appearance.GradientHeight) || Appearance.GradientHeight < 0.f || Appearance.GradientHeight > 1.f || !std::isfinite(Appearance.Saturation) || Appearance.Saturation < 0.f || Appearance.Saturation > 1.f || !std::isfinite(Appearance.Intensity) || Appearance.Intensity < 0.f || Appearance.Intensity > 1.f || !std::isfinite(Appearance.PanelOpacity) || Appearance.PanelOpacity < 0.f || Appearance.PanelOpacity > 1.f || !std::isfinite(Appearance.BlurRadius) || Appearance.BlurRadius < 0.f || Appearance.BlurRadius > 40.f || PanelMode < static_cast<int>(EPanelTransparency::AllPanels) || PanelMode > static_cast<int>(EPanelTransparency::Disabled))
	{
		return std::nullopt;
	}

	Appearance.Accent = {.Red = static_cast<std::uint8_t>(Red), .Green = static_cast<std::uint8_t>(Green), .Blue = static_cast<std::uint8_t>(Blue), .Alpha = 255};
	if (Version == 1 && Appearance.Accent == FToolUIColor{.Red = 84, .Green = 108, .Blue = 232, .Alpha = 255})
	{
		Appearance.Accent = FEditorAppearance{}.Accent;
	}

	Appearance.PanelTransparency = static_cast<EPanelTransparency>(PanelMode);
	return Appearance;
}

void SaveEditorAppearance(const std::filesystem::path& Path, const FEditorAppearance& Appearance) noexcept
{
	std::ofstream Stream(Path, std::ios::trunc);
	if (!Stream)
	{
		return;
	}

	Stream << "HertaEditorAppearance 5\n";
	Stream << "Accent " << static_cast<int>(Appearance.Accent.Red) << ' ' << static_cast<int>(Appearance.Accent.Green) << ' ' << static_cast<int>(Appearance.Accent.Blue) << '\n';
	Stream << "Gradient " << Appearance.GradientHeight << ' ' << Appearance.Saturation << ' ' << Appearance.Intensity << '\n';
	Stream << "Panel " << static_cast<int>(Appearance.PanelTransparency) << '\n';
	Stream << "Glass " << Appearance.PanelOpacity << ' ' << Appearance.BlurRadius << '\n';
	Stream << "Display " << (Appearance.Display.bHdrOutput ? 1 : 0) << ' ' << Appearance.Display.PaperWhite << ' ' << Appearance.Display.PeakLuminance << '\n';
	Stream << "Culture " << Appearance.Culture << '\n';
}

struct FToolUIDocument
{
	ImGuiID Id = 0;
	std::string Label;
	int LastFrame = 0;
};

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
	std::vector<std::byte> LogFontBytes;
	std::vector<std::byte> EmojiFontBytes;
	const FLocalization* Text = nullptr;
	std::string LayoutPath;
	std::filesystem::path AppearancePath;
	std::unordered_map<ImGuiID, bool> PreviousDockState;
	std::unordered_map<ImGuiID, bool> PresentedPanels;
	// Open asset editors in tab order. A document that skips a frame has closed.
	std::vector<FToolUIDocument> Documents;
	// The document in front of the level workspace, or 0 while the level is in front.
	ImGuiID ActiveDocument = 0;
	ImGuiID CloseRequestedDocument = 0;
	ImGuiID FocusRequestedDocument = 0;
	std::string LevelTabLabel = "Level";
	// Whether each open BeginPanel began an ImGui window, so EndPanel ends only those.
	std::vector<bool> PanelBegun;
	std::optional<FToolUIError> PendingError;
	ImGuiContext* Context = nullptr;
	ImFont* RegularFont = nullptr;
	ImFont* MediumFont = nullptr;
	ImFont* LogFont = nullptr;
	ImGuiStyle BaseStyle;
	void (*BackendPlatformDestroyWindow)(ImGuiViewport*) = nullptr;
	ImGuiID DockspaceId = 0;
	bool bViewportImmersive = false;
	bool bFocusViewportRequested = false;
	// A captured pointer's position is unbounded, and ImGui given it would find a click wherever it drifted, focusing another panel or none.
	// ImGui sees it at the viewport's centre instead, and its motion collects here.
	bool bCursorCaptured = false;
	ImVec2 CapturedMousePosition{};
	FToolUIItemRecorder ItemRecorder{};

	// Pointer steps applied one per frame, since ImGui only starts a drag after the button has been held across frames.
	struct FPointerStep
	{
		float X = 0.f;
		float Y = 0.f;
		// Pressed, released, or neither.
		int Button = -1;
	};

	std::deque<FPointerStep> PointerSteps{};
	FToolUIFrameStats FrameStats{};
	bool bRecordItems = false;
	std::optional<std::array<double, 2>> CapturedCursor{};
	FToolUIViewportPosition CapturedMotion{};
	FToolUIViewportPosition CapturedFrameMotion{};
	std::vector<std::filesystem::path> DroppedFiles;
	FToolUICanvasBounds WorkspaceCanvas;
	float CurrentStyleScale = 1.f;
	float MainMenuRight = 0.f;
	double MainCursorX = 0.0;
	double MainCursorY = 0.0;
	EWindowSystem WindowSystem = EWindowSystem::Unknown;
	bool bMainCursorPositionValid = false;
	bool bProgrammaticWindowPosition = false;
	bool bVSync = true;
	bool bBuildDefaultLayout = false;

	struct FFloatRequest
	{
		FToolUICanvasBounds Bounds;
		int Frames = 2;
	};

	std::unordered_map<std::string, FFloatRequest> FloatRequests;
	bool bOutlinerDockMigrationComplete = false;
	bool bContentDockMigrationComplete = false;
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
	const float BlurRadius = std::clamp(Appearance.BlurRadius, 0.f, 40.f);
	if (BlurRadius > 0.f && Viewport.Size.x > 0.f && Viewport.Size.y > 0.f)
	{
		// Reserved renderer IDs encode the shared backdrop radius in framebuffer pixels.
		const auto PixelRadius = static_cast<ImTextureID>(std::clamp(std::round(BlurRadius * Viewport.DpiScale), 1.f, 160.f));
		const ImTextureID BackdropId = std::numeric_limits<ImTextureID>::max() - PixelRadius;
		const ImVec2 UVMinimum{(Minimum.x - Viewport.Pos.x) / Viewport.Size.x, (Minimum.y - Viewport.Pos.y) / Viewport.Size.y};
		const ImVec2 UVMaximum{(Maximum.x - Viewport.Pos.x) / Viewport.Size.x, (Maximum.y - Viewport.Pos.y) / Viewport.Size.y};
		DrawList.AddImageRounded(ImTextureRef(BackdropId), Minimum, Maximum, UVMinimum, UVMaximum, IM_COL32_WHITE, Radius);
	}

	DrawList.AddRectFilled(Minimum, Maximum, ImGui::ColorConvertFloat4ToU32(WithAlpha(ToolUITheme::Surface0, ClampToolUIUnit(Appearance.PanelOpacity))), Radius);
	if (Radius > 0.f)
	{
		DrawList.AddRect(Minimum, Maximum, ImGui::ColorConvertFloat4ToU32(WithAlpha(ToolUITheme::NeutralAccent, ClampToolUIUnit(Appearance.PanelOpacity) * 0.28f)), Radius, 0, Viewport.DpiScale);
	}
}

// Icons share one style: filled shapes with rounded corners and round-capped strokes, in a 16-unit box centered on Center, in two tones, the
// color and the same color at 45 percent for depth.
class FIconPen
{
public:
	FIconPen(ImDrawList& InDraw, const ImVec2 InCenter, const float InScale, const ImU32 InColor);

	[[nodiscard]] ImVec2 At(float X, float Y) const;
	[[nodiscard]] ImU32 Tone(float Opacity) const;
	void Circle(float X, float Y, float Radius, float Opacity = 1.f) const;
	void Rect(float X0, float Y0, float X1, float Y1, float Rounding, float Opacity = 1.f) const;
	// A convex polygon; bConcave triangulates any simple polygon instead.
	void Polygon(std::initializer_list<ImVec2> Points, float Opacity = 1.f, bool bConcave = false) const;
	// A polyline with round caps and joins, Width in icon units.
	void Stroke(std::span<const ImVec2> Points, float Width, float Opacity = 1.f, bool bClosed = false) const;
	void Stroke(std::initializer_list<ImVec2> Points, float Width, float Opacity = 1.f, bool bClosed = false) const;
	// A polygon filled and outlined with a thin round stroke, which rounds its corners.
	void RoundedPolygon(std::initializer_list<ImVec2> Points, float Rounding, float Opacity = 1.f) const;
	void Ring(float X, float Y, float Radius, float Width, float Opacity = 1.f) const;
	void Frame(float X0, float Y0, float X1, float Y1, float Rounding, float Width, float Opacity = 1.f) const;
	[[nodiscard]] std::vector<ImVec2> Arc(float X, float Y, float Radius, float From, float To) const;

private:
	ImDrawList& Draw;
	ImVec2 Center;
	float Scale = 1.f;
	ImU32 Color = 0;
};

FIconPen::FIconPen(ImDrawList& InDraw, const ImVec2 InCenter, const float InScale, const ImU32 InColor)
    : Draw(InDraw)
    , Center(InCenter)
    , Scale(InScale)
    , Color(InColor)
{
}

ImVec2 FIconPen::At(const float X, const float Y) const
{
	return {Center.x + X * Scale, Center.y + Y * Scale};
}

ImU32 FIconPen::Tone(const float Opacity) const
{
	const auto Alpha = static_cast<float>((Color >> IM_COL32_A_SHIFT) & 0xFFu) * Opacity;
	return (Color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(std::lround(Alpha)) << IM_COL32_A_SHIFT);
}

void FIconPen::Circle(const float X, const float Y, const float Radius, const float Opacity) const
{
	Draw.AddCircleFilled(At(X, Y), Radius * Scale, Tone(Opacity));
}

void FIconPen::Rect(const float X0, const float Y0, const float X1, const float Y1, const float Rounding, const float Opacity) const
{
	Draw.AddRectFilled(At(X0, Y0), At(X1, Y1), Tone(Opacity), Rounding * Scale);
}

void FIconPen::Polygon(const std::initializer_list<ImVec2> Points, const float Opacity, const bool bConcave) const
{
	std::array<ImVec2, 8> Placed{};
	std::size_t Count = 0;
	for (const ImVec2 Point : Points)
	{
		Placed[Count++] = At(Point.x, Point.y);
	}

	if (bConcave)
	{
		Draw.AddConcavePolyFilled(Placed.data(), static_cast<int>(Count), Tone(Opacity));
	}
	else
	{
		Draw.AddConvexPolyFilled(Placed.data(), static_cast<int>(Count), Tone(Opacity));
	}
}

void FIconPen::Stroke(const std::span<const ImVec2> Points, const float Width, const float Opacity, const bool bClosed) const
{
	std::vector<ImVec2> Placed;
	Placed.reserve(Points.size());
	for (const ImVec2 Point : Points)
	{
		Placed.push_back(At(Point.x, Point.y));
	}

	const ImU32 Ink = Tone(Opacity);
	Draw.AddPolyline(Placed.data(), static_cast<int>(Placed.size()), Ink, bClosed ? ImDrawFlags_Closed : ImDrawFlags_None, Width * Scale);
	// Discs at every vertex round the caps and joins, which ImGui draws square.
	for (const ImVec2 Point : Placed)
	{
		Draw.AddCircleFilled(Point, Width * Scale * 0.5f, Ink, 12);
	}
}

void FIconPen::Stroke(const std::initializer_list<ImVec2> Points, const float Width, const float Opacity, const bool bClosed) const
{
	Stroke(std::span(Points.begin(), Points.size()), Width, Opacity, bClosed);
}

void FIconPen::RoundedPolygon(const std::initializer_list<ImVec2> Points, const float Rounding, const float Opacity) const
{
	Polygon(Points, Opacity);
	Stroke(Points, Rounding, Opacity, true);
}

void FIconPen::Ring(const float X, const float Y, const float Radius, const float Width, const float Opacity) const
{
	Draw.AddCircle(At(X, Y), Radius * Scale, Tone(Opacity), 0, Width * Scale);
}

void FIconPen::Frame(const float X0, const float Y0, const float X1, const float Y1, const float Rounding, const float Width, const float Opacity) const
{
	Draw.AddRect(At(X0, Y0), At(X1, Y1), Tone(Opacity), Rounding * Scale, 0, Width * Scale);
}

std::vector<ImVec2> FIconPen::Arc(const float X, const float Y, const float Radius, const float From, const float To) const
{
	const int Steps = std::max(4, static_cast<int>(std::abs(To - From) * 6.f));
	std::vector<ImVec2> Points;
	for (int Step = 0; Step <= Steps; ++Step)
	{
		const float Angle = From + (To - From) * static_cast<float>(Step) / static_cast<float>(Steps);
		Points.emplace_back(X + Radius * std::cos(Angle), Y + Radius * std::sin(Angle));
	}

	return Points;
}

void DrawToolUIIcon(ImDrawList* const Draw, const ImVec2 Center, const float Scale, const ImU32 Color, const EToolUIMenuIcon Icon)
{
	const FIconPen Pen(*Draw, Center, Scale, Color);
	constexpr float Dim = 0.45f;
	constexpr float Mid = 0.72f;
	switch (Icon)
	{
		case EToolUIMenuIcon::Entity:
			Pen.Circle(0.f, 0.f, 6.5f, Dim);
			Pen.Circle(0.f, 0.f, 3.f);
			break;
		case EToolUIMenuIcon::Cube:
			// Three faces, lit from above, with hairline gaps between them.
			Pen.Polygon({{0.f, -7.f}, {6.f, -3.6f}, {0.f, -0.2f}, {-6.f, -3.6f}});
			Pen.Polygon({{-6.2f, -2.8f}, {-0.5f, 0.5f}, {-0.5f, 7.f}, {-6.2f, 3.7f}}, Mid);
			Pen.Polygon({{6.2f, -2.8f}, {6.2f, 3.7f}, {0.5f, 7.f}, {0.5f, 0.5f}}, Dim);
			break;
		case EToolUIMenuIcon::Light:
		case EToolUIMenuIcon::TimeOfDay:
			Pen.Circle(0.f, 0.f, 3.4f);
			for (int Ray = 0; Ray < 8; ++Ray)
			{
				const float Angle = static_cast<float>(Ray) * std::numbers::pi_v<float> / 4.f;
				Pen.Stroke({{std::cos(Angle) * 5.2f, std::sin(Angle) * 5.2f}, {std::cos(Angle) * 6.8f, std::sin(Angle) * 6.8f}}, 1.6f);
			}

			break;
		case EToolUIMenuIcon::SkyAtmosphere:
			Pen.Circle(2.6f, -2.6f, 3.6f, Dim);
			Pen.Circle(-2.2f, 1.6f, 2.8f);
			Pen.Circle(1.2f, 0.4f, 3.4f);
			Pen.Circle(4.4f, 2.6f, 2.4f);
			Pen.Rect(-6.6f, 1.8f, 6.8f, 6.2f, 2.2f);
			break;
		case EToolUIMenuIcon::Fog:
			Pen.Rect(-6.5f, -5.2f, 4.f, -2.8f, 1.2f);
			Pen.Rect(-4.f, -1.2f, 6.5f, 1.2f, 1.2f, Mid);
			Pen.Rect(-6.5f, 2.8f, 3.f, 5.2f, 1.2f, Dim);
			break;
		case EToolUIMenuIcon::Physics:
			// A ball with motion lines behind it.
			Pen.Circle(1.6f, 0.f, 5.f);
			Pen.Stroke({{-7.f, -2.6f}, {-4.4f, -2.6f}}, 1.6f, Dim);
			Pen.Stroke({{-7.2f, 0.6f}, {-4.2f, 0.6f}}, 1.6f, Dim);
			Pen.Stroke({{-6.4f, 3.6f}, {-4.f, 3.6f}}, 1.6f, Dim);
			break;
		case EToolUIMenuIcon::Material:
			Pen.Circle(0.f, 0.f, 6.5f, Dim);
			Pen.Circle(-1.f, -1.f, 4.6f);
			break;
		case EToolUIMenuIcon::Sound:
			Pen.Polygon({{-7.f, -2.5f}, {-4.f, -2.5f}, {0.f, -6.2f}, {0.f, 6.2f}, {-4.f, 2.5f}, {-7.f, 2.5f}});
			Pen.Stroke(Pen.Arc(0.6f, 0.f, 3.2f, -0.85f, 0.85f), 1.6f);
			Pen.Stroke(Pen.Arc(0.6f, 0.f, 6.2f, -0.85f, 0.85f), 1.6f, Dim);
			break;
		case EToolUIMenuIcon::Folder:
		{
			// The accent color rather than the text color, so folders read as places apart from the actions around them.
			const ImVec4 Accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
			const FIconPen Tinted(*Draw, Center, Scale, ImGui::GetColorU32(Accent));
			const FIconPen Shaded(*Draw, Center, Scale, ImGui::GetColorU32(ImVec4{Accent.x * 0.68f, Accent.y * 0.68f, Accent.z * 0.68f, Accent.w}));
			Shaded.Rect(-7.f, -6.5f, -0.5f, -2.f, 1.5f);
			Shaded.Rect(-7.f, -4.5f, 7.f, 6.f, 1.8f);
			Tinted.Rect(-7.f, -2.f, 7.f, 6.f, 1.8f);
			break;
		}
		case EToolUIMenuIcon::Panel:
		case EToolUIMenuIcon::Layout:
			Pen.Rect(-7.f, -6.f, 7.f, 6.f, 2.f, Dim);
			Pen.Rect(-7.f, -6.f, 7.f, -2.6f, 2.f);
			if (Icon == EToolUIMenuIcon::Layout)
			{
				Pen.Rect(2.f, -1.4f, 7.f, 6.f, 1.6f);
			}

			break;
		case EToolUIMenuIcon::Outliner:
			for (const auto& [Y, Opacity] : {std::pair{-4.2f, 1.f}, std::pair{0.f, Dim}, std::pair{4.2f, Dim}})
			{
				Pen.Circle(-5.4f, Y, 1.4f);
				Pen.Rect(-2.6f, Y - 1.f, 6.8f, Y + 1.f, 1.f, Opacity);
			}

			break;
		case EToolUIMenuIcon::Details:
			// Three sliders, each with its knob.
			for (const auto& [Y, Knob] : {std::pair{-4.1f, -2.f}, std::pair{0.f, 3.f}, std::pair{4.1f, -4.f}})
			{
				Pen.Rect(-6.8f, Y - 0.8f, 6.8f, Y + 0.8f, 0.8f, Dim);
				Pen.Circle(Knob, Y, 2.1f);
			}

			break;
		case EToolUIMenuIcon::Open:
		case EToolUIMenuIcon::ContentBrowser:
			Pen.Rect(-7.f, -6.5f, -0.5f, -2.f, 1.5f, Dim);
			Pen.Rect(-7.f, -4.5f, 6.f, 5.f, 1.8f, Dim);
			Pen.RoundedPolygon({{-6.1f, 5.1f}, {-3.9f, -0.3f}, {6.7f, -0.3f}, {4.5f, 5.1f}}, 1.8f);
			break;
		case EToolUIMenuIcon::Log:
			Pen.Rect(-7.f, -6.f, 7.f, 6.f, 2.2f, Dim);
			Pen.Stroke({{-3.8f, -2.2f}, {-1.2f, 0.2f}, {-3.8f, 2.6f}}, 1.7f);
			Pen.Stroke({{1.f, 3.f}, {4.4f, 3.f}}, 1.7f);
			break;
		case EToolUIMenuIcon::Performance:
			// A frame-time pulse on a monitor.
			Pen.Rect(-7.f, -6.f, 7.f, 6.f, 2.2f, Dim);
			Pen.Stroke({{-4.8f, 0.6f}, {-2.4f, 0.6f}, {-0.8f, -3.2f}, {1.2f, 3.6f}, {2.8f, 0.6f}, {4.8f, 0.6f}}, 1.6f);
			break;
		case EToolUIMenuIcon::Sync:
			Pen.Circle(0.f, 0.f, 6.6f, Dim);
			Pen.Stroke({{0.f, -3.2f}, {0.f, 0.f}, {2.8f, 1.6f}}, 1.7f);
			break;
		case EToolUIMenuIcon::Import:
			Pen.Rect(-7.f, 1.5f, 7.f, 6.5f, 2.f, Dim);
			Pen.Rect(-1.1f, -7.f, 1.1f, -0.5f, 1.1f);
			Pen.RoundedPolygon({{-4.f, -1.6f}, {4.f, -1.6f}, {0.f, 3.6f}}, 1.f);
			break;
		case EToolUIMenuIcon::Save:
			Pen.Rect(-6.5f, -6.5f, 6.5f, 6.5f, 2.f, Dim);
			Pen.Rect(-3.2f, -6.5f, 3.2f, -2.4f, 1.f);
			Pen.Rect(-4.f, 1.2f, 4.f, 6.5f, 1.2f);
			break;
		case EToolUIMenuIcon::Undo:
		case EToolUIMenuIcon::Redo:
		{
			// A hooked arrow: along the top, round the right, and back along the bottom; redo mirrors it.
			const float Side = Icon == EToolUIMenuIcon::Undo ? 1.f : -1.f;
			std::vector<ImVec2> Hook{{-4.f * Side, -3.2f}};
			for (const ImVec2 Point : Pen.Arc(1.5f, 1.f, 4.2f, -std::numbers::pi_v<float> * 0.5f, std::numbers::pi_v<float> * 0.5f))
			{
				Hook.emplace_back(Point.x * Side, Point.y);
			}

			Hook.emplace_back(-1.f * Side, 5.2f);
			Pen.Stroke(Hook, 1.8f);
			Pen.RoundedPolygon({{-7.f * Side, -3.2f}, {-3.4f * Side, -6.4f}, {-3.4f * Side, 0.f}}, 1.f);
			break;
		}
		case EToolUIMenuIcon::Add:
			Pen.Rect(-6.f, -1.2f, 6.f, 1.2f, 1.2f);
			Pen.Rect(-1.2f, -6.f, 1.2f, 6.f, 1.2f);
			break;
		case EToolUIMenuIcon::Copy:
		case EToolUIMenuIcon::Duplicate:
			Pen.Rect(-6.5f, -6.5f, 2.5f, 2.5f, 2.f, Dim);
			Pen.Rect(-2.5f, -2.5f, 6.5f, 6.5f, 2.f);
			break;
		case EToolUIMenuIcon::Paste:
			Pen.Rect(-6.f, -5.f, 6.f, 7.f, 2.f, Dim);
			Pen.Rect(-3.2f, -7.f, 3.2f, -3.f, 1.4f);
			Pen.Rect(-3.5f, 0.2f, 3.5f, 1.8f, 0.8f);
			Pen.Rect(-3.5f, 3.4f, 1.5f, 5.f, 0.8f);
			break;
		case EToolUIMenuIcon::Delete:
			Pen.Rect(-6.5f, -5.4f, 6.5f, -3.4f, 1.f);
			Pen.Rect(-2.2f, -7.f, 2.2f, -5.f, 1.f);
			Pen.Polygon({{-5.2f, -2.2f}, {5.2f, -2.2f}, {4.3f, 6.6f}, {-4.3f, 6.6f}}, Dim);
			Pen.Stroke({{-1.6f, -0.2f}, {-1.6f, 4.f}}, 1.5f);
			Pen.Stroke({{1.6f, -0.2f}, {1.6f, 4.f}}, 1.5f);
			break;
		case EToolUIMenuIcon::SelectAll:
			Pen.Rect(-6.8f, -6.8f, 6.8f, 6.8f, 2.4f, Dim);
			Pen.Stroke({{-3.2f, 0.2f}, {-0.8f, 2.6f}, {3.4f, -2.f}}, 1.9f);
			break;
		case EToolUIMenuIcon::Select:
			// A mouse pointer with an upright left edge and a tail, so it does not read as a play arrow.
			Pen.Polygon({{-4.f, -7.f}, {4.6f, 1.6f}, {0.9f, 1.8f}, {3.f, 6.f}, {1.2f, 6.8f}, {-0.9f, 2.6f}, {-4.f, 5.4f}}, 1.f, true);
			Pen.Stroke({{-4.f, -7.f}, {4.6f, 1.6f}, {0.9f, 1.8f}, {3.f, 6.f}, {1.2f, 6.8f}, {-0.9f, 2.6f}, {-4.f, 5.4f}}, 1.2f, 1.f, true);
			break;
		case EToolUIMenuIcon::Move:
			Pen.Stroke({{-5.f, 0.f}, {5.f, 0.f}}, 1.6f);
			Pen.Stroke({{0.f, -5.f}, {0.f, 5.f}}, 1.6f);
			Pen.Polygon({{-7.4f, 0.f}, {-4.4f, -2.8f}, {-4.4f, 2.8f}});
			Pen.Polygon({{7.4f, 0.f}, {4.4f, 2.8f}, {4.4f, -2.8f}});
			Pen.Polygon({{0.f, -7.4f}, {2.8f, -4.4f}, {-2.8f, -4.4f}});
			Pen.Polygon({{0.f, 7.4f}, {-2.8f, 4.4f}, {2.8f, 4.4f}});
			Pen.Circle(0.f, 0.f, 2.f, Dim);
			break;
		case EToolUIMenuIcon::Rotate:
		case EToolUIMenuIcon::Reset:
		{
			// Rotate turns clockwise and Reset mirrors it, each with its head at the top.
			constexpr float Pi = std::numbers::pi_v<float>;
			const float Side = Icon == EToolUIMenuIcon::Rotate ? 1.f : -1.f;
			const std::vector<ImVec2> Path = Icon == EToolUIMenuIcon::Rotate ? Pen.Arc(0.f, 1.f, 6.f, 0.f, Pi * 1.5f - 0.25f) : Pen.Arc(0.f, 1.f, 6.f, -Pi * 0.5f + 0.25f, Pi);
			Pen.Stroke(Path, 1.8f);
			Pen.RoundedPolygon({{3.4f * Side, -5.f}, {-0.6f * Side, -7.8f}, {-0.6f * Side, -2.2f}}, 1.f);
			break;
		}
		case EToolUIMenuIcon::Scale:
			Pen.Rect(-7.f, 0.f, 0.f, 7.f, 1.6f, Dim);
			Pen.Stroke({{-2.5f, 2.5f}, {4.2f, -4.2f}}, 1.7f);
			Pen.RoundedPolygon({{7.f, -7.f}, {7.f, -1.8f}, {1.8f, -7.f}}, 1.f);
			break;
		case EToolUIMenuIcon::World:
			Pen.Circle(0.f, 0.f, 6.6f, Dim);
			Pen.Ring(0.f, 0.f, 6.6f, 1.5f);
			Draw->AddEllipse(Center, {3.f * Scale, 6.4f * Scale}, Color, 0.f, 24, 1.5f * Scale);
			Pen.Stroke({{-6.4f, 0.f}, {6.4f, 0.f}}, 1.5f);
			break;
		case EToolUIMenuIcon::Grid:
			for (int Row = 0; Row < 3; ++Row)
			{
				for (int Column = 0; Column < 3; ++Column)
				{
					const float X = -6.3f + static_cast<float>(Column) * 4.6f;
					const float Y = -6.3f + static_cast<float>(Row) * 4.6f;
					Pen.Rect(X, Y, X + 3.4f, Y + 3.4f, 1.f);
				}
			}

			break;
		case EToolUIMenuIcon::Focus:
			Pen.Stroke({{-6.2f, -2.4f}, {-6.2f, -6.2f}, {-2.4f, -6.2f}}, 1.8f);
			Pen.Stroke({{2.4f, -6.2f}, {6.2f, -6.2f}, {6.2f, -2.4f}}, 1.8f);
			Pen.Stroke({{6.2f, 2.4f}, {6.2f, 6.2f}, {2.4f, 6.2f}}, 1.8f);
			Pen.Stroke({{-2.4f, 6.2f}, {-6.2f, 6.2f}, {-6.2f, 2.4f}}, 1.8f);
			Pen.Circle(0.f, 0.f, 2.4f);
			break;
		case EToolUIMenuIcon::Settings:
			for (const float Y : {-5.f, 0.f, 5.f})
			{
				Pen.Circle(0.f, Y, 1.7f);
			}

			break;
		case EToolUIMenuIcon::Play:
			Pen.RoundedPolygon({{-4.f, -6.f}, {6.f, 0.f}, {-4.f, 6.f}}, 2.2f);
			break;
		case EToolUIMenuIcon::Stop:
			Pen.Rect(-5.5f, -5.5f, 5.5f, 5.5f, 2.f);
			break;
		case EToolUIMenuIcon::Simulate:
			// A flask, half full.
			Pen.Rect(-3.4f, -7.4f, 3.4f, -5.6f, 0.9f);
			Pen.Polygon({{-2.f, -5.6f}, {2.f, -5.6f}, {2.f, -1.2f}, {6.6f, 6.6f}, {-6.6f, 6.6f}, {-2.f, -1.2f}}, Dim, true);
			Pen.Polygon({{-4.4f, 2.6f}, {4.4f, 2.6f}, {6.6f, 6.6f}, {-6.6f, 6.6f}});
			break;
		case EToolUIMenuIcon::LockClosed:
		case EToolUIMenuIcon::LockOpen:
		{
			// The open lock swings its shackle right of the body.
			const bool bOpen = Icon == EToolUIMenuIcon::LockOpen;
			const float ShackleX = bOpen ? 3.2f : 0.f;
			const float ShackleRadius = bOpen ? 2.8f : 3.2f;
			const float Top = bOpen ? -3.4f : -3.2f;
			std::vector<ImVec2> Shackle{{ShackleX - ShackleRadius, -0.8f}};
			for (const ImVec2 Point : Pen.Arc(ShackleX, Top, ShackleRadius, std::numbers::pi_v<float>, std::numbers::pi_v<float> * 2.f))
			{
				Shackle.push_back(Point);
			}

			Shackle.emplace_back(ShackleX + ShackleRadius, bOpen ? -2.f : -0.8f);
			Pen.Stroke(Shackle, 1.8f);
			const float BodyX = bOpen ? -1.7f : 0.f;
			Pen.Rect(BodyX - 5.3f, -0.8f, BodyX + 5.3f, 7.f, 2.f);
			Pen.Circle(BodyX, 3.f, 1.5f, Dim);
			break;
		}
		case EToolUIMenuIcon::Search:
			Pen.Circle(-1.2f, -1.2f, 5.f, Dim);
			Pen.Ring(-1.2f, -1.2f, 4.2f, 1.7f);
			Pen.Stroke({{2.6f, 2.6f}, {6.4f, 6.4f}}, 2.4f);
			break;
		case EToolUIMenuIcon::Close:
			Pen.Circle(0.f, 0.f, 6.8f, Dim);
			Pen.Stroke({{-2.8f, -2.8f}, {2.8f, 2.8f}}, 1.8f);
			Pen.Stroke({{2.8f, -2.8f}, {-2.8f, 2.8f}}, 1.8f);
			break;
		case EToolUIMenuIcon::Appearance:
			Pen.Rect(-6.8f, -3.9f, 6.8f, -2.1f, 0.9f, Dim);
			Pen.Rect(-6.8f, 2.1f, 6.8f, 3.9f, 0.9f, Dim);
			Pen.Circle(-2.f, -3.f, 2.3f);
			Pen.Circle(3.f, 3.f, 2.3f);
			break;
		case EToolUIMenuIcon::Camera:
			Pen.Rect(-7.f, -4.6f, 7.f, 5.4f, 2.2f, Dim);
			Pen.Rect(-3.6f, -6.4f, 1.4f, -3.8f, 1.f);
			Pen.Circle(0.f, 0.4f, 2.8f);
			break;
		case EToolUIMenuIcon::CameraSpeed:
		{
			constexpr float Pi = std::numbers::pi_v<float>;
			Pen.Stroke(Pen.Arc(0.f, 1.2f, 6.f, Pi * 0.8f, Pi * 2.2f), 2.f, Dim);
			Pen.Stroke(Pen.Arc(0.f, 1.2f, 6.f, Pi * 0.8f, Pi * 1.45f), 2.f);
			Pen.Stroke({{0.f, 1.2f}, {3.2f, -2.2f}}, 1.8f);
			Pen.Circle(0.f, 1.2f, 1.8f);
			break;
		}
		case EToolUIMenuIcon::LogPrompt:
			Pen.Stroke({{-4.4f, -4.f}, {-0.4f, 0.f}, {-4.4f, 4.f}}, 2.f);
			Pen.Stroke({{2.4f, 4.f}, {6.6f, 4.f}}, 2.f);
			break;
		case EToolUIMenuIcon::Exit:
		default:
			Pen.Rect(-7.f, -6.5f, 1.f, 6.5f, 2.f, Dim);
			Pen.Stroke({{-2.4f, 0.f}, {4.2f, 0.f}}, 1.8f);
			Pen.RoundedPolygon({{7.f, 0.f}, {3.4f, 3.4f}, {3.4f, -3.4f}}, 1.f);
			break;
	}
}

void ActivateDocument(FToolUIContext::FImplementation& Owner, const ImGuiID Id)
{
	Owner.ActiveDocument = Id;
	Owner.FocusRequestedDocument = Id;
	Owner.bFocusViewportRequested = Id == 0;
}

// Asset editors open as major tabs beside the level in a row under the title bar, as in Unreal.
void DrawDocumentTabs(FToolUIContext::FImplementation& Owner, ImDrawList& DrawList, const float Left, const float Top, const float RowHeight, const float Scale)
{
	const int Frame = ImGui::GetFrameCount();
	for (std::size_t Index = Owner.Documents.size(); Index-- > 0;)
	{
		if (Owner.Documents[Index].LastFrame >= Frame - 1)
		{
			continue;
		}

		// Closing the tab in front shows its left neighbour, or the level.
		if (Owner.Documents[Index].Id == Owner.ActiveDocument)
		{
			ActivateDocument(Owner, Index > 0 ? Owner.Documents[Index - 1].Id : 0);
		}

		Owner.Documents.erase(Owner.Documents.begin() + static_cast<std::ptrdiff_t>(Index));
	}

	const float Height = RowHeight - 6.f * Scale;
	const float Y = Top + (RowHeight - Height) * 0.5f;
	const float Padding = 12.f * Scale;
	const float CloseSize = 16.f * Scale;
	const float Rounding = Owner.Metrics.TitleBarControlRounding * Scale;
	float X = Left + 6.f * Scale;
	const auto DrawTab = [&](const ImGuiID Id, const std::string& Label, const bool bClosable)
	{
		const ImVec2 TextSize = ImGui::CalcTextSize(Label.c_str());
		const float Width = Padding * 2.f + TextSize.x + (bClosable ? CloseSize + 4.f * Scale : 0.f);
		const ImVec2 Minimum{X, Y};
		const ImVec2 Maximum{X + Width, Y + Height};
		ImGui::SetCursorScreenPos(Minimum);
		ImGui::PushID(static_cast<int>(Id));
		ImGui::InvisibleButton("##DocumentTab", {Width, Height});
		ImGui::PopID();

		const bool bActive = Owner.ActiveDocument == Id;
		const bool bHovered = ImGui::IsItemHovered();
		const ImVec2 CloseMinimum{Maximum.x - Padding * 0.5f - CloseSize, Y + (Height - CloseSize) * 0.5f};
		const ImVec2 CloseMaximum{CloseMinimum.x + CloseSize, CloseMinimum.y + CloseSize};
		const bool bCloseHovered = bClosable && bHovered && ImGui::IsMouseHoveringRect(CloseMinimum, CloseMaximum);
		if (bClosable && (ImGui::IsItemClicked(ImGuiMouseButton_Middle) || (bCloseHovered && ImGui::IsItemClicked(ImGuiMouseButton_Left))))
		{
			ActivateDocument(Owner, Id);
			Owner.CloseRequestedDocument = Id;
		}
		else if (!bActive && ImGui::IsItemClicked(ImGuiMouseButton_Left))
		{
			ActivateDocument(Owner, Id);
		}

		if (bActive || bHovered)
		{
			DrawList.AddRectFilled(Minimum, Maximum, bActive ? IM_COL32(255, 255, 255, 16) : IM_COL32(255, 255, 255, 24), Rounding);
		}

		const ImU32 TextColor = ToImGuiPackedColor(bActive || bHovered ? ToolUITheme::TextPrimary : ToolUITheme::TextMuted);
		DrawList.AddText({X + Padding, Y + (Height - TextSize.y) * 0.5f}, TextColor, Label.c_str());
		if (bActive)
		{
			const float Thickness = 2.f * Scale;
			DrawList.AddRectFilled({X + Padding, Maximum.y - Thickness}, {X + Padding + TextSize.x, Maximum.y}, ToImGuiPackedColor(ToolUITheme::NeutralAccent), Thickness * 0.5f);
		}

		if (bClosable && (bActive || bHovered))
		{
			if (bCloseHovered)
			{
				DrawList.AddRectFilled(CloseMinimum, CloseMaximum, IM_COL32(255, 255, 255, 32), Rounding);
			}

			const ImVec2 Center{(CloseMinimum.x + CloseMaximum.x) * 0.5f, (CloseMinimum.y + CloseMaximum.y) * 0.5f};
			const ImU32 CrossColor = ToImGuiPackedColor(bCloseHovered ? ToolUITheme::TextPrimary : ToolUITheme::TextMuted);
			DrawToolUIIcon(&DrawList, Center, Scale * 0.6f, CrossColor, EToolUIMenuIcon::Close);
		}

		X = Maximum.x + 2.f * Scale;
	};

	DrawTab(0, Owner.LevelTabLabel, false);
	for (const FToolUIDocument& Document : Owner.Documents)
	{
		DrawTab(Document.Id, Document.Label, true);
	}
}

void DrawWindowSurfaces(FToolUIContext::FImplementation& Owner)
{
	// ImGui owns dock tabs and popup creation. Keep their surface/underline adapter here, without changing docking internals.
	for (ImGuiWindow* const Window : Owner.Context->Windows)
	{
		if (!Window->Active || Window->Hidden)
		{
			continue;
		}

		const bool bPopup = (Window->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip)) != 0;
		const auto Panel = Owner.PresentedPanels.find(Window->ID);
		const bool bPanel = Panel != Owner.PresentedPanels.end();
		if (!bPopup && !bPanel)
		{
			continue;
		}

		ImGuiDockNode* const Node = Window->DockNode;
		if (Node && Node->VisibleWindow != Window)
		{
			continue;
		}

		ImDrawList* const DrawList = Node && Node->HostWindow ? Node->HostWindow->DrawList : Window->DrawList;
		const ImVec2 Minimum = Node ? Node->Pos : Window->Pos;
		const ImVec2 Size = Node ? Node->Size : Window->Size;
		const ImVec2 Maximum{Minimum.x + Size.x, Minimum.y + Size.y};
		const float Radius = Node ? 0.f : Window->WindowRounding;
		if (bPopup || (bPanel && !Panel->second && IsToolUIPanelTransparent(Owner.Appearance.PanelTransparency, Node != nullptr)))
		{
			ImDrawListSplitter Splitter;
			Splitter.Split(DrawList, 2);
			Splitter.SetCurrentChannel(DrawList, 1);
			DrawList->PushClipRect(Minimum, Maximum, false);
			DrawGlassSurface(*DrawList, *Window->Viewport, Owner.Appearance, Minimum, Maximum, Radius);
			DrawList->PopClipRect();
			Splitter.SwapChannels(DrawList, 0, 1);
			Splitter.Merge(DrawList);
		}

		if (Node && Node->TabBar && !Node->IsHiddenTabBar())
		{
			const ImGuiTabBar& Bar = *Node->TabBar;
			// A hairline under the whole tab row gives the active underline a track to sit on.
			const float LineScale = Window->Viewport->DpiScale;
			DrawList->AddLine({Bar.BarRect.Min.x, Bar.BarRect.Max.y - 0.5f * LineScale}, {Bar.BarRect.Max.x, Bar.BarRect.Max.y - 0.5f * LineScale}, ToImGuiPackedColor(ToolUITheme::Border), LineScale);
			for (const ImGuiTabItem& Tab : Bar.Tabs)
			{
				const bool bOutliner = Tab.Window && Tab.Window->ID == ImHashStr("Outliner");
				const bool bDetails = Tab.Window && Tab.Window->ID == ImHashStr("Details");
				const bool bContent = Tab.Window && Tab.Window->ID == ImHashStr("Content Browser");
				const bool bPerformance = Tab.Window && Tab.Window->ID == ImHashStr("Performance");
				const bool bLog = Tab.Window && Tab.Window->ID == ImHashStr("Output Log");
				const bool bWorld = Tab.Window && Tab.Window->ID == ImHashStr("World Settings");
				if (bOutliner || bDetails || bContent || bPerformance || bLog || bWorld)
				{
					const float Scale = Window->Viewport->DpiScale;
					const float Left = Bar.BarRect.Min.x + Tab.Offset - Bar.ScrollingAnim;
					const ImVec2 Center{Left + Bar.FramePadding.x + 6.f * Scale, (Bar.BarRect.Min.y + Bar.BarRect.Max.y) * 0.5f};
					const bool bHovered = Owner.Context->HoveredWindow == Node->HostWindow && ImGui::IsMouseHoveringRect(Tab.Window->DC.DockTabItemRect.Min, Tab.Window->DC.DockTabItemRect.Max, false);
					const ImU32 Color = ToImGuiPackedColor(Tab.ID == Bar.VisibleTabId || bHovered ? ToolUITheme::TextPrimary : ToolUITheme::TextMuted);
					DrawList->PushClipRect({Bar.ScrollingRectMinX, Bar.BarRect.Min.y}, {Bar.ScrollingRectMaxX, Bar.BarRect.Max.y}, false);
					DrawToolUIIcon(DrawList, Center, Scale, Color, bOutliner ? EToolUIMenuIcon::Outliner : bDetails   ? EToolUIMenuIcon::Details
					                                                                                   : bPerformance ? EToolUIMenuIcon::Performance
					                                                                                   : bLog         ? EToolUIMenuIcon::Log
					                                                                                   : bWorld       ? EToolUIMenuIcon::World
					                                                                                                  : EToolUIMenuIcon::ContentBrowser);
					DrawList->PopClipRect();
				}

				if (Tab.ID != Bar.VisibleTabId)
				{
					continue;
				}

				const float Left = std::max(Bar.ScrollingRectMinX, Bar.BarRect.Min.x + Tab.Offset - Bar.ScrollingAnim);
				const float Right = std::min(Bar.ScrollingRectMaxX, Bar.BarRect.Min.x + Tab.Offset - Bar.ScrollingAnim + Tab.Width);
				DrawList->PushClipRect(Bar.BarRect.Min, Bar.BarRect.Max, false);
				const float UnderlineThickness = 2.f * LineScale;
				if (Right > Left)
				{
					DrawList->AddRectFilled({Left, Bar.BarRect.Max.y - UnderlineThickness}, {Right, Bar.BarRect.Max.y}, ToImGuiPackedColor(ToolUITheme::NeutralAccent), UnderlineThickness * 0.5f);
				}

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

void UpdateTitleBarUiCaptureRegions(FWindow& Window, const ImGuiViewport& Viewport, const ImGuiWindow* const IgnoredWindow, const float MainMenuRight = 0.f)
{
	FTitleBarHitTestState HitTestState = Window.GetTitleBarHitTestState();
	HitTestState.UiCaptureRegionCount = 0;
	HitTestState.bUiCapturesEntireTitleBar = false;

	const FTitleBarLayout& Layout = HitTestState.Layout;
	const ImRect TitleBarRect{
	    Viewport.Pos,
	    {Viewport.Pos.x + static_cast<float>(Layout.WindowWidth), Viewport.Pos.y + static_cast<float>(Layout.TitleBarHeight)},
	};

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
		    {std::min(UiWindow->OuterRectClipped.Max.x, TitleBarRect.Max.x), std::min(UiWindow->OuterRectClipped.Max.y, TitleBarRect.Max.y)},
		};

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
		    .MaximumY = std::clamp(static_cast<int>(std::ceil(Intersection.Max.y - Viewport.Pos.y)), 0, Layout.TitleBarHeight),
		};
	}

	if (MainMenuRight > Viewport.Pos.x && HitTestState.UiCaptureRegionCount < HitTestState.UiCaptureRegions.size())
	{
		HitTestState.UiCaptureRegions[HitTestState.UiCaptureRegionCount++] = {
		    .MinimumX = Layout.TitleBarHeight,
		    .MinimumY = 0,
		    .MaximumX = std::clamp(static_cast<int>(std::ceil(MainMenuRight - Viewport.Pos.x)), 0, Layout.WindowWidth),
		    .MaximumY = Layout.TitleBarHeight,
		};
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
	const FToolUIViewportPosition Position = ResolveToolUIViewportPosition(Owner.bProgrammaticWindowPosition, Data->CachedPosition, {.X = static_cast<float>(PlatformPosition.X), .Y = static_cast<float>(PlatformPosition.Y)});
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
			Data->CachedPosition = {.X = static_cast<float>(X), .Y = static_cast<float>(Y)};
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

	Callbacks.FilesDropped = [&Owner](FWindow&, const std::span<const std::filesystem::path> Paths)
	{
		Owner.DroppedFiles.insert(Owner.DroppedFiles.end(), Paths.begin(), Paths.end());
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
			if (Owner.bCursorCaptured)
			{
				Input.AddMousePosEvent(Owner.CapturedMousePosition.x, Owner.CapturedMousePosition.y);
			}
			else if (bCursorPositionValid)
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

		if (Owner.bCursorCaptured)
		{
			if (Owner.CapturedCursor)
			{
				Owner.CapturedMotion.X += static_cast<float>(X - (*Owner.CapturedCursor)[0]);
				Owner.CapturedMotion.Y += static_cast<float>(Y - (*Owner.CapturedCursor)[1]);
			}

			Owner.CapturedCursor = std::array{X, Y};
			ImGui::GetIO().AddMousePosEvent(Owner.CapturedMousePosition.x, Owner.CapturedMousePosition.y);
			return;
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
	Data->Window = *WindowResult;
#ifdef __linux__
	Data->Window->SetActionPolicy({.bRequireMinimizeSupport = true});
#endif
	Data->CachedPosition = {.X = Viewport->Pos.x, .Y = Viewport->Pos.y};
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

	Data->CachedPosition = {.X = Position.x, .Y = Position.y};
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
	const FToolUIViewportPosition CachedPosition = Data != nullptr ? Data->CachedPosition : FToolUIViewportPosition{.X = Viewport->Pos.x, .Y = Viewport->Pos.y};
	const FToolUIViewportPosition Position = ResolveToolUIViewportPosition(GetToolUIImplementation()->bProgrammaticWindowPosition, CachedPosition, {.X = static_cast<float>(PlatformPosition.X), .Y = static_cast<float>(PlatformPosition.Y)});
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
		return {1.f, 1.f};
	}

	return {
	    static_cast<float>(Window->GetFramebufferWidth()) / static_cast<float>(Window->GetWidth()),
	    static_cast<float>(Window->GetFramebufferHeight()) / static_cast<float>(Window->GetHeight()),
	};
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
	return Window != nullptr && Owner.WindowSystem != EWindowSystem::Wayland ? std::max(0.01f, Window->GetContentScale()) : 1.f;
}

[[nodiscard]] ImVec4 PlatformGetWindowWorkAreaInsets(ImGuiViewport* const Viewport)
{
	if (Viewport == ImGui::GetMainViewport())
	{
		return {};
	}

	FWindow* const Window = GetViewportWindow(Viewport);
	return Window != nullptr ? ImVec4{0.f, static_cast<float>(Window->GetTitleBarHitTestState().Layout.TitleBarHeight), 0.f, 0.f} : ImVec4{};
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
	    static_cast<std::uint32_t>(std::max(0, Window.GetFramebufferHeight())),
	};
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
		DrawGlassSurface(*DrawList, *Viewport, Owner.Appearance, Minimum, TitleBarMaximum, 0.f);
		DrawList->AddLine({Minimum.x, TitleBarMaximum.y}, {TitleBarMaximum.x, TitleBarMaximum.y}, ToImGuiPackedColor(ToolUITheme::BorderSoft));
		DrawList->AddCircle({Minimum.x + 18.f * Scale, Minimum.y + 18.f * Scale}, 7.f * Scale, ToImGuiPackedColor(ToolUITheme::NeutralAccent), 24, 2.f * Scale);
		DrawList->AddCircleFilled({Minimum.x + 18.f * Scale, Minimum.y + 18.f * Scale}, 2.f * Scale, ToImGuiPackedColor(ToolUITheme::NeutralAccent));
		DrawWindowControls(*DrawList, Minimum, Layout);
		const std::string_view Title = Data->Window->GetTitle();
		DrawList->AddText(Owner.MediumFont, Owner.Metrics.BaseFontSize * std::max(1.f, Viewport->DpiScale), {Minimum.x + 50.f * Scale, Minimum.y + 10.f * Scale}, ToImGuiPackedColor(ToolUITheme::TextPrimary), Title.data(), Title.data() + Title.size());
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

	std::expected<std::vector<std::byte>, FToolUIError> LogFontBytes = ReadFile(Descriptor.LogFontPath);
	if (!LogFontBytes)
	{
		return std::unexpected(std::move(LogFontBytes.error()));
	}

	// Emoji are a nicety; a missing font leaves them as boxes rather than failing the editor.
	std::vector<std::byte> EmojiFontBytes = Descriptor.EmojiFontPath.empty() ? std::vector<std::byte>{} : ReadFile(Descriptor.EmojiFontPath).value_or(std::vector<std::byte>{});

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
#ifdef __linux__
		Implementation->Window->SetActionPolicy({.bRequireMinimizeSupport = true});
#endif
		Implementation->BackendWindow = static_cast<GLFWwindow*>(Descriptor.Window->GetBackendHandle().Value);
		Implementation->WindowSystem = Descriptor.Application->GetCapabilities().WindowSystem;
		Implementation->bProgrammaticWindowPosition = Descriptor.Application->GetCapabilities().bProgrammaticWindowPosition;
		Implementation->Appearance = Descriptor.Appearance;
		if (const std::optional<FEditorAppearance> SavedAppearance = LoadEditorAppearance(Descriptor.AppearancePath))
		{
			Implementation->Appearance = *SavedAppearance;
		}

		Implementation->Renderer = std::move(Descriptor.Renderer);
		Implementation->bVSync = Descriptor.bVSync;
		Implementation->VSyncChanged = std::move(Descriptor.VSyncChanged);
		Implementation->RegularFontBytes = std::move(*RegularFontBytes);
		Implementation->MediumFontBytes = std::move(*MediumFontBytes);
		Implementation->LogFontBytes = std::move(*LogFontBytes);
		Implementation->EmojiFontBytes = std::move(EmojiFontBytes);
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
		// ImGui silently turns multi-viewports off unless the renderer also claims support.
		if (bMultiViewportRendererAvailable)
		{
			Input.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
			Input.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
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
		// A merge joins the font added just before it; ImGui searches merged sources in order, so emoji come from Noto only where the
		// main font has no glyph. A missing emoji font leaves emoji as boxes rather than failing the editor.
		const auto AddFont = [&](std::vector<std::byte>& Bytes) -> ImFont*
		{
			ImFontConfig Configuration = FontConfiguration;
			ImFont* const Font = Input.Fonts->AddFontFromMemoryTTF(Bytes.data(), static_cast<int>(Bytes.size()), 0.f, &Configuration);
			if (Font && !Implementation->EmojiFontBytes.empty())
			{
				ImFontConfig EmojiConfiguration = FontConfiguration;
				EmojiConfiguration.MergeMode = true;
				Input.Fonts->AddFontFromMemoryTTF(Implementation->EmojiFontBytes.data(), static_cast<int>(Implementation->EmojiFontBytes.size()), 0.f, &EmojiConfiguration);
			}

			return Font;
		};

		Implementation->RegularFont = AddFont(Implementation->RegularFontBytes);
		Implementation->MediumFont = AddFont(Implementation->MediumFontBytes);
		Implementation->LogFont = AddFont(Implementation->LogFontBytes);
		if (Implementation->RegularFont == nullptr || Implementation->MediumFont == nullptr || Implementation->LogFont == nullptr)
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

const FToolUIFrameStats& FToolUIContext::GetFrameStats() const noexcept
{
	return Implementation->FrameStats;
}

void FToolUIContext::SetItemRecording(const bool bEnabled) noexcept
{
	Implementation->bRecordItems = bEnabled;
}

std::span<const FToolUIItem> FToolUIContext::GetRecordedItems() const noexcept
{
	return Implementation->ItemRecorder.Items;
}

void FToolUIContext::QueueClick(const float X, const float Y, const int Button, const bool bDouble)
{
	ImGui::SetCurrentContext(Implementation->Context);
	QueueToolUIClick(X, Y, Button, bDouble);
}

void FToolUIContext::QueueDrag(const float FromX, const float FromY, const float ToX, const float ToY)
{
	constexpr int Moves = 6;
	Implementation->PointerSteps.push_back({.X = FromX, .Y = FromY, .Button = 1});
	for (int Move = 1; Move <= Moves; ++Move)
	{
		const float Alpha = static_cast<float>(Move) / Moves;
		Implementation->PointerSteps.push_back({.X = FromX + (ToX - FromX) * Alpha, .Y = FromY + (ToY - FromY) * Alpha, .Button = -1});
	}

	// A frame hovering the target lets it accept before the release delivers.
	Implementation->PointerSteps.push_back({.X = ToX, .Y = ToY, .Button = -1});
	Implementation->PointerSteps.push_back({.X = ToX, .Y = ToY, .Button = 0});
}

void FToolUIContext::QueueText(const std::string_view Text)
{
	ImGui::SetCurrentContext(Implementation->Context);
	QueueToolUIText(Text);
}

bool FToolUIContext::QueueKey(const std::string_view Chord)
{
	ImGui::SetCurrentContext(Implementation->Context);
	return QueueToolUIKey(Chord);
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
		SaveEditorAppearance(Implementation->AppearancePath, Implementation->Appearance);
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

void FToolUIContext::PushLogFont()
{
	ImGui::PushFont(Implementation->LogFont, Implementation->Metrics.BaseFontSize * 0.9f);
}

void FToolUIContext::PopLogFont()
{
	ImGui::PopFont();
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
	Implementation->CapturedFrameMotion = std::exchange(Implementation->CapturedMotion, {});
	if (Implementation->bCursorCaptured)
	{
		// The GLFW backend reports the focused window's cursor every frame, and a disabled cursor keeps travelling unseen across the panels.
		ImGui::GetIO().AddMousePosEvent(Implementation->CapturedMousePosition.x, Implementation->CapturedMousePosition.y);
	}
	else if (!Implementation->bProgrammaticWindowPosition)
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

	// Alt belongs to editor shortcuts and camera controls, not menu-layer navigation.
	const ImGuiID AltOwner = ImHashStr("HertaAltShortcuts");
	ImGui::SetKeyOwner(ImGuiKey_LeftAlt, AltOwner);
	ImGui::SetKeyOwner(ImGuiKey_RightAlt, AltOwner);
	if (!Implementation->PointerSteps.empty())
	{
		// After the platform backend, so the step's position wins over the real cursor for this frame.
		const auto Step = Implementation->PointerSteps.front();
		Implementation->PointerSteps.pop_front();
		ImGui::GetIO().AddMousePosEvent(Step.X, Step.Y);
		if (Step.Button >= 0)
		{
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, Step.Button == 1);
		}
	}

	Implementation->ItemRecorder.BeginFrame(*Implementation->Context, Implementation->bRecordItems);
	ImGui::NewFrame();
	Implementation->PresentedPanels.clear();
	for (ImGuiWindow* const Window : Implementation->Context->Windows)
	{
		if (Window->DockNode && Window->DockNode->TabBar)
		{
			const ImGuiTabBar& Bar = *Window->DockNode->TabBar;
			const bool bSelected = (Bar.NextSelectedTabId != 0 ? Bar.NextSelectedTabId : Bar.VisibleTabId) == Window->TabId;
			const bool bHovered = Implementation->Context->HoveredWindow == Window->DockNode->HostWindow && ImGui::IsMouseHoveringRect(Window->DC.DockTabItemRect.Min, Window->DC.DockTabItemRect.Max, false);
			Window->DockStyle.Colors[ImGuiWindowDockStyleCol_Text] = ToImGuiPackedColor(bSelected || bHovered ? ToolUITheme::TextPrimary : ToolUITheme::TextMuted);
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

	if (!Implementation->bContentDockMigrationComplete && !Implementation->bViewportImmersive)
	{
		MigrateContentDockHeight(Implementation->DockspaceId);
		Implementation->bContentDockMigrationComplete = true;
	}

	OrderContentDockTabs();
	DrawWindowSurfaces(*Implementation);
	DrawDetachedViewportChrome(*Implementation);
	UpdateTitleBarUiCaptureRegions(*Implementation->Window, *ImGui::GetMainViewport(), ImGui::FindWindowByName("HertaWorkspaceHost"), Implementation->MainMenuRight);
	ImGui::Render();
	Implementation->bFrameActive = false;
	FToolUIFrameStats& Stats = Implementation->FrameStats;
	Stats = {};
	for (const ImGuiViewport* const Viewport : ImGui::GetPlatformIO().Viewports)
	{
		const ImDrawData* const Draw = Viewport->DrawData;
		if (Draw == nullptr || !Draw->Valid)
		{
			continue;
		}

		++Stats.PlatformWindows;
		Stats.DrawLists += static_cast<std::uint32_t>(Draw->CmdListsCount);
		Stats.Vertices += static_cast<std::uint32_t>(Draw->TotalVtxCount);
		Stats.Indices += static_cast<std::uint32_t>(Draw->TotalIdxCount);
		for (const ImDrawList* const List : Draw->CmdLists)
		{
			Stats.DrawCommands += static_cast<std::uint32_t>(List->CmdBuffer.Size);
		}
	}

	for (const ImGuiWindow* const Window : Implementation->Context->Windows)
	{
		Stats.Windows += Window->WasActive && !Window->Hidden ? 1u : 0u;
	}
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

bool ToolUIToggle(const char* const Id, bool* const bValue)
{
	ImGuiWindow* const Window = ImGui::GetCurrentWindow();
	if (Window->SkipItems)
	{
		return false;
	}

	const float TrackHeight = std::round(ImGui::GetFontSize() * 0.9f);
	const float TrackWidth = std::round(TrackHeight * 1.8f);
	const float RowHeight = ImGui::GetFrameHeight();
	const float ItemWidth = std::max(TrackWidth, ImGui::CalcItemWidth());
	const ImVec2 Position{Window->DC.CursorPos.x + ItemWidth - TrackWidth, Window->DC.CursorPos.y};
	const ImRect Bounds{Position, {Position.x + TrackWidth, Position.y + RowHeight}};
	const ImGuiID WidgetId = Window->GetID(Id);
	ImGui::ItemSize({ItemWidth, RowHeight}, ImGui::GetStyle().FramePadding.y);
	if (!ImGui::ItemAdd(Bounds, WidgetId))
	{
		return false;
	}

	bool bHovered = false;
	bool bHeld = false;
	const bool bPressed = ImGui::ButtonBehavior(Bounds, WidgetId, &bHovered, &bHeld);
	if (bPressed)
	{
		*bValue = !*bValue;
		ImGui::MarkItemEdited(WidgetId);
	}

	ImGui::RenderNavCursor(Bounds, WidgetId);
	const float Radius = TrackHeight * 0.5f;
	const ImVec2 TrackMin{Bounds.Min.x, std::round(Bounds.GetCenter().y - Radius)};
	const ImVec2 TrackMax{Bounds.Max.x, TrackMin.y + TrackHeight};
	ImDrawList* const DrawList = Window->DrawList;
	if (*bValue)
	{
		DrawList->AddRectFilled(TrackMin, TrackMax, ImGui::GetColorU32(ImGuiCol_CheckMark, bHovered ? 1.f : 0.85f), Radius);
	}
	else
	{
		DrawList->AddRectFilled(TrackMin, TrackMax, ImGui::GetColorU32(bHovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), Radius);
		DrawList->AddRect(TrackMin, TrackMax, ImGui::GetColorU32(ImGuiCol_Border), Radius);
	}

	const float KnobRadius = Radius - std::max(2.f, std::round(TrackHeight * 0.15f));
	const float KnobX = *bValue ? TrackMax.x - Radius : TrackMin.x + Radius;
	DrawList->AddCircleFilled({KnobX, TrackMin.y + Radius}, KnobRadius, ToImGuiPackedColor(*bValue ? ToolUITheme::Surface0 : ToolUITheme::TextMuted));
	return bPressed;
}

bool FToolUIContext::IsPanelFocused(const std::string_view Name) const noexcept
{
	return IsToolUIPanelFocused(Name, Implementation->bViewportImmersive);
}

bool FToolUIContext::IsPanelHovered(const std::string_view Name) const noexcept
{
	return IsToolUIPanelHovered(Name, Implementation->bViewportImmersive);
}

bool FToolUIContext::IsDocumentInFront() const noexcept
{
	return Implementation->ActiveDocument != 0;
}

void FToolUIContext::FocusViewport() noexcept
{
	Implementation->ActiveDocument = 0;
	Implementation->bFocusViewportRequested = true;
}

void FToolUIContext::SetLevelTabLabel(const std::string_view Label)
{
	Implementation->LevelTabLabel = Label;
}

namespace
{
// The field being edited and its text and cursor after the previous callback. ImGui edits one field at a time.
struct FGraphemeInputState
{
	ImGuiID Field = 0;
	std::string Text;
	int Cursor = 0;
};

FGraphemeInputState GraphemeInput;

int GraphemeInputCallback(ImGuiInputTextCallbackData* const Data)
{
	auto& Text = *static_cast<std::string*>(Data->UserData);
	if (Data->EventFlag == ImGuiInputTextFlags_CallbackResize)
	{
		Text.resize(static_cast<std::size_t>(Data->BufTextLen));
		Data->Buf = Text.data();
		return 0;
	}

	const ImGuiID Field = ImGui::GetActiveID();
	const std::string_view Current(Data->Buf, static_cast<std::size_t>(Data->BufTextLen));
	if (Data->EventFlag == ImGuiInputTextFlags_CallbackEdit && GraphemeInput.Field == Field)
	{
		if (const std::optional<FTextEdit> Repaired = RepairGraphemeEdit(GraphemeInput.Text, Current))
		{
			Data->DeleteChars(0, Data->BufTextLen);
			Data->InsertChars(0, Repaired->Text.data(), Repaired->Text.data() + Repaired->Text.size());
			Data->CursorPos = static_cast<int>(Repaired->Cursor);
			Data->SelectionStart = Data->SelectionEnd = Data->CursorPos;
		}
	}
	else if (GraphemeInput.Field == Field)
	{
		// ImGui steps by code point; finishing the step in the same direction crosses the whole cluster.
		const bool bForward = Data->CursorPos > GraphemeInput.Cursor;
		const auto Snap = [&Current, bForward](const int Offset)
		{
			return static_cast<int>(SnapToGrapheme(Current, static_cast<std::size_t>(Offset), bForward));
		};

		Data->CursorPos = Snap(Data->CursorPos);
		Data->SelectionStart = Snap(Data->SelectionStart);
		Data->SelectionEnd = Snap(Data->SelectionEnd);
	}

	GraphemeInput.Field = Field;
	GraphemeInput.Text.assign(Data->Buf, static_cast<std::size_t>(Data->BufTextLen));
	GraphemeInput.Cursor = Data->CursorPos;
	return 0;
}
}

bool ToolUIInputText(const char* const Id, std::string& Text, const char* const Hint, const float Width)
{
	ImGui::SetNextItemWidth(Width > 0.f ? Width : -FLT_MIN);
	constexpr ImGuiInputTextFlags Flags = ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_CallbackAlways;
	return ImGui::InputTextWithHint(Id, Hint ? Hint : "", Text.data(), Text.capacity() + 1, Flags, GraphemeInputCallback, &Text);
}

bool ToolUIMenuItem(const std::string_view Label, const EToolUIMenuIcon Icon, bool* const bSelected, const char* const Shortcut)
{
	const float Scale = ImGui::GetFontSize() / ImGui::GetStyle().FontSizeBase;
	const float LabelWidth = ImGui::CalcTextSize(Label.data(), Label.data() + Label.size(), false).x + 36.f * Scale;
	const float ShortcutWidth = Shortcut != nullptr ? ImGui::CalcTextSize(Shortcut).x : 0.f;
	// Hidden labels still need to reserve their icon and text columns for menu auto-sizing.
	ImGui::GetCurrentWindow()->DC.MenuColumns.DeclColumns(0.f, LabelWidth, ShortcutWidth, ImGui::GetFontSize() * 1.2f);
	const std::string Display = std::format("##{}", Label);
	const bool bPressed = ImGui::MenuItem(Display.c_str(), Shortcut, bSelected);
	const ImVec2 Min = ImGui::GetItemRectMin();
	const ImVec2 Max = ImGui::GetItemRectMax();
	const ImVec2 Center{Min.x + 16.f * Scale, (Min.y + Max.y) * 0.5f};
	ImDrawList* const Draw = ImGui::GetWindowDrawList();
	Draw->AddText({Min.x + 36.f * Scale, Center.y - ImGui::GetFontSize() * 0.5f}, ImGui::GetColorU32(ImGuiCol_Text), Label.data(), Label.data() + Label.size());
	const ImU32 Color = ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled);
	DrawToolUIIcon(Draw, Center, Scale, Color, Icon);
	return bPressed;
}

bool ToolUIButton(const std::string_view Label, const EToolUIMenuIcon Icon, const float Height)
{
	const float Scale = ImGui::GetFontSize() / ImGui::GetStyle().FontSizeBase;
	const float Width = ImGui::CalcTextSize(Label.data(), Label.data() + Label.size()).x + 36.f * Scale;
	const std::string Id = std::format("##{}", Label);
	const bool bPressed = ImGui::Button(Id.c_str(), {Width, Height > 0.f ? Height : ImGui::GetFrameHeight()});
	const ImVec2 Minimum = ImGui::GetItemRectMin();
	const ImVec2 Maximum = ImGui::GetItemRectMax();
	const ImVec2 Center{Minimum.x + 12.f * Scale, (Minimum.y + Maximum.y) * 0.5f};
	ImDrawList* const Draw = ImGui::GetWindowDrawList();
	DrawToolUIIcon(Draw, Center, Scale, ImGui::GetColorU32(ImGuiCol_Text), Icon);
	Draw->AddText({Minimum.x + 24.f * Scale, Center.y - ImGui::GetFontSize() * 0.5f}, ImGui::GetColorU32(ImGuiCol_Text), Label.data(), Label.data() + Label.size());
	return bPressed;
}

void ToolUIIcon(const EToolUIMenuIcon Icon, const float CenterX, const float CenterY, const float Scale)
{
	DrawToolUIIcon(ImGui::GetWindowDrawList(), {CenterX, CenterY}, Scale, ImGui::GetColorU32(ImGuiCol_TextDisabled), Icon);
}

void ToolUIIcon(const EToolUIMenuIcon Icon, const float CenterX, const float CenterY, const float Scale, const std::uint32_t Color)
{
	DrawToolUIIcon(ImGui::GetWindowDrawList(), {CenterX, CenterY}, Scale, Color, Icon);
}

void FToolUIContext::DrawWorkspace(const std::string_view ApplicationTitle, const std::function<void()>& DrawWindowMenuItems, const std::function<void()>& DrawStatusItems, const std::function<void()>& DrawFileMenuItems, const std::function<void()>& DrawEditMenuItems, const std::function<void()>& DrawToolsMenuItems)
{
	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Q, ImGuiInputFlags_RouteGlobal))
	{
		Implementation->Window->RequestClose();
	}

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
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.f, 0.f});
	ImGui::Begin("HertaWorkspaceHost", nullptr, HostFlags);
	ImGui::PopStyleVar();

	ImDrawList* const DrawList = ImGui::GetWindowDrawList();
	const FTitleBarLayout& TitleBarLayout = Implementation->Window->GetTitleBarHitTestState().Layout;
	DrawList->PushClipRect(ViewportMinimum, ViewportMaximum, false);
	const float TitleBarHeight = static_cast<float>(TitleBarLayout.TitleBarHeight);
	const float ChromeScale = TitleBarHeight / static_cast<float>(DefaultTitleBarHeight);
	const float TitleBarBottom = ViewportMinimum.y + TitleBarHeight;
	const float StatusBarHeight = Implementation->Metrics.StatusBarHeight * ChromeScale;
	const float TabRowHeight = 34.f * ChromeScale;
	const float TabRowBottom = TitleBarBottom + TabRowHeight;
	Herta::DrawGlassSurface(*DrawList, *Viewport, Implementation->Appearance, ViewportMinimum, {ViewportMaximum.x, TabRowBottom}, 0.f);
	DrawList->AddLine({ViewportMinimum.x, TabRowBottom - 1.f}, {ViewportMaximum.x, TabRowBottom - 1.f}, ToImGuiPackedColor(ToolUITheme::BorderSoft));

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

	DrawList->AddCircle({SystemMinimum.x + 18.f * ChromeScale, SystemMinimum.y + 18.f * ChromeScale}, 7.f * ChromeScale, ToImGuiPackedColor(ToolUITheme::NeutralAccent), 24, 2.f * ChromeScale);
	DrawList->AddCircleFilled({SystemMinimum.x + 18.f * ChromeScale, SystemMinimum.y + 18.f * ChromeScale}, 2.f * ChromeScale, ToImGuiPackedColor(ToolUITheme::NeutralAccent));
	DrawWindowControls(*DrawList, ViewportMinimum, TitleBarLayout);

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.f * ChromeScale, 7.f * ChromeScale});
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.f * ChromeScale, 6.f * ChromeScale});
	if (ImGui::BeginMenuBar())
	{
		const float MenuTop = ViewportMinimum.y + (TitleBarHeight - ImGui::GetFontSize()) * 0.5f - ImGui::GetStyle().FramePadding.y;
		ImGui::SetCursorScreenPos({SystemMaximum.x + 4.f * ChromeScale, MenuTop});
		const ImVec2 MinimumMenuSize{220.f * ChromeScale + ImGui::GetStyle().WindowPadding.x * 2.f, 0.f};
		constexpr ImVec2 MaximumMenuSize{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
		ImGui::SetNextWindowSizeConstraints(MinimumMenuSize, MaximumMenuSize);
		const auto Text = [this](const FTextId Id, const std::string_view English)
		{
			return Implementation->Text ? Implementation->Text->Format(Id) : std::string(English);
		};

		// The visible label changes with the language; the ID after ### keeps each menu's identity.
		if (ImGui::BeginMenu(std::format("{}###File", Text(Editor::Menu_File, "File")).c_str()))
		{
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.f * ChromeScale, 6.f * ChromeScale});
			if (DrawFileMenuItems)
			{
				DrawFileMenuItems();
				ImGui::Spacing();
			}

			ImGui::TextUnformatted(Text(Editor::Menu_Workspace, "Workspace").c_str());
			ImGui::Separator();
			if (Implementation->Text && ImGui::BeginMenu(std::format("{}###Language", Text(Editor::Menu_Language, "Language")).c_str()))
			{
				// Each language shows in its own name, so someone who cannot read the current one still finds theirs.
				std::vector<std::string> Cultures = Implementation->Text->GetAvailableCultures();
				if (GetBuildConfiguration() != EBuildConfiguration::Shipping)
				{
					Cultures.emplace_back(PseudoCulture);
				}

				for (const std::string& Culture : Cultures)
				{
					bool bSelected = Implementation->Appearance.Culture == Culture;
					if (ToolUIMenuItem(Implementation->Text->FormatIn(Culture, Editor::Language_Name), EToolUIMenuIcon::Panel, &bSelected))
					{
						FEditorAppearance Appearance = Implementation->Appearance;
						Appearance.Culture = Culture;
						SetAppearance(std::move(Appearance));
					}
				}

				ImGui::EndMenu();
			}

			bool bVSync = Implementation->bVSync;
			if (ToolUIMenuItem(Text(Editor::Menu_VSync, "VSync"), EToolUIMenuIcon::Sync, &bVSync))
			{
				Implementation->bVSync = !Implementation->bVSync;
				if (Implementation->VSyncChanged)
				{
					Implementation->VSyncChanged(Implementation->bVSync);
				}
			}

			if (ToolUIMenuItem(Text(Editor::Menu_ResetLayout, "Reset layout"), EToolUIMenuIcon::Layout))
			{
				Implementation->bBuildDefaultLayout = true;
			}

			ImGui::Separator();
			if (ToolUIMenuItem(Text(Editor::Menu_Exit, "Exit"), EToolUIMenuIcon::Exit, nullptr, "Ctrl+Q"))
			{
				Implementation->Window->RequestClose();
			}

			ImGui::PopStyleVar();
			ImGui::EndMenu();
		}

		for (const auto& [Id, English] : {std::pair{Editor::Menu_Edit, "Edit"}, std::pair{Editor::Menu_Window, "Window"}, std::pair{Editor::Menu_Tools, "Tools"}, std::pair{Editor::Menu_Help, "Help"}})
		{
			const bool bWindowMenu = Id.Hash == Editor::Menu_Window.Hash;
			const bool bEditMenu = Id.Hash == Editor::Menu_Edit.Hash;
			const bool bToolsMenu = Id.Hash == Editor::Menu_Tools.Hash;
			ImGui::SetNextWindowSizeConstraints(MinimumMenuSize, MaximumMenuSize);
			if (ImGui::BeginMenu(std::format("{}###{}", Text(Id, English), English).c_str(), (bWindowMenu && static_cast<bool>(DrawWindowMenuItems)) || (bEditMenu && static_cast<bool>(DrawEditMenuItems)) || (bToolsMenu && static_cast<bool>(DrawToolsMenuItems))))
			{
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.f * ChromeScale, 6.f * ChromeScale});
				if (bWindowMenu)
				{
					ImGui::TextUnformatted(Text(Editor::Menu_Panels, "Panels").c_str());
					ImGui::Separator();
					DrawWindowMenuItems();
				}
				else if (bToolsMenu)
				{
					DrawToolsMenuItems();
				}
				else
				{
					DrawEditMenuItems();
				}

				ImGui::PopStyleVar();
				ImGui::EndMenu();
			}
		}

		Implementation->MainMenuRight = ImGui::GetItemRectMax().x + 8.f * ChromeScale;
		ImGui::EndMenuBar();
	}

	ImGui::PopStyleVar(2);

	const std::string Version = std::format("Herta {}", GetBuildRevision());

	const float RightEdge = [&]
	{
		float Edge = ViewportMinimum.x + static_cast<float>(TitleBarLayout.WindowWidth) - 12.f * ChromeScale;
		for (const ETitleBarHitRegion Region : {ETitleBarHitRegion::MinimizeButton, ETitleBarHitRegion::MaximizeButton, ETitleBarHitRegion::CloseButton})
		{
			const FTitleBarControlBounds Bounds = GetTitleBarControlBounds(TitleBarLayout, Region);
			if (Bounds.bVisible)
			{
				Edge = std::min(Edge, ViewportMinimum.x + static_cast<float>(Bounds.MinimumX) - 12.f * ChromeScale);
			}
		}

		return Edge;
	}();

	float TextRight = RightEdge;
	const auto DrawRightText = [&](const std::string_view Value, const ImU32 Color)
	{
		const float Width = ImGui::CalcTextSize(Value.data(), Value.data() + Value.size()).x;
		if (TextRight - Width > Implementation->MainMenuRight + 20.f * ChromeScale)
		{
			DrawList->AddText({TextRight - Width, ViewportMinimum.y + 10.f * ChromeScale}, Color, Value.data(), Value.data() + Value.size());
			TextRight -= Width + 24.f * ChromeScale;
		}
	};

	DrawRightText(Version, ToImGuiPackedColor(ToolUITheme::TextMuted));
	DrawRightText(ApplicationTitle, ToImGuiPackedColor(ToolUITheme::TextSecondary));
	const float Fps = ImGui::GetIO().Framerate;
	if (Fps > 0.f)
	{
		const std::string Values = std::format("{:.1f} / {:.1f} ms", Fps, 1000.f / Fps);
		const float LabelWidth = ImGui::CalcTextSize("FPS: ").x;
		const float Width = LabelWidth + ImGui::CalcTextSize(Values.c_str()).x;
		if (TextRight - Width > Implementation->MainMenuRight + 20.f * ChromeScale)
		{
			const ImVec2 Position{TextRight - Width, ViewportMinimum.y + 10.f * ChromeScale};
			DrawList->AddText(Position, ToImGuiPackedColor(ToolUITheme::TextMuted), "FPS: ");
			DrawList->AddText({Position.x + LabelWidth, Position.y}, ToImGuiPackedColor(ToolUITheme::TextPrimary), Values.c_str());
		}
	}

	DrawDocumentTabs(*Implementation, *DrawList, ViewportMinimum.x, TitleBarBottom, TabRowHeight, ChromeScale);
	Implementation->WorkspaceCanvas = ResolveToolUIWorkspaceCanvas({.X = ViewportMinimum.x, .Y = ViewportMinimum.y, .Width = Viewport->Size.x, .Height = Viewport->Size.y}, TabRowBottom - ViewportMinimum.y, StatusBarHeight);
	ImGui::SetCursorScreenPos({Implementation->WorkspaceCanvas.X, Implementation->WorkspaceCanvas.Y});
	const ImVec2 DockSize{std::max(1.f, Implementation->WorkspaceCanvas.Width), std::max(1.f, Implementation->WorkspaceCanvas.Height)};
	// Changing the ID migrates pre-Viewport layouts once while preserving user layouts from this version onward.
	Implementation->DockspaceId = ImGui::GetID("HertaEditorDockspaceV3");
	const bool bDockspaceMissing = ImGui::DockBuilderGetNode(Implementation->DockspaceId) == nullptr;
	ImVec4 DockspaceBackground = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
	// Panel surfaces supply their own opacity; the dock host must not cover the scene behind them.
	DockspaceBackground.w = 0.f;
	ImGui::PushStyleColor(ImGuiCol_WindowBg, DockspaceBackground);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.f * ChromeScale, ImGui::GetStyle().FramePadding.y});
	// A document in front keeps the level layout alive without drawing it, as the immersive viewport does.
	ImGui::DockSpace(Implementation->DockspaceId, DockSize, Implementation->bViewportImmersive || Implementation->ActiveDocument != 0 ? ImGuiDockNodeFlags_KeepAliveOnly : ImGuiDockNodeFlags_PassthruCentralNode);
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();
	if (Implementation->bBuildDefaultLayout || bDockspaceMissing)
	{
		ImGui::DockBuilderRemoveNode(Implementation->DockspaceId);
		ImGui::DockBuilderAddNode(Implementation->DockspaceId, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(Implementation->DockspaceId, DockSize);
		ImGuiID CenterId = Implementation->DockspaceId;
		// The side column spans the full height; content and log share the area under the Viewport.
		const float DetailsFraction = std::clamp(440.f * ChromeScale / DockSize.x, 0.2f, 0.4f);
		const ImGuiID SideId = ImGui::DockBuilderSplitNode(CenterId, ImGuiDir_Right, DetailsFraction, nullptr, &CenterId);
		const ImGuiID BottomId = ImGui::DockBuilderSplitNode(CenterId, ImGuiDir_Down, 0.36f, nullptr, &CenterId);
		ImGuiID DetailsId = SideId;
		const ImGuiID OutlinerId = ImGui::DockBuilderSplitNode(SideId, ImGuiDir_Up, 0.35f, nullptr, &DetailsId);
		if (ImGuiDockNode* const CenterNode = ImGui::DockBuilderGetNode(CenterId))
		{
			CenterNode->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
		}

		ImGui::DockBuilderDockWindow("Outliner", OutlinerId);
		ImGui::DockBuilderDockWindow("Details", DetailsId);
		ImGui::DockBuilderDockWindow("Performance", DetailsId);
		ImGui::DockBuilderDockWindow("World Settings", DetailsId);
		ImGui::DockBuilderDockWindow("Viewport", CenterId);
		ImGui::DockBuilderDockWindow("Content Browser", BottomId);
		ImGui::DockBuilderDockWindow("Output Log", BottomId);
		ImGui::DockBuilderFinish(Implementation->DockspaceId);
		Implementation->bBuildDefaultLayout = false;
		Implementation->bOutlinerDockMigrationComplete = true;
	}

	if (!Implementation->bOutlinerDockMigrationComplete)
	{
		Implementation->bOutlinerDockMigrationComplete = true;
		if (ImGui::FindWindowSettingsByID(ImHashStr("Outliner")) == nullptr)
		{
			const ImGuiWindowSettings* const AnchorSettings = ImGui::FindWindowSettingsByID(ImHashStr("Details"));
			if (AnchorSettings != nullptr && AnchorSettings->DockId != 0)
			{
				ImGuiDockNode* const DetailsNode = ImGui::DockBuilderGetNode(AnchorSettings->DockId);
				if (DetailsNode != nullptr && DetailsNode->IsLeafNode() && ImGui::DockNodeGetRootNode(DetailsNode)->ID == Implementation->DockspaceId)
				{
					const ImGuiID OutlinerId = ImGui::DockBuilderSplitNode(AnchorSettings->DockId, ImGuiDir_Up, 0.35f, nullptr, nullptr);
					ImGui::DockBuilderDockWindow("Outliner", OutlinerId);
					ImGui::DockBuilderFinish(Implementation->DockspaceId);
				}
			}
		}
	}

	const ImVec2 StatusMinimum{ViewportMinimum.x, ViewportMaximum.y - StatusBarHeight};
	Herta::DrawGlassSurface(*DrawList, *Viewport, Implementation->Appearance, StatusMinimum, ViewportMaximum, 0.f);
	DrawList->AddLine(StatusMinimum, {ViewportMaximum.x, StatusMinimum.y}, ToImGuiPackedColor(ToolUITheme::BorderSoft));
	ImGui::SetCursorScreenPos(StatusMinimum);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10.f * ChromeScale, 6.f * ChromeScale});
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

bool FToolUIContext::BeginPanel(const std::string_view Name, bool* const bOpen, const EToolUIPanelKind Kind)
{
	const bool bViewport = Kind == EToolUIPanelKind::Viewport;
	const bool bImmersive = bViewport && Implementation->bViewportImmersive;
	const ImGuiID WindowId = bImmersive ? ImHashStr(ImmersiveViewportName) : ImHashStr(Name.data(), Name.size());
	const bool bDocument = Kind == EToolUIPanelKind::Document;
	if (bDocument)
	{
		const std::string_view TabLabel = Name.substr(0, Name.find("###"));
		const auto Existing = std::ranges::find(Implementation->Documents, WindowId, &FToolUIDocument::Id);
		if (Existing == Implementation->Documents.end())
		{
			// As in Unreal, a newly opened asset editor comes to the front.
			Implementation->Documents.push_back({.Id = WindowId, .Label = std::string(TabLabel), .LastFrame = ImGui::GetFrameCount()});
			Implementation->ActiveDocument = WindowId;
			Implementation->FocusRequestedDocument = WindowId;
		}
		else
		{
			Existing->Label = TabLabel;
			Existing->LastFrame = ImGui::GetFrameCount();
		}

		// A panel that asks for focus is reopening an asset that is already open.
		if ((GImGui->NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasFocus) != 0)
		{
			Implementation->ActiveDocument = WindowId;
		}

		if (Implementation->CloseRequestedDocument == WindowId)
		{
			Implementation->CloseRequestedDocument = 0;
			if (bOpen)
			{
				*bOpen = false;
			}
		}
	}

	// Only the tab in front draws: one document, or the level panels.
	const bool bBehindTab = bDocument ? WindowId != Implementation->ActiveDocument : Implementation->ActiveDocument != 0 && Kind != EToolUIPanelKind::Drawer;
	if (bBehindTab)
	{
		GImGui->NextWindowData.ClearFlags();
		Implementation->PanelBegun.push_back(false);
		return false;
	}

	const auto Previous = Implementation->PreviousDockState.find(WindowId);
	const bool bPreviouslyDocked = bDocument || (Previous != Implementation->PreviousDockState.end() && Previous->second);
	if (Name == "Content Browser" && ImGui::FindWindowSettingsByID(WindowId) == nullptr)
	{
		// Add the new browser beside the log without resetting an existing workspace.
		const auto* const LogSettings = ImGui::FindWindowSettingsByID(ImHashStr("Output Log"));
		if (LogSettings && LogSettings->DockId != 0)
		{
			ImGui::SetNextWindowDockID(LogSettings->DockId, ImGuiCond_FirstUseEver);
		}
	}

	ImGui::SetNextWindowBgAlpha(ResolveToolUIPanelBackgroundAlpha(Implementation->Appearance.PanelTransparency, bPreviouslyDocked, bViewport));
	if (bViewport)
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});

		// Keep the main viewport anchored, including layouts saved while it was floating.
		if (const ImGuiDockNode* const CenterNode = ImGui::DockBuilderGetCentralNode(Implementation->DockspaceId))
		{
			ImGui::SetNextWindowDockID(CenterNode->ID, ImGuiCond_Always);
		}
	}

	// ImGui gives a window its own native window only when a user drags it out. A floated panel briefly refuses to merge into the main window instead, which needs a frame to undock and one to take the new viewport.
	ImGuiWindowClass Class;
	if (const auto Float = Implementation->FloatRequests.find(std::string(Name)); Float != Implementation->FloatRequests.end() && !bViewport)
	{
		ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
		ImGui::SetNextWindowPos({Float->second.Bounds.X, Float->second.Bounds.Y}, ImGuiCond_Always);
		ImGui::SetNextWindowSize({Float->second.Bounds.Width, Float->second.Bounds.Height}, ImGuiCond_Always);
		Class.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
		if (--Float->second.Frames == 0)
		{
			Implementation->FloatRequests.erase(Float);
		}
	}

	ImGui::SetNextWindowClass(&Class);

	// Documents and drawers fill the workspace below the title bar instead of docking.
	const FToolUICanvasBounds& Canvas = Implementation->WorkspaceCanvas;
	if (bDocument)
	{
		ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
		ImGui::SetNextWindowPos({Canvas.X, Canvas.Y}, ImGuiCond_Always);
		ImGui::SetNextWindowSize({Canvas.Width, Canvas.Height}, ImGuiCond_Always);
		if (Implementation->FocusRequestedDocument == WindowId)
		{
			Implementation->FocusRequestedDocument = 0;
			ImGui::SetNextWindowFocus();
		}
	}

	if (Kind == EToolUIPanelKind::Drawer)
	{
		const float Height = std::round(Canvas.Height * 0.4f);
		ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
		ImGui::SetNextWindowPos({Canvas.X, Canvas.Y + Canvas.Height - Height}, ImGuiCond_Always);
		ImGui::SetNextWindowSize({Canvas.Width, Height}, ImGuiCond_Always);
	}

	if ((Name == "Performance" || Name == "World Settings") && ImGui::FindWindowSettingsByID(WindowId) == nullptr)
	{
		// Add the panel as a tab beside Details without resetting an existing workspace.
		const auto* const DetailsSettings = ImGui::FindWindowSettingsByID(ImHashStr("Details"));
		if (DetailsSettings && DetailsSettings->DockId != 0)
		{
			ImGui::SetNextWindowDockID(DetailsSettings->DockId, ImGuiCond_FirstUseEver);
		}
	}

	const std::string Label = Name == "Outliner" || Name == "Details" || Name == "Content Browser" || Name == "Performance" || Name == "Output Log" || Name == "World Settings" ? std::format("      {}###{}", Name, Name) : std::string(Name);
	constexpr ImGuiWindowFlags DrawerFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
	// A document must rise above the full-window workspace host to receive the mouse, so it comes to the front when focused; drawers open focused above it.
	constexpr ImGuiWindowFlags DocumentFlags = DrawerFlags | ImGuiWindowFlags_NoTitleBar;
	const ImGuiWindowFlags KindFlags = Kind == EToolUIPanelKind::Drawer ? DrawerFlags : bDocument ? DocumentFlags
	                                                                                              : ImGuiWindowFlags_None;
	const ImGuiWindowFlags Flags = (bViewport ? ImGuiWindowFlags_NoMove : ImGuiWindowFlags_None) | KindFlags | (Implementation->bViewportImmersive ? ImGuiWindowFlags_NoInputs : ImGuiWindowFlags_None);

	if (bImmersive)
	{
		// Submit the original window so docking retains its place while the overlay is active.
		ImGui::Begin(Label.c_str(), bOpen, Flags);
		ImGui::End();
	}

	if (bViewport && !bImmersive && Implementation->bFocusViewportRequested)
	{
		ImGui::SetNextWindowFocus();
	}

	const bool bVisible = bImmersive ? BeginImmersiveViewport(Implementation->WorkspaceCanvas, Implementation->bFocusViewportRequested) : ImGui::Begin(Label.c_str(), bOpen, Flags);
	Implementation->PanelBegun.push_back(true);

	// A drawer is a quick pick: it closes when focus leaves it or on Escape. Dragging an asset out must not close it before the drop lands, and a click elsewhere closes it on release so the button that toggles it does not reopen it.
	if (Kind == EToolUIPanelKind::Drawer && bOpen && *bOpen)
	{
		const bool bKeep = ImGui::IsWindowAppearing() || ImGui::IsAnyMouseDown() || ImGui::GetDragDropPayload() != nullptr || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
		if (!bKeep && (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false))))
		{
			*bOpen = false;
		}
	}
	if (bViewport)
	{
		Implementation->bFocusViewportRequested = false;
	}

	if (bViewport)
	{
		ImGui::PopStyleVar();
	}

	Implementation->PreviousDockState[WindowId] = ImGui::IsWindowDocked();
	if (ImGuiDockNode* const Node = ImGui::GetCurrentWindow()->DockNode)
	{
		Node->LocalFlags |= ImGuiDockNodeFlags_NoWindowMenuButton | ImGuiDockNodeFlags_NoCloseButton;
	}

	if (bVisible && (!Implementation->bViewportImmersive || bViewport))
	{
		Implementation->PresentedPanels[WindowId] = bViewport;
	}

	// The viewport alone needs no tab; once documents share its node, the tab bar switches between them.
	if (bViewport)
	{
		if (ImGuiDockNode* const Node = ImGui::GetCurrentWindow()->DockNode)
		{
			if (Node->Windows.Size > 1)
			{
				Node->LocalFlags &= ~ImGuiDockNodeFlags_NoTabBar;
			}
			else
			{
				Node->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
			}
		}
	}

	return bVisible && (!Implementation->bViewportImmersive || bViewport);
}

void FToolUIContext::EndPanel()
{
	const bool bBegun = Implementation->PanelBegun.back();
	Implementation->PanelBegun.pop_back();
	if (bBegun)
	{
		ImGui::End();
	}
}

void FToolUIContext::MigrateContentDockHeight(const std::uint32_t DockspaceId)
{
	const auto* const LogSettings = ImGui::FindWindowSettingsByID(ImHashStr("Output Log"));
	const auto* const ViewportSettings = ImGui::FindWindowSettingsByID(ImHashStr("Viewport"));
	ImGuiDockNode* const Bottom = LogSettings ? ImGui::DockBuilderGetNode(LogSettings->DockId) : nullptr;
	ImGuiDockNode* const Parent = Bottom ? Bottom->ParentNode : nullptr;
	ImGuiDockNode* const Center = Parent ? Parent->ChildNodes[0] : nullptr;
	if (!Bottom || !Parent || !Center || !ViewportSettings || !Bottom->IsLeafNode() || !Center->IsCentralNode()
	    || Parent->SplitAxis != ImGuiAxis_Y || Parent->ChildNodes[1] != Bottom
	    || ViewportSettings->DockId != Center->ID || ImGui::DockNodeGetRootNode(Bottom)->ID != DockspaceId
	    || Parent->Size.y <= ImGui::GetStyle().DockingSeparatorSize
	    || std::abs(Bottom->Size.y - (Parent->Size.y - ImGui::GetStyle().DockingSeparatorSize) * 0.26f) > 1.f)
	{
		return;
	}

	for (auto* Settings = GImGui->SettingsWindows.begin(); Settings != nullptr; Settings = GImGui->SettingsWindows.next_chunk(Settings))
	{
		if (Settings->DockId == Bottom->ID && Settings->ID != ImHashStr("Output Log") && Settings->ID != ImHashStr("Content Browser"))
		{
			return;
		}
	}

	// Only the original split is migrated; other saved panel sizes and placements stay authored by the user.
	const float AvailableHeight = Parent->Size.y - ImGui::GetStyle().DockingSeparatorSize;
	const float BottomHeight = AvailableHeight * 0.3f;
	ImGui::DockBuilderSetNodeSize(Bottom->ID, {Bottom->Size.x, BottomHeight});
	ImGui::DockBuilderSetNodeSize(Center->ID, {Center->Size.x, AvailableHeight - BottomHeight});
	ImGui::MarkIniSettingsDirty();
}

bool FToolUIContext::OrderContentDockTabs()
{
	const ImGuiWindow* const Browser = ImGui::FindWindowByID(ImHashStr("Content Browser"));
	const ImGuiWindow* const Log = ImGui::FindWindowByID(ImHashStr("Output Log"));
	if (!Browser || !Log || !Browser->DockNode || Browser->DockNode != Log->DockNode || !Browser->DockNode->TabBar)
	{
		return false;
	}

	ImGuiTabBar* const Bar = Browser->DockNode->TabBar;
	ImGuiTabItem* const Tab = ImGui::TabBarFindTabByID(Bar, Browser->TabId);
	if (!Tab || !ImGui::TabBarFindTabByID(Bar, Log->TabId) || Bar->ReorderRequestTabId != 0)
	{
		return false;
	}

	const int Order = ImGui::TabBarGetTabOrder(Bar, Tab);
	if (Order > 0)
	{
		ImGui::TabBarQueueReorder(Bar, Tab, -Order);
	}

	return true;
}

void FToolUIContext::SetCursorCaptured(const bool bCaptured)
{
	if (bCaptured && !Implementation->bCursorCaptured)
	{
		ImGui::SetCurrentContext(Implementation->Context);
		const ImGuiID Id = Implementation->bViewportImmersive ? ImHashStr(ImmersiveViewportName) : ImHashStr("Viewport");
		const ImGuiWindow* const Panel = ImGui::FindWindowByID(Id);
		Implementation->CapturedMousePosition = Panel ? ImVec2{Panel->Pos.x + Panel->Size.x * 0.5f, Panel->Pos.y + Panel->Size.y * 0.5f} : ImGui::GetIO().MousePos;
		Implementation->CapturedCursor.reset();
		Implementation->CapturedMotion = {};
		Implementation->CapturedFrameMotion = {};
		ImGui::GetIO().AddMousePosEvent(Implementation->CapturedMousePosition.x, Implementation->CapturedMousePosition.y);
	}

	Implementation->bCursorCaptured = bCaptured;
	if (Implementation->Window)
	{
		Implementation->Window->SetCursorCaptured(bCaptured);
	}
}

FToolUIViewportPosition FToolUIContext::GetCapturedMouseMotion() const noexcept
{
	return Implementation->bCursorCaptured ? Implementation->CapturedFrameMotion : FToolUIViewportPosition{};
}

void FToolUIContext::SetViewportImmersive(const bool bImmersive) noexcept
{
	if (Implementation->bViewportImmersive == bImmersive)
	{
		return;
	}

	Implementation->bViewportImmersive = bImmersive;
	Implementation->bFocusViewportRequested = true;
}

bool FToolUIContext::IsViewportImmersive() const noexcept
{
	return Implementation->bViewportImmersive;
}

void FToolUIContext::FloatPanel(const std::string_view Name, const FToolUICanvasBounds Bounds)
{
	Implementation->FloatRequests.insert_or_assign(std::string(Name), FImplementation::FFloatRequest{.Bounds = Bounds});
}

void FToolUIContext::ResetLayout() noexcept
{
	Implementation->bBuildDefaultLayout = true;
}

std::size_t FToolUIContext::GetPlatformWindowCount() const noexcept
{
	ImGui::SetCurrentContext(Implementation->Context);
	return static_cast<std::size_t>(ImGui::GetPlatformIO().Viewports.Size);
}

FToolUICanvasBounds FToolUIContext::GetMainWindowBounds() const noexcept
{
	ImGui::SetCurrentContext(Implementation->Context);
	const ImGuiViewport* const Main = ImGui::GetMainViewport();
	return {.X = Main->Pos.x, .Y = Main->Pos.y, .Width = Main->Size.x, .Height = Main->Size.y};
}

std::vector<std::filesystem::path> FToolUIContext::TakeDroppedFiles()
{
	return std::exchange(Implementation->DroppedFiles, {});
}

std::optional<FToolUICanvasBounds> FToolUIContext::GetWorkspaceCanvasForCurrentPanel() const noexcept
{
	const ImGuiWindow* const Window = ImGui::GetCurrentWindow();
	if (Implementation->bViewportImmersive && Window->ID == ImHashStr(ImmersiveViewportName))
	{
		return Implementation->WorkspaceCanvas;
	}

	if (Window->Viewport != ImGui::GetMainViewport() || Window->DockNode == nullptr || ImGui::DockNodeGetRootNode(Window->DockNode)->ID != Implementation->DockspaceId)
	{
		return std::nullopt;
	}

	return Implementation->WorkspaceCanvas;
}

void FToolUIContext::DrawGlassSurface(const float X, const float Y, const float Width, const float Height, const float Radius) const
{
	if (Width <= 0.f || Height <= 0.f)
	{
		return;
	}

	Herta::DrawGlassSurface(*ImGui::GetWindowDrawList(), *ImGui::GetWindowViewport(), Implementation->Appearance, {X, Y}, {X + Width, Y + Height}, Radius);
}

bool FToolUIContext::DrawSearchField(const char* const Label, const char* const Hint, char* const Buffer, const std::size_t BufferSize) const
{
	const float Scale = ImGui::GetFontSize() / Implementation->Metrics.BaseFontSize;
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {32.f * Scale, 5.f * Scale});
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFontSize() * 0.5f + 5.f * Scale);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f * Scale);
	ImGui::PushStyleColor(ImGuiCol_Border, ToImGuiColor(ToolUITheme::Border));
	const bool bChanged = ImGui::InputTextWithHint(Label, Hint, Buffer, BufferSize);
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(3);

	const ImVec2 Minimum = ImGui::GetItemRectMin();
	const ImVec2 Maximum = ImGui::GetItemRectMax();
	const ImVec2 Center{Minimum.x + 13.5f * Scale, (Minimum.y + Maximum.y) * 0.5f - 1.5f * Scale};
	ImDrawList* const DrawList = ImGui::GetWindowDrawList();
	DrawList->PushClipRect(Minimum, Maximum, true);
	DrawToolUIIcon(DrawList, {Center.x + 1.1f * Scale, Center.y + 1.1f * Scale}, Scale * 0.9f, ImGui::GetColorU32(ImGuiCol_TextDisabled), EToolUIMenuIcon::Search);
	DrawList->PopClipRect();
	return bChanged;
}

void FToolUIContext::SetText(const FLocalization* const Text) noexcept
{
	Implementation->Text = Text;
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
