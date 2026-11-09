#include "Herta/GameFramework/GameHud.h"

#include <algorithm>
#include <format>

namespace Herta
{
std::string FormatGameSeconds(const double Seconds)
{
	const auto Minutes = static_cast<int>(Seconds / 60.);
	return Minutes > 0 ? std::format("{}:{:05.2f}", Minutes, Seconds - Minutes * 60.) : std::format("{:.2f}", Seconds);
}

std::optional<std::size_t> DrawGameHud(FGameUI& UI, const FGameObjective& Objective, const bool bPaused, const std::span<const std::string_view> MenuItems, std::size_t& MenuFocus, const FGameUIInput& MenuInput, const std::string_view MenuKey)
{
	const std::uint32_t Panel = MakeGameUIColor(12, 14, 20, 170);
	const std::uint32_t Text = MakeGameUIColor(238, 240, 246);
	const std::uint32_t Accent = MakeGameUIColor(242, 196, 92);
	const std::string Status = Objective.State == EGameObjectiveState::Ready      ? "Leave the start zone to begin the course"
	                           : Objective.State == EGameObjectiveState::Running  ? std::format("Time {}", FormatGameSeconds(Objective.ElapsedSeconds))
	                           : Objective.State == EGameObjectiveState::Finished ? std::format("Finished in {} s", FormatGameSeconds(Objective.ElapsedSeconds))
	                                                                              : std::string("Explore the playground");
	const std::string Best = Objective.BestSeconds ? std::format("Best {} s", FormatGameSeconds(*Objective.BestSeconds)) : std::string{};
	// Panels pad text evenly, so a line sits centered whatever the font's metrics.
	constexpr FVector2 Padding{20.f, 14.f};
	constexpr float Margin = 24.f;
	constexpr float Radius = 12.f;
	const FVector2 StatusSize = UI.MeasureText(Status, EGameUIFont::Body);
	const float LineHeight = StatusSize.Y;
	const float PanelWidth = std::max(StatusSize.X, UI.MeasureText(Best, EGameUIFont::Body).X) + Padding.X * 2.f;
	UI.DrawRect({.Anchor = {1.f, 0.f}, .Pivot = {1.f, 0.f}, .Offset = {-Margin, Margin}}, {PanelWidth, LineHeight * (Objective.BestSeconds ? 2.f : 1.f) + Padding.Y * 2.f}, Panel, Radius);
	UI.DrawText(Status, {.Anchor = {1.f, 0.f}, .Pivot = {1.f, 0.f}, .Offset = {-Margin - Padding.X, Margin + Padding.Y}}, EGameUIFont::Body, Objective.State == EGameObjectiveState::Finished ? Accent : Text);
	if (Objective.BestSeconds)
	{
		UI.DrawText(Best, {.Anchor = {1.f, 0.f}, .Pivot = {1.f, 0.f}, .Offset = {-Margin - Padding.X, Margin + Padding.Y + LineHeight}}, EGameUIFont::Body, MakeGameUIColor(170, 176, 190));
	}

	if (!bPaused)
	{
		UI.DrawCircle({.Anchor = {0.5f, 0.5f}, .Pivot = {0.5f, 0.5f}}, 8.f, MakeGameUIColor(255, 255, 255, 220));
		const std::string Hint = std::format("WASD move    Space jump    Shift sprint    C slide    R restart    {} menu", MenuKey);
		const FVector2 HintSize = UI.MeasureText(Hint, EGameUIFont::Body);
		UI.DrawRect({.Anchor = {0.5f, 1.f}, .Pivot = {0.5f, 1.f}, .Offset = {0.f, -Margin}}, {HintSize.X + Padding.X * 2.f, HintSize.Y + Padding.Y * 2.f}, Panel, Radius);
		UI.DrawText(Hint, {.Anchor = {0.5f, 1.f}, .Pivot = {0.5f, 1.f}, .Offset = {0.f, -Margin - Padding.Y}}, EGameUIFont::Body, MakeGameUIColor(220, 224, 232));
		return std::nullopt;
	}

	// Oversized so the dimming covers the screen at any aspect ratio.
	UI.DrawRect({.Anchor = {0.5f, 0.5f}, .Pivot = {0.5f, 0.5f}}, {100000.f, 100000.f}, MakeGameUIColor(0, 0, 0, 140));
	UI.DrawText("Paused", {.Anchor = {0.5f, 0.5f}, .Pivot = {0.5f, 1.f}, .Offset = {0.f, -200.f}}, EGameUIFont::Title, Text);
	return UI.DrawMenu(MenuItems, MenuFocus, MenuInput);
}
}
