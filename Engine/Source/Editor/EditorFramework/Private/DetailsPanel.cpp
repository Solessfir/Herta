#include "DetailsPanel.h"

#include "Herta/EditorCore/PreviewScaleEdit.h"
#include "Herta/ToolUI/ToolUI.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <im3d.h>
#include <im3d_math.h>
#include <imgui.h>
#include <numbers>
#include <string_view>
#include <utility>

namespace Herta
{
namespace
{
constexpr std::array AxisColors{IM_COL32(213, 123, 127, 255), IM_COL32(131, 185, 147, 255), IM_COL32(124, 158, 213, 255)};

[[nodiscard]] bool MatchesSearch(const std::string_view Name, const std::string_view Query)
{
	return std::search(Name.begin(), Name.end(), Query.begin(), Query.end(), [](const char Left, const char Right)
	                   {
		                   return std::tolower(static_cast<unsigned char>(Left)) == std::tolower(static_cast<unsigned char>(Right));
	                   }) != Name.end();
}

void DrawPreviewNotice()
{
	const float AvailableHeight = ImGui::GetContentRegionAvail().y;
	const float BottomGap = ImGui::GetTextLineHeightWithSpacing() + 4.0f * ImGui::GetFontSize() / 15.0f;
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, AvailableHeight - BottomGap));
	ImGui::TextDisabled("Preview changes are not saved.");
}

void DrawCubeIcon(const ImVec2 Position, const float Size)
{
	ImDrawList* const Draw = ImGui::GetWindowDrawList();
	Draw->AddRectFilled(Position, {Position.x + Size, Position.y + Size}, IM_COL32(255, 255, 255, 9), 4.0f * Size / 30.0f);
	Draw->AddRect(Position, {Position.x + Size, Position.y + Size}, IM_COL32(255, 255, 255, 25), 4.0f * Size / 30.0f);
	const ImVec2 Center{Position.x + Size * 0.5f, Position.y + Size * 0.5f};
	const float Radius = Size * 0.23f;
	const ImVec2 Top{Center.x, Center.y - Radius};
	const ImVec2 LeftTop{Center.x - Radius, Center.y - Radius * 0.5f};
	const ImVec2 RightTop{Center.x + Radius, Center.y - Radius * 0.5f};
	const ImVec2 Middle{Center.x, Center.y};
	const ImVec2 LeftBottom{LeftTop.x, Center.y + Radius * 0.7f};
	const ImVec2 RightBottom{RightTop.x, LeftBottom.y};
	const ImVec2 Bottom{Center.x, Center.y + Radius * 1.2f};
	const ImU32 Color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
	const float Stroke = std::max(1.0f, Size / 30.0f);
	for (const auto& [From, To] : {std::pair{Top, LeftTop}, std::pair{Top, RightTop}, std::pair{LeftTop, Middle}, std::pair{RightTop, Middle}, std::pair{Middle, Bottom}, std::pair{LeftTop, LeftBottom}, std::pair{RightTop, RightBottom}, std::pair{LeftBottom, Bottom}, std::pair{RightBottom, Bottom}})
	{
		Draw->AddLine(From, To, Color, Stroke);
	}
}

void DrawCheckerThumbnail(const ImVec2 Position, const float Size)
{
	ImDrawList* const Draw = ImGui::GetWindowDrawList();
	const float Cell = Size / 4.0f;
	const float Seam = std::max(1.0f, Size / 40.0f);
	Draw->AddRectFilled(Position, {Position.x + Size, Position.y + Size}, IM_COL32(70, 83, 96, 255));
	for (int Row = 0; Row < 4; ++Row)
	{
		for (int Column = 0; Column < 4; ++Column)
		{
			const ImVec2 Minimum{Position.x + static_cast<float>(Column) * Cell, Position.y + static_cast<float>(Row) * Cell};
			Draw->AddRectFilled({Minimum.x + Seam, Minimum.y + Seam}, {Minimum.x + Cell, Minimum.y + Cell}, (Row + Column) % 2 == 0 ? IM_COL32(145, 160, 174, 255) : IM_COL32(82, 99, 117, 255));
		}
	}
	Draw->AddRect(Position, {Position.x + Size, Position.y + Size}, ImGui::GetColorU32(ImGuiCol_Border));
}

