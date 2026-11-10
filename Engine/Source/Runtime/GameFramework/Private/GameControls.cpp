#include "Herta/GameFramework/GameControls.h"

#include "Herta/GameFramework/GameText.h"

#include <algorithm>
#include <charconv>
#include <format>

namespace Herta
{
namespace
{
namespace Game = Text::Game;
constexpr std::array<FTextId, GameActionCount> ActionNames{
    Game::Action_MoveForward,
    Game::Action_MoveBack,
    Game::Action_MoveLeft,
    Game::Action_MoveRight,
    Game::Action_Jump,
    Game::Action_Sprint,
    Game::Action_Crouch,
    Game::Action_Attack,
    Game::Action_Block,
    Game::Action_Throw,
    Game::Action_ToggleCamera,
    Game::Action_Ragdoll,
    Game::Action_Restart,
};

// Settings keys stay stable even if the shown names change.
constexpr std::array<std::string_view, GameActionCount> ActionKeys{"MoveForward", "MoveBack", "MoveLeft", "MoveRight", "Jump", "Sprint", "Crouch", "Attack", "Block", "Throw", "ToggleCamera", "Ragdoll", "Restart"};

template <typename T>
[[nodiscard]] std::optional<T> ParseInteger(const std::string_view Text)
{
	T Value{};
	const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Value);
	return Error == std::errc{} && End == Text.data() + Text.size() ? std::optional(Value) : std::nullopt;
}

[[nodiscard]] bool IsValidKey(const int Code)
{
	// Escape opens the menu that rebinds everything, so it cannot be taken.
	return Code >= static_cast<int>(EKey::Space) && Code <= static_cast<int>(EKey::Last) && Code != static_cast<int>(EKey::Escape);
}
}

const FGameBinding& FGameBindings::Get(const EGameAction Action) const
{
	return Actions[static_cast<std::size_t>(Action)];
}

FGameBinding& FGameBindings::Get(const EGameAction Action)
{
	return Actions[static_cast<std::size_t>(Action)];
}

FGameBindings GetDefaultGameBindings()
{
	FGameBindings Bindings;
	const auto Set = [&Bindings](const EGameAction Action, const FGameBinding Binding)
	{
		Bindings.Get(Action) = Binding;
	};

	Set(EGameAction::MoveForward, {.Key = EKey::W});
	Set(EGameAction::MoveBack, {.Key = EKey::S});
	Set(EGameAction::MoveLeft, {.Key = EKey::A});
	Set(EGameAction::MoveRight, {.Key = EKey::D});
	Set(EGameAction::Jump, {.Key = EKey::Space, .Pad = EGamepadButton::South});
	Set(EGameAction::Sprint, {.Key = EKey::LeftShift, .Pad = EGamepadButton::LeftThumb});
	Set(EGameAction::Crouch, {.Key = EKey::C, .Pad = EGamepadButton::East});
	Set(EGameAction::Attack, {.Mouse = EMouseButton::Left, .Pad = EGamepadButton::RightShoulder});
	Set(EGameAction::Block, {.Mouse = EMouseButton::Right, .Pad = EGamepadButton::LeftShoulder});
	Set(EGameAction::Throw, {.Key = EKey::F, .Pad = EGamepadButton::West});
	Set(EGameAction::ToggleCamera, {.Key = EKey::V, .Pad = EGamepadButton::North});
	Set(EGameAction::Ragdoll, {.Key = EKey::Z});
	Set(EGameAction::Restart, {.Key = EKey::R, .Pad = EGamepadButton::Back});
	return Bindings;
}

std::string GetGameActionName(const FLocalization& Text, const EGameAction Action)
{
	return Text.Format(ActionNames[static_cast<std::size_t>(Action)]);
}

