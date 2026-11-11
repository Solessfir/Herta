#include "Herta/GameFramework/GameHud.h"

#include "Herta/GameFramework/GameControls.h"
#include "Herta/GameFramework/GameText.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace Herta
{
std::string FormatGameSeconds(const FCulture& Culture, const double Seconds)
{
	constexpr FNumberFormat Hundredths{.MinimumFractionDigits = 2, .MaximumFractionDigits = 2, .bGrouping = false};
	// Rounding first keeps 59.999 from showing as 0:60.00.
	const double Rounded = std::round(std::max(Seconds, 0.) * 100.) / 100.;
	const auto Minutes = static_cast<int>(Rounded / 60.);
	if (Minutes == 0)
	{
		return Culture.FormatNumber(Rounded, Hundredths);
	}

	const double Remainder = Rounded - Minutes * 60.;
	return std::format("{}:{}{}", Minutes, Remainder < 10. ? "0" : "", Culture.FormatNumber(Remainder, Hundredths));
}

namespace
{
// Posture reads from yellow toward a hot red as it nears breaking, as Sekiro's bar does.
[[nodiscard]] std::uint32_t PostureColor(const float Fraction)
{
	const float Heat = std::clamp((Fraction - 0.4f) / 0.6f, 0.f, 1.f);
	return MakeGameUIColor(255, static_cast<std::uint8_t>(206.f - 150.f * Heat), static_cast<std::uint8_t>(64.f - 40.f * Heat));
}

// A bar that fills outward from its middle; empty, it is not drawn at all.
void DrawPostureBar(FGameUI& UI, const FGameUIPlacement& Placement, const float Width, const float Fraction)
{
	if (Fraction <= 0.f)
	{
		return;
	}

	constexpr float Height = 12.f;
	UI.DrawRect(Placement, {Width + 6.f, Height + 6.f}, MakeGameUIColor(10, 10, 12, 190), 4.f);
	UI.DrawRect(Placement, {Width * std::min(Fraction, 1.f), Height}, PostureColor(Fraction), 3.f);
	// The middle tick marks where the two halves meet.
	UI.DrawRect(Placement, {3.f, Height + 6.f}, MakeGameUIColor(255, 244, 220, Fraction >= 1.f ? 255 : 150));
}

// A panel in the corner, clear of the target's posture bar at the top center: a column of key caps and a column of what they do.
void DrawControlHints(FGameUI& UI, const std::span<const FGameControlHint> Hints)
{
	constexpr FVector2 Anchor{1.f, 0.f};
	constexpr FVector2 Padding{14.f, 11.f};
	constexpr float Margin = 24.f;
	constexpr float KeyHeight = 30.f;
	constexpr float KeyPadding = 9.f;
	constexpr float KeyGap = 5.f;
	constexpr float RowHeight = 37.f;
	constexpr float ColumnGap = 14.f;
	const auto KeyWidth = [&](const std::string_view Key)
	{
		return std::max(UI.MeasureText(Key, EGameUIFont::Small).X + KeyPadding * 2.f, KeyHeight);
	};

	float KeysWidth = 0.f;
	float ActionWidth = 0.f;
	for (const FGameControlHint& Hint : Hints)
	{
		float Width = KeyGap * static_cast<float>(Hint.Keys.size() - 1);
		for (const std::string& Key : Hint.Keys)
		{
			Width += KeyWidth(Key);
		}

		KeysWidth = std::max(KeysWidth, Width);
		ActionWidth = std::max(ActionWidth, UI.MeasureText(Hint.Action, EGameUIFont::Small).X);
	}

	const FVector2 PanelSize{KeysWidth + ColumnGap + ActionWidth + Padding.X * 2.f, RowHeight * static_cast<float>(Hints.size()) - (RowHeight - KeyHeight) + Padding.Y * 2.f};
	UI.DrawRect({.Anchor = Anchor, .Pivot = {1.f, 0.f}, .Offset = {-Margin, Margin}}, PanelSize, MakeGameUIColor(12, 14, 20, 170), 11.f);

	const float Left = -Margin - PanelSize.X + Padding.X;
	float CenterY = Margin + Padding.Y + KeyHeight * 0.5f;
	for (const FGameControlHint& Hint : Hints)
	{
		float X = Left;
		for (const std::string& Key : Hint.Keys)
		{
			// The darker lip under the face reads as the side of a physical key.
			const float Width = KeyWidth(Key);
			UI.DrawRect({.Anchor = Anchor, .Pivot = {0.f, 0.5f}, .Offset = {X, CenterY + 2.f}}, {Width, KeyHeight}, MakeGameUIColor(122, 128, 144), 5.f);
			UI.DrawRect({.Anchor = Anchor, .Pivot = {0.f, 0.5f}, .Offset = {X, CenterY}}, {Width, KeyHeight}, MakeGameUIColor(238, 240, 246), 5.f);
			UI.DrawText(Key, {.Anchor = Anchor, .Pivot = {0.5f, 0.5f}, .Offset = {X + Width * 0.5f, CenterY}}, EGameUIFont::Small, MakeGameUIColor(18, 20, 28));
			X += Width + KeyGap;
		}

		UI.DrawText(Hint.Action, {.Anchor = Anchor, .Pivot = {0.f, 0.5f}, .Offset = {Left + KeysWidth + ColumnGap, CenterY}}, EGameUIFont::Small, MakeGameUIColor(228, 232, 240));
		CenterY += RowHeight;
	}
}
}

void DrawCombatHud(FGameUI& UI, const FGameCombatView& Combat)
{
	constexpr float Margin = 24.f;
	constexpr FVector2 HealthSize{360.f, 10.f};
	const FGameUIPlacement Health{.Anchor = {0.f, 1.f}, .Pivot = {0.f, 1.f}, .Offset = {Margin, -Margin}};
	UI.DrawRect(Health, {HealthSize.X + 6.f, HealthSize.Y + 6.f}, MakeGameUIColor(10, 10, 12, 190), 4.f);
	UI.DrawRect({.Anchor = Health.Anchor, .Pivot = Health.Pivot, .Offset = {Margin + 3.f, -Margin - 3.f}}, {HealthSize.X * std::clamp(Combat.Health, 0.f, 1.f), HealthSize.Y}, MakeGameUIColor(196, 32, 38), 3.f);
	DrawPostureBar(UI, {.Anchor = {0.5f, 1.f}, .Pivot = {0.5f, 0.5f}, .Offset = {0.f, -120.f}}, 440.f, Combat.Posture);
	if (!Combat.Target)
	{
		return;
	}

	DrawPostureBar(UI, {.Anchor = {0.5f, 0.f}, .Pivot = {0.5f, 0.5f}, .Offset = {0.f, 64.f}}, 360.f, Combat.Target->Posture);
	UI.DrawRect({.Anchor = {0.5f, 0.f}, .Pivot = {0.5f, 0.5f}, .Offset = {0.f, 84.f}}, {200.f * std::clamp(Combat.Target->Health, 0.f, 1.f), 4.f}, MakeGameUIColor(196, 32, 38, 220), 2.f);
	if (Combat.Target->bDeathblow)
	{
		UI.DrawCircle({.Anchor = {0.5f, 0.f}, .Pivot = {0.5f, 0.5f}, .Offset = {0.f, 36.f}}, 22.f, MakeGameUIColor(220, 20, 30));
		UI.DrawCircle({.Anchor = {0.5f, 0.f}, .Pivot = {0.5f, 0.5f}, .Offset = {0.f, 36.f}}, 10.f, MakeGameUIColor(255, 210, 200));
	}
}

std::optional<std::size_t> DrawGameHud(FGameUI& UI, const FLocalization& Text, const bool bPaused, const std::span<const std::string_view> MenuItems, std::size_t& MenuFocus, const FGameUIInput& MenuInput, const std::string_view MenuKey, const std::span<const FGameControlHint> ControlHints)
{
	const std::uint32_t TextColor = MakeGameUIColor(238, 240, 246);
	namespace Game = Text::Game;
	if (!bPaused)
	{
		const std::vector<FGameControlHint> DefaultHints = ControlHints.empty() ? GetGameControlHints(Text, GetDefaultGameBindings(), MenuKey) : std::vector<FGameControlHint>{};
		DrawControlHints(UI, ControlHints.empty() ? std::span<const FGameControlHint>(DefaultHints) : ControlHints);
		return std::nullopt;
	}

	if (MenuItems.empty())
	{
		return std::nullopt;
	}

	// Oversized so the dimming covers the screen at any aspect ratio.
	UI.DrawRect({.Anchor = {0.5f, 0.5f}, .Pivot = {0.5f, 0.5f}}, {100000.f, 100000.f}, MakeGameUIColor(0, 0, 0, 140));
	UI.DrawText(Text.Format(Game::Menu_Title_Paused), {.Anchor = {0.5f, 0.5f}, .Pivot = {0.5f, 1.f}, .Offset = {0.f, -200.f}}, EGameUIFont::Title, TextColor);
	return UI.DrawMenu(MenuItems, MenuFocus, MenuInput);
}
}