void DrawSpaceSelector(const char* const Label, EDetailsTransformSpace& Space)
{
	const float Scale = ImGui::GetFontSize() / 15.0f;
	const ImVec2 Size{76.0f * Scale, ImGui::GetFrameHeight()};
	const ImVec2 Position = ImGui::GetCursorScreenPos();
	if (ImGui::InvisibleButton("Coordinate space##Space", Size, ImGuiButtonFlags_EnableNav))
	{
		ImGui::OpenPopup("Space");
	}
	ImDrawList* const DrawList = ImGui::GetWindowDrawList();
	if (ImGui::IsItemHovered())
	{
		DrawList->AddRectFilled(Position, {Position.x + Size.x, Position.y + Size.y}, IM_COL32(255, 255, 255, 12), 4.0f * Scale);
	}
	if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
	{
		DrawList->AddRect(Position, {Position.x + Size.x, Position.y + Size.y}, ImGui::GetColorU32(ImGuiCol_NavCursor), 4.0f * Scale);
	}
	DrawList->AddText({Position.x + 2.0f * Scale, Position.y + (Size.y - ImGui::GetFontSize()) * 0.5f}, ImGui::GetColorU32(ImGuiCol_Text), Label);
	const float ArrowX = Position.x + Size.x - 11.0f * Scale;
	const float ArrowY = Position.y + Size.y * 0.5f;
	const ImU32 ArrowColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
	DrawList->AddLine({ArrowX - 3.0f * Scale, ArrowY - 1.0f * Scale}, {ArrowX, ArrowY + 2.0f * Scale}, ArrowColor, Scale);
	DrawList->AddLine({ArrowX, ArrowY + 2.0f * Scale}, {ArrowX + 3.0f * Scale, ArrowY - 1.0f * Scale}, ArrowColor, Scale);
	if (ImGui::BeginPopup("Space"))
	{
		if (ImGui::MenuItem("Local", nullptr, Space == EDetailsTransformSpace::Local))
		{
			Space = EDetailsTransformSpace::Local;
		}
		if (ImGui::MenuItem("World", nullptr, Space == EDetailsTransformSpace::World))
		{
			Space = EDetailsTransformSpace::World;
		}
		ImGui::EndPopup();
	}
}

void DrawLockButton(bool& bLocked)
{
	const float Scale = ImGui::GetFontSize() / 15.0f;
	const ImVec2 Size{17.0f * Scale, ImGui::GetFrameHeight()};
	const ImVec2 Position = ImGui::GetCursorScreenPos();
	if (ImGui::InvisibleButton("Scale proportions##ScaleLock", Size, ImGuiButtonFlags_EnableNav))
	{
		bLocked = !bLocked;
	}
	const ImU32 Color = ImGui::GetColorU32(bLocked ? ImGuiCol_Text : ImGuiCol_TextDisabled);
	ImDrawList* const DrawList = ImGui::GetWindowDrawList();
	if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
	{
		DrawList->AddRect(Position, {Position.x + Size.x, Position.y + Size.y}, ImGui::GetColorU32(ImGuiCol_NavCursor), 3.0f * Scale);
	}
	const ImVec2 Center{Position.x + Size.x * 0.5f, Position.y + Size.y * 0.5f};
	DrawList->AddRect({Center.x - 4.0f * Scale, Center.y}, {Center.x + 4.0f * Scale, Center.y + 5.0f * Scale}, Color, Scale, 0, Scale);
	const float Offset = bLocked ? 0.0f : 3.0f * Scale;
	DrawList->AddLine({Center.x - 2.5f * Scale + Offset, Center.y}, {Center.x - 2.5f * Scale + Offset, Center.y - 4.0f * Scale}, Color, Scale);
	DrawList->AddLine({Center.x - 2.5f * Scale + Offset, Center.y - 4.0f * Scale}, {Center.x + 2.5f * Scale + Offset, Center.y - 4.0f * Scale}, Color, Scale);
	DrawList->AddLine({Center.x + 2.5f * Scale + Offset, Center.y - 4.0f * Scale}, {Center.x + 2.5f * Scale + Offset, Center.y - (bLocked ? 0.0f : 2.0f) * Scale}, Color, Scale);
}