std::string GetKeyName(const EKey Key)
{
	const int Code = static_cast<int>(Key);
	if (Code >= static_cast<int>(EKey::A) && Code <= static_cast<int>(EKey::Z))
	{
		return std::string(1, static_cast<char>(Code));
	}

	if (Code >= static_cast<int>(EKey::Zero) && Code <= static_cast<int>(EKey::Nine))
	{
		return std::string(1, static_cast<char>(Code));
	}

	if (Code >= static_cast<int>(EKey::F1) && Code <= static_cast<int>(EKey::F25))
	{
		return std::format("F{}", Code - static_cast<int>(EKey::F1) + 1);
	}

	if (Code >= static_cast<int>(EKey::KeypadZero) && Code <= static_cast<int>(EKey::KeypadNine))
	{
		return std::format("Keypad {}", Code - static_cast<int>(EKey::KeypadZero));
	}

	switch (Key)
	{
		case EKey::Space:
			return "Space";
		case EKey::Apostrophe:
			return "'";
		case EKey::Comma:
			return ",";
		case EKey::Minus:
			return "-";
		case EKey::Period:
			return ".";
		case EKey::Slash:
			return "/";
		case EKey::Semicolon:
			return ";";
		case EKey::Equal:
			return "=";
		case EKey::LeftBracket:
			return "[";
		case EKey::Backslash:
			return "\\";
		case EKey::RightBracket:
			return "]";
		case EKey::GraveAccent:
			return "`";
		case EKey::Enter:
			return "Enter";
		case EKey::Tab:
			return "Tab";
		case EKey::Backspace:
			return "Backspace";
		case EKey::Insert:
			return "Insert";
		case EKey::Delete:
			return "Delete";
		case EKey::Right:
			return "Right";
		case EKey::Left:
			return "Left";
		case EKey::Down:
			return "Down";
		case EKey::Up:
			return "Up";
		case EKey::PageUp:
			return "Page Up";
		case EKey::PageDown:
			return "Page Down";
		case EKey::Home:
			return "Home";
		case EKey::End:
			return "End";
		case EKey::CapsLock:
			return "Caps Lock";
		case EKey::LeftShift:
			return "Left Shift";
		case EKey::RightShift:
			return "Right Shift";
		case EKey::LeftControl:
			return "Left Ctrl";
		case EKey::RightControl:
			return "Right Ctrl";
		case EKey::LeftAlt:
			return "Left Alt";
		case EKey::RightAlt:
			return "Right Alt";
		case EKey::KeypadEnter:
			return "Keypad Enter";
		default:
			break;
	}

	return std::format("Key {}", Code);
}

std::string_view GetMouseButtonName(const EMouseButton Button)
{
	constexpr std::array<std::string_view, 8> Names{"Left mouse", "Right mouse", "Middle mouse", "Mouse 4", "Mouse 5", "Mouse 6", "Mouse 7", "Mouse 8"};
	const auto Index = static_cast<std::size_t>(Button);
	return Index < Names.size() ? Names[Index] : "Mouse";
}

std::string_view GetGamepadButtonName(const EGamepadButton Button)
{
	constexpr std::array<std::string_view, static_cast<std::size_t>(EGamepadButton::Last) + 1> Names{"A", "B", "X", "Y", "LB", "RB", "Back", "Start", "Guide", "Left stick", "Right stick", "D-pad up", "D-pad right", "D-pad down", "D-pad left"};
	return Names[static_cast<std::size_t>(Button)];
}

std::string FormatKeyboardBinding(const FGameBinding& Binding, const std::string_view Unbound)
{
	return Binding.Key ? GetKeyName(*Binding.Key) : Binding.Mouse ? std::string(GetMouseButtonName(*Binding.Mouse))
	                                                              : std::string(Unbound);
}

std::string FormatGamepadBinding(const FGameBinding& Binding, const std::string_view Unbound)
{
	return Binding.Pad ? std::string(GetGamepadButtonName(*Binding.Pad)) : std::string(Unbound);
}

