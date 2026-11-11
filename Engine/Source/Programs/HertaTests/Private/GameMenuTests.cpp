#include "Herta/Assets/VirtualFileSystem.h"
#include "Herta/Core/Build.h"
#include "Herta/GameFramework/GameHud.h"
#include "Herta/GameFramework/GameMenu.h"
#include "Herta/GameFramework/GameText.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <format>
#include <ostream>

namespace Herta
{
namespace
{
const FLocalization& GetEngineText()
{
	static const FLocalization Text = []
	{
		FLocalization Loaded;
		FVirtualFileSystem Files(std::filesystem::current_path() / CatalogDirectory);
		Files.MountDirectory(Files.GetRoot());
		const auto Catalogs = LoadTextCatalogs(Files, {});
		REQUIRE(Catalogs);
		REQUIRE(Loaded.SetCatalogs(*Catalogs));
		return Loaded;
	}();

	return Text;
}
}

TEST_CASE("Game bindings drive gameplay input, move keys between actions, and round-trip through settings")
{
	FGameBindings Bindings = GetDefaultGameBindings();
	const std::array Held{EKey::W, EKey::D, EKey::LeftShift};
	const std::array Pressed{EKey::Space};
	const std::array Mouse{EMouseButton::Left};
	const FGameInput Input = BuildKeyboardGameInput(Bindings, {.HeldKeys = Held, .PressedKeys = Pressed, .PressedMouse = Mouse, .MouseDelta = {10.f, 0.f}, .Sensitivity = 0.01f});
	CHECK(Input.Move.X == 1.f);
	CHECK(Input.Move.Y == 1.f);
	CHECK(Input.bSprint);
	CHECK(Input.bJump);
	CHECK(Input.bAttack);
	CHECK_FALSE(Input.bThrow);
	CHECK(Input.YawDelta == doctest::Approx(-0.1f));

	// Binding Space to Throw takes it from Jump, so one key never does two things.
	BindKeyboard(Bindings, EGameAction::Throw, EKey::Space, std::nullopt);
	CHECK_FALSE(Bindings.Get(EGameAction::Jump).Key);
	const FGameInput Rebound = BuildKeyboardGameInput(Bindings, {.PressedKeys = Pressed});
	CHECK(Rebound.bThrow);
	CHECK_FALSE(Rebound.bJump);
	BindKeyboard(Bindings, EGameAction::Jump, std::nullopt, EMouseButton::Middle);
	CHECK(FormatKeyboardBinding(Bindings.Get(EGameAction::Jump)) == "Middle mouse");
	BindGamepad(Bindings, EGameAction::Ragdoll, EGamepadButton::South);
	CHECK_FALSE(Bindings.Get(EGameAction::Jump).Pad);

	FGamepadState Pad;
	Pad.Buttons[static_cast<std::size_t>(EGamepadButton::South)] = true;
	Pad.LeftY = -1.f;
	const FGameGamepad Gamepad = GetBoundGamepad(Bindings, Pad);
	CHECK(Gamepad.Move.Y == 1.f);
	CHECK_FALSE(Gamepad.bJump);

	FGameBindings Loaded = GetDefaultGameBindings();
	const std::string Text = SerializeGameBindings(Bindings);
	for (std::size_t Start = 0; Start < Text.size();)
	{
		const std::size_t End = Text.find('\n', Start);
		const std::string_view Line = std::string_view(Text).substr(Start, End - Start);
		const std::size_t Equals = Line.find('=');
		CHECK(ApplyGameBindingSetting(Loaded, Line.substr(0, Equals), Line.substr(Equals + 1)));
		Start = End + 1;
	}

	CHECK(Loaded == Bindings);
	CHECK_FALSE(ApplyGameBindingSetting(Loaded, "bind.Jump", "key:256"));
	CHECK_FALSE(ApplyGameBindingSetting(Loaded, "bind.Jump.pad", "7"));
	CHECK_FALSE(ApplyGameBindingSetting(Loaded, "bind.Fly", "key:70"));
	CHECK_FALSE(ApplyGameBindingSetting(Loaded, "fullscreen", "1"));
	const std::vector<FGameControlHint> Hints = GetGameControlHints(GetEngineText(), GetDefaultGameBindings(), "Esc");
	REQUIRE(Hints.size() == 9);
	CHECK(Hints[0].Keys == std::vector<std::string>{"W", "A", "S", "D"});
	CHECK(Hints[0].Action == "Move");
	CHECK(Hints[1].Keys == std::vector<std::string>{"Space"});
	CHECK(Hints[8].Keys == std::vector<std::string>{"Esc"});
}

TEST_CASE("The game menu owns input while open, routes back out screen by screen, and rebinds without leaking keys to gameplay")
{
	FGameMenu Menu(GetEngineText());
	FGameMenuSettings Settings;
	CHECK_FALSE(Menu.OwnsInput());
	CHECK(Menu.Update({.bActivate = true}, Settings).empty());
	CHECK_FALSE(Menu.IsOpen());

	// Losing focus opens the pause menu, like Escape.
	(void)Menu.Update({.bFocusLost = true}, Settings);
	CHECK(Menu.GetScreen() == EGameMenuScreen::Pause);
	CHECK(Menu.OwnsInput());

	// Load is disabled without a save, so navigation skips it.
	(void)Menu.Update({.bDown = true}, Settings);
	(void)Menu.Update({.bDown = true}, Settings);
	CHECK(Menu.GetFocus() == 3);
	CHECK(Menu.GetItems(Settings)[2] == "Load game (no save yet)");
	CHECK(Menu.Update({.bActivate = true}, Settings) == std::vector{EGameMenuCommand::Restart});
	CHECK_FALSE(Menu.IsOpen());

	(void)Menu.Update({.bPause = true}, Settings);
	(void)Menu.Update({.bUp = true}, Settings);
	(void)Menu.Update({.bUp = true}, Settings);
	CHECK(Menu.GetFocus() == 4);
	(void)Menu.Update({.bActivate = true}, Settings);
	REQUIRE(Menu.GetScreen() == EGameMenuScreen::Settings);

	// Left and Right change the focused setting and report it. HDR is unavailable here, so Down skips it.
	(void)Menu.Update({.bDown = true}, Settings);
	CHECK(Menu.GetFocus() == 2);
	CHECK(Menu.Update({.bLeft = true}, Settings) == std::vector{EGameMenuCommand::SettingsChanged});
	CHECK(Settings.Quality == 1);
	(void)Menu.Update({.bDown = true}, Settings);
	(void)Menu.Update({.bLeft = true}, Settings);
	(void)Menu.Update({.bLeft = true}, Settings);
	CHECK(Settings.Volumes[0] == doctest::Approx(0.8f));
	CHECK(Menu.GetItems(Settings)[3] == "Master volume   < 80% >");
	for (int Press = 0; Press < 20; ++Press)
	{
		(void)Menu.Update({.bRight = true}, Settings);
	}

	CHECK(Settings.Volumes[0] == 1.f);

	// Controls: activating Jump waits for a key; the next key binds and gameplay never sees it, Escape cancels instead.
	for (int Press = 0; Press < 5; ++Press)
	{
		(void)Menu.Update({.bDown = true}, Settings);
	}

	(void)Menu.Update({.bActivate = true}, Settings);
	REQUIRE(Menu.GetScreen() == EGameMenuScreen::Controls);
	for (int Press = 0; Press < 4; ++Press)
	{
		(void)Menu.Update({.bDown = true}, Settings);
	}

	(void)Menu.Update({.bActivate = true}, Settings);
	REQUIRE(Menu.GetCapture() == EGameAction::Jump);
	CHECK(Menu.GetItems(Settings)[4] == "Jump   press a key or button...");
	(void)Menu.Update({.bPause = true}, Settings);
	CHECK_FALSE(Menu.GetCapture());
	CHECK(Menu.GetScreen() == EGameMenuScreen::Controls);
	(void)Menu.Update({.bActivate = true}, Settings);
	CHECK(Menu.Update({.bActivate = true, .Key = EKey::J}, Settings) == std::vector{EGameMenuCommand::SettingsChanged});
	CHECK(Settings.Bindings.Get(EGameAction::Jump).Key == EKey::J);
	(void)Menu.Update({.bActivate = true}, Settings);
	(void)Menu.Update({.Pad = EGamepadButton::Start}, Settings);
	CHECK(Menu.GetCapture());
	(void)Menu.Update({.Pad = EGamepadButton::North}, Settings);
	CHECK(Settings.Bindings.Get(EGameAction::Jump).Pad == EGamepadButton::North);
	CHECK_FALSE(Settings.Bindings.Get(EGameAction::ToggleCamera).Pad);

	// Escape backs out one screen at a time, then resumes.
	(void)Menu.Update({.bBack = true}, Settings);
	CHECK(Menu.GetScreen() == EGameMenuScreen::Settings);
	(void)Menu.Update({.bPause = true}, Settings);
	CHECK(Menu.GetScreen() == EGameMenuScreen::Pause);
	CHECK(Menu.Update({.bPause = true}, Settings) == std::vector{EGameMenuCommand::Resume});
	CHECK_FALSE(Menu.OwnsInput());

	Menu.ShowNotice("Game saved", 1.f);
	Menu.Tick(0.5f);
	CHECK(Menu.GetNotice() == "Game saved");
	Menu.Tick(0.6f);
	CHECK(Menu.GetNotice().empty());
}

TEST_CASE("The game menu and HUD text follow the culture, with plurals, number formats, and each language named in itself")
{
	FLocalization Text;
	FVirtualFileSystem Files(std::filesystem::current_path() / CatalogDirectory);
	Files.MountDirectory(Files.GetRoot());
	const auto Catalogs = LoadTextCatalogs(Files, {});
	REQUIRE(Catalogs);
	REQUIRE(Text.SetCatalogs(*Catalogs));
	CHECK(Text.GetAvailableCultures() == std::vector<std::string>{"en", "pl", "ru"});

	FGameMenu Menu(Text);
	FGameMenuSettings Settings{.Volumes = {0.8f, 1.f, 1.f}, .bCanLoad = true, .SaveTime = FCivilTime{.Year = 2026, .Month = 11, .Day = 10, .Hour = 21, .Minute = 5}};
	(void)Menu.Update({.bPause = true}, Settings);
	REQUIRE(Text.SetCulture("pl"));
	CHECK(Menu.GetItems(Settings)[0] == "Wznów");
	CHECK(Menu.GetItems(Settings)[2] == "Wczytaj grę   10.11.2026, 21:05");
	REQUIRE(Text.SetCulture("ru"));
	CHECK(Menu.GetItems(Settings)[0] == "Продолжить");

	// The language setting cycles through the loaded cultures, then pseudolocalization outside Shipping.
	(void)Menu.Update({.bDown = true}, Settings);
	(void)Menu.Update({.bDown = true}, Settings);
	(void)Menu.Update({.bDown = true}, Settings);
	(void)Menu.Update({.bDown = true}, Settings);
	(void)Menu.Update({.bActivate = true}, Settings);
	REQUIRE(Menu.GetScreen() == EGameMenuScreen::Settings);
	CHECK(Menu.GetItems(Settings)[3] == "Общая громкость   < 80\u00a0% >");
	CHECK(Menu.GetItems(Settings)[7] == "Язык   < English >");
	for (int Press = 0; Press < 6; ++Press)
	{
		(void)Menu.Update({.bDown = true}, Settings);
	}

	CHECK(Menu.GetFocus() == 7);
	CHECK(Menu.Update({.bRight = true}, Settings) == std::vector{EGameMenuCommand::SettingsChanged});
	CHECK(Settings.Culture == "pl");
	CHECK(Menu.GetItems(Settings)[7] == "Язык   < Polski >");
	(void)Menu.Update({.bLeft = true}, Settings);
	(void)Menu.Update({.bLeft = true}, Settings);
	CHECK(Settings.Culture == (GetBuildConfiguration() == EBuildConfiguration::Shipping ? "ru" : std::string(PseudoCulture)));

	// Course times use the culture's decimal separator, and a finish under a minute picks the plural form of the shown number.
	CHECK(FormatGameSeconds(*FCulture::Find("en"), 65.204) == "1:05.20");
	CHECK(FormatGameSeconds(*FCulture::Find("pl"), 12.345) == "12,35");
	CHECK(FormatGameSeconds(*FCulture::Find("ru"), 59.999) == "1:00,00");
	const auto Finished = [&Text](const double Seconds, const std::uint32_t Digits)
	{
		return Text.Format(Text::Game::Hud_FinishedSeconds, std::array{FMessageArgument{"seconds", FFixedNumber{.Value = Seconds, .FractionDigits = Digits}}});
	};

	REQUIRE(Text.SetCulture("ru"));
	CHECK(Finished(21., 0) == "Финиш за 21 секунду");
	CHECK(Finished(5., 0) == "Финиш за 5 секунд");
	CHECK(Finished(21., 2) == "Финиш за 21,00 секунды");
	REQUIRE(Text.SetCulture("pl"));
	CHECK(Finished(3., 0) == "Ukończono w 3 sekundy");
	CHECK(Finished(1., 2) == "Ukończono w 1,00 sekundy");
	REQUIRE(Text.SetCulture("en"));
	CHECK(Finished(1., 0) == "Finished in 1 second");
	CHECK(Finished(1., 2) == "Finished in 1.00 seconds");
}
}
