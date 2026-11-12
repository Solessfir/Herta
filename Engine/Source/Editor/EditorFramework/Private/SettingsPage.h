#pragma once

#include "Herta/ToolUI/ToolUI.h"

#include <span>
#include <string>
#include <string_view>

namespace Herta
{
struct FSettingsSection
{
	std::string Label{};
	// A non-empty heading starts a group above this section.
	std::string_view Heading{};
	// Shown at the right of the section's row, such as a count.
	std::string Detail{};
};

// A document tab laid out as a section list on the left and a centered form column, as Project Settings, Preferences, and Plugins share.
// Returns whether the tab is in front; only then draw the form and call EndSettingsPage.
[[nodiscard]] bool BeginSettingsPage(FToolUIContext& ToolUI, std::string_view TabLabel, EToolUIMenuIcon Icon, bool& bOpen, std::span<const FSettingsSection> Sections, int& Selected);
void EndSettingsPage(FToolUIContext& ToolUI);
// The form's title. Items the caller adds on the same line sit to its right.
void DrawSettingsTitle(std::string_view Title);
void DrawSettingsHeading(std::string_view Heading);
// Starts one row: the label and its hint on the left, with the next item placed at the right edge and given ControlWidth logical pixels.
void BeginSettingsRow(std::string_view Label, std::string_view Hint, float ControlWidth = 250.f);
}
