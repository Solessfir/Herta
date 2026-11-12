#include "EditorFrameworkPrivate.h"
#include "Herta/Core/Build.h"
#include "Herta/Platform/FileSystem.h"
#include "Herta/ToolUI/EditorText.h"
#include "SettingsPage.h"

#include <fstream>

namespace Herta
{
namespace
{
namespace Editor = Text::Editor;

[[nodiscard]] constexpr const char* GetPanelTransparencyLabel(const EPanelTransparency Mode) noexcept
{
	switch (Mode)
	{
		case EPanelTransparency::AllPanels:
			return "All panels";
		case EPanelTransparency::FloatingOnly:
			return "Floating panels";
		case EPanelTransparency::DockedOnly:
			return "Docked panels";
		case EPanelTransparency::Disabled:
			return "Opaque panels";
	}

	return "All panels";
}

// Percent sliders edit fractions the appearance stores from zero to one.
void DrawPercentRow(const std::string_view Label, const std::string_view Hint, const char* const Id, float& Fraction)
{
	float Percent = Fraction * 100.f;
	BeginSettingsRow(Label, Hint);
	if (DrawNumericSliderFloat(Id, &Percent, 0.f, 100.f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
	{
		Fraction = Percent / 100.f;
	}
}
}

void FEditorFramework::FImplementation::DrawHdrDisplaySettings(FEditorDisplay& Display)
{
	BeginSettingsRow("HDR output", DisplayState.bHdrSupported ? "Present the editor in HDR10." : "This display does not offer HDR10. On Windows, turn on Use HDR in display settings.");
	ImGui::BeginDisabled(!DisplayState.bHdrSupported && !Display.bHdrOutput);
	ToolUIToggle("##HdrOutput", &Display.bHdrOutput);
	ImGui::EndDisabled();
	if (!Display.bHdrOutput || !DisplayState.bHdrActive)
	{
		bHdrCalibration = false;
		return;
	}

	float Peak = DisplayState.PeakLuminance;
	BeginSettingsRow("Peak brightness", "The brightest highlight the display shows.");
	if (DrawNumericSliderFloat("##HdrPeak", &Peak, MinimumHdrPeakLuminance, MaximumHdrPeakLuminance, Display.PeakLuminance > 0.f ? "%.0f nits" : "%.0f nits (system)", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
	{
		Display.PeakLuminance = Peak;
	}

	float PaperWhite = DisplayState.PaperWhite;
	BeginSettingsRow("Paper white", "Brightness of white in the editor and of a white surface in the scene.");
	if (DrawNumericSliderFloat("##HdrPaperWhite", &PaperWhite, MinimumHdrPaperWhite, MaximumHdrPaperWhite, Display.PaperWhite > 0.f ? "%.0f nits" : "%.0f nits (system)", ImGuiSliderFlags_AlwaysClamp))
	{
		Display.PaperWhite = PaperWhite;
	}

	BeginSettingsRow("Calibrate peak", "Shows a test pattern in the level viewport. Raise Peak brightness until the inner square disappears.");
	ToolUIToggle("##HdrCalibration", &bHdrCalibration);
	BeginSettingsRow("System values", DisplayState.SystemPeakLuminance > 0.f ? "Follow the brightness the operating system reports for this display." : "The operating system reports no brightness here, so 200 and 1000 nits apply.");
	ImGui::BeginDisabled(Display.PaperWhite == 0.f && Display.PeakLuminance == 0.f);
	if (ImGui::Button("Use system values", {ImGui::CalcItemWidth(), 0.f}))
	{
		Display.PaperWhite = 0.f;
		Display.PeakLuminance = 0.f;
	}

	ImGui::EndDisabled();
}

void FEditorFramework::FImplementation::DrawPreferencesTab()
{
	if (!bPreferencesOpen)
	{
		bHdrCalibration = false;
		return;
	}

	if (std::exchange(bPreferencesFocusRequested, false))
	{
		ImGui::SetNextWindowFocus();
	}

	const std::array<FSettingsSection, 3> Sections{{
	    {.Label = "Appearance", .Heading = "Editor"},
	    {.Label = Localization.Format(Editor::Menu_Language)},
	    {.Label = "Display"},
	}};

	const std::string TabLabel = Localization.Format(Editor::Menu_Preferences) + "###Preferences";
	if (!BeginSettingsPage(*ToolUI, TabLabel, EToolUIMenuIcon::Appearance, bPreferencesOpen, Sections, PreferencesSection))
	{
		bHdrCalibration = false;
		return;
	}

	const float Scale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
	FEditorAppearance Appearance = ToolUI->GetAppearance();
	DrawSettingsTitle(Sections[static_cast<std::size_t>(PreferencesSection)].Label);
	if (PreferencesSection == 0)
	{
		const float ResetWidth = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2.f;
		ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ResetWidth);
		if (ImGui::Button("Reset"))
		{
			// Display settings describe the monitor and the language the reader, not the look, so they survive a reset.
			FEditorAppearance Defaults{};
			Defaults.Display = Appearance.Display;
			Defaults.Culture = Appearance.Culture;
			Appearance = std::move(Defaults);
		}

		ImGui::SetItemTooltip("Restore the default appearance");
		DrawSettingsHeading("Panels");
		BeginSettingsRow("Transparency", "Which panels let the scene show through.");
		if (ImGui::BeginCombo("##PanelTransparency", GetPanelTransparencyLabel(Appearance.PanelTransparency)))
		{
			for (const EPanelTransparency Mode : {EPanelTransparency::AllPanels, EPanelTransparency::FloatingOnly, EPanelTransparency::DockedOnly, EPanelTransparency::Disabled})
			{
				if (ImGui::Selectable(GetPanelTransparencyLabel(Mode), Appearance.PanelTransparency == Mode))
				{
					Appearance.PanelTransparency = Mode;
				}
			}

			ImGui::EndCombo();
		}

		DrawPercentRow("Opacity", "How solid a transparent panel is.", "##Opacity", Appearance.PanelOpacity);
		BeginSettingsRow("Blur", "Backdrop blur behind transparent panels.");
		DrawNumericSliderFloat("##Blur", &Appearance.BlurRadius, 0.f, 40.f, "%.0f px", ImGuiSliderFlags_AlwaysClamp);

		DrawSettingsHeading("Color");
		std::array Color = {
		    static_cast<float>(Appearance.Accent.Red) / 255.f,
		    static_cast<float>(Appearance.Accent.Green) / 255.f,
		    static_cast<float>(Appearance.Accent.Blue) / 255.f,
		};

		BeginSettingsRow("Accent", "Tints the window gradient.");
		if (ImGui::ColorEdit3("##AccentColor", Color.data()))
		{
			Appearance.Accent.Red = static_cast<std::uint8_t>(std::lround(Color[0] * 255.f));
			Appearance.Accent.Green = static_cast<std::uint8_t>(std::lround(Color[1] * 255.f));
			Appearance.Accent.Blue = static_cast<std::uint8_t>(std::lround(Color[2] * 255.f));
		}

		// The presets right-align under the color field, as wide as they are.
		const float Swatch = 22.f * Scale;
		const float Gap = 4.f * Scale;
		const float PresetsWidth = static_cast<float>(ToolUITheme::Presets.size()) * (Swatch + Gap) - Gap;
		ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - PresetsWidth);
		for (std::size_t Index = 0; Index < ToolUITheme::Presets.size(); ++Index)
		{
			if (Index > 0)
			{
				ImGui::SameLine(0.f, Gap);
			}

			const FToolUIColorPreset& Preset = ToolUITheme::Presets[Index];
			ImGui::PushID(static_cast<int>(Index));
			if (ImGui::ColorButton("##Preset", ImGui::ColorConvertU32ToFloat4(PackColor(Preset.Color)), ImGuiColorEditFlags_NoTooltip, {Swatch, Swatch}))
			{
				Appearance.Accent = Preset.Color;
			}

			ImGui::SetItemTooltip("%.*s", static_cast<int>(Preset.Name.size()), Preset.Name.data());
			ImGui::PopID();
		}

		DrawPercentRow("Gradient height", "How far the accent tint reaches down the window.", "##GradientHeight", Appearance.GradientHeight);
		DrawPercentRow("Saturation", "How vivid the tint is.", "##Saturation", Appearance.Saturation);
		DrawPercentRow("Intensity", "How strongly the tint shows over the dark frame.", "##Intensity", Appearance.Intensity);
	}
	else if (PreferencesSection == 1)
	{
		ImGui::Dummy({0.f, 6.f * Scale});
		// Each language shows in its own name, so someone who cannot read the current one still finds theirs.
		std::vector<std::string> Cultures = Localization.GetAvailableCultures();
		if (GetBuildConfiguration() != EBuildConfiguration::Shipping)
		{
			Cultures.emplace_back(PseudoCulture);
		}

		ImGui::PushStyleVar(ImGuiStyleVar_SelectableRounding, ImGui::GetStyle().FrameRounding);
		for (const std::string& Culture : Cultures)
		{
			const std::string Name = Localization.FormatIn(Culture, Editor::Language_Name);
			ImGui::PushID(Culture.c_str());
			const ImVec2 Start = ImGui::GetCursorScreenPos();
			const float Height = 32.f * Scale;
			const bool bCurrent = Appearance.Culture == Culture;
			if (ImGui::Selectable("##Culture", bCurrent, 0, {0.f, Height}))
			{
				Appearance.Culture = Culture;
			}

			ImDrawList* const Draw = ImGui::GetWindowDrawList();
			const float TextY = Start.y + (Height - ImGui::GetFontSize()) * 0.5f;
			Draw->AddText({Start.x + 12.f * Scale, TextY}, ImGui::GetColorU32(ImGuiCol_Text), Name.c_str());
			const float Right = ImGui::GetItemRectMax().x - 12.f * Scale;
			Draw->AddText({Right - ImGui::CalcTextSize(Culture.c_str()).x, TextY}, ImGui::GetColorU32(ImGuiCol_TextDisabled), Culture.c_str());
			ImGui::PopID();
		}

		ImGui::PopStyleVar();
	}
	else
	{
		DrawSettingsHeading("Presentation");
		bool bVSync = ToolUI->IsVSyncEnabled();
		BeginSettingsRow(Localization.Format(Editor::Menu_VSync), "Present frames in step with the display's refresh.");
		if (ToolUIToggle("##VSync", &bVSync))
		{
			ToolUI->SetVSyncEnabled(bVSync);
		}

		DrawSettingsHeading("HDR");
		DrawHdrDisplaySettings(Appearance.Display);
	}

	if (PreferencesSection != 2)
	{
		bHdrCalibration = false;
	}

	ToolUI->SetAppearance(std::move(Appearance));
	EndSettingsPage(*ToolUI);
}

void FEditorFramework::FImplementation::DrawProjectSettingsTab()
{
	if (!bProjectSettingsOpen)
	{
		ProjectSettings.reset();
		return;
	}

	if (std::exchange(bProjectSettingsFocusRequested, false))
	{
		ImGui::SetNextWindowFocus();
	}

	// The tab edits a copy of the descriptor on disk, read when it opens or the project changes.
	if (!ProjectSettings || ProjectSettings->Path != ProjectPath)
	{
		ProjectSettings = FProjectSettingsState{.Path = ProjectPath};
		if (ProjectPath.empty())
		{
			ProjectSettings->Message = "No project is open. The level was opened on its own.";
		}
		else if (auto Loaded = LoadProject(ProjectPath))
		{
			ProjectSettings->Saved = Loaded->Descriptor;
			ProjectSettings->Draft = std::move(Loaded->Descriptor);
			ProjectSettings->bLoaded = true;
		}
		else
		{
			ProjectSettings->Message = Loaded.error().Message;
		}
	}

	FProjectSettingsState& State = *ProjectSettings;
	const std::array<FSettingsSection, 4> Sections{{
	    {.Label = "General", .Heading = "Project"},
	    {.Label = "Modules", .Detail = std::to_string(State.Draft.Modules.size())},
	    {.Label = "Targets", .Detail = std::to_string(State.Draft.Targets.size())},
	    {.Label = "Content", .Detail = std::to_string(State.Draft.ContentRoots.size())},
	}};

	const bool bDirty = State.bLoaded && (State.Draft.Name != State.Saved.Name || State.Draft.StartingLevel != State.Saved.StartingLevel);
	const std::string TabLabel = std::format("{}{}###ProjectSettings", Localization.Format(Editor::Menu_ProjectSettings), bDirty ? "*" : "");
	if (!BeginSettingsPage(*ToolUI, TabLabel, EToolUIMenuIcon::Settings, bProjectSettingsOpen, Sections, State.Section))
	{
		return;
	}

	const float Scale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
	DrawSettingsTitle(Sections[static_cast<std::size_t>(State.Section)].Label);
	if (State.Section == 0 && State.bLoaded)
	{
		const float SaveWidth = ImGui::CalcTextSize("Save").x + 44.f * Scale;
		ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - SaveWidth);
		ImGui::BeginDisabled(!bDirty || State.Draft.Name.empty());
		if (ToolUIButton("Save", EToolUIMenuIcon::Save) || (bDirty && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteFocused)))
		{
			State.Message.clear();
			// The descriptor is replaced in one rename, so a failed write leaves the old one in place.
			const std::filesystem::path Temporary = State.Path.string() + ".tmp";
			const auto Text = SerializeProject(State.Draft);
			bool bWritten = false;
			if (Text)
			{
				std::ofstream File(Temporary, std::ios::binary | std::ios::trunc);
				File << *Text;
				File.close();
				bWritten = !File.fail() && MoveAtomically(Temporary, State.Path, EMoveMode::ReplaceExisting).has_value();
			}

			if (bWritten)
			{
				State.Saved = State.Draft;
			}
			else
			{
				State.Message = Text ? "Could not write " + State.Path.string() : Text.error().Message;
			}
		}

		ImGui::EndDisabled();
	}

