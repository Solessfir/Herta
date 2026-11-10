#include "Herta/GameFramework/GameHud.h"

#include "Herta/GameFramework/GameControls.h"
#include "Herta/GameFramework/GameText.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

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

std::optional<std::size_t> DrawGameHud(FGameUI& UI, const FLocalization& Text, const FGameObjective& Objective, const bool bPaused, const std::span<const std::string_view> MenuItems, std::size_t& MenuFocus, const FGameUIInput& MenuInput, const std::string_view MenuKey, const std::string_view ControlsHint)
{
	const std::uint32_t Panel = MakeGameUIColor(12, 14, 20, 170);
	const std::uint32_t TextColor = MakeGameUIColor(238, 240, 246);
	const std::uint32_t Accent = MakeGameUIColor(242, 196, 92);
	namespace Game = Text::Game;
	const FCulture& Culture = Text.GetCulture();
	const auto Time = [&Culture](const double Seconds)
	{
		return std::array{FMessageArgument{"time", FormatGameSeconds(Culture, Seconds)}};
	};

	std::string Status;
	switch (Objective.State)
	{
		case EGameObjectiveState::Ready:
			Status = Text.Format(Game::Hud_Ready);
			break;
		case EGameObjectiveState::Running:
			Status = Text.Format(Game::Hud_Time, Time(Objective.ElapsedSeconds));
			break;
		case EGameObjectiveState::Finished:
			// Under a minute the time is a plain number of seconds, so the message can pick the plural form that matches it.
			Status = Objective.ElapsedSeconds < 59.995 ? Text.Format(Game::Hud_FinishedSeconds, std::array{FMessageArgument{"seconds", FFixedNumber{.Value = Objective.ElapsedSeconds, .FractionDigits = 2}}})
			                                           : Text.Format(Game::Hud_Finished, Time(Objective.ElapsedSeconds));
			break;
		default:
			Status = Text.Format(Game::Hud_Explore);
			break;
	}

	const std::string Best = Objective.BestSeconds ? Text.Format(Game::Hud_Best, Time(*Objective.BestSeconds)) : std::string{};
	// Panels pad text evenly, so a line sits centered whatever the font's metrics.
	constexpr FVector2 Padding{20.f, 14.f};
	constexpr float Margin = 24.f;
	constexpr float Radius = 12.f;
	const FVector2 StatusSize = UI.MeasureText(Status, EGameUIFont::Body);
	const float LineHeight = StatusSize.Y;
	const float PanelWidth = std::max(StatusSize.X, UI.MeasureText(Best, EGameUIFont::Body).X) + Padding.X * 2.f;
	UI.DrawRect({.Anchor = {1.f, 0.f}, .Pivot = {1.f, 0.f}, .Offset = {-Margin, Margin}}, {PanelWidth, LineHeight * (Objective.BestSeconds ? 2.f : 1.f) + Padding.Y * 2.f}, Panel, Radius);
	UI.DrawText(Status, {.Anchor = {1.f, 0.f}, .Pivot = {1.f, 0.f}, .Offset = {-Margin - Padding.X, Margin + Padding.Y}}, EGameUIFont::Body, Objective.State == EGameObjectiveState::Finished ? Accent : TextColor);
	if (Objective.BestSeconds)
	{
		UI.DrawText(Best, {.Anchor = {1.f, 0.f}, .Pivot = {1.f, 0.f}, .Offset = {-Margin - Padding.X, Margin + Padding.Y + LineHeight}}, EGameUIFont::Body, MakeGameUIColor(170, 176, 190));
	}

	if (!bPaused)
	{
		UI.DrawCircle({.Anchor = {0.5f, 0.5f}, .Pivot = {0.5f, 0.5f}}, 8.f, MakeGameUIColor(255, 255, 255, 220));
		const std::string Hint = ControlsHint.empty() ? FormatGameControlsHint(Text, GetDefaultGameBindings(), MenuKey) : std::string(ControlsHint);
		const FVector2 HintSize = UI.MeasureText(Hint, EGameUIFont::Body);
		UI.DrawRect({.Anchor = {0.5f, 1.f}, .Pivot = {0.5f, 1.f}, .Offset = {0.f, -Margin}}, {HintSize.X + Padding.X * 2.f, HintSize.Y + Padding.Y * 2.f}, Panel, Radius);
		UI.DrawText(Hint, {.Anchor = {0.5f, 1.f}, .Pivot = {0.5f, 1.f}, .Offset = {0.f, -Margin - Padding.Y}}, EGameUIFont::Body, MakeGameUIColor(220, 224, 232));
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
