#include "Herta/GameFramework/GameMenu.h"

#include "Herta/Core/Build.h"
#include "Herta/GameFramework/GameText.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace Herta
{
namespace
{
namespace Game = Text::Game;
constexpr std::array<std::string_view, 3> QualityKeys{"low", "medium", "high"};

// The cultures the language setting cycles through: every loaded one, and pseudolocalization outside Shipping builds for checking layouts.
[[nodiscard]] std::vector<std::string> GetMenuCultures(const FLocalization& Text)
{
	std::vector<std::string> Cultures = Text.GetAvailableCultures();
	if (GetBuildConfiguration() != EBuildConfiguration::Shipping)
	{
		Cultures.emplace_back(PseudoCulture);
	}

	return Cultures;
}

[[nodiscard]] float Step(const float Value, const int Direction, const float Increment, const float Minimum, const float Maximum)
{
	// Rounding to the step keeps repeated presses from drifting off it.
	return std::clamp(std::round(Value / Increment + static_cast<float>(Direction)) * Increment, Minimum, Maximum);
}
}

std::vector<FGameMenu::FItem> FGameMenu::GetScreenItems(const FGameMenuSettings& Settings) const
{
	switch (Screen)
	{
		case EGameMenuScreen::Pause:
			return {{EItem::Resume}, {EItem::Save}, {.Kind = EItem::Load, .bEnabled = Settings.bCanLoad}, {EItem::Restart}, {EItem::Settings}, {EItem::Quit}};
		case EGameMenuScreen::Settings:
			return {{EItem::Fullscreen}, {.Kind = EItem::Hdr, .bEnabled = Settings.bHdrAvailable}, {EItem::Quality}, {EItem::MasterVolume}, {EItem::EffectsVolume}, {EItem::AmbientVolume}, {EItem::Sensitivity}, {EItem::Language}, {EItem::Controls}, {EItem::Back}};
		case EGameMenuScreen::Controls:
		{
			std::vector<FItem> Items;
			for (std::size_t Action = 0; Action < GameActionCount; ++Action)
			{
				Items.push_back({.Kind = EItem::Binding, .Action = Action});
			}

			Items.push_back({EItem::ResetBindings});
			Items.push_back({EItem::Back});
			return Items;
		}
		case EGameMenuScreen::Closed:
			break;
	}

	return {};
}

FGameMenu::FGameMenu(const FLocalization& InText)
    : TextSource(&InText)
{
}

std::vector<std::string> FGameMenu::GetItems(const FGameMenuSettings& Settings) const
{
	const FLocalization& Localization = *TextSource;
	const auto Enabled = [](const bool bEnabled)
	{
		return std::array{FMessageArgument{"enabled", std::string(bEnabled ? "true" : "false")}};
	};

	const auto Volume = [&Localization](const FTextId Id, const float Value)
	{
		return Localization.Format(Id, std::array{FMessageArgument{"volume", static_cast<double>(Value)}});
	};

	std::vector<std::string> Labels;
	for (const FItem& Item : GetScreenItems(Settings))
	{
		switch (Item.Kind)
		{
			case EItem::Resume:
				Labels.push_back(Localization.Format(Game::Menu_Resume));
				break;
			case EItem::Save:
				Labels.push_back(Localization.Format(Game::Menu_Save));
				break;
			case EItem::Load:
				if (Settings.bCanLoad && Settings.SaveTime)
				{
					Labels.push_back(Localization.Format(Game::Menu_Load, std::array{FMessageArgument{"when", *Settings.SaveTime}}));
				}
				else
				{
					Labels.push_back(Localization.Format(Settings.bCanLoad ? Game::Menu_Load : Game::Menu_LoadUnavailable));
				}

				break;
			case EItem::Restart:
				Labels.push_back(Localization.Format(Game::Menu_Restart));
				break;
			case EItem::Settings:
				Labels.push_back(Localization.Format(Game::Menu_Settings));
				break;
			case EItem::Quit:
				Labels.push_back(Localization.Format(Game::Menu_Quit));
				break;
			case EItem::Fullscreen:
				Labels.push_back(Localization.Format(Game::Settings_Fullscreen, Enabled(Settings.bFullscreen)));
				break;
			case EItem::Hdr:
				Labels.push_back(Settings.bHdrAvailable ? Localization.Format(Game::Settings_Hdr, Enabled(Settings.bHdr)) : Localization.Format(Game::Settings_HdrUnavailable));
				break;
			case EItem::Quality:
				Labels.push_back(Localization.Format(Game::Settings_Quality, std::array{FMessageArgument{"quality", std::string(QualityKeys[std::min<std::size_t>(Settings.Quality, 2)])}}));
				break;
			case EItem::MasterVolume:
				Labels.push_back(Volume(Game::Settings_MasterVolume, Settings.Volumes[0]));
				break;
			case EItem::EffectsVolume:
				Labels.push_back(Volume(Game::Settings_EffectsVolume, Settings.Volumes[1]));
				break;
			case EItem::AmbientVolume:
				Labels.push_back(Volume(Game::Settings_AmbientVolume, Settings.Volumes[2]));
				break;
			case EItem::Sensitivity:
				Labels.push_back(Localization.Format(Game::Settings_Sensitivity, std::array{FMessageArgument{"value", FFixedNumber{.Value = Settings.Sensitivity, .FractionDigits = 1}}}));
				break;
			case EItem::Language:
				Labels.push_back(Localization.Format(Game::Settings_Language, std::array{FMessageArgument{"language", Localization.FormatIn(Settings.Culture, Game::Language_Name)}}));
				break;
			case EItem::Controls:
				Labels.push_back(Localization.Format(Game::Menu_Controls));
				break;
			case EItem::Binding:
			{
				const auto Action = static_cast<EGameAction>(Item.Action);
				const FGameBinding& Binding = Settings.Bindings.Get(Action);
				const std::string Name = GetGameActionName(Localization, Action);
				if (Capture == Action)
				{
					Labels.push_back(Localization.Format(Game::Controls_Capture, std::array{FMessageArgument{"action", Name}}));
					break;
				}

				const std::string Unbound = Localization.Format(Game::Controls_Unbound);
				const std::array Arguments{
				    FMessageArgument{"action", Name},
				    FMessageArgument{"keyboard", FormatKeyboardBinding(Binding, Unbound)},
				    FMessageArgument{"gamepad", FormatGamepadBinding(Binding, Unbound)},
				};

				Labels.push_back(Localization.Format(Game::Controls_Binding, Arguments));
				break;
			}
			case EItem::ResetBindings:
				Labels.push_back(Localization.Format(Game::Menu_ResetBindings));
				break;
			case EItem::Back:
				Labels.push_back(Localization.Format(Game::Menu_Back));
				break;
		}
	}

	return Labels;
}

void FGameMenu::Open()
{
	Screen = EGameMenuScreen::Pause;
	Focus = 0;
	Capture.reset();
}

void FGameMenu::Close()
{
	Screen = EGameMenuScreen::Closed;
	Capture.reset();
}

void FGameMenu::ShowNotice(std::string Text, const float Seconds)
{
	Notice = std::move(Text);
	NoticeSeconds = Seconds;
}

void FGameMenu::Tick(const float DeltaSeconds)
{
	NoticeSeconds = std::max(NoticeSeconds - DeltaSeconds, 0.f);
	if (NoticeSeconds == 0.f)
	{
		Notice.clear();
	}
}

bool FGameMenu::IsOpen() const noexcept
{
	return Screen != EGameMenuScreen::Closed;
}

bool FGameMenu::OwnsInput() const noexcept
{
	return IsOpen() || Capture.has_value();
}

EGameMenuScreen FGameMenu::GetScreen() const noexcept
{
	return Screen;
}

std::size_t FGameMenu::GetFocus() const noexcept
{
	return Focus;
}

std::optional<EGameAction> FGameMenu::GetCapture() const noexcept
{
	return Capture;
}

const std::string& FGameMenu::GetNotice() const noexcept
{
	return Notice;
}

void FGameMenu::Back(std::vector<EGameMenuCommand>& Commands)
{
	if (Screen == EGameMenuScreen::Controls)
	{
		Screen = EGameMenuScreen::Settings;
		Focus = 7;
	}
	else if (Screen == EGameMenuScreen::Settings)
	{
		Screen = EGameMenuScreen::Pause;
		Focus = 4;
	}
	else
	{
		Close();
		Commands.push_back(EGameMenuCommand::Resume);
	}
}

void FGameMenu::MoveFocus(const int Direction, const std::vector<FItem>& Items)
{
	if (Items.empty())
	{
		return;
	}

	// Disabled items are skipped, and at least one item on every screen is enabled.
	for (std::size_t Tries = 0; Tries < Items.size(); ++Tries)
	{
		Focus = (Focus + Items.size() + static_cast<std::size_t>(Direction + static_cast<int>(Items.size()))) % Items.size();
		if (Items[Focus].bEnabled)
		{
			return;
		}
	}
}

void FGameMenu::Adjust(const FItem& Item, const int Direction, FGameMenuSettings& Settings, std::vector<EGameMenuCommand>& Commands)
{
	switch (Item.Kind)
	{
		case EItem::Fullscreen:
			Settings.bFullscreen = !Settings.bFullscreen;
			break;
		case EItem::Hdr:
			Settings.bHdr = !Settings.bHdr;
			break;
		case EItem::Quality:
			Settings.Quality = static_cast<std::uint32_t>((static_cast<int>(std::min<std::uint32_t>(Settings.Quality, 2)) + 3 + Direction) % 3);
			break;
		case EItem::MasterVolume:
		case EItem::EffectsVolume:
		case EItem::AmbientVolume:
		{
			float& Volume = Settings.Volumes[static_cast<std::size_t>(Item.Kind) - static_cast<std::size_t>(EItem::MasterVolume)];
			Volume = Step(Volume, Direction, 0.1f, 0.f, 1.f);
			break;
		}
		case EItem::Sensitivity:
			Settings.Sensitivity = Step(Settings.Sensitivity, Direction, 0.1f, 0.2f, 5.f);
			break;
		case EItem::Language:
		{
			const std::vector<std::string> Cultures = GetMenuCultures(*TextSource);
			const auto Current = std::ranges::find(Cultures, Settings.Culture);
			const std::ptrdiff_t Index = Current != Cultures.end() ? Current - Cultures.begin() : 0;
			const auto Count = static_cast<std::ptrdiff_t>(Cultures.size());
			Settings.Culture = Cultures[static_cast<std::size_t>((Index + Direction + Count) % Count)];
			break;
		}
		default:
			return;
	}

	Commands.push_back(EGameMenuCommand::SettingsChanged);
}

void FGameMenu::Activate(const FItem& Item, FGameMenuSettings& Settings, std::vector<EGameMenuCommand>& Commands)
{
	if (!Item.bEnabled)
	{
		return;
	}

	switch (Item.Kind)
	{
		case EItem::Resume:
			Close();
			Commands.push_back(EGameMenuCommand::Resume);
			break;
		case EItem::Save:
			Commands.push_back(EGameMenuCommand::Save);
			break;
		case EItem::Load:
			Close();
			Commands.push_back(EGameMenuCommand::Load);
			break;
		case EItem::Restart:
			Close();
			Commands.push_back(EGameMenuCommand::Restart);
			break;
		case EItem::Settings:
			Screen = EGameMenuScreen::Settings;
			Focus = 0;
			break;
		case EItem::Quit:
			Commands.push_back(EGameMenuCommand::Quit);
			break;
		case EItem::Controls:
			Screen = EGameMenuScreen::Controls;
			Focus = 0;
			break;
		case EItem::Binding:
			Capture = static_cast<EGameAction>(Item.Action);
			break;
		case EItem::ResetBindings:
			Settings.Bindings = GetDefaultGameBindings();
			Commands.push_back(EGameMenuCommand::SettingsChanged);
			break;
		case EItem::Back:
			Back(Commands);
			break;
		default:
			Adjust(Item, 1, Settings, Commands);
			break;
	}
}

std::vector<EGameMenuCommand> FGameMenu::Update(const FGameMenuInput& Input, FGameMenuSettings& Settings)
{
	std::vector<EGameMenuCommand> Commands;
	if (Screen == EGameMenuScreen::Closed)
	{
		if (Input.bPause || Input.bFocusLost)
		{
			Open();
		}

		return Commands;
	}

	// A binding takes the next key, mouse button, or gamepad button; Escape and Start only cancel it.
	if (Capture)
	{
		if (Input.bPause || Input.bBack || Input.bFocusLost)
		{
			Capture.reset();
		}
		else if (Input.Key && *Input.Key != EKey::Escape)
		{
			BindKeyboard(Settings.Bindings, *Capture, Input.Key, std::nullopt);
			Capture.reset();
			Commands.push_back(EGameMenuCommand::SettingsChanged);
		}
		else if (Input.Mouse)
		{
			BindKeyboard(Settings.Bindings, *Capture, std::nullopt, Input.Mouse);
			Capture.reset();
			Commands.push_back(EGameMenuCommand::SettingsChanged);
		}
		else if (Input.Pad && *Input.Pad != EGamepadButton::Start)
		{
			BindGamepad(Settings.Bindings, *Capture, *Input.Pad);
			Capture.reset();
			Commands.push_back(EGameMenuCommand::SettingsChanged);
		}

		return Commands;
	}

	if (Input.bPause || Input.bBack)
	{
		Back(Commands);
		return Commands;
	}

	const std::vector<FItem> Items = GetScreenItems(Settings);
	Focus = std::min(Focus, Items.size() - 1);
	if (Input.bUp)
	{
		MoveFocus(-1, Items);
	}

	if (Input.bDown)
	{
		MoveFocus(1, Items);
	}

	if (Input.bLeft || Input.bRight)
	{
		Adjust(Items[Focus], Input.bRight ? 1 : -1, Settings, Commands);
	}

	if (Input.bActivate)
	{
		Activate(Items[Focus], Settings, Commands);
	}

	return Commands;
}

std::vector<EGameMenuCommand> FGameMenu::Draw(FGameUI& UI, const FGameMenuInput& Input, FGameMenuSettings& Settings)
{
	std::vector<EGameMenuCommand> Commands;
	const std::uint32_t Text = MakeGameUIColor(238, 240, 246);
	const std::uint32_t Panel = MakeGameUIColor(12, 14, 20, 200);
	if (!Notice.empty())
	{
		const FVector2 Size = UI.MeasureText(Notice, EGameUIFont::Body);
		UI.DrawRect({.Anchor = {0.5f, 0.f}, .Pivot = {0.5f, 0.f}, .Offset = {0.f, 24.f}}, {Size.X + 40.f, Size.Y + 28.f}, Panel, 12.f);
		UI.DrawText(Notice, {.Anchor = {0.5f, 0.f}, .Pivot = {0.5f, 0.f}, .Offset = {0.f, 38.f}}, EGameUIFont::Body, MakeGameUIColor(242, 196, 92));
	}

	if (Screen == EGameMenuScreen::Closed)
	{
		return Commands;
	}

	// Oversized so the dimming covers the screen at any aspect ratio.
	UI.DrawRect({.Anchor = {0.5f, 0.5f}, .Pivot = {0.5f, 0.5f}}, {100000.f, 100000.f}, MakeGameUIColor(0, 0, 0, 140));
	const std::string Title = TextSource->Format(Screen == EGameMenuScreen::Pause ? Game::Menu_Title_Paused : Screen == EGameMenuScreen::Settings ? Game::Menu_Title_Settings : Game::Menu_Title_Controls);
	const std::vector<std::string> Labels = GetItems(Settings);
	const float Height = static_cast<float>(Labels.size()) * 62.f;
	UI.DrawText(Title, {.Anchor = {0.5f, 0.5f}, .Pivot = {0.5f, 1.f}, .Offset = {0.f, -Height * 0.5f - 60.f}}, EGameUIFont::Title, Text);
	std::vector<std::string_view> Views(Labels.begin(), Labels.end());

	// The menu's own keys are handled in Update, so only the pointer reaches the list here.
	const FGameUIInput Pointer{.Pointer = Capture ? std::nullopt : Input.Pointer, .bClicked = Input.bClicked && !Capture};
	if (const auto Chosen = UI.DrawMenu(Views, Focus, Pointer))
	{
		const std::vector<FItem> Items = GetScreenItems(Settings);
		if (*Chosen < Items.size())
		{
			Activate(Items[*Chosen], Settings, Commands);
		}
	}

	const std::string Hint = TextSource->Format(Capture ? Game::Hint_Capture : Screen == EGameMenuScreen::Pause ? Game::Hint_Pause : Game::Hint_Settings);
	const FVector2 Size = UI.MeasureText(Hint, EGameUIFont::Body);
	UI.DrawRect({.Anchor = {0.5f, 1.f}, .Pivot = {0.5f, 1.f}, .Offset = {0.f, -24.f}}, {Size.X + 40.f, Size.Y + 28.f}, Panel, 12.f);
	UI.DrawText(Hint, {.Anchor = {0.5f, 1.f}, .Pivot = {0.5f, 1.f}, .Offset = {0.f, -38.f}}, EGameUIFont::Body, MakeGameUIColor(220, 224, 232));
	return Commands;
}
}
