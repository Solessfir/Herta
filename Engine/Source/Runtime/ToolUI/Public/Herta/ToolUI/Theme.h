#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace Herta
{
struct FToolUIColor
{
	std::uint8_t Red = 0;
	std::uint8_t Green = 0;
	std::uint8_t Blue = 0;
	std::uint8_t Alpha = 255;

	[[nodiscard]] constexpr bool operator==(const FToolUIColor&) const noexcept = default;
};

struct FToolUIGradient
{
	FToolUIColor TopLeft;
	FToolUIColor TopRight;
	FToolUIColor BottomLeft;
	FToolUIColor BottomRight;
};

enum class EPanelTransparency : std::uint8_t
{
	AllPanels,
	FloatingOnly,
	DockedOnly,
	Disabled
};

struct FEditorAppearance
{
	FToolUIColor Accent{184, 184, 184, 255};
	float GradientHeight = 0.5f;
	float Saturation = 0.8f;
	float Intensity = 0.15f;
	float PanelOpacity = 0.95f;
	float BlurRadius = 24.f;
	EPanelTransparency PanelTransparency = EPanelTransparency::AllPanels;

	[[nodiscard]] constexpr bool operator==(const FEditorAppearance&) const noexcept = default;
};

struct FToolUIThemeMetrics
{
	float BaseFontSize = 15.625f;
	float TitleBarHeight = 36.f;
	float StatusBarHeight = 38.f;
	float WindowPadding = 12.f;
	float WindowRounding = 6.f;
	float ChildRounding = 6.f;
	float PopupRounding = 5.f;
	float FrameRounding = 4.f;
	float PrimaryButtonRounding = 8.f;
	float TitleBarControlRounding = 4.f;
	float ScrollbarSize = 12.f;
	float ScrollbarRounding = 5.f;
};

struct FToolUIColorPreset
{
	std::string_view Name;
	FToolUIColor Color;
};

namespace ToolUITheme
{
inline constexpr FToolUIColor Canvas{25, 25, 25, 255};
inline constexpr FToolUIColor TitleBar{27, 27, 27, 255};
inline constexpr FToolUIColor Surface0{23, 23, 23, 255};
inline constexpr FToolUIColor Surface1{28, 28, 28, 255};
inline constexpr FToolUIColor Surface2{36, 36, 36, 255};
inline constexpr FToolUIColor SurfaceHover{44, 44, 44, 255};
inline constexpr FToolUIColor Border{52, 52, 52, 255};
inline constexpr FToolUIColor BorderSoft{37, 37, 37, 255};
inline constexpr FToolUIColor TextPrimary{255, 255, 255, 255};
inline constexpr FToolUIColor TextSecondary{190, 190, 193, 255};
inline constexpr FToolUIColor TextMuted{148, 148, 152, 255};
inline constexpr FToolUIColor NeutralAccent{205, 205, 210, 255};
inline constexpr FToolUIColor NeutralAccentHover{232, 232, 235, 255};
inline constexpr FToolUIColor TitleBarControlHover{255, 255, 255, 24};
inline constexpr FToolUIColor CloseHover{196, 43, 28, 255};
inline constexpr FToolUIColor Warning{246, 196, 101, 255};
inline constexpr FToolUIColor Error{255, 125, 125, 255};
inline constexpr float TrailingIntensityRatio = 0.4f;
inline constexpr float UnfocusedSaturationRatio = 0.9f;
inline constexpr float SubtleTint = 0.06f;
inline constexpr float HoverTint = 0.1f;
inline constexpr float ActiveTint = 0.16f;
inline constexpr float StrongTint = 0.24f;

inline constexpr std::array Presets = {
    FToolUIColorPreset{"Gray", FEditorAppearance{}.Accent},
    FToolUIColorPreset{"Amber", {232, 139, 118, 255}},
    FToolUIColorPreset{"Rust", {205, 148, 30, 255}},
    FToolUIColorPreset{"Olive", {143, 179, 87, 255}},
    FToolUIColorPreset{"Grass", {77, 177, 122, 255}},
    FToolUIColorPreset{"Ocean", {46, 169, 183, 255}},
    FToolUIColorPreset{"Sky", {67, 164, 210, 255}},
    FToolUIColorPreset{"Cobalt", {84, 108, 232, 255}},
    FToolUIColorPreset{"Violet", {147, 80, 220, 255}},
    FToolUIColorPreset{"Plum", {194, 83, 177, 255}},
};
}

[[nodiscard]] constexpr float ClampToolUIUnit(const float Value) noexcept
{
	return Value < 0.f ? 0.f : Value > 1.f ? 1.f
	                                       : Value;
}

[[nodiscard]] constexpr FToolUIColor MixToolUIColor(const FToolUIColor Left, const FToolUIColor Right, const float Amount) noexcept
{
	const float ClampedAmount = ClampToolUIUnit(Amount);
	const auto MixChannel = [ClampedAmount](const std::uint8_t First, const std::uint8_t Second) constexpr
	{
		return static_cast<std::uint8_t>(static_cast<float>(First) + (static_cast<float>(Second) - static_cast<float>(First)) * ClampedAmount + 0.5f);
	};

	return {MixChannel(Left.Red, Right.Red), MixChannel(Left.Green, Right.Green), MixChannel(Left.Blue, Right.Blue), MixChannel(Left.Alpha, Right.Alpha)};
}

[[nodiscard]] constexpr FToolUIColor ResolveToolUISaturation(const FToolUIColor Color, const float Saturation) noexcept
{
	const std::uint8_t Maximum = Color.Red > Color.Green ? (Color.Red > Color.Blue ? Color.Red : Color.Blue) : (Color.Green > Color.Blue ? Color.Green : Color.Blue);
	return MixToolUIColor({Maximum, Maximum, Maximum, Color.Alpha}, Color, Saturation);
}

[[nodiscard]] constexpr FToolUIColor AdditiveToolUITint(const FToolUIColor Base, const FToolUIColor Tint, const float Strength) noexcept
{
	const auto AddChannel = [Strength](const std::uint8_t BaseChannel, const std::uint8_t TintChannel) constexpr
	{
		const float Value = static_cast<float>(BaseChannel) + static_cast<float>(TintChannel) * ClampToolUIUnit(Strength);
		return static_cast<std::uint8_t>(Value > 255.f ? 255.f : Value + 0.5f);
	};

	return {AddChannel(Base.Red, Tint.Red), AddChannel(Base.Green, Tint.Green), AddChannel(Base.Blue, Tint.Blue), Base.Alpha};
}

[[nodiscard]] constexpr FToolUIGradient ResolveToolUIGradient(const FEditorAppearance& Appearance, const bool bFocused) noexcept
{
	const float FocusSaturation = bFocused ? Appearance.Saturation : Appearance.Saturation * ToolUITheme::UnfocusedSaturationRatio;
	const FToolUIColor Accent = ResolveToolUISaturation(Appearance.Accent, FocusSaturation);
	return {
	    MixToolUIColor(ToolUITheme::Canvas, Accent, Appearance.Intensity),
	    MixToolUIColor(ToolUITheme::Canvas, Accent, Appearance.Intensity * ToolUITheme::TrailingIntensityRatio),
	    ToolUITheme::Canvas,
	    ToolUITheme::Canvas,
	};
}

[[nodiscard]] constexpr bool IsToolUIPanelTransparent(const EPanelTransparency Mode, const bool bDocked) noexcept
{
	switch (Mode)
	{
		case EPanelTransparency::AllPanels:
			return true;
		case EPanelTransparency::FloatingOnly:
			return !bDocked;
		case EPanelTransparency::DockedOnly:
			return bDocked;
		case EPanelTransparency::Disabled:
			return false;
	}

	return false;
}

[[nodiscard]] constexpr float ResolveToolUIPanelBackgroundAlpha(const EPanelTransparency Mode, const bool bDocked, const bool bSceneViewport) noexcept
{
	// Scene images and glass surfaces are composed separately from ImGui window backgrounds.
	return bSceneViewport || IsToolUIPanelTransparent(Mode, bDocked) ? 0.f : 1.f;
}
}