[[nodiscard]] bool DrawResetButton(const char* const Label)
{
	const float Scale = ImGui::GetFontSize() / 15.0f;
	const ImVec2 Size{18.0f * Scale, ImGui::GetFrameHeight()};
	const ImVec2 Position = ImGui::GetCursorScreenPos();
	const bool bReset = ImGui::InvisibleButton("Reset transform##Reset", Size, ImGuiButtonFlags_EnableNav);
	ImDrawList* const DrawList = ImGui::GetWindowDrawList();
	if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
	{
		DrawList->AddRect(Position, {Position.x + Size.x, Position.y + Size.y}, ImGui::GetColorU32(ImGuiCol_NavCursor), 3.0f * Scale);
	}
	const ImVec2 Center{Position.x + Size.x * 0.5f, Position.y + Size.y * 0.5f};
	const ImU32 Color = ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled);
	DrawList->PathArcTo(Center, 4.0f * Scale, 0.0f, 5.0f, 12);
	DrawList->PathStroke(Color, 0, Scale);
	DrawList->AddTriangleFilled({Center.x + 0.5f * Scale, Center.y - 5.0f * Scale}, {Center.x + 5.0f * Scale, Center.y - 5.0f * Scale}, {Center.x + 3.0f * Scale, Center.y - 1.0f * Scale}, Color);
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Reset %s", Label);
	}
	return bReset;
}

template <typename Change>
bool DrawTransformRow(const char* const Label, Im3d::Vec3& Value, const float Speed, const float Minimum, const float Maximum, const float Reset, EDetailsTransformSpace& Space, Change&& OnChange, bool* const bLocked = nullptr)
{
	ImGui::PushID(Label);
	const float Scale = ImGui::GetFontSize() / 15.0f;
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4.0f * Scale, ImGui::GetStyle().ItemSpacing.y});
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.0f * Scale, 4.0f * Scale});
	const float Spacing = ImGui::GetStyle().ItemSpacing.x;
	const float LabelWidth = 76.0f * Scale;
	const float LockWidth = 17.0f * Scale + Spacing;
	const float ResetWidth = 18.0f * Scale;
	const float Width = std::max(1.0f, (ImGui::GetContentRegionAvail().x - LabelWidth - LockWidth - ResetWidth - Spacing * 4.0f) / 3.0f);
	DrawSpaceSelector(Label, Space);
	if (bLocked != nullptr)
	{
		ImGui::SameLine();
		DrawLockButton(*bLocked);
	}
	else
	{
		ImGui::SameLine();
		ImGui::Dummy({17.0f * Scale, ImGui::GetFrameHeight()});
	}
	ImGui::SameLine();
	for (int Axis = 0; Axis < 3; ++Axis)
	{
		if (Axis > 0)
		{
			ImGui::SameLine();
		}
		ImGui::PushID(Axis);
		ImGui::SetNextItemWidth(Width);
		float Candidate = Value[Axis];
		if (ImGui::DragFloat("##Value", &Candidate, Speed, Minimum, Maximum, "%.3f", ImGuiSliderFlags_AlwaysClamp))
		{
			OnChange(Axis, Candidate);
		}
		const ImVec2 Min = ImGui::GetItemRectMin();
		const ImVec2 Max = ImGui::GetItemRectMax();
		ImGui::GetWindowDrawList()->AddRectFilled({Min.x, Min.y + 3.0f}, {Min.x + 2.0f, Max.y - 3.0f}, AxisColors[static_cast<std::size_t>(Axis)]);
		ImGui::PopID();
	}
	ImGui::SameLine();
	const bool bReset = DrawResetButton(Label);
	if (bReset)
	{
		Value = Im3d::Vec3(Reset);
	}
	ImGui::PopStyleVar(2);
	ImGui::PopID();
	return bReset;
}
}

