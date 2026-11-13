#pragma once

#include "Herta/ToolUI/ToolUI.h"

#include <imgui.h>

namespace Herta
{
inline constexpr float ViewportIconButtonSize = 28.f;

[[nodiscard]] constexpr bool CanFitViewportToolbarIsland(const float MinimumX, const float Width, const float LeftOccupiedEnd, const float RightOccupiedStart, const float Gap) noexcept
{
	return MinimumX - LeftOccupiedEnd >= Gap && RightOccupiedStart - (MinimumX + Width) >= Gap;
}

inline bool ViewportIconButton(const char* const Id, const EToolUIMenuIcon Icon, const char* const Tooltip, const float Scale, const bool bSelected = false)
{
	const ImVec2 Position = ImGui::GetCursorScreenPos();
	const ImVec2 Size{ViewportIconButtonSize * Scale, ViewportIconButtonSize * Scale};
	const bool bPressed = ImGui::InvisibleButton(Id, Size, ImGuiButtonFlags_EnableNav);
	const bool bHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
	ImDrawList* const Draw = ImGui::GetWindowDrawList();
	const float Highlight = bSelected ? 0.2f : bHovered ? 0.1f
	                                                    : 0.f;
	Draw->AddRectFilled(Position, {Position.x + Size.x, Position.y + Size.y}, ImGui::GetColorU32(ImVec4{1, 1, 1, Highlight}), Size.y * 0.5f);
	if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
	{
		Draw->AddRect(Position, {Position.x + Size.x, Position.y + Size.y}, ImGui::GetColorU32(ImGuiCol_NavCursor), Size.y * 0.5f, Scale);
	}

	const ImU32 Color = Icon == EToolUIMenuIcon::Play ? IM_COL32(105, 200, 139, 255) : ImGui::GetColorU32(bSelected || bHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
	ToolUIIcon(Icon, Position.x + Size.x * 0.5f, Position.y + Size.y * 0.5f, Scale, Color);

	if (bHovered)
	{
		ImGui::SetTooltip("%s", Tooltip);
	}

	return bPressed;
}
}
