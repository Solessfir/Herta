#pragma once

#include "Herta/GameFramework/GameControls.h"
#include "Herta/GameUI/GameUI.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Herta
{
enum class EGameMenuScreen : std::uint8_t
{
	Closed,
	Pause,
	Settings,
	Controls,
};

// What the menu edits. The host keeps these, applies them when the menu reports SettingsChanged, and saves them.
struct FGameMenuSettings
{
	bool bFullscreen = false;
	bool bHdrAvailable = false;
	bool bHdr = false;
	// ERenderQuality's value: Low, Medium, or High.
	std::uint32_t Quality = 2;
	// Linear gains for the Master, Effects, and Ambient buses.
	std::array<float, 3> Volumes{1.f, 1.f, 1.f};
	// A multiple of the default mouse sensitivity.
	float Sensitivity = 1.f;
	FGameBindings Bindings = GetDefaultGameBindings();
	// Whether there is a save to load.
	bool bCanLoad = false;
};

enum class EGameMenuCommand : std::uint8_t
{
	Resume,
	Save,
	Load,
	Restart,
	Quit,
	SettingsChanged,
};

// One frame of menu input from the keyboard, mouse, and gamepad. The last key, mouse button, and gamepad button pressed are what a binding
// takes while the menu waits for one.
struct FGameMenuInput
{
	std::optional<FVector2> Pointer{};
	bool bClicked = false;
	bool bUp = false;
	bool bDown = false;
	bool bLeft = false;
	bool bRight = false;
	bool bActivate = false;
	// Escape or B: back out of a screen, or out of the menu.
	bool bBack = false;
	// Escape or Start: open the menu, or back out like bBack.
	bool bPause = false;
	bool bFocusLost = false;
	std::optional<EKey> Key{};
	std::optional<EMouseButton> Mouse{};
	std::optional<EGamepadButton> Pad{};
};

// The game's menus: the pause menu, settings, and controls with rebinding, plus short notices such as "Game saved". While it is open, or
// waiting for a key to bind, it owns all input and gameplay gets none, so a key pressed to rebind never also jumps or attacks.
class FGameMenu final
{
public:
	// Handles focus, navigation, and choices from the keyboard and gamepad; pointer clicks arrive through Draw.
	[[nodiscard]] std::vector<EGameMenuCommand> Update(const FGameMenuInput& Input, FGameMenuSettings& Settings);
	// Draws the open screen and any notice, and activates the item the pointer clicks.
	[[nodiscard]] std::vector<EGameMenuCommand> Draw(FGameUI& UI, const FGameMenuInput& Input, FGameMenuSettings& Settings);
	void Open();
	void Close();
	void ShowNotice(std::string Text, float Seconds = 3.f);
	void Tick(float DeltaSeconds);

	[[nodiscard]] bool IsOpen() const noexcept;
	// Whether gameplay should ignore this frame's input.
	[[nodiscard]] bool OwnsInput() const noexcept;
	[[nodiscard]] EGameMenuScreen GetScreen() const noexcept;
	[[nodiscard]] std::size_t GetFocus() const noexcept;
	// The action waiting for a key or button, if any.
	[[nodiscard]] std::optional<EGameAction> GetCapture() const noexcept;
	// The open screen's items as drawn, for tests and accessibility.
	[[nodiscard]] std::vector<std::string> GetItems(const FGameMenuSettings& Settings) const;
	[[nodiscard]] const std::string& GetNotice() const noexcept;

private:
	enum class EItem : std::uint8_t
	{
		Resume,
		Save,
		Load,
		Restart,
		Settings,
		Quit,
		Fullscreen,
		Hdr,
		Quality,
		MasterVolume,
		EffectsVolume,
		AmbientVolume,
		Sensitivity,
		Controls,
		Binding,
		ResetBindings,
		Back,
	};

	struct FItem
	{
		EItem Kind = EItem::Back;
		std::size_t Action = 0;
		bool bEnabled = true;
	};

	[[nodiscard]] std::vector<FItem> GetScreenItems(const FGameMenuSettings& Settings) const;
	void Activate(const FItem& Item, FGameMenuSettings& Settings, std::vector<EGameMenuCommand>& Commands);
	void Adjust(const FItem& Item, int Direction, FGameMenuSettings& Settings, std::vector<EGameMenuCommand>& Commands);
	void Back(std::vector<EGameMenuCommand>& Commands);
	void MoveFocus(int Direction, const std::vector<FItem>& Items);

	EGameMenuScreen Screen = EGameMenuScreen::Closed;
	std::size_t Focus = 0;
	std::optional<EGameAction> Capture{};
	std::string Notice{};
	float NoticeSeconds = 0.f;
};
}
