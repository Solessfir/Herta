#include "DetailsPanel.h"

#include "Herta/Assets/AssetSearch.h"
#include "Herta/EditorCore/PreviewScaleEdit.h"
#include "Herta/EditorCore/TransformText.h"
#include "Herta/ToolUI/ToolUI.h"
#include "NumericField.h"
#include "PreviewScene.h"

#include <im3d.h>
#include <im3d_math.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Herta
{
namespace
{
constexpr std::array AxisColors{IM_COL32(213, 123, 127, 255), IM_COL32(131, 185, 147, 255), IM_COL32(124, 158, 213, 255)};

enum class ETransformClipboardFormat
{
	XYZ,
	Rotation
};

[[nodiscard]] bool MatchesSearch(const std::string_view Name, const std::string_view Query)
{
	return std::ranges::search(Name, Query, [](const char Left, const char Right)
	{
		return std::tolower(static_cast<unsigned char>(Left)) == std::tolower(static_cast<unsigned char>(Right));
	}) != Name.end();
}

void DrawCubeIcon(const ImVec2 Position, const float Size)
{
	ImDrawList* const Draw = ImGui::GetWindowDrawList();
	Draw->AddRectFilled(Position, {Position.x + Size, Position.y + Size}, IM_COL32(255, 255, 255, 9), 4.f * Size / 30.f);
	Draw->AddRect(Position, {Position.x + Size, Position.y + Size}, IM_COL32(255, 255, 255, 25), 4.f * Size / 30.f);
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
	const float Stroke = std::max(1.f, Size / 30.f);
	for (const auto& [From, To] : {std::pair{Top, LeftTop}, std::pair{Top, RightTop}, std::pair{LeftTop, Middle}, std::pair{RightTop, Middle}, std::pair{Middle, Bottom}, std::pair{LeftTop, LeftBottom}, std::pair{RightTop, RightBottom}, std::pair{LeftBottom, Bottom}, std::pair{RightBottom, Bottom}})
	{
		Draw->AddLine(From, To, Color, Stroke);
	}
}

void DrawCheckerThumbnail(const ImVec2 Position, const float Size)
{
	ImDrawList* const Draw = ImGui::GetWindowDrawList();
	const float Cell = Size / 4.f;
	const float Seam = std::max(1.f, Size / 40.f);
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

std::optional<Im3d::Vec3> DrawSpaceSelector(const char* const Label, EDetailsTransformSpace& Space, const Im3d::Vec3& Value, const ETransformClipboardFormat ClipboardFormat)
{
	const float Scale = ImGui::GetFontSize() / 15.f;
	const ImVec2 Size{82.f * Scale, ImGui::GetFrameHeight()};
	const ImVec2 Position = ImGui::GetCursorScreenPos();
	const bool bPressed = ImGui::InvisibleButton("Coordinate space##Space", Size, ImGuiButtonFlags_EnableNav);
	const bool bHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride);
	const ImGuiIO& Io = ImGui::GetIO();
	const bool bCopyRow = bHovered && Io.KeyShift && Io.MouseClicked[ImGuiMouseButton_Right];
	const bool bPasteRow = bHovered && Io.KeyShift && Io.MouseClicked[ImGuiMouseButton_Left];
	if (bCopyRow)
	{
		const FVector3 Vector{Value.x, Value.y, Value.z};
		const std::string ClipboardText = ClipboardFormat == ETransformClipboardFormat::Rotation ? FormatTransformRotationClipboard(Vector) : FormatTransformVectorClipboard(Vector);
		ImGui::SetClipboardText(ClipboardText.c_str());
	}

	std::optional<Im3d::Vec3> PastedValue;
	if (bPasteRow)
	{
		ImGui::ClearActiveID();
		if (const char* const Clipboard = ImGui::GetClipboardText(); Clipboard != nullptr)
		{
			const auto Parsed = ClipboardFormat == ETransformClipboardFormat::Rotation ? ParseTransformRotationClipboard(Clipboard) : ParseTransformVectorClipboard(Clipboard);
			if (Parsed)
			{
				PastedValue = Im3d::Vec3(Parsed->X, Parsed->Y, Parsed->Z);
			}
		}
	}

	if (bPressed && !bPasteRow)
	{
		ImGui::OpenPopup("Space");
	}

	ImDrawList* const DrawList = ImGui::GetWindowDrawList();
	if (ImGui::IsItemHovered())
	{
		DrawList->AddRectFilled(Position, {Position.x + Size.x, Position.y + Size.y}, IM_COL32(255, 255, 255, 12), 4.f * Scale);
	}

	if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
	{
		DrawList->AddRect(Position, {Position.x + Size.x, Position.y + Size.y}, ImGui::GetColorU32(ImGuiCol_NavCursor), 4.f * Scale);
	}

	DrawList->AddText({Position.x + 8.f * Scale, Position.y + (Size.y - ImGui::GetFontSize()) * 0.5f}, ImGui::GetColorU32(ImGuiCol_Text), Label);
	const float ArrowX = Position.x + Size.x - 11.f * Scale;
	const float ArrowY = Position.y + Size.y * 0.5f;
	const ImU32 ArrowColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
	DrawList->AddLine({ArrowX - 3.f * Scale, ArrowY - 1.f * Scale}, {ArrowX, ArrowY + 2.f * Scale}, ArrowColor, Scale);
	DrawList->AddLine({ArrowX, ArrowY + 2.f * Scale}, {ArrowX + 3.f * Scale, ArrowY - 1.f * Scale}, ArrowColor, Scale);
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

	return PastedValue;
}

void DrawLockButton(bool& bLocked)
{
	const float Scale = ImGui::GetFontSize() / 15.f;
	const ImVec2 Size{17.f * Scale, ImGui::GetFrameHeight()};
	const ImVec2 Position = ImGui::GetCursorScreenPos();
	if (ImGui::InvisibleButton("Scale proportions##ScaleLock", Size, ImGuiButtonFlags_EnableNav))
	{
		bLocked = !bLocked;
	}

	const ImU32 Color = ImGui::GetColorU32(bLocked ? ImGuiCol_Text : ImGuiCol_TextDisabled);
	ImDrawList* const DrawList = ImGui::GetWindowDrawList();
	if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
	{
		DrawList->AddRect(Position, {Position.x + Size.x, Position.y + Size.y}, ImGui::GetColorU32(ImGuiCol_NavCursor), 3.f * Scale);
	}

	const ImVec2 Center{Position.x + Size.x * 0.5f - 3.f * Scale, Position.y + Size.y * 0.5f};
	DrawList->AddRect({Center.x - 4.f * Scale, Center.y}, {Center.x + 4.f * Scale, Center.y + 5.f * Scale}, Color, Scale, 0, Scale);
	const float Offset = bLocked ? 0.f : 3.f * Scale;
	DrawList->AddLine({Center.x - 2.5f * Scale + Offset, Center.y}, {Center.x - 2.5f * Scale + Offset, Center.y - 4.f * Scale}, Color, Scale);
	DrawList->AddLine({Center.x - 2.5f * Scale + Offset, Center.y - 4.f * Scale}, {Center.x + 2.5f * Scale + Offset, Center.y - 4.f * Scale}, Color, Scale);
	DrawList->AddLine({Center.x + 2.5f * Scale + Offset, Center.y - 4.f * Scale}, {Center.x + 2.5f * Scale + Offset, Center.y - (bLocked ? 0.f : 2.f) * Scale}, Color, Scale);
}

[[nodiscard]] bool DrawResetButton(const char* const Label)
{
	const float Scale = ImGui::GetFontSize() / 15.f;
	const ImVec2 Size{18.f * Scale, ImGui::GetFrameHeight()};
	const ImVec2 Position = ImGui::GetCursorScreenPos();
	const bool bReset = ImGui::InvisibleButton("Reset transform##Reset", Size, ImGuiButtonFlags_EnableNav);
	ImDrawList* const DrawList = ImGui::GetWindowDrawList();
	if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
	{
		DrawList->AddRect(Position, {Position.x + Size.x, Position.y + Size.y}, ImGui::GetColorU32(ImGuiCol_NavCursor), 3.f * Scale);
	}

	const ImVec2 Center{Position.x + Size.x * 0.5f, Position.y + Size.y * 0.5f};
	const ImU32 Color = ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled);
	DrawList->PathArcTo(Center, 4.f * Scale, 0.f, 5.f, 12);
	DrawList->PathStroke(Color, 0, Scale);
	DrawList->AddTriangleFilled({Center.x + 0.5f * Scale, Center.y - 5.f * Scale}, {Center.x + 5.f * Scale, Center.y - 5.f * Scale}, {Center.x + 3.f * Scale, Center.y - 1.f * Scale}, Color);
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Reset %s", Label);
	}

	return bReset;
}