	if (!State.Message.empty())
	{
		ImGui::PushTextWrapPos(0.f);
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(PackColor(ToolUITheme::Error)), "%s", State.Message.c_str());
		ImGui::PopTextWrapPos();
	}

	const auto ReadOnlyRow = [](const std::string_view Label, const std::string_view Hint, const std::string& Value)
	{
		BeginSettingsRow(Label, Hint);
		const float Width = ImGui::CalcItemWidth();
		const ImVec2 Start = ImGui::GetCursorScreenPos();
		ImGui::Dummy({Width, ImGui::GetFrameHeight()});
		ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), {Start.x, Start.y + ImGui::GetStyle().FramePadding.y}, {Start.x + Width, Start.y + ImGui::GetFrameHeight()}, Start.x + Width, Value.c_str(), nullptr, nullptr);
		ImGui::SetItemTooltip("%s", Value.c_str());
	};

	if (!State.bLoaded)
	{
		EndSettingsPage(*ToolUI);
		return;
	}

	if (State.Section == 0)
	{
		DrawSettingsHeading("Identity");
		BeginSettingsRow("Name", "Shown in the title bar and in packaged builds.");
		ToolUIInputText("##ProjectName", State.Draft.Name, nullptr, ImGui::CalcItemWidth());
		BeginSettingsRow("Starting level", "Opened by the editor and played first by the game, relative to the project.");
		ToolUIInputText("##StartingLevel", State.Draft.StartingLevel, "Levels/Level.hlevel", ImGui::CalcItemWidth());
		ReadOnlyRow("Engine", "The engine checkout this project builds against.", State.Draft.EngineAssociation);
		ReadOnlyRow("Project ID", "Stable identity, written when the project was created.", State.Draft.Id);
		ReadOnlyRow("Descriptor", "The file these settings are saved to.", State.Path.generic_string());
	}
	else if (State.Section == 1)
	{
		DrawSettingsHeading("Game modules");
		for (const FProjectModule& Module : State.Draft.Modules)
		{
			std::string Dependencies;
			for (const std::string& Dependency : Module.Dependencies)
			{
				Dependencies += (Dependencies.empty() ? "Depends on " : ", ") + Dependency;
			}

			ReadOnlyRow(Module.Name, Dependencies, Module.Source);
		}
	}
	else if (State.Section == 2)
	{
		DrawSettingsHeading("Build targets");
		for (const FProjectTarget& Target : State.Draft.Targets)
		{
			std::string Modules;
			for (const std::string& Module : Target.Modules)
			{
				Modules += (Modules.empty() ? "" : ", ") + Module;
			}

			ReadOnlyRow(Target.Name, Modules, Target.Type);
		}
	}
	else
	{
		DrawSettingsHeading("Content roots");
		for (const FProjectContentRoot& Root : State.Draft.ContentRoots)
		{
			ReadOnlyRow(Root.Name, "Mounted under this name in the Content Browser.", Root.Path);
		}
	}

	EndSettingsPage(*ToolUI);
}
}
