#pragma once

#include <algorithm>
#include <cmath>
#include <imgui.h>

namespace Herta
{
inline constexpr float ViewportIconButtonSize = 28.0f;

[[nodiscard]] constexpr bool CanFitViewportToolbarIsland(const float MinimumX, const float Width, const float LeftOccupiedEnd, const float RightOccupiedStart, const float Gap) noexcept
{
	return MinimumX - LeftOccupiedEnd >= Gap && RightOccupiedStart - (MinimumX + Width) >= Gap;
}

enum class EViewportIcon
{
	Select,
	Move,
	Rotate,
	Scale,
	World,
	Grid,
	Focus,
	Settings,
	Play,
	Stop,
	Simulate
};

inline bool ViewportIconButton(const char* const Id, const EViewportIcon Icon, const char* const Tooltip, const float Scale, const bool bSelected = false, const bool bReducedMotion = false)
{
	const ImVec2 Position = ImGui::GetCursorScreenPos();
	const ImVec2 Size{ViewportIconButtonSize * Scale, ViewportIconButtonSize * Scale};
	const bool bPressed = ImGui::InvisibleButton(Id, Size, ImGuiButtonFlags_EnableNav);
	const bool bHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
	ImDrawList* const Draw = ImGui::GetWindowDrawList();
	const ImGuiID ItemId = ImGui::GetItemID();
	float Highlight = ImGui::GetStateStorage()->GetFloat(ItemId);
	Highlight += ((bSelected ? 0.20f : bHovered ? 0.10f
	                                            : 0.0f) -
	              Highlight) *
	             (bReducedMotion ? 1.0f : std::min(1.0f, ImGui::GetIO().DeltaTime * 16.0f));
	ImGui::GetStateStorage()->SetFloat(ItemId, Highlight);
	Draw->AddRectFilled(Position, {Position.x + Size.x, Position.y + Size.y}, ImGui::GetColorU32(ImVec4{1, 1, 1, Highlight}), Size.y * 0.5f);
	if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
		Draw->AddRect(Position, {Position.x + Size.x, Position.y + Size.y}, ImGui::GetColorU32(ImGuiCol_NavCursor), Size.y * 0.5f, 0, Scale);
	const ImU32 Color = Icon == EViewportIcon::Play ? IM_COL32(105, 200, 139, 255) : ImGui::GetColorU32(bSelected || bHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
	const ImVec2 Center{Position.x + Size.x * 0.5f, Position.y + Size.y * 0.5f};
	const auto Point = [&](const float X, const float Y)
	{
		return ImVec2{Center.x + X * Scale, Center.y + Y * Scale};
	};
	const auto Line = [&](const float X, const float Y, const float EndX, const float EndY)
	{
		Draw->AddLine(Point(X, Y), Point(EndX, EndY), Color, 1.25f * Scale);
	};
	switch (Icon)
	{
		case EViewportIcon::Select:
			Line(-5, -7, 5, 0);
			Line(5, 0, 0, 1);
			Line(0, 1, -3, 6);
			Line(-3, 6, -5, -7);
			break;
		case EViewportIcon::Move:
			Line(-7, 0, 7, 0);
			Line(0, -7, 0, 7);
			Line(-7, 0, -4, -3);
			Line(-7, 0, -4, 3);
			Line(7, 0, 4, -3);
			Line(7, 0, 4, 3);
			Line(0, -7, -3, -4);
			Line(0, -7, 3, -4);
			Line(0, 7, -3, 4);
			Line(0, 7, 3, 4);
			break;
		case EViewportIcon::Rotate:
			Draw->PathArcTo(Center, 6.0f * Scale, 0.3f, 5.5f, 16);
			Draw->PathStroke(Color, 0, 1.25f * Scale);
			Line(5, -6, 5, -1);
			Line(5, -1, 0, -1);
			break;
		case EViewportIcon::Scale:
			Line(-6, 6, 6, -6);
			Line(6, -6, 1, -6);
			Line(6, -6, 6, -1);
			Line(-6, 0, -6, 6);
			Line(-6, 6, 0, 6);
			break;
		case EViewportIcon::World:
			Draw->AddCircle(Center, 7.0f * Scale, Color, 20, Scale);
			Line(-7, 0, 7, 0);
			Draw->AddEllipse(Center, {3 * Scale, 7 * Scale}, Color, 0, 20, Scale);
			break;
		case EViewportIcon::Grid:
			Draw->AddRect(Point(-6, -6), Point(6, 6), Color, 1, 0, Scale);
			Line(-2, -6, -2, 6);
			Line(2, -6, 2, 6);
			Line(-6, -2, 6, -2);
			Line(-6, 2, 6, 2);
			break;
		case EViewportIcon::Focus:
			Line(-6, -2, -6, -6);
			Line(-6, -6, -2, -6);
			Line(2, -6, 6, -6);
			Line(6, -6, 6, -2);
			Line(6, 2, 6, 6);
			Line(6, 6, 2, 6);
			Line(-2, 6, -6, 6);
			Line(-6, 6, -6, 2);
			Draw->AddCircle(Center, 2 * Scale, Color);
			break;
		case EViewportIcon::Settings:
			for (const float Y : {-5.0f, 0.0f, 5.0f})
				Draw->AddCircleFilled(Point(0, Y), 1.2f * Scale, Color);
			break;
		case EViewportIcon::Stop:
			Draw->AddRectFilled(Point(-5, -5), Point(5, 5), Color, Scale);
			break;
		case EViewportIcon::Play:
			Draw->AddTriangleFilled(Point(-4, -6), Point(6, 0), Point(-4, 6), Color);
			break;
		case EViewportIcon::Simulate:
			Line(-3, -7, 3, -7);
			Line(-2, -7, -2, -1);
			Line(2, -7, 2, -1);
			Line(-2, -1, -6, 6);
			Line(-6, 6, 6, 6);
			Line(6, 6, 2, -1);
			Line(-4, 2, 4, 2);
			break;
	}
	if (bHovered)
		ImGui::SetTooltip("%s", Tooltip);
	return bPressed;
}
}