template <typename Change, typename ChangeRow>
bool DrawTransformRow(const char* const Label, Im3d::Vec3& Value, const float Speed, const float Minimum, const float Maximum, const float Reset, EDetailsTransformSpace& Space, const ETransformClipboardFormat ClipboardFormat, Change&& OnChange, ChangeRow&& OnRowChange, bool* const bLocked = nullptr)
{
	ImGui::PushID(Label);
	const float Scale = ImGui::GetFontSize() / 15.f;
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4.f * Scale, ImGui::GetStyle().ItemSpacing.y});
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.f * Scale, 4.f * Scale});
	const float Spacing = ImGui::GetStyle().ItemSpacing.x;
	const float LabelWidth = 82.f * Scale;
	const float LockWidth = 17.f * Scale + Spacing;
	const float ResetWidth = 18.f * Scale;
	const float Width = std::max(1.f, (ImGui::GetContentRegionAvail().x - LabelWidth - LockWidth - ResetWidth - Spacing * 4.f) / 3.f);
	if (const auto PastedValue = DrawSpaceSelector(Label, Space, Value, ClipboardFormat))
	{
		OnRowChange(*PastedValue);
	}

	if (bLocked != nullptr)
	{
		ImGui::SameLine();
		DrawLockButton(*bLocked);
	}
	else
	{
		ImGui::SameLine();
		ImGui::Dummy({17.f * Scale, ImGui::GetFrameHeight()});
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
		const ImGuiSliderFlags Flags = Minimum < Maximum ? ImGuiSliderFlags_AlwaysClamp : 0;
		if (DrawNumericDragFloat("##Value", &Candidate, Speed, Minimum, Maximum, "%.3f", Flags))
		{
			OnChange(Axis, Candidate);
		}

		const ImVec2 Min = ImGui::GetItemRectMin();
		const ImVec2 Max = ImGui::GetItemRectMax();
		ImGui::GetWindowDrawList()->AddRectFilled({Min.x, Min.y + 3.f}, {Min.x + 2.f, Max.y - 3.f}, AxisColors[static_cast<std::size_t>(Axis)]);
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

FDetailsMeshResult DrawPreviewDetailsPanel(FToolUIContext& ToolUI, bool& bOpen, const bool bSelected, const bool bDragging, Im3d::Vec3& Translation, Im3d::Mat3& Rotation, Im3d::Vec3& Scale, FDetailsPanelState& State, std::string& ObjectLabel, const std::size_t SelectedCount, const FDetailsMeshField* const Mesh)
{
	FDetailsMeshResult MeshResult;
	if (!ToolUI.BeginPanel("Details", &bOpen))
	{
		ToolUI.EndPanel();
		return MeshResult;
	}

	if (!bSelected)
	{
		State.bRenaming = false;
		State.bRenameRequested = false;
		ImGui::TextDisabled("Select an object in the viewport.");
		ToolUI.EndPanel();
		return MeshResult;
	}

	const float UiScale = ImGui::GetFontSize() / 15.f;
	const ImVec2 HeadingPosition = ImGui::GetCursorScreenPos();
	ImGui::Dummy({30.f * UiScale, 30.f * UiScale});
	DrawCubeIcon(HeadingPosition, 30.f * UiScale);
	ImGui::SameLine();
	ImGui::BeginGroup();
	const float HeadingTextX = ImGui::GetCursorPosX();
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {ImGui::GetStyle().ItemSpacing.x, 2.f * UiScale});
	const std::string SelectionText = std::to_string(SelectedCount) + " selected";
	const bool bStartRename = !bDragging && (State.bRenameRequested || (!State.bRenaming && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F2, false)));
	State.bRenameRequested = false;
	if (bDragging)
	{
		State.bRenaming = false;
	}

	if (bStartRename)
	{
		State.RenameBuffer.fill('\0');
		std::copy_n(ObjectLabel.begin(), std::min(ObjectLabel.size(), State.RenameBuffer.size() - 1), State.RenameBuffer.begin());
		State.bRenaming = true;
		ImGui::SetKeyboardFocusHere();
	}

	if (State.bRenaming)
	{
		ImGui::SetNextItemWidth(std::max(40.f * UiScale, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(SelectionText.c_str()).x - ImGui::GetStyle().ItemSpacing.x));
		const bool bCancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
		ImGui::PushStyleColor(ImGuiCol_NavCursor, {0, 0, 0, 0});
		const bool bCommit = ImGui::InputText("##ObjectLabel", State.RenameBuffer.data(), State.RenameBuffer.size(), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
		ImGui::PopStyleColor();
		if (bCancel || bCommit || (!bStartRename && ImGui::IsItemDeactivated()))
		{
			if (!bCancel)
			{
				RenamePreviewObject(ObjectLabel, State.RenameBuffer.data());
			}

			State.bRenaming = false;
		}
	}
	else
	{
		ImGui::TextUnformatted(ObjectLabel.data(), ObjectLabel.data() + ObjectLabel.size());
		if (!bDragging && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			State.bRenameRequested = true;
		}

		if (!bDragging && ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Double-click or press F2 to rename this object.");
		}
	}

	ImGui::SameLine();
	ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(SelectionText.c_str()).x));
	ImGui::TextDisabled("%s", SelectionText.c_str());
	ImGui::SetCursorPosX(HeadingTextX);
	ImGui::TextDisabled("Static Mesh");
	ImGui::PopStyleVar();
	ImGui::EndGroup();
	ImGui::Spacing();
	ImGui::SetNextItemWidth(-1.f);
	ToolUI.DrawSearchField("##PropertySearch", "Search", State.Search.data(), State.Search.size());
	const std::string_view Query(State.Search.data());
	const bool bShowAll = Query.empty() || MatchesSearch("Transform", Query);
	const bool bLocation = bShowAll || MatchesSearch("Location", Query);
	const bool bRotation = bShowAll || MatchesSearch("Rotation", Query);
	const bool bScale = bShowAll || MatchesSearch("Scale", Query);
	const bool bTransform = bLocation || bRotation || bScale;
	const bool bHasMesh = Mesh != nullptr && Mesh->Selected >= 0 && static_cast<std::size_t>(Mesh->Selected) < Mesh->Options.size();
	const std::string_view MeshLabel = bHasMesh ? std::string_view(Mesh->Options[static_cast<std::size_t>(Mesh->Selected)]) : std::string_view("No mesh");
	const bool bMesh = Query.empty() || MatchesSearch("Static Mesh", Query) || MatchesSearch(ObjectLabel, Query) || MatchesSearch(MeshLabel, Query);
	if (!bTransform && !bMesh)
	{
		ImGui::TextDisabled("No matching properties.");
		ToolUI.EndPanel();
		return MeshResult;
	}

	const auto DrawSectionHeader = [](const char* const Label)
	{
		const ImVec2 Position = ImGui::GetCursorScreenPos();
		const ImVec2 Padding = ImGui::GetStyle().FramePadding;
		const float FontSize = ImGui::GetFontSize();
		const ImU32 TextColor = ImGui::GetColorU32(ImGuiCol_Text);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
		ImGui::PushStyleColor(ImGuiCol_Header, {0, 0, 0, 0});
		ImGui::PushStyleColor(ImGuiCol_Text, {0, 0, 0, 0});
		const bool bOpen = ImGui::TreeNodeEx(Label, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_Framed);
		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar();
		const float ArrowScale = 0.7f;
		const float ArrowInsetY = FontSize * (1.f - ArrowScale) * 0.5f;
		ImDrawList* const DrawList = ImGui::GetWindowDrawList();
		ImGui::RenderArrow(DrawList, {Position.x + Padding.x, Position.y + Padding.y + ArrowInsetY}, TextColor, bOpen ? ImGuiDir_Down : ImGuiDir_Right, ArrowScale);
		DrawList->AddText({Position.x + FontSize + Padding.x * 2.f, Position.y + Padding.y}, TextColor, Label);
		return bOpen;
	};

	if (bTransform && !Query.empty())
	{
		ImGui::SetNextItemOpen(true, ImGuiCond_Always);
	}

	if (bTransform && DrawSectionHeader("Transform"))
	{
		ImGui::BeginDisabled(bDragging);
		if (bLocation)
		{
			DrawTransformRow("Location", Translation, 0.01f, 0.f, 0.f, 0.f, State.Spaces[0], ETransformClipboardFormat::XYZ, [&](const int Axis, const float Candidate)
			{
				if (std::isfinite(Candidate))
				{
					Translation[Axis] = Candidate;
				}
			}, [&](const Im3d::Vec3& Candidate)
			{
				if (!std::isfinite(Candidate.x) || !std::isfinite(Candidate.y) || !std::isfinite(Candidate.z))
				{
					return false;
				}

				Translation = Candidate;
				return true;
			});
		}

		Im3d::Vec3 RotationDegrees = Im3d::ToEulerXYZ(Rotation) * (180.f / std::numbers::pi_v<float>);
		const bool bRotationReset = bRotation && DrawTransformRow("Rotation", RotationDegrees, 0.1f, -360.f, 360.f, 0.f, State.Spaces[1], ETransformClipboardFormat::Rotation, [&](const int Axis, const float Candidate)
		{
			if (std::isfinite(Candidate))
			{
				RotationDegrees[Axis] = Candidate;
				Im3d::Vec3 Radians = RotationDegrees * (std::numbers::pi_v<float> / 180.f);
				Rotation = Im3d::FromEulerXYZ(Radians);
			}
		}, [&](const Im3d::Vec3& Candidate)
		{
			if (!std::isfinite(Candidate.x) || !std::isfinite(Candidate.y) || !std::isfinite(Candidate.z))
			{
				return false;
			}

			RotationDegrees = {std::clamp(Candidate.x, -360.f, 360.f), std::clamp(Candidate.y, -360.f, 360.f), std::clamp(Candidate.z, -360.f, 360.f)};
			Im3d::Vec3 Radians = RotationDegrees * (std::numbers::pi_v<float> / 180.f);
			Rotation = Im3d::FromEulerXYZ(Radians);
			return true;
		});

		if (bRotationReset)
		{
			Im3d::Vec3 Zero(0.f);
			Rotation = Im3d::FromEulerXYZ(Zero);
		}

		if (bScale)
		{
			DrawTransformRow("Scale", Scale, 0.01f, MinimumPreviewScale, MaximumPreviewScale, 1.f, State.Spaces[2], ETransformClipboardFormat::XYZ, [&](const int Axis, const float Candidate)
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
			}, [&](const Im3d::Vec3& Candidate)
			{
				if (!std::isfinite(Candidate.x) || !std::isfinite(Candidate.y) || !std::isfinite(Candidate.z))
				{
					return false;
				}

				Scale = {std::clamp(Candidate.x, MinimumPreviewScale, MaximumPreviewScale), std::clamp(Candidate.y, MinimumPreviewScale, MaximumPreviewScale), std::clamp(Candidate.z, MinimumPreviewScale, MaximumPreviewScale)};
				return true;
			}, &State.bScaleLocked);
		}

		ImGui::EndDisabled();
	}

	if (bMesh && !Query.empty())
	{
		ImGui::SetNextItemOpen(true, ImGuiCond_Always);
	}

	if (bMesh && DrawSectionHeader("Static Mesh"))
	{
		// Asset picker row: thumbnail beside the file name, with the folder or load status underneath.
		const std::size_t FolderEnd = MeshLabel.rfind('/');
		const std::string MeshName(FolderEnd == std::string_view::npos ? MeshLabel : MeshLabel.substr(FolderEnd + 1));
		const std::string_view MeshFolder = FolderEnd == std::string_view::npos ? std::string_view() : MeshLabel.substr(0, FolderEnd);
		const float ThumbnailSize = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y + ImGui::GetTextLineHeight();
		// ponytail: one placeholder thumbnail for every mesh until asset thumbnails exist.
		const ImVec2 ThumbnailPosition = ImGui::GetCursorScreenPos();
		ImGui::Dummy({ThumbnailSize, ThumbnailSize});
		DrawCheckerThumbnail(ThumbnailPosition, ThumbnailSize);
		ImGui::SameLine();
		ImGui::BeginGroup();
		if (Mesh != nullptr && !Mesh->Options.empty())
		{
			ImGui::BeginDisabled(bDragging);
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::BeginCombo("##PreviewMesh", MeshName.c_str()))
			{
				MeshResult.bOptionsOpened = ImGui::IsWindowAppearing();
				if (MeshResult.bOptionsOpened)
				{
					State.MeshSearch.fill('\0');
					ImGui::SetKeyboardFocusHere();
				}

				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::InputTextWithHint("##MeshSearch", "Search assets", State.MeshSearch.data(), State.MeshSearch.size());
				std::vector<std::string_view> Candidates(Mesh->Options.begin(), Mesh->Options.end());
				const std::optional<std::vector<FAssetSearchMatch>> Matches = SearchAssets(Candidates, State.MeshSearch.data());
				if (Matches && Matches->empty())
				{
					ImGui::TextDisabled("No matching assets");
				}

				for (const FAssetSearchMatch& Match : Matches.value_or(std::vector<FAssetSearchMatch>{}))
				{
					const std::size_t Index = Match.Index;
					const bool bCurrent = static_cast<int>(Index) == Mesh->Selected;
					if (ImGui::Selectable(Mesh->Options[Index].c_str(), bCurrent) && !bCurrent)
					{
						MeshResult.Chosen = static_cast<int>(Index);
					}
				}

				ImGui::EndCombo();
			}

			ImGui::EndDisabled();
			ImGui::SetItemTooltip("%.*s", static_cast<int>(MeshLabel.size()), MeshLabel.data());
		}
		else
		{
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(MeshName.c_str());
		}

		const bool bStatus = Mesh != nullptr && !Mesh->Status.empty();
		const std::string_view Detail = bStatus ? Mesh->Status : MeshFolder;
		ImGui::PushStyleColor(ImGuiCol_Text, bStatus && Mesh->bError ? ImVec4{0.92f, 0.38f, 0.33f, 1.f} : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::PushTextWrapPos(0.f);
		ImGui::TextUnformatted(Detail.data(), Detail.data() + Detail.size());
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
		ImGui::EndGroup();
	}

	ToolUI.EndPanel();
	return MeshResult;
}
}