void BindKeyboard(FGameBindings& Bindings, const EGameAction Action, const std::optional<EKey> Key, const std::optional<EMouseButton> Mouse)
{
	for (FGameBinding& Other : Bindings.Actions)
	{
		if ((Key && Other.Key == Key) || (Mouse && Other.Mouse == Mouse))
		{
			Other.Key.reset();
			Other.Mouse.reset();
		}
	}

	FGameBinding& Binding = Bindings.Get(Action);
	Binding.Key = Key;
	Binding.Mouse = Key ? std::nullopt : Mouse;
}

void BindGamepad(FGameBindings& Bindings, const EGameAction Action, const EGamepadButton Button)
{
	for (FGameBinding& Other : Bindings.Actions)
	{
		if (Other.Pad == Button)
		{
			Other.Pad.reset();
		}
	}

	Bindings.Get(Action).Pad = Button;
}

std::string SerializeGameBindings(const FGameBindings& Bindings)
{
	std::string Text;
	for (std::size_t Index = 0; Index < GameActionCount; ++Index)
	{
		const FGameBinding& Binding = Bindings.Actions[Index];
		const std::string Keyboard = Binding.Key ? std::format("key:{}", static_cast<int>(*Binding.Key)) : Binding.Mouse ? std::format("mouse:{}", static_cast<int>(*Binding.Mouse))
		                                                                                                                 : std::string("none");
		const std::string Pad = Binding.Pad ? std::format("{}", static_cast<int>(*Binding.Pad)) : std::string("none");
		Text += std::format("bind.{}={}\nbind.{}.pad={}\n", ActionKeys[Index], Keyboard, ActionKeys[Index], Pad);
	}

	return Text;
}

bool ApplyGameBindingSetting(FGameBindings& Bindings, const std::string_view Key, const std::string_view Value)
{
	if (!Key.starts_with("bind."))
	{
		return false;
	}

	std::string_view Name = Key.substr(5);
	const bool bPad = Name.ends_with(".pad");
	Name = bPad ? Name.substr(0, Name.size() - 4) : Name;
	const auto Found = std::ranges::find(ActionKeys, Name);
	if (Found == ActionKeys.end())
	{
		return false;
	}

	FGameBinding& Binding = Bindings.Actions[static_cast<std::size_t>(Found - ActionKeys.begin())];
	if (bPad)
	{
		const auto Button = ParseInteger<int>(Value);
		if (Value == "none")
		{
			Binding.Pad.reset();
			return true;
		}

		if (!Button || *Button < 0 || *Button > static_cast<int>(EGamepadButton::Last) || *Button == static_cast<int>(EGamepadButton::Start))
		{
			return false;
		}

		Binding.Pad = static_cast<EGamepadButton>(*Button);
		return true;
	}

	if (Value == "none")
	{
		Binding.Key.reset();
		Binding.Mouse.reset();
		return true;
	}

	if (Value.starts_with("key:"))
	{
		const auto Code = ParseInteger<int>(Value.substr(4));
		if (!Code || !IsValidKey(*Code))
		{
			return false;
		}

		Binding.Key = static_cast<EKey>(*Code);
		Binding.Mouse.reset();
		return true;
	}

	if (Value.starts_with("mouse:"))
	{
		const auto Button = ParseInteger<int>(Value.substr(6));
		if (!Button || *Button < 0 || *Button > 7)
		{
			return false;
		}

		Binding.Mouse = static_cast<EMouseButton>(*Button);
		Binding.Key.reset();
		return true;
	}

	return false;
}

bool IsGameActionHeld(const FGameBindings& Bindings, const EGameAction Action, const FGameKeyboardState& State)
{
	const FGameBinding& Binding = Bindings.Get(Action);
	return (Binding.Key && std::ranges::contains(State.HeldKeys, *Binding.Key)) || (Binding.Mouse && std::ranges::contains(State.HeldMouse, *Binding.Mouse));
}