void DrawPreviewDetailsPanel(FToolUIContext& ToolUI, const bool bSelected, const bool bDragging, Im3d::Vec3& Translation, Im3d::Mat3& Rotation, Im3d::Vec3& Scale, FDetailsPanelState& State)
{
	if (!ToolUI.BeginPanel("Details"))
	{
		ToolUI.EndPanel();
		return;
	}
	if (!bSelected)
	{
		ImGui::TextDisabled("Select an object in the viewport.");
		DrawPreviewNotice();
		ToolUI.EndPanel();
		return;
	}
	const float UiScale = ImGui::GetFontSize() / 15.0f;
	const ImVec2 HeadingPosition = ImGui::GetCursorScreenPos();
	ImGui::Dummy({30.0f * UiScale, 30.0f * UiScale});
	DrawCubeIcon(HeadingPosition, 30.0f * UiScale);
	ImGui::SameLine();
	ImGui::BeginGroup();
	const float HeadingTextX = ImGui::GetCursorPosX();
	ImGui::TextUnformatted("Preview Cube");
	ImGui::SameLine();
	ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("1 selected").x));
	ImGui::TextDisabled("1 selected");
	ImGui::SetCursorPosX(HeadingTextX);
	ImGui::TextDisabled("Static mesh");
	ImGui::EndGroup();
	ImGui::Spacing();
	ImGui::SetNextItemWidth(-1.0f);
	ImGui::InputTextWithHint("##PropertySearch", "Search properties...", State.Search.data(), State.Search.size());
	const std::string_view Query(State.Search.data());
	const bool bShowAll = Query.empty() || MatchesSearch("Transform", Query);
	const bool bLocation = bShowAll || MatchesSearch("Location", Query);
	const bool bRotation = bShowAll || MatchesSearch("Rotation", Query);
	const bool bScale = bShowAll || MatchesSearch("Scale", Query);
	const bool bTransform = bLocation || bRotation || bScale;
	const bool bMesh = Query.empty() || MatchesSearch("Static Mesh", Query) || MatchesSearch("Preview Cube", Query) || MatchesSearch("Built-in", Query) || MatchesSearch("Checker material", Query) || MatchesSearch("Checkerboard", Query);
	if (!bTransform && !bMesh)
	{
		ImGui::TextDisabled("No matching properties.");
		DrawPreviewNotice();
		ToolUI.EndPanel();
		return;
	}
	constexpr ImGuiTreeNodeFlags SectionFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
	if (bTransform && !Query.empty())
	{
		ImGui::SetNextItemOpen(true, ImGuiCond_Always);
	}
	if (bTransform && ImGui::TreeNodeEx("Transform", SectionFlags))
	{
		ImGui::BeginDisabled(bDragging);
		if (bLocation)
		{
			DrawTransformRow("Location", Translation, 0.01f, 0.0f, 0.0f, 0.0f, State.Spaces[0], [&](const int Axis, const float Candidate)
			                 {
				                 if (std::isfinite(Candidate))
				                 {
					                 Translation[Axis] = Candidate;
				                 }
			                 });
		}
		Im3d::Vec3 RotationDegrees = Im3d::ToEulerXYZ(Rotation) * (180.0f / std::numbers::pi_v<float>);
		const bool bRotationReset = bRotation && DrawTransformRow("Rotation", RotationDegrees, 0.1f, -360.0f, 360.0f, 0.0f, State.Spaces[1], [&](const int Axis, const float Candidate)
		                                                          {
			                                                          if (std::isfinite(Candidate))
			                                                          {
				                                                          RotationDegrees[Axis] = Candidate;
				                                                          Im3d::Vec3 Radians = RotationDegrees * (std::numbers::pi_v<float> / 180.0f);
				                                                          Rotation = Im3d::FromEulerXYZ(Radians);
			                                                          }
		                                                          });
		if (bRotationReset)
		{
			Im3d::Vec3 Zero(0.0f);
			Rotation = Im3d::FromEulerXYZ(Zero);
		}
		if (bScale)
		{
			DrawTransformRow("Scale", Scale, 0.01f, MinimumPreviewScale, MaximumPreviewScale, 1.0f, State.Spaces[2], [&](const int Axis, const float Candidate)
			                 {
				                 if (!std::isfinite(Candidate) || Candidate < MinimumPreviewScale || Candidate > MaximumPreviewScale)
				                 {
					                 return;
				                 }
				                 if (State.bScaleLocked)
				                 {
					                 std::array Current{Scale.x, Scale.y, Scale.z};
					                 if (TrySetProportionalPreviewScale(Current, static_cast<std::size_t>(Axis), Candidate))
					                 {
						                 Scale = {Current[0], Current[1], Current[2]};
					                 }
				                 }
				                 else
				                 {
					                 Scale[Axis] = Candidate;
				                 }
			                 },
			                 &State.bScaleLocked);
		}
		ImGui::EndDisabled();
	}
	if (bMesh && !Query.empty())
	{
		ImGui::SetNextItemOpen(true, ImGuiCond_Always);
	}
	if (bMesh && ImGui::TreeNodeEx("Static Mesh", SectionFlags))
	{
		const ImVec2 ThumbnailPosition = ImGui::GetCursorScreenPos();
		ImGui::Dummy({40.0f * UiScale, 40.0f * UiScale});
		DrawCheckerThumbnail(ThumbnailPosition, 40.0f * UiScale);
		ImGui::SameLine();
		ImGui::BeginGroup();
		ImGui::TextUnformatted("Preview Cube");
		ImGui::TextDisabled("Built-in / Checker material");
		ImGui::EndGroup();
	}
	DrawPreviewNotice();
	ToolUI.EndPanel();
}
}
