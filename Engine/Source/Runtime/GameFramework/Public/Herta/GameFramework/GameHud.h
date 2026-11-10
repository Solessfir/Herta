#pragma once

#include "Herta/GameFramework/GameSession.h"
#include "Herta/GameUI/GameUI.h"
#include "Herta/Localization/Localization.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Herta
{
// A course time with two decimals in the culture's digits: "12.34", or "1:05.20" past a minute.
[[nodiscard]] std::string FormatGameSeconds(const FCulture& Culture, double Seconds);

// The game's screen overlay, shared by the standalone game and editor Play: the objective, a crosshair and controls while playing, and the pause menu while paused.
// The host supplies the menu items, such as Fullscreen in the standalone game or Stop playing in the editor, and acts on the item this returns.
// MenuKey names the key that pauses in the controls hint, and ControlsHint replaces the default hint, such as with rebound keys. A host
// that draws its own menu passes no items: paused, the HUD then shows only the objective.
// The fight's bars: vitality at the bottom left, posture filling out from the middle above the controls, and the target's posture at the top
// with a red mark while a deathblow is open.
void DrawCombatHud(FGameUI& UI, const FGameCombatView& Combat);
std::optional<std::size_t> DrawGameHud(FGameUI& UI, const FLocalization& Text, const FGameObjective& Objective, bool bPaused, std::span<const std::string_view> MenuItems, std::size_t& MenuFocus, const FGameUIInput& MenuInput, std::string_view MenuKey = "Esc", std::string_view ControlsHint = {});
}