bool WasGameActionPressed(const FGameBindings& Bindings, const EGameAction Action, const FGameKeyboardState& State)
{
	const FGameBinding& Binding = Bindings.Get(Action);
	return (Binding.Key && std::ranges::contains(State.PressedKeys, *Binding.Key)) || (Binding.Mouse && std::ranges::contains(State.PressedMouse, *Binding.Mouse));
}

FGameInput BuildKeyboardGameInput(const FGameBindings& Bindings, const FGameKeyboardState& State)
{
	const auto Held = [&](const EGameAction Action)
	{
		return IsGameActionHeld(Bindings, Action, State) ? 1.f : 0.f;
	};

	const auto Pressed = [&](const EGameAction Action)
	{
		return WasGameActionPressed(Bindings, Action, State);
	};

	return {
	    .Move = {Held(EGameAction::MoveRight) - Held(EGameAction::MoveLeft), Held(EGameAction::MoveForward) - Held(EGameAction::MoveBack)},
	    .YawDelta = -State.MouseDelta.X * State.Sensitivity,
	    .PitchDelta = -State.MouseDelta.Y * State.Sensitivity,
	    .bJump = Pressed(EGameAction::Jump),
	    .bSprint = Held(EGameAction::Sprint) != 0.f,
	    .bCrouch = Held(EGameAction::Crouch) != 0.f,
	    .bThrow = Pressed(EGameAction::Throw),
	    .bToggleCamera = Pressed(EGameAction::ToggleCamera),
	    .bToggleRagdoll = Pressed(EGameAction::Ragdoll),
	    .bAttack = Pressed(EGameAction::Attack),
	    .bBlock = Held(EGameAction::Block) != 0.f,
	};
}

FGameGamepad GetBoundGamepad(const FGameBindings& Bindings, const FGamepadState& Gamepad)
{
	const auto Down = [&](const EGameAction Action)
	{
		const auto& Pad = Bindings.Get(Action).Pad;
		return Pad && Gamepad.IsDown(*Pad);
	};

	return {
	    .Move = {Gamepad.LeftX, -Gamepad.LeftY},
	    .Look = {Gamepad.RightX, -Gamepad.RightY},
	    .bJump = Down(EGameAction::Jump),
	    .bSprint = Down(EGameAction::Sprint),
	    .bCrouch = Down(EGameAction::Crouch),
	    .bThrow = Down(EGameAction::Throw),
	    .bToggleCamera = Down(EGameAction::ToggleCamera),
	    .bAttack = Down(EGameAction::Attack),
	    .bBlock = Down(EGameAction::Block),
	};
}

std::string FormatGameControlsHint(const FLocalization& Text, const FGameBindings& Bindings, const std::string_view MenuKey)
{
	const std::string Unbound = Text.Format(Game::Controls_Unbound);
	const auto Name = [&](const EGameAction Action) -> FMessageValue
	{
		return FormatKeyboardBinding(Bindings.Get(Action), Unbound);
	};

	const std::array Arguments{
	    FMessageArgument{"forward", Name(EGameAction::MoveForward)},
	    FMessageArgument{"left", Name(EGameAction::MoveLeft)},
	    FMessageArgument{"back", Name(EGameAction::MoveBack)},
	    FMessageArgument{"right", Name(EGameAction::MoveRight)},
	    FMessageArgument{"jump", Name(EGameAction::Jump)},
	    FMessageArgument{"sprint", Name(EGameAction::Sprint)},
	    FMessageArgument{"slide", Name(EGameAction::Crouch)},
	    FMessageArgument{"attack", Name(EGameAction::Attack)},
	    FMessageArgument{"block", Name(EGameAction::Block)},
	    FMessageArgument{"ragdoll", Name(EGameAction::Ragdoll)},
	    FMessageArgument{"restart", Name(EGameAction::Restart)},
	    FMessageArgument{"menu", std::string(MenuKey)},
	};

	return Text.Format(Game::Hud_Controls, Arguments);
}
}
