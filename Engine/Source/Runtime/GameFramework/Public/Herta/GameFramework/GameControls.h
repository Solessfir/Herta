#pragma once

#include "Herta/Application/Application.h"
#include "Herta/GameFramework/GameSession.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Herta
{
// What the player can rebind. Moving with a gamepad stays on the sticks, and pausing stays on Escape and Start, so a player can always
// reach the menu that rebinds everything else.
enum class EGameAction : std::uint8_t
{
	MoveForward,
	MoveBack,
	MoveLeft,
	MoveRight,
	Jump,
	Sprint,
	Crouch,
	Attack,
	Block,
	Throw,
	ToggleCamera,
	Ragdoll,
	Restart,
};

inline constexpr std::size_t GameActionCount = static_cast<std::size_t>(EGameAction::Restart) + 1;

// Each action has a keyboard slot, holding a key or a mouse button, and a gamepad slot.
struct FGameBinding
{
	bool operator==(const FGameBinding&) const = default;

	std::optional<EKey> Key{};
	std::optional<EMouseButton> Mouse{};
	std::optional<EGamepadButton> Pad{};
};

struct FGameBindings
{
	bool operator==(const FGameBindings&) const = default;

	[[nodiscard]] const FGameBinding& Get(EGameAction Action) const;
	[[nodiscard]] FGameBinding& Get(EGameAction Action);

	std::array<FGameBinding, GameActionCount> Actions{};
};

// WASD, Space, Shift, C, the mouse buttons, F, V, Z, and R, with the gamepad's A, B, X, Y, shoulders, left stick click, and Back.
[[nodiscard]] FGameBindings GetDefaultGameBindings();
[[nodiscard]] std::string_view GetGameActionName(EGameAction Action);
[[nodiscard]] std::string GetKeyName(EKey Key);
[[nodiscard]] std::string_view GetMouseButtonName(EMouseButton Button);
[[nodiscard]] std::string_view GetGamepadButtonName(EGamepadButton Button);
// "Space" or "Left mouse", or "Unbound".
[[nodiscard]] std::string FormatKeyboardBinding(const FGameBinding& Binding);
[[nodiscard]] std::string FormatGamepadBinding(const FGameBinding& Binding);

// Binds Key or Mouse to Action's keyboard slot, taking it from any other action that had it, so one key never does two things.
void BindKeyboard(FGameBindings& Bindings, EGameAction Action, std::optional<EKey> Key, std::optional<EMouseButton> Mouse);
void BindGamepad(FGameBindings& Bindings, EGameAction Action, EGamepadButton Button);
// Settings lines, one "bind.<Action>=key:<code>" or "mouse:<button>" and one "bind.<Action>.pad=<button>" per action.
[[nodiscard]] std::string SerializeGameBindings(const FGameBindings& Bindings);
// Applies one settings line; returns false when the key is not a binding or the value is unusable.
bool ApplyGameBindingSetting(FGameBindings& Bindings, std::string_view Key, std::string_view Value);

// One frame of keyboard and mouse: what is held now, and what was pressed since the last frame.
struct FGameKeyboardState
{
	std::span<const EKey> HeldKeys{};
	std::span<const EKey> PressedKeys{};
	std::span<const EMouseButton> HeldMouse{};
	std::span<const EMouseButton> PressedMouse{};
	FVector2 MouseDelta{};
	float Sensitivity = 0.0022f;
};

[[nodiscard]] bool IsGameActionHeld(const FGameBindings& Bindings, EGameAction Action, const FGameKeyboardState& State);
[[nodiscard]] bool WasGameActionPressed(const FGameBindings& Bindings, EGameAction Action, const FGameKeyboardState& State);
// Gameplay input from the keyboard and mouse through the bindings; Restart is the host's to act on.
[[nodiscard]] FGameInput BuildKeyboardGameInput(const FGameBindings& Bindings, const FGameKeyboardState& State);
// A gamepad's buttons through the bindings, with movement and looking on the sticks.
[[nodiscard]] FGameGamepad GetBoundGamepad(const FGameBindings& Bindings, const FGamepadState& Gamepad);
// The HUD's controls line, naming the keys bound now.
[[nodiscard]] std::string FormatGameControlsHint(const FGameBindings& Bindings, std::string_view MenuKey);
}
