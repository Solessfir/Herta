#pragma once

#include "Herta/GameFramework/GameSession.h"
#include "Herta/GameUI/GameUI.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Herta
{
[[nodiscard]] std::string FormatGameSeconds(double Seconds);

// The game's screen overlay, shared by the standalone game and editor Play: the objective, a crosshair and controls while playing, and the pause menu while paused.
// The host supplies the menu items, such as Fullscreen in the standalone game or Stop playing in the editor, and acts on the item this returns.
// MenuKey names the key that pauses in the controls hint.
std::optional<std::size_t> DrawGameHud(FGameUI& UI, const FGameObjective& Objective, bool bPaused, std::span<const std::string_view> MenuItems, std::size_t& MenuFocus, const FGameUIInput& MenuInput, std::string_view MenuKey = "Esc");
}
