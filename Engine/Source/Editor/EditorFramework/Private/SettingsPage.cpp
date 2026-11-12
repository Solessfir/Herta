#include "SettingsPage.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

namespace Herta
{
namespace
{
[[nodiscard]] float GetSettingsScale()
{
	return ImGui::GetFontSize() / 15.f;
}
}

bool BeginSettingsPage(FToolUIContext& ToolUI, const std::string_view TabLabel, const EToolUIMenuIcon Icon, bool& bOpen, const std::span<const FSettingsSection> Sections, int& Selected)
{
	ToolUI.SetNextDocumentIcon(Icon);
	if (!ToolUI.BeginPanel(TabLabel, &bOpen, EToolUIPanelKind::Document))
	{
		ToolUI.EndPanel();
		return false;
	}

	const float Scale = GetSettingsScale();
	Selected = std::clamp(Selected, 0, std::max(0, static_cast<int>(Sections.size()) - 1));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.f * Scale, 8.f * Scale});
	ImGui::BeginChild("##Sections", {216.f * Scale, 0.f}, ImGuiChildFlags_AlwaysUseWindowPadding);
	ImGui::PopStyleVar();
	ImGui::PushStyleVar(ImGuiStyleVar_SelectableRounding, ImGui::GetStyle().FrameRounding);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.f * Scale, 2.f * Scale});
	for (int Index = 0; Index < static_cast<int>(Sections.size()); ++Index)
	{
		const FSettingsSection& Section = Sections[static_cast<std::size_t>(Index)];
		if (!Section.Heading.empty())
		{
			ImGui::Dummy({0.f, Index > 0 ? 8.f * Scale : 2.f * Scale});
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.f * Scale);
			ImGui::TextDisabled("%.*s", static_cast<int>(Section.Heading.size()), Section.Heading.data());
		}

		ImGui::PushID(Index);
		const ImVec2 Start = ImGui::GetCursorScreenPos();
		const float Height = 28.f * Scale;
		if (ImGui::Selectable("##Section", Selected == Index, 0, {0.f, Height}))
		{
			Selected = Index;
		}

		const bool bLit = Selected == Index || ImGui::IsItemHovered();
		ImDrawList* const Draw = ImGui::GetWindowDrawList();
		const float TextY = Start.y + (Height - ImGui::GetFontSize()) * 0.5f;
		Draw->AddText({Start.x + 10.f * Scale, TextY}, ImGui::GetColorU32(bLit ? ImGuiCol_Text : ImGuiCol_TextDisabled), Section.Label.c_str());
		if (!Section.Detail.empty())
		{
			const float Right = ImGui::GetItemRectMax().x - 10.f * Scale;
			Draw->AddText({Right - ImGui::CalcTextSize(Section.Detail.c_str()).x, TextY}, ImGui::GetColorU32(ImGuiCol_TextDisabled), Section.Detail.c_str());
		}

		ImGui::PopID();
	}

	ImGui::PopStyleVar(2);
	ImGui::EndChild();

	// A hairline keeps the list and the form apart on the one panel surface.
	const ImVec2 ListMaximum = ImGui::GetItemRectMax();
	ImGui::GetWindowDrawList()->AddLine({ListMaximum.x + 4.f * Scale, ImGui::GetItemRectMin().y}, {ListMaximum.x + 4.f * Scale, ListMaximum.y}, ImGui::GetColorU32(ImGuiCol_Border));
	ImGui::SameLine(0.f, 9.f * Scale);
	ImGui::BeginChild("##Form", {0.f, 0.f});
	const float ColumnWidth = std::min(ImGui::GetContentRegionAvail().x, 720.f * Scale);
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::floor((ImGui::GetContentRegionAvail().x - ColumnWidth) * 0.5f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {20.f * Scale, 20.f * Scale});
	ImGui::BeginChild("##Column", {ColumnWidth, 0.f}, ImGuiChildFlags_AlwaysUseWindowPadding);
	ImGui::PopStyleVar();
	return true;
}

void EndSettingsPage(FToolUIContext& ToolUI)
{
	ImGui::EndChild();
	ImGui::EndChild();
	ToolUI.EndPanel();
}

void DrawSettingsTitle(const std::string_view Title)
{
	ImGui::PushFont(nullptr, std::round(ImGui::GetFontSize() * 1.25f));
	ImGui::TextUnformatted(Title.data(), Title.data() + Title.size());
	ImGui::PopFont();
}

void DrawSettingsHeading(const std::string_view Heading)
{
	const float Scale = GetSettingsScale();
	ImGui::Dummy({0.f, 10.f * Scale});
	ImGui::TextUnformatted(Heading.data(), Heading.data() + Heading.size());
	ImGui::Separator();
	ImGui::Dummy({0.f, 2.f * Scale});
}

void BeginSettingsRow(const std::string_view Label, const std::string_view Hint, const float ControlWidth)
{
	const float Scale = GetSettingsScale();
	const float Width = ControlWidth * Scale;
	const float Frame = ImGui::GetFrameHeight();
	const float TextBlock = ImGui::GetFontSize() * (Hint.empty() ? 1.f : 2.f) + (Hint.empty() ? 0.f : 2.f * Scale);
	// The row is as tall as its two text lines; the control centers on it, and the padding below comes from the next row's own offset.
	const float Padding = std::max(0.f, std::floor((std::max(TextBlock, Frame) + 14.f * Scale - Frame) * 0.5f));
	const ImVec2 Start = ImGui::GetCursorScreenPos();
	const float Right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
	const float TextTop = Start.y + Padding + (Frame - TextBlock) * 0.5f;
	ImDrawList* const Draw = ImGui::GetWindowDrawList();
	const ImVec4 Clip{Start.x, Start.y, Right - Width - 12.f * Scale, Start.y + Frame + Padding * 2.f};
	Draw->AddText(nullptr, 0.f, {Start.x, TextTop}, ImGui::GetColorU32(ImGuiCol_Text), Label.data(), Label.data() + Label.size(), 0.f, &Clip);
	if (!Hint.empty())
	{
		Draw->AddText(nullptr, 0.f, {Start.x, TextTop + ImGui::GetFontSize() + 2.f * Scale}, ImGui::GetColorU32(ImGuiCol_TextDisabled), Hint.data(), Hint.data() + Hint.size(), 0.f, &Clip);
	}

	ImGui::SetCursorScreenPos({Right - Width, Start.y + Padding});
	ImGui::SetNextItemWidth(Width);
}
}
