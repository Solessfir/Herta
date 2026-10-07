#include "DetailsPanel.h"

#include "ContentBrowser.h"
#include "Herta/Assets/AssetSearch.h"
#include "Herta/EditorCore/PreviewScaleEdit.h"
#include "Herta/EditorCore/TransformText.h"
#include "Herta/Level/LevelDescriptors.h"
#include "Herta/ToolUI/ToolUI.h"
#include "NumericField.h"
#include "PreviewLevel.h"

#include <im3d.h>
#include <im3d_math.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
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
constexpr float TransformLabelWidth = 72.f;

float GetPropertyLabelWidth()
{
	const float Scale = ImGui::GetFontSize() / 15.f;
	return std::max((TransformLabelWidth + 25.f) * Scale, ImGui::CalcTextSize("Angular damping").x + 12.f * Scale);
}

ImVec4 GetPropertyLabelColor()
{
	return ImLerp(ImGui::GetStyleColorVec4(ImGuiCol_Text), ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), 0.6f);
}

struct FBodyPropertyDisplay
{
	std::string_view Key;
	std::string_view SearchAlias;
	std::string_view Tooltip;
	float FLevelRigidBodySettings::* Member = nullptr;
	float Speed = 0.01f;
	const char* Format = "%.3f";
	bool bDynamicOnly = false;
};

constexpr std::array BodyProperties{
    FBodyPropertyDisplay{.Key = "massKg", .SearchAlias = "Mass (kg)", .Tooltip = "Mass in kilograms. Only dynamic bodies respond to forces.", .Member = &FLevelRigidBodySettings::MassKg, .Speed = 0.1f, .Format = "%.3f kg", .bDynamicOnly = true},
    FBodyPropertyDisplay{.Key = "friction", .SearchAlias = "Surface friction", .Tooltip = "Resistance to sliding contact. Applies to static and dynamic bodies.", .Member = &FLevelRigidBodySettings::Friction},
    FBodyPropertyDisplay{.Key = "restitution", .SearchAlias = "Restitution", .Tooltip = "Fraction of impact speed retained after a collision.", .Member = &FLevelRigidBodySettings::Restitution},
    FBodyPropertyDisplay{.Key = "linearDamping", .SearchAlias = "Linear Damping", .Tooltip = "Reduces linear velocity over time, in inverse seconds.", .Member = &FLevelRigidBodySettings::LinearDamping, .Format = "%.2f /s", .bDynamicOnly = true},
    FBodyPropertyDisplay{.Key = "angularDamping", .SearchAlias = "Angular Damping", .Tooltip = "Reduces angular velocity over time, in inverse seconds.", .Member = &FLevelRigidBodySettings::AngularDamping, .Format = "%.2f /s", .bDynamicOnly = true},
    FBodyPropertyDisplay{.Key = "gravityScale", .SearchAlias = "Gravity", .Tooltip = "Multiplier for the world's gravity acceleration.", .Member = &FLevelRigidBodySettings::GravityScale, .Speed = 0.05f, .Format = "%.2f", .bDynamicOnly = true},
};

const FLevelPropertyDescriptor& GetBodyPropertyDescriptor(const FBodyPropertyDisplay& Property)
{
	return *FindLevelPropertyDescriptor(GetLevelComponentDescriptor(ELevelComponentType::RigidBody), Property.Key);
}

enum class ETransformClipboardFormat
{
	XYZ,
	Rotation
};

void FlushDetailsEdit(const FDetailsEditCallbacks* const Edits, FDetailsMeshResult& Result, const bool bCanceled)
{
	Result.bEditFinished = false;
	Result.bEditCanceled = false;
	if (Edits != nullptr && Edits->Flush)
	{
		Edits->Flush(bCanceled);
	}
}

void BeginDetailsEdit(const FDetailsEditCallbacks* const Edits, FDetailsMeshResult& Result)
{
	if (Result.bEditFinished || Result.bEditCanceled)
	{
		FlushDetailsEdit(Edits, Result, Result.bEditCanceled);
	}

	if (Edits != nullptr && Edits->Begin)
	{
		Edits->Begin();
	}
}

[[nodiscard]] bool MatchesSearch(const std::string_view Name, const std::string_view Query)
{
	return std::ranges::search(Name, Query, [](const char Left, const char Right)
	{
		return std::tolower(static_cast<unsigned char>(Left)) == std::tolower(static_cast<unsigned char>(Right));
	}).begin()
	       != Name.end();
}

[[nodiscard]] bool MatchesBodyProperty(const FBodyPropertyDisplay& Property, const std::string_view Query)
{
	return MatchesSearch(GetBodyPropertyDescriptor(Property).Label, Query) || MatchesSearch(Property.SearchAlias, Query);
}

void DrawObjectIcon(const ImVec2 Position, const float Size, const bool bEntity)
{
	ImDrawList* const Draw = ImGui::GetWindowDrawList();
	Draw->AddRectFilled(Position, {Position.x + Size, Position.y + Size}, IM_COL32(255, 255, 255, 9), 4.f * Size / 30.f);
	Draw->AddRect(Position, {Position.x + Size, Position.y + Size}, IM_COL32(255, 255, 255, 25), 4.f * Size / 30.f);
	const ImVec2 Center{Position.x + Size * 0.5f, Position.y + Size * 0.5f};
	if (bEntity)
	{
		const ImU32 Color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
		Draw->AddCircle(Center, Size * 0.22f, Color, 16, std::max(1.f, Size / 30.f));
		Draw->AddCircleFilled(Center, Size * 0.055f, Color);
		return;
	}

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

EDetailsComponentAction DrawAddComponent(FToolUIContext& ToolUI, FDetailsPanelState& State, const FDetailsComponentField& Components, const bool bDragging)
{
	ImGui::BeginDisabled(bDragging);
	const bool bRequested = std::exchange(State.bAddComponentRequested, false);
	const float ButtonScale = ImGui::GetFontSize() / 15.f;
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.f * ButtonScale, 4.f * ButtonScale});
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f * ButtonScale);
	ImGui::PushStyleColor(ImGuiCol_Button, {1, 1, 1, 0.04f});
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {1, 1, 1, 0.08f});
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, {1, 1, 1, 0.12f});
	const bool bButtonPressed = ImGui::Button("+ Add Component");
	ImGui::PopStyleColor(3);
	ImGui::PopStyleVar(3);
	if (bButtonPressed || (bRequested && !bDragging))
	{
		State.ComponentSearch.fill('\0');
		State.ComponentResult = -1;
		ImGui::SetNavCursorVisible(false);
		ImGui::OpenPopup("Add component");
	}

	const float Scale = ImGui::GetFontSize() / ToolUI.GetMetrics().BaseFontSize;
	ImGui::SetNextWindowSizeConstraints({280.f * Scale, 0.f}, {280.f * Scale, FLT_MAX});
	ImGui::SetNextWindowBgAlpha(0.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12.f * Scale, 10.f * Scale});
	ImGui::PushStyleColor(ImGuiCol_NavCursor, {0.f, 0.f, 0.f, 0.f});
	EDetailsComponentAction Action = EDetailsComponentAction::None;
	if (ImGui::BeginPopup("Add component", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
	{
		const ImVec2 Position = ImGui::GetWindowPos();
		const ImVec2 Size = ImGui::GetWindowSize();
		ToolUI.DrawGlassSurface(Position.x, Position.y, Size.x, Size.y, ToolUI.GetMetrics().PopupRounding * Scale);
		ImGui::TextUnformatted("Add Component");
		ImGui::Separator();
		if (ImGui::IsWindowAppearing())
		{
			ImGui::SetKeyboardFocusHere();
		}

		const ImGuiID Owner = ImGui::GetID("##ComponentNavigation");
		for (const ImGuiKey Key : {ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Escape})
		{
			ImGui::SetKeyOwner(Key, Owner, ImGuiInputFlags_LockThisFrame);
		}

		const bool bUp = ImGui::IsKeyPressed(ImGuiKey_UpArrow, ImGuiInputFlags_Repeat, Owner);
		const bool bDown = ImGui::IsKeyPressed(ImGuiKey_DownArrow, ImGuiInputFlags_Repeat, Owner);
		const bool bConfirm = ImGui::IsKeyPressed(ImGuiKey_Enter, ImGuiInputFlags_None, Owner) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, ImGuiInputFlags_None, Owner);
		const bool bCancel = ImGui::IsKeyPressed(ImGuiKey_Escape, ImGuiInputFlags_None, Owner);
		ImGui::SetNextItemWidth(-FLT_MIN);
		const bool bSearchChanged = ToolUI.DrawSearchField("##ComponentSearch", "Search components", State.ComponentSearch.data(), State.ComponentSearch.size());
		const std::array<std::string_view, 10> Names{"Static Mesh", "Rigid Body", "Directional Light", "Sky Light", "Point Light", "Spot Light", "Rect Light", "Sky Atmosphere", "Height Fog", "Soft Body"};
		const std::array Added{Components.bAllMesh, Components.bAllBody, Components.bAllLight, Components.bAllLight, Components.bAllLight, Components.bAllLight, Components.bAllLight, Components.bAllSkyAtmosphere, Components.bAllHeightFog, Components.bAllSoftBody};
		constexpr std::array Actions{
		    EDetailsComponentAction::AddStaticMesh,
		    EDetailsComponentAction::AddRigidBody,
		    EDetailsComponentAction::AddDirectionalLight,
		    EDetailsComponentAction::AddSkyLight,
		    EDetailsComponentAction::AddPointLight,
		    EDetailsComponentAction::AddSpotLight,
		    EDetailsComponentAction::AddRectLight,
		    EDetailsComponentAction::AddSkyAtmosphere,
		    EDetailsComponentAction::AddHeightFog,
		    EDetailsComponentAction::AddSoftBody,
		};
		const auto Matches = SearchAssets(Names, State.ComponentSearch.data());
		std::vector<int> Available;
		for (const FAssetSearchMatch& Match : Matches.value_or(std::vector<FAssetSearchMatch>{}))
		{
			if (!Added[Match.Index])
			{
				Available.push_back(static_cast<int>(Match.Index));
			}
		}

		auto Selected = std::ranges::find(Available, State.ComponentResult);
		if (bSearchChanged || Selected == Available.end())
		{
			State.ComponentResult = Available.empty() ? -1 : Available.front();
			Selected = Available.begin();
		}

		if ((bUp || bDown) && !Available.empty())
		{
			ImGui::NavMoveRequestCancel();
			const int Count = static_cast<int>(Available.size());
			const int Index = static_cast<int>(Selected - Available.begin());
			State.ComponentResult = Available[static_cast<std::size_t>((Index + (bUp ? Count - 1 : 1)) % Count)];
		}

		if (!Matches || Matches->empty())
		{
			ImGui::TextDisabled("No matching components");
		}

		for (const FAssetSearchMatch& Match : Matches.value_or(std::vector<FAssetSearchMatch>{}))
		{
			const std::size_t Index = Match.Index;
			ImGui::BeginDisabled(Added[Index]);
			ImDrawList* const Draw = ImGui::GetWindowDrawList();
			ImDrawListSplitter Layers;
			Layers.Split(Draw, 2);
			Layers.SetCurrentChannel(Draw, 1);
			const EToolUIMenuIcon Icon = Index == 0 ? EToolUIMenuIcon::Cube : Index == 1 ? EToolUIMenuIcon::Physics
			                                                              : Index == 7   ? EToolUIMenuIcon::SkyAtmosphere
			                                                              : Index == 8   ? EToolUIMenuIcon::Fog
			                                                                             : EToolUIMenuIcon::Light;
			const bool bPressed = ToolUIMenuItem(Names[Index], Icon, nullptr, Added[Index] ? "Added" : nullptr);
			const ImVec2 Minimum = ImGui::GetItemRectMin();
			const ImVec2 Maximum = ImGui::GetItemRectMax();
			Layers.SetCurrentChannel(Draw, 0);
			if (State.ComponentResult == static_cast<int>(Index))
			{
				Draw->AddRectFilled(Minimum, Maximum, ImGui::GetColorU32(ImGuiCol_Header), ImGui::GetStyle().MenuItemRounding);
			}

			Layers.Merge(Draw);
			if (!Added[Index] && (bPressed || (bConfirm && State.ComponentResult == static_cast<int>(Index))))
			{
				Action = Actions[Index];
			}

			ImGui::EndDisabled();
		}

		if (bCancel || Action != EDetailsComponentAction::None)
		{
			if (bCancel)
			{
				Action = EDetailsComponentAction::None;
			}

			ImGui::ClearActiveID();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	ImGui::PopStyleColor();
	ImGui::PopStyleVar();
	ImGui::EndDisabled();
	return bDragging ? EDetailsComponentAction::None : Action;
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

std::optional<Im3d::Vec3> DrawSpaceSelector(const char* const Label, const float Width, EDetailsTransformSpace& Space, const Im3d::Vec3& Value, const ETransformClipboardFormat ClipboardFormat)
{
	const float Scale = ImGui::GetFontSize() / 15.f;
	const ImVec2 Size{Width, ImGui::GetFrameHeight()};
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

	DrawList->AddText({Position.x, Position.y + (Size.y - ImGui::GetFontSize()) * 0.5f}, ImGui::GetColorU32(GetPropertyLabelColor()), Label);
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

	const bool bHovered = ImGui::IsItemHovered();
	const ImU32 Color = ImGui::GetColorU32(bLocked || bHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
	ImDrawList* const DrawList = ImGui::GetWindowDrawList();
	if (bHovered)
	{
		DrawList->AddRectFilled(Position, {Position.x + Size.x, Position.y + Size.y}, ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.f * Scale);
	}

	if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
	{
		DrawList->AddRect(Position, {Position.x + Size.x, Position.y + Size.y}, ImGui::GetColorU32(ImGuiCol_NavCursor), 3.f * Scale);
	}

	const ImVec2 Center{Position.x + Size.x * 0.5f - 3.f * Scale, Position.y + Size.y * 0.5f};
	const float Stroke = 1.4f * Scale;
	DrawList->AddRect({Center.x - 4.5f * Scale, Center.y - 0.5f * Scale}, {Center.x + 4.5f * Scale, Center.y + 6.f * Scale}, Color, 1.5f * Scale, 0, Stroke);
	DrawList->AddCircleFilled({Center.x, Center.y + 2.f * Scale}, 0.8f * Scale, Color);
	DrawList->AddLine({Center.x, Center.y + 2.f * Scale}, {Center.x, Center.y + 4.f * Scale}, Color, Scale);
	const float Offset = bLocked ? 0.f : 3.f * Scale;
	DrawList->PathLineTo({Center.x - 3.f * Scale + Offset, Center.y + 0.5f * Scale});
	DrawList->PathArcTo({Center.x + Offset, Center.y - 3.f * Scale}, 3.f * Scale, std::numbers::pi_v<float>, std::numbers::pi_v<float> * 2.f, 12);
	DrawList->PathLineTo({Center.x + 3.f * Scale + Offset, Center.y + (bLocked ? 0.5f : -1.5f) * Scale});
	DrawList->PathStroke(Color, 0, Stroke);
	ImGui::SetItemTooltip(bLocked ? "Unlock scale proportions" : "Lock scale proportions");
}

[[nodiscard]] bool DrawResetButton(const char* const Label)
{
	const float Scale = ImGui::GetFontSize() / 15.f;
	const ImVec2 Size{18.f * Scale, ImGui::GetFrameHeight()};
	const ImVec2 Position = ImGui::GetCursorScreenPos();
	const bool bReset = ImGui::InvisibleButton("Reset property##Reset", Size, ImGuiButtonFlags_EnableNav);
	ImDrawList* const DrawList = ImGui::GetWindowDrawList();

	if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
	{
		DrawList->AddRect(Position, {Position.x + Size.x, Position.y + Size.y}, ImGui::GetColorU32(ImGuiCol_NavCursor), 3.f * Scale);
	}

	const ImVec2 Center{Position.x + Size.x * 0.5f, Position.y + Size.y * 0.5f};
	const ImU32 Color = ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled);
	const ImVec2 Tip{Center.x - 5.f * Scale, Center.y - Scale};
	DrawList->PathLineTo({Center.x - 2.f * Scale, Center.y - 4.f * Scale});
	DrawList->PathLineTo(Tip);
	DrawList->PathLineTo({Center.x - 2.f * Scale, Center.y + 2.f * Scale});
	DrawList->PathStroke(Color, 0, 1.4f * Scale);
	DrawList->PathLineTo(Tip);
	DrawList->PathArcTo({Center.x + 2.f * Scale, Center.y + 1.5f * Scale}, 2.5f * Scale, -std::numbers::pi_v<float> * 0.5f, std::numbers::pi_v<float> * 0.5f, 8);
	DrawList->PathLineTo({Center.x + Scale, Center.y + 4.f * Scale});
	DrawList->PathStroke(Color, 0, 1.4f * Scale);

	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Reset %s to default", Label);
	}

	return bReset;
}

void DrawMaterialSlots(FToolUIContext& ToolUI, FDetailsPanelState& State, const FDetailsComponentField& Components, const bool bDragging, FDetailsMeshResult& Result)
{
	if (Components.MaterialSlots.empty())
	{
		return;
	}

	const float Scale = ImGui::GetFontSize() / 15.f;
	ImGui::Spacing();
	ImGui::TextDisabled("Materials");
	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {0.f, 2.f * Scale});
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.f * Scale, 4.f * Scale});
	ImGui::BeginDisabled(bDragging);
	if (ImGui::BeginTable("##MeshMaterials", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings))
	{
		ImGui::TableSetupColumn("##Slot", ImGuiTableColumnFlags_WidthFixed, std::min(GetPropertyLabelWidth(), ImGui::GetContentRegionAvail().x * 0.38f));
		ImGui::TableSetupColumn("##Material", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("##Edit", ImGuiTableColumnFlags_WidthFixed, 26.f * Scale);
		ImGui::TableSetupColumn("##Reset", ImGuiTableColumnFlags_WidthFixed, 22.f * Scale);
		const std::size_t OptionCount = std::min(Components.MaterialOptionIds.size(), Components.MaterialOptionLabels.size());

		for (std::size_t SlotIndex = 0; SlotIndex < Components.MaterialSlots.size(); ++SlotIndex)
		{
			const auto& Slot = Components.MaterialSlots[SlotIndex];
			ImGui::PushID(static_cast<int>(SlotIndex));
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			const std::string Name = Slot.Name.empty() ? std::string("Slot ") + std::to_string(SlotIndex) : Slot.Name;
			const ImVec2 Position = ImGui::GetCursorScreenPos();
			const float Width = std::max(1.f, ImGui::GetContentRegionAvail().x - 4.f * Scale);
			ImGui::Dummy({Width, ImGui::GetTextLineHeight()});
			ImGui::PushStyleColor(ImGuiCol_Text, GetPropertyLabelColor());
			ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), Position, {Position.x + Width, Position.y + ImGui::GetFontSize()}, Position.x + Width, Name.c_str(), Name.c_str() + Name.size(), nullptr);
			ImGui::PopStyleColor();
			ImGui::SetItemTooltip("%s", Name.c_str());
			ImGui::TableSetColumnIndex(1);
			ImGui::SetNextItemWidth(-FLT_MIN);
			std::string_view Label = Slot.bMixed ? "Multiple values" : Slot.Selected.IsValid() ? "Missing material"
			                                                                                   : "Default (imported)";
			for (std::size_t Index = 0; Index < OptionCount; ++Index)
			{
				if (!Slot.bMixed && Slot.Selected == Components.MaterialOptionIds[Index])
				{
					Label = Components.MaterialOptionLabels[Index];
				}
			}

			const auto Slash = Label.rfind('/');
			const std::string Display(Slash == std::string_view::npos ? Label : Label.substr(Slash + 1));
			if (ImGui::BeginCombo("##Material", Display.c_str()))
			{
				if (ImGui::IsWindowAppearing())
				{
					State.MaterialSearch.fill('\0');
					ImGui::SetKeyboardFocusHere();
				}

				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::PushStyleColor(ImGuiCol_NavCursor, {0, 0, 0, 0});
				ToolUI.DrawSearchField("##MaterialSearch", "Search materials", State.MaterialSearch.data(), State.MaterialSearch.size());
				ImGui::PopStyleColor();
				if (ImGui::Selectable("Default (imported)", !Slot.bMixed && !Slot.Selected.IsValid()))
				{
					Result.MaterialChosen = {{SlotIndex, FAssetId{}}};
				}

				const std::vector<std::string_view> Names(Components.MaterialOptionLabels.begin(), Components.MaterialOptionLabels.begin() + static_cast<std::ptrdiff_t>(OptionCount));
				const auto Matches = SearchAssets(Names, State.MaterialSearch.data());
				ImGuiListClipper Clipper;
				Clipper.Begin(Matches ? static_cast<int>(Matches->size()) : 0);
				while (Clipper.Step())
				{
					for (int MatchIndex = Clipper.DisplayStart; MatchIndex < Clipper.DisplayEnd; ++MatchIndex)
					{
						const std::size_t Index = (*Matches)[static_cast<std::size_t>(MatchIndex)].Index;
						if (ImGui::Selectable(Components.MaterialOptionLabels[Index].c_str(), !Slot.bMixed && Components.MaterialOptionIds[Index] == Slot.Selected))
						{
							Result.MaterialChosen = {{SlotIndex, Components.MaterialOptionIds[Index]}};
						}
					}
				}

				ImGui::EndCombo();
			}

			ImGui::SetItemTooltip("%.*s", static_cast<int>(Label.size()), Label.data());
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload(ContentAssetPayload))
				{
					FAssetId Material;
					if (Payload->DataSize == sizeof(Material))
					{
						std::memcpy(&Material, Payload->Data, sizeof(Material));
						if (std::ranges::any_of(Components.MaterialOptionIds, [Material](const FAssetId Id)
						{
							return Id == Material;
						}))
						{
							Result.MaterialChosen = {{SlotIndex, Material}};
						}
					}
				}

				ImGui::EndDragDropTarget();
			}

			if (ImGui::BeginPopupContextItem("Material slot"))
			{
				if (ToolUIMenuItem("Reset to imported material", EToolUIMenuIcon::Undo))
				{
					Result.MaterialChosen = {{SlotIndex, FAssetId{}}};
				}

				ImGui::BeginDisabled(Slot.bMixed || !Slot.Selected.IsValid());
				if (ToolUIMenuItem("Edit material", EToolUIMenuIcon::Material))
				{
					Result.MaterialOpenRequested = Slot.Selected;
				}

				ImGui::EndDisabled();
				ImGui::EndPopup();
			}

			ImGui::TableSetColumnIndex(2);
			ImGui::BeginDisabled(Slot.bMixed || !Slot.Selected.IsValid());
			if (ImGui::Button("...##EditMaterial", {22.f * Scale, 0.f}))
			{
				Result.MaterialOpenRequested = Slot.Selected;
			}

			ImGui::SetItemTooltip("Edit material");
			ImGui::EndDisabled();
			ImGui::TableSetColumnIndex(3);
			if ((Slot.bMixed || Slot.Selected.IsValid()) && DrawResetButton(Name.c_str()))
			{
				Result.MaterialChosen = {{SlotIndex, FAssetId{}}};
			}

			ImGui::PopID();
		}

		ImGui::EndTable();
	}

	ImGui::EndDisabled();
	ImGui::PopStyleVar(2);
}

bool MatchesVisualSection(const ELevelComponentType Type, const std::string_view Query)
{
	const auto& Descriptor = GetLevelComponentDescriptor(Type);
	return Query.empty() || MatchesSearch(Descriptor.Label, Query) || (Type == ELevelComponentType::Light && MatchesSearch("Shadow distance", Query)) || std::ranges::any_of(Descriptor.Properties, [Query](const auto& Property)
	{
		return MatchesSearch(Property.Label, Query);
	});
}

bool IsVisualPropertyVisible(const FLevelEntity& Entity, const ELevelComponentType Type, const std::string_view Key)
{
	if (Type == ELevelComponentType::Light)
	{
		const auto& Light = *Entity.Light;
		if (Key == "range")
		{
			return Light.Type != ELightType::Sky && (Light.Type != ELightType::Directional || Light.bCastShadows);
		}

		if (Key == "innerConeAngle" || Key == "outerConeAngle")
		{
			return Light.Type == ELightType::Spot;
		}

		if (Key == "width" || Key == "height")
		{
			return Light.Type == ELightType::Rect;
		}

		if (Key == "environment" || Key == "ambientStrength" || Key == "environmentVisible")
		{
			return Light.Type == ELightType::Sky;
		}

		if (Key == "temperatureKelvin")
		{
			return Light.bUseTemperature;
		}

		if (Key == "castShadows")
		{
			return Light.Type != ELightType::Sky;
		}

		if (Key == "shadowBias" || Key == "shadowNormalBias")
		{
			return Light.Type != ELightType::Sky && Light.bCastShadows;
		}
	}

	if (Type == ELevelComponentType::HeightFog && (Key == "quality" || Key == "anisotropy"))
	{
		return Entity.HeightFog->bVolumetric;
	}

	if (Type == ELevelComponentType::SoftBody)
	{
		const ESoftBodyShape Shape = Entity.SoftBody->Shape;
		if (Key == "height")
		{
			return Shape == ESoftBodyShape::Cloth;
		}

		if (Key == "pressure")
		{
			return Shape == ESoftBodyShape::Ball;
		}

		if (Key == "pinned")
		{
			return Shape != ESoftBodyShape::Ball;
		}
	}

	return true;
}

void DrawVisualProperties(FToolUIContext& ToolUI, const ELevelComponentType Type, FLevelEntity& Entity, const std::span<bool> Mixed, FDetailsPanelState& State, const FDetailsComponentField& Components, const std::string_view Query, const bool bDragging, const FDetailsEditCallbacks* Edits, FDetailsMeshResult& Result)
{
	const auto& Descriptor = GetLevelComponentDescriptor(Type);
	const float Scale = ImGui::GetFontSize() / 15.f;
	ImGui::PushID(Descriptor.TypeId.data());
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.f * Scale, 4.f * Scale});
	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {0.f, 2.f * Scale});
	ImGui::BeginDisabled(bDragging);
	if (ImGui::BeginTable("##VisualProperties", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings))
	{
		const float LabelWidth = std::min(GetPropertyLabelWidth(), ImGui::GetContentRegionAvail().x * 0.44f);
		ImGui::TableSetupColumn("##Label", ImGuiTableColumnFlags_WidthFixed, LabelWidth);
		ImGui::TableSetupColumn("##Value", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("##Reset", ImGuiTableColumnFlags_WidthFixed, 22.f * Scale);

		for (std::size_t Index = 0; Index < Descriptor.Properties.size(); ++Index)
		{
			const auto& Property = Descriptor.Properties[Index];
			std::string_view DisplayLabel = Type == ELevelComponentType::Light && Property.Key == "range" && Entity.Light->Type == ELightType::Directional ? "Shadow distance" : Property.Label;
			if (Type == ELevelComponentType::SoftBody && Property.Key == "length")
			{
				constexpr std::array<std::string_view, 3> LengthLabels{"Length", "Width", "Diameter"};
				DisplayLabel = LengthLabels[static_cast<std::size_t>(Entity.SoftBody->Shape)];
			}

			if (Type == ELevelComponentType::SoftBody && Property.Key == "thickness" && Entity.SoftBody->Shape != ESoftBodyShape::Rope)
			{
				DisplayLabel = "Collision radius";
			}
			if ((!Query.empty() && !MatchesSearch(Descriptor.Label, Query) && !MatchesSearch(DisplayLabel, Query)) || !IsVisualPropertyVisible(Entity, Type, Property.Key))
			{
				continue;
			}

			const auto Previous = GetLevelVisualProperty(Entity, Type, Property.Key);
			if (!Previous)
			{
				continue;
			}

			FLevelPropertyValue Candidate = *Previous;
			FLevelPropertyValue Default = Property.Default;
			if (Type == ELevelComponentType::Light && Property.Key == "intensity")
			{
				Default = Entity.Light->Type == ELightType::Directional ? 50'000.f : (Entity.Light->Type == ELightType::Sky ? 1.f : 1000.f);
			}

			ImGui::PushID(static_cast<int>(Index));
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::PushStyleColor(ImGuiCol_Text, GetPropertyLabelColor());
			const float LabelLimit = ImGui::GetCursorScreenPos().x + LabelWidth - 4.f * Scale;
			const ImVec2 LabelPosition = ImGui::GetCursorScreenPos();
			ImGui::Dummy({std::max(1.f, LabelWidth - 4.f * Scale), ImGui::GetTextLineHeight()});
			ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), LabelPosition, {LabelLimit, LabelPosition.y + ImGui::GetFontSize()}, LabelLimit, DisplayLabel.data(), DisplayLabel.data() + DisplayLabel.size(), nullptr);
			ImGui::PopStyleColor();
			ImGui::SetItemTooltip("%.*s", static_cast<int>(DisplayLabel.size()), DisplayLabel.data());
			ImGui::TableSetColumnIndex(1);
			ImGui::SetNextItemWidth(-FLT_MIN);
			const std::size_t StateIndex = Index + (Type == ELevelComponentType::Light ? 0 : Type == ELevelComponentType::SkyAtmosphere ? 17 : Type == ELevelComponentType::HeightFog ? 24 : 32);
			const float Conversion = Property.Unit == ELevelPropertyUnit::Radians ? 180.f / std::numbers::pi_v<float> : 1.f;
			float NumericValue = std::get_if<float>(&Candidate) ? std::get<float>(Candidate) * Conversion : 0.f;
			FNumericEditLifecycle Edit{
			    .Begin = [&]
			{
				BeginDetailsEdit(Edits, Result);
				State.VisualPropertyWasMixed[StateIndex] = Mixed[Index];
				if (Edits && Edits->ReadVisualProperty)
				{
					Candidate = Edits->ReadVisualProperty(Type, Property.Key).value_or(Candidate);
				}

				if (const auto* Value = std::get_if<float>(&Candidate))
				{
					NumericValue = *Value * Conversion;
				}
			},
			    .Flush = [&](const bool bCanceled)
			{
				FlushDetailsEdit(Edits, Result, bCanceled);
				if (Edits && Edits->ReadVisualProperty)
				{
					Candidate = Edits->ReadVisualProperty(Type, Property.Key).value_or(Candidate);
				}

				if (const auto* Value = std::get_if<float>(&Candidate))
				{
					NumericValue = *Value * Conversion;
				}

				if (bCanceled)
				{
					Mixed[Index] = State.VisualPropertyWasMixed[StateIndex];
				}
			},
			};
			bool bChanged = false;
			bool bImmediate = false;
			if (Property.Type == ELevelPropertyType::Float)
			{
				float Minimum = Property.Range ? static_cast<float>(Property.Range->Minimum) * Conversion : 0.f;
				float Maximum = Property.Range ? static_cast<float>(Property.Range->Maximum) * Conversion : 1.f;
				if (Type == ELevelComponentType::Light && Property.Key == "innerConeAngle")
				{
					Maximum = std::min(Maximum, Entity.Light->OuterConeAngle * Conversion);
				}

				if (Type == ELevelComponentType::Light && Property.Key == "outerConeAngle")
				{
					Minimum = std::max(Minimum, Entity.Light->InnerConeAngle * Conversion);
				}

				const char* Format = Property.Unit == ELevelPropertyUnit::Meters ? "%.2f m" : Property.Unit == ELevelPropertyUnit::InverseMeters ? "%.3f /m"
				                                                                          : Property.Unit == ELevelPropertyUnit::Kilograms       ? "%.2f kg"
				                                                                          : Property.Key == "pressure"                           ? "%.0f Pa m3"
				                                                                          : Property.Unit == ELevelPropertyUnit::Kelvin          ? "%.0f K"
				                                                                          : Property.Unit == ELevelPropertyUnit::Radians         ? "%.1f°"
				                                                                                                                                 : "%.3f";
				if (Type == ELevelComponentType::Light && Property.Key == "intensity")
				{
					Format = Entity.Light->Type == ELightType::Directional ? "%.0f lx" : Entity.Light->Type == ELightType::Sky ? "%.2fx"
					                                                                                                           : "%.0f lm";
				}

				const float Speed = Property.Unit == ELevelPropertyUnit::Kelvin || Property.Key == "intensity" || Property.Key == "planetRadius" || Property.Key == "atmosphereHeight" || Property.Key == "pressure" ? 10.f
				                    : Property.Unit == ELevelPropertyUnit::Radians                                                                                                     ? 0.1f
				                    : Property.Key == "shadowBias"                                                                                                                     ? 0.0001f
				                    : Property.Key == "density"                                                                                                                        ? 0.0005f
				                                                                                                                                                                       : 0.01f;
				bChanged = DrawNumericDragFloat("##Value", &NumericValue, Speed, Minimum, Maximum, Mixed[Index] ? "Multiple values" : Format, ImGuiSliderFlags_AlwaysClamp, &Edit, true);
				Candidate = NumericValue / Conversion;
			}
			else if (Property.Type == ELevelPropertyType::Boolean)
			{
				bool Value = std::get<bool>(Candidate);
				bChanged = ToolUIToggle("##Value", &Value);
				Candidate = Value;
				bImmediate = true;
			}
			else if (Property.Type == ELevelPropertyType::Vector3)
			{
				const auto Color = std::get<FVector3>(Candidate);
				std::array Values{Color.X, Color.Y, Color.Z};
				bChanged = DrawLinearColorField("##Value", Values);
				Candidate = FVector3{Values[0], Values[1], Values[2]};
				if (ImGui::IsItemActivated())
				{
					BeginDetailsEdit(Edits, Result);
				}

				Result.bEditFinished |= ImGui::IsItemDeactivatedAfterEdit();
			}
			else if (Property.Type == ELevelPropertyType::LightType || Property.Type == ELevelPropertyType::FogQuality || Property.Type == ELevelPropertyType::SoftBodyShape)
			{
				constexpr std::array<std::string_view, 5> Lights{"Directional", "Sky", "Point", "Spot", "Rect"};
				constexpr std::array<std::string_view, 3> Qualities{"Low", "Medium", "High"};
				constexpr std::array<std::string_view, 3> Shapes{"Rope", "Cloth", "Ball"};
				const std::span<const std::string_view> Labels = Property.Type == ELevelPropertyType::LightType ? std::span<const std::string_view>{Lights} : Property.Type == ELevelPropertyType::FogQuality ? std::span<const std::string_view>{Qualities} : std::span<const std::string_view>{Shapes};
				const auto Current = Property.Type == ELevelPropertyType::LightType    ? static_cast<std::size_t>(std::get<ELightType>(Candidate))
				                     : Property.Type == ELevelPropertyType::FogQuality ? static_cast<std::size_t>(std::get<EFogQuality>(Candidate))
				                                                                       : static_cast<std::size_t>(std::get<ESoftBodyShape>(Candidate));
				if (ImGui::BeginCombo("##Value", Mixed[Index] ? "Multiple values" : Labels[Current].data()))
				{
					for (std::size_t Option = 0; Option < Labels.size(); ++Option)
					{
						if (ImGui::Selectable(Labels[Option].data(), !Mixed[Index] && Option == Current))
						{
							Candidate = Property.Type == ELevelPropertyType::LightType    ? FLevelPropertyValue{static_cast<ELightType>(Option)}
							            : Property.Type == ELevelPropertyType::FogQuality ? FLevelPropertyValue{static_cast<EFogQuality>(Option)}
							                                                              : FLevelPropertyValue{static_cast<ESoftBodyShape>(Option)};
							bChanged = true;
						}
					}

					ImGui::EndCombo();
				}

				bImmediate = true;
			}
			else if (Property.Type == ELevelPropertyType::AssetReference || Property.Type == ELevelPropertyType::ObjectReference)
			{
				const bool bSun = Property.Type == ELevelPropertyType::ObjectReference;
				// Soft bodies reference a material; Sky Lights reference an environment texture.
				const bool bMaterial = Type == ELevelComponentType::SoftBody;
				const auto Labels = bSun ? Components.SunLabels : bMaterial ? Components.MaterialOptionLabels : Components.EnvironmentLabels;
				const std::span<const FAssetId> AssetIds = bMaterial ? Components.MaterialOptionIds : Components.EnvironmentIds;
				const std::size_t Count = std::min(Labels.size(), bSun ? Components.SunIds.size() : AssetIds.size());
				const char* const NoneLabel = bSun ? "Automatic sun" : bMaterial ? "Default material" : "Procedural sky";
				const char* CurrentLabel = NoneLabel;
				if (bSun ? std::get<FObjectId>(Candidate).IsValid() : std::get<FAssetId>(Candidate).IsValid())
				{
					CurrentLabel = bSun ? "Missing sun (automatic)" : bMaterial ? "Missing material" : "Missing environment";
				}

				for (std::size_t Option = 0; Option < Count; ++Option)
				{
					if (bSun ? Components.SunIds[Option] == std::get<FObjectId>(Candidate) : AssetIds[Option] == std::get<FAssetId>(Candidate))
					{
						CurrentLabel = Labels[Option].c_str();
					}
				}

				if (ImGui::BeginCombo("##Value", Mixed[Index] ? "Multiple values" : CurrentLabel))
				{
					if (ImGui::IsWindowAppearing())
					{
						State.VisualReferenceSearch.fill('\0');
						ImGui::SetKeyboardFocusHere();
					}

					ImGui::SetNextItemWidth(-FLT_MIN);
					ImGui::PushStyleColor(ImGuiCol_NavCursor, {0, 0, 0, 0});
					ToolUI.DrawSearchField("##ReferenceSearch", bSun ? "Search lights" : bMaterial ? "Search materials" : "Search environments", State.VisualReferenceSearch.data(), State.VisualReferenceSearch.size());
					ImGui::PopStyleColor();
					std::vector<std::string_view> Candidates(Labels.begin(), Labels.begin() + static_cast<std::ptrdiff_t>(Count));
					const auto Matches = SearchAssets(Candidates, State.VisualReferenceSearch.data());
					if (ImGui::Selectable(NoneLabel))
					{
						Candidate = bSun ? FLevelPropertyValue{FObjectId{}} : FLevelPropertyValue{FAssetId{}};
						bChanged = true;
					}

					ImGuiListClipper Clipper;
					Clipper.Begin(Matches ? static_cast<int>(Matches->size()) : 0);
					while (Clipper.Step())
					{
						for (int Option = Clipper.DisplayStart; Option < Clipper.DisplayEnd; ++Option)
						{
							const std::size_t AssetIndex = (*Matches)[static_cast<std::size_t>(Option)].Index;
							if (ImGui::Selectable(Labels[AssetIndex].c_str()))
							{
								Candidate = bSun ? FLevelPropertyValue{Components.SunIds[AssetIndex]} : FLevelPropertyValue{AssetIds[AssetIndex]};
								bChanged = true;
							}
						}
					}

					ImGui::EndCombo();
				}

				bImmediate = true;
			}

			if (bChanged && !Edit.bCanceled)
			{
				if (bImmediate || Property.Type == ELevelPropertyType::Vector3)
				{
					BeginDetailsEdit(Edits, Result);
				}

				if (Edits && Edits->ApplyVisualProperty)
				{
					Edits->ApplyVisualProperty(Type, Property.Key, Candidate);
				}

				if (SetLevelVisualProperty(Entity, Type, Property.Key, Candidate))
				{
					Mixed[Index] = false;
				}

				Result.bEditFinished |= bImmediate;
			}

			Result.bEditFinished |= Edit.bFinished;
			Result.bEditCanceled |= Edit.bCanceled;
			ImGui::TableSetColumnIndex(2);
			if (Mixed[Index] || *GetLevelVisualProperty(Entity, Type, Property.Key) != Default)
			{
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.f * Scale);
				if (DrawResetButton(Property.Label.data()))
				{
					BeginDetailsEdit(Edits, Result);
					if (Edits && Edits->ApplyVisualProperty)
					{
						Edits->ApplyVisualProperty(Type, Property.Key, Default);
					}

					if (SetLevelVisualProperty(Entity, Type, Property.Key, Default))
					{
						Mixed[Index] = false;
					}

					Result.bEditFinished = true;
				}
			}

			ImGui::PopID();
		}

		ImGui::EndTable();
	}

	ImGui::EndDisabled();
	ImGui::PopStyleVar(2);
	ImGui::PopID();
}

template <typename Change, typename ChangeRow>
bool DrawTransformRow(const char* const Label, Im3d::Vec3& Value, const float Speed, const float Minimum, const float Maximum, const float Reset, EDetailsTransformSpace& Space, const ETransformClipboardFormat ClipboardFormat, const char* const Format, Change&& OnChange, ChangeRow&& OnRowChange, const FDetailsEditCallbacks* const Edits, FDetailsMeshResult& Result, bool* const bLocked = nullptr, const std::function<void()>& RefreshValue = {})
{
	const auto BeginEdit = [&]
	{
		BeginDetailsEdit(Edits, Result);
		if (RefreshValue)
		{
			RefreshValue();
		}
	};

	ImGui::PushID(Label);
	const float Scale = ImGui::GetFontSize() / 15.f;
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4.f * Scale, ImGui::GetStyle().ItemSpacing.y});
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.f * Scale, 4.f * Scale});
	const float Spacing = ImGui::GetStyle().ItemSpacing.x;
	const float LockWidth = 17.f * Scale + Spacing;
	const float LabelWidth = GetPropertyLabelWidth() - LockWidth - Spacing;
	const float ResetWidth = 18.f * Scale;
	const float Width = std::max(1.f, (ImGui::GetContentRegionAvail().x - LabelWidth - LockWidth - ResetWidth - Spacing * 4.f) / 3.f);

	if (const auto PastedValue = DrawSpaceSelector(Label, LabelWidth, Space, Value, ClipboardFormat))
	{
		BeginEdit();
		OnRowChange(*PastedValue);
		Result.bEditFinished = true;
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
		const ImGuiSliderFlags Flags = ImGuiSliderFlags_NoRoundToFormat | (Minimum < Maximum ? ImGuiSliderFlags_AlwaysClamp : 0);
		FNumericEditLifecycle Edit{
		    .Begin = [&]
		{
			BeginEdit();
			Candidate = Value[Axis];
		},
		    .Flush = [&](const bool bCanceled)
		{
			FlushDetailsEdit(Edits, Result, bCanceled);
			if (RefreshValue)
			{
				RefreshValue();
			}

			Candidate = Value[Axis];
		},
		};

		if (DrawNumericDragFloat("##Value", &Candidate, Speed, Minimum, Maximum, Format, Flags, &Edit, true) && !Edit.bCanceled)
		{
			OnChange(Axis, Candidate);
		}

		Result.bEditFinished |= Edit.bFinished;
		Result.bEditCanceled |= Edit.bCanceled;

		const ImVec2 Min = ImGui::GetItemRectMin();
		const ImVec2 Max = ImGui::GetItemRectMax();
		ImGui::GetWindowDrawList()->AddRectFilled({Min.x, Min.y + 3.f}, {Min.x + 2.f, Max.y - 3.f}, AxisColors[static_cast<std::size_t>(Axis)]);
		ImGui::PopID();
	}

	ImGui::SameLine();
	const bool bChanged = Value.x != Reset || Value.y != Reset || Value.z != Reset;
	const bool bReset = bChanged && DrawResetButton(Label);

	if (!bChanged)
	{
		ImGui::Dummy({ResetWidth, ImGui::GetFrameHeight()});
	}

	if (bReset)
	{
		BeginEdit();
		Value = Im3d::Vec3(Reset);
		Result.bEditFinished = true;
	}

	ImGui::PopStyleVar(2);
	ImGui::PopID();
	return bReset;
}
}

bool DrawDetailsResetButton(const char* const Label)
{
	return DrawResetButton(Label);
}

bool DrawLinearColorField(const char* const Label, const std::span<float> Channels)
{
	std::array<float, 4> Display{0.f, 0.f, 0.f, 1.f};
	for (std::size_t Index = 0; Index < 3; ++Index)
	{
		const float Value = Channels[Index];
		Display[Index] = Value <= 0.0031308f ? 12.92f * Value : 1.055f * std::pow(Value, 1.f / 2.4f) - 0.055f;
	}

	const bool bAlpha = Channels.size() == 4;
	if (bAlpha)
	{
		Display[3] = Channels[3];
	}

	const ImGuiColorEditFlags Flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs | (bAlpha ? ImGuiColorEditFlags_AlphaBar : 0);
	const bool bChanged = bAlpha ? ImGui::ColorEdit4(Label, Display.data(), Flags) : ImGui::ColorEdit3(Label, Display.data(), Flags);
	if (bChanged)
	{
		for (std::size_t Index = 0; Index < 3; ++Index)
		{
			const float Value = Display[Index];
			Channels[Index] = Value <= 0.04045f ? Value / 12.92f : std::pow((Value + 0.055f) / 1.055f, 2.4f);
		}

		if (bAlpha)
		{
			Channels[3] = Display[3];
		}
	}

	return bChanged;
}

FDetailsMeshResult DrawPreviewDetailsPanel(FToolUIContext& ToolUI, bool& bOpen, const bool bSelected, const bool bDragging, Im3d::Vec3& Translation, Im3d::Mat3& Rotation, Im3d::Vec3& Scale, FDetailsPanelState& State, const std::string& ObjectLabel, const std::size_t SelectedCount, const FDetailsMeshField* const Mesh, const FDetailsEditCallbacks* const Edits, FDetailsComponentField* const Components)
{
	FDetailsMeshResult MeshResult;
	const auto& TransformDescriptor = GetLevelComponentDescriptor(ELevelComponentType::Transform);
	const auto& MeshDescriptor = GetLevelComponentDescriptor(ELevelComponentType::StaticMesh);
	const auto& BodyDescriptor = GetLevelComponentDescriptor(ELevelComponentType::RigidBody);
	const auto& LocationProperty = TransformDescriptor.Properties[0];
	const auto& RotationProperty = TransformDescriptor.Properties[1];
	const auto& ScaleProperty = TransformDescriptor.Properties[2];
	const auto& BodyTypeProperty = BodyDescriptor.Properties[0];
	const auto DefaultBodyType = std::get<ELevelBodyType>(BodyTypeProperty.Default);

	if (!ToolUI.BeginPanel("Details", &bOpen))
	{
		ToolUI.EndPanel();
		return MeshResult;
	}

	if (!bSelected)
	{
		State.bAddComponentRequested = false;
		ImGui::TextDisabled("Select an object in the viewport.");
		ToolUI.EndPanel();
		return MeshResult;
	}

	const float UiScale = ImGui::GetFontSize() / 15.f;
	const ImVec2 HeadingPosition = ImGui::GetCursorScreenPos();
	ImGui::Dummy({30.f * UiScale, 30.f * UiScale});
	const bool bEntity = Components != nullptr && Mesh == nullptr;
	DrawObjectIcon(HeadingPosition, 30.f * UiScale, bEntity);
	ImGui::SameLine();
	const float HeadingTextX = ImGui::GetCursorPosX();
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {ImGui::GetStyle().ItemSpacing.x, 2.f * UiScale});
	const std::string SelectionText = std::to_string(SelectedCount) + " selected";
	const float SelectionWidth = ImGui::CalcTextSize(SelectionText.c_str()).x;
	const float ActionWidth = Components != nullptr ? ImGui::CalcTextSize("+ Add Component").x + 12.f * UiScale : 0.f;
	const float TrailingWidth = std::max(ActionWidth, SelectionWidth);
	const float NameWidth = std::max(1.f, ImGui::GetContentRegionAvail().x - TrailingWidth - ImGui::GetStyle().ItemSpacing.x);
	const float HeadingActionX = HeadingTextX + NameWidth + ImGui::GetStyle().ItemSpacing.x;
	ImGui::BeginGroup();
	ImGui::Dummy({NameWidth, ImGui::GetTextLineHeight() + 8.f * UiScale});
	const ImVec2 NamePosition{ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y + 4.f * UiScale};
	ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), NamePosition, ImGui::GetItemRectMax(), ImGui::GetItemRectMax().x, ObjectLabel.data(), ObjectLabel.data() + ObjectLabel.size(), nullptr);
	ImGui::SetItemTooltip("%.*s", static_cast<int>(ObjectLabel.size()), ObjectLabel.data());
	ImGui::TextDisabled("%s", bEntity ? "Entity" : MeshDescriptor.Label.data());
	ImGui::EndGroup();
	ImGui::SameLine(HeadingActionX);
	ImGui::BeginGroup();
	if (Components != nullptr)
	{
		MeshResult.ComponentAction = DrawAddComponent(ToolUI, State, *Components, bDragging);
	}

	ImGui::SetCursorPosX(HeadingActionX + TrailingWidth - SelectionWidth);
	ImGui::TextDisabled("%s", SelectionText.c_str());
	ImGui::EndGroup();
	ImGui::PopStyleVar();
	ImGui::Spacing();
	ImGui::SetNextItemWidth(-1.f);
	ToolUI.DrawSearchField("##PropertySearch", "Search", State.Search.data(), State.Search.size());
	const std::string_view Query(State.Search.data());
	const bool bShowAll = Query.empty() || MatchesSearch(TransformDescriptor.Label, Query);
	const bool bLocation = bShowAll || MatchesSearch(LocationProperty.Label, Query);
	const bool bRotation = bShowAll || MatchesSearch(RotationProperty.Label, Query);
	const bool bScale = bShowAll || MatchesSearch(ScaleProperty.Label, Query);
	const bool bTransform = bLocation || bRotation || bScale;
	const bool bHasMesh = Mesh != nullptr && Mesh->Selected >= 0 && static_cast<std::size_t>(Mesh->Selected) < Mesh->Options.size();
	// cppcheck-suppress containerOutOfBounds
	// bHasMesh checks the selected index against the options size.
	const std::string_view MeshLabel = Components != nullptr && (!Components->bAllMesh || Components->bMixedMeshAsset) ? std::string_view("Multiple values") : bHasMesh ? std::string_view(Mesh->Options[static_cast<std::size_t>(Mesh->Selected)])
	                                                                                                                                                                    : std::string_view(Components != nullptr ? "No asset selected" : "No mesh");
	const bool bMesh = (Components == nullptr || Components->bAnyMesh) && (Query.empty() || MatchesSearch(MeshDescriptor.Label, Query) || MatchesSearch(ObjectLabel, Query) || MatchesSearch(MeshLabel, Query) || MatchesSearch("Materials", Query) || (Components && std::ranges::any_of(Components->MaterialSlots, [Query](const auto& Slot)
	{
		return MatchesSearch(Slot.Name, Query);
	})));
	const bool bBodyProperty = std::ranges::any_of(BodyProperties, [&](const FBodyPropertyDisplay& Property)
	{
		return MatchesBodyProperty(Property, Query);
	});
	const bool bBody = Components != nullptr && Components->bAnyBody && (Query.empty() || bBodyProperty || MatchesSearch(BodyDescriptor.Label, Query) || MatchesSearch(BodyTypeProperty.Label, Query) || MatchesSearch("Shape", Query) || MatchesSearch("Static", Query) || MatchesSearch("Dynamic", Query));
	const bool bLight = Components && Components->bAnyLight && MatchesVisualSection(ELevelComponentType::Light, Query);
	const bool bAtmosphere = Components && Components->bAnySkyAtmosphere && MatchesVisualSection(ELevelComponentType::SkyAtmosphere, Query);
	const bool bFog = Components && Components->bAnyHeightFog && MatchesVisualSection(ELevelComponentType::HeightFog, Query);
	const bool bSoftBody = Components && Components->bAnySoftBody && MatchesVisualSection(ELevelComponentType::SoftBody, Query);
	if (!bTransform && !bMesh && !bBody && !bLight && !bAtmosphere && !bFog && !bSoftBody)
	{
		ImGui::TextDisabled("No matching properties.");
		ToolUI.EndPanel();
		return MeshResult;
	}

	const auto DrawSectionHeader = [&](const char* const Label, const bool bMixed = false, const EDetailsComponentAction RemoveAction = EDetailsComponentAction::None)
	{
		ImGui::Spacing();
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {ImGui::GetStyle().FramePadding.x, 3.f * UiScale});
		const ImVec2 Position = ImGui::GetCursorScreenPos();
		const ImVec2 Padding = ImGui::GetStyle().FramePadding;
		const float FontSize = ImGui::GetFontSize();
		const bool bRemovable = Components != nullptr && RemoveAction != EDetailsComponentAction::None;
		const float ActionX = ImGui::GetWindowContentRegionMax().x + ImGui::GetWindowPos().x - ImGui::GetFrameHeight();
		ImDrawList* const DrawList = ImGui::GetWindowDrawList();
		const ImVec2 HeaderBackgroundMaximum{Position.x + ImGui::GetContentRegionAvail().x, Position.y + ImGui::GetFrameHeight()};
		DrawList->AddRectFilled(Position, HeaderBackgroundMaximum, ImGui::GetColorU32(ImGuiCol_TableHeaderBg), ImGui::GetStyle().FrameRounding);
		DrawList->AddRect(Position, HeaderBackgroundMaximum, ImGui::GetColorU32(ImGuiCol_Separator), ImGui::GetStyle().FrameRounding);
		if (bRemovable)
		{
			ImGui::PushClipRect(Position, {ActionX, ImGui::GetCurrentWindow()->ClipRect.Max.y}, true);
		}

		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
		ImGui::PushStyleColor(ImGuiCol_Header, {0, 0, 0, 0});
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0, 0, 0, 0});
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, {0, 0, 0, 0});
		ImGui::PushStyleColor(ImGuiCol_Text, {0, 0, 0, 0});
		const bool bOpen = ImGui::TreeNodeEx(Label, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding);
		ImGui::PopStyleColor(4);
		ImGui::PopStyleVar();
		if (ImGui::IsItemHovered())
		{
			DrawList->AddRectFilled(Position, HeaderBackgroundMaximum, ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered), ImGui::GetStyle().FrameRounding);
		}

		const float ArrowScale = 0.7f;
		const float ArrowInsetY = FontSize * (1.f - ArrowScale) * 0.5f;
		ImGui::RenderArrow(DrawList, {Position.x + Padding.x, Position.y + Padding.y + ArrowInsetY}, ImGui::GetColorU32(GetPropertyLabelColor()), bOpen ? ImGuiDir_Down : ImGuiDir_Right, ArrowScale);
		const std::string DisplayLabel = std::string(Label) + (bMixed ? " (mixed)" : "");
		const ImVec2 TextPosition{Position.x + FontSize + Padding.x * 2.f, Position.y + Padding.y};
		ImGui::RenderTextEllipsis(DrawList, TextPosition, {bRemovable ? ActionX : ImGui::GetItemRectMax().x, Position.y + FontSize + Padding.y}, bRemovable ? ActionX : ImGui::GetItemRectMax().x, DisplayLabel.c_str(), DisplayLabel.c_str() + DisplayLabel.size(), nullptr);
		if (bRemovable)
		{
			const ImVec2 HeaderMaximum = ImGui::GetItemRectMax();
			ImGui::PopClipRect();
			ImGui::PushID(Label);
			ImGui::SetCursorScreenPos({ActionX, Position.y});
			ImGui::BeginDisabled(bDragging);
			if (ImGui::InvisibleButton("##RemoveComponent", {ImGui::GetFrameHeight(), HeaderMaximum.y - Position.y}, ImGuiButtonFlags_EnableNav))
			{
				MeshResult.ComponentAction = RemoveAction;
			}

			const ImVec2 Center{ActionX + ImGui::GetFrameHeight() * 0.5f, (Position.y + HeaderMaximum.y) * 0.5f};
			if (ImGui::IsItemHovered() || (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible))
			{
				DrawList->AddRectFilled({ActionX, Position.y}, {ActionX + ImGui::GetFrameHeight(), HeaderMaximum.y}, ImGui::GetColorU32(ImGuiCol_HeaderHovered), ImGui::GetStyle().FrameRounding);
			}

			const float Radius = 4.f * UiScale;
			const ImU32 Color = ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled);
			DrawList->AddLine({Center.x - Radius, Center.y - Radius}, {Center.x + Radius, Center.y + Radius}, Color, UiScale);
			DrawList->AddLine({Center.x + Radius, Center.y - Radius}, {Center.x - Radius, Center.y + Radius}, Color, UiScale);
			ImGui::SetItemTooltip("Remove %s component", Label);

			ImGui::EndDisabled();
			ImGui::PopID();
		}

		ImGui::PopStyleVar();
		return bOpen;
	};

	if (bTransform && !Query.empty())
	{
		ImGui::SetNextItemOpen(true, ImGuiCond_Always);
	}

	if (bTransform && DrawSectionHeader(TransformDescriptor.Label.data()))
	{
		ImGui::BeginDisabled(bDragging);

		if (bLocation)
		{
			DrawTransformRow(LocationProperty.Label.data(), Translation, 0.01f, -1.e7f, 1.e7f, static_cast<float>(std::get<FWorldPosition>(LocationProperty.Default).Meters.X), State.Spaces[0], ETransformClipboardFormat::XYZ, "%.2f m", [&](const int Axis, const float Candidate)
			{
				if (std::isfinite(Candidate) && std::abs(Candidate) <= 1.e7f)
				{
					Translation[Axis] = Candidate;
				}
			}, [&](const Im3d::Vec3& Candidate)
			{
				if (!std::isfinite(Candidate.x) || !std::isfinite(Candidate.y) || !std::isfinite(Candidate.z) || std::abs(Candidate.x) > 1.e7f || std::abs(Candidate.y) > 1.e7f || std::abs(Candidate.z) > 1.e7f)
				{
					return false;
				}

				Translation = Candidate;
				return true;
			}, Edits, MeshResult);
		}

		Im3d::Vec3 RotationDegrees = Im3d::ToEulerXYZ(Rotation) * (180.f / std::numbers::pi_v<float>);
		const bool bRotationReset = bRotation && DrawTransformRow(RotationProperty.Label.data(), RotationDegrees, 0.1f, -360.f, 360.f, 0.f, State.Spaces[1], ETransformClipboardFormat::Rotation, "%.1f°", [&](const int Axis, const float Candidate)
		{
			if (std::isfinite(Candidate))
			{
				RotationDegrees[Axis] = Candidate;
				const Im3d::Vec3 Radians = RotationDegrees * (std::numbers::pi_v<float> / 180.f);
				Rotation = FromPreviewEulerXYZ(Radians);
			}
		}, [&](const Im3d::Vec3& Candidate)
		{
			if (!std::isfinite(Candidate.x) || !std::isfinite(Candidate.y) || !std::isfinite(Candidate.z))
			{
				return false;
			}

			RotationDegrees = {std::clamp(Candidate.x, -360.f, 360.f), std::clamp(Candidate.y, -360.f, 360.f), std::clamp(Candidate.z, -360.f, 360.f)};
			const Im3d::Vec3 Radians = RotationDegrees * (std::numbers::pi_v<float> / 180.f);
			Rotation = FromPreviewEulerXYZ(Radians);
			return true;
		}, Edits, MeshResult, nullptr, [&]
		{
			// A canceled preceding gesture can restore the authored matrix during this row.
			RotationDegrees = Im3d::ToEulerXYZ(Rotation) * (180.f / std::numbers::pi_v<float>);
		});

		if (bRotationReset)
		{
			Rotation = FromPreviewEulerXYZ(Im3d::Vec3{0.f});
		}

		if (bScale)
		{
			DrawTransformRow(ScaleProperty.Label.data(), Scale, 0.01f, MinimumPreviewScale, MaximumPreviewScale, std::get<FVector3>(ScaleProperty.Default).X, State.Spaces[2], ETransformClipboardFormat::XYZ, "%.3f", [&](const int Axis, const float Candidate)
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
			}, Edits, MeshResult, &State.bScaleLocked);
		}

		ImGui::EndDisabled();
	}

	if (bMesh && !Query.empty())
	{
		ImGui::SetNextItemOpen(true, ImGuiCond_Always);
	}

	if (bMesh && DrawSectionHeader(MeshDescriptor.Label.data(), Components != nullptr && !Components->bAllMesh, EDetailsComponentAction::RemoveStaticMesh))
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
				ImGui::PushStyleColor(ImGuiCol_NavCursor, {0, 0, 0, 0});
				ToolUI.DrawSearchField("##MeshSearch", "Search assets", State.MeshSearch.data(), State.MeshSearch.size());
				ImGui::PopStyleColor();
				std::vector<std::string_view> Candidates(Mesh->Options.begin(), Mesh->Options.end());
				const std::optional<std::vector<FAssetSearchMatch>> Matches = SearchAssets(Candidates, State.MeshSearch.data());
				if (Matches && Matches->empty())
				{
					ImGui::TextDisabled("No matching assets");
				}

				if (Matches)
				{
					ImGuiListClipper Clipper;
					Clipper.Begin(static_cast<int>(Matches->size()));
					while (Clipper.Step())
					{
						for (int MatchIndex = Clipper.DisplayStart; MatchIndex < Clipper.DisplayEnd; ++MatchIndex)
						{
							const std::size_t Index = (*Matches)[static_cast<std::size_t>(MatchIndex)].Index;
							const bool bCurrent = static_cast<int>(Index) == Mesh->Selected && (Components == nullptr || !Components->bMixedMeshAsset);
							if (ImGui::Selectable(Mesh->Options[Index].c_str(), bCurrent) && !bCurrent)
							{
								MeshResult.Chosen = static_cast<int>(Index);
							}
						}
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

		if (Components)
		{
			DrawMaterialSlots(ToolUI, State, *Components, bDragging, MeshResult);
		}
	}

	if (bBody && !Query.empty())
	{
		ImGui::SetNextItemOpen(true, ImGuiCond_Always);
	}

	if (bBody && DrawSectionHeader(BodyDescriptor.Label.data(), !Components->bAllBody, EDetailsComponentAction::RemoveRigidBody))
	{
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.f * UiScale, 4.f * UiScale});
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {0.f, 2.f * UiScale});
		if (ImGui::BeginTable("##RigidBodySettings", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings))
		{
			ImGui::TableSetupColumn("##BodyLabel", ImGuiTableColumnFlags_WidthFixed, GetPropertyLabelWidth());
			ImGui::TableSetupColumn("##BodyValue", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("##BodyReset", ImGuiTableColumnFlags_WidthFixed, 22.f * UiScale);
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::PushStyleColor(ImGuiCol_Text, GetPropertyLabelColor());
			ImGui::TextUnformatted(BodyTypeProperty.Label.data());
			ImGui::PopStyleColor();
			ImGui::TableSetColumnIndex(1);
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::BeginDisabled(bDragging);
			const char* const BodyLabel = !Components->bAllBody || !Components->BodyType ? "Multiple values" : *Components->BodyType == ELevelBodyType::Static ? "Static"
			                                                                                                                                                   : "Dynamic";
			if (ImGui::BeginCombo("##BodyType", BodyLabel))
			{
				for (const auto& [Label, Type] : {std::pair{"Static", ELevelBodyType::Static}, std::pair{"Dynamic", ELevelBodyType::Dynamic}})
				{
					const bool bCurrent = Components->BodyType == Type;
					if (ImGui::Selectable(Label, bCurrent) && !bCurrent)
					{
						MeshResult.BodyTypeChosen = Type;
					}
				}

				ImGui::EndCombo();
			}

			ImGui::TableSetColumnIndex(2);
			if (Components->BodyType != DefaultBodyType)
			{
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.f * UiScale);
				if (DrawResetButton(BodyTypeProperty.Label.data()))
				{
					MeshResult.BodyTypeChosen = DefaultBodyType;
				}
			}

			ImGui::EndDisabled();

			for (std::size_t Index = 0; Index < BodyProperties.size(); ++Index)
			{
				const FBodyPropertyDisplay& Property = BodyProperties[Index];
				const FLevelPropertyDescriptor& Descriptor = GetBodyPropertyDescriptor(Property);
				if (!Query.empty() && !MatchesBodyProperty(Property, Query))
				{
					continue;
				}

				ImGui::TableNextRow();
				ImGui::PushID(static_cast<int>(Index));
				ImGui::TableSetColumnIndex(0);
				ImGui::AlignTextToFramePadding();
				ImGui::PushStyleColor(ImGuiCol_Text, GetPropertyLabelColor());
				ImGui::TextUnformatted(Descriptor.Label.data(), Descriptor.Label.data() + Descriptor.Label.size());
				ImGui::PopStyleColor();
				ImGui::SetItemTooltip("%.*s", static_cast<int>(Property.Tooltip.size()), Property.Tooltip.data());
				ImGui::TableSetColumnIndex(1);
				ImGui::SetNextItemWidth(-FLT_MIN);
				const bool bMixed = Components->MixedBodySettings[Index];
				float Candidate = Components->BodySettings.*Property.Member;
				FNumericEditLifecycle Edit{
				    .Begin = [&]
				{
					BeginDetailsEdit(Edits, MeshResult);
					State.BodyPropertyWasMixed[Index] = Components->MixedBodySettings[Index];
					if (Edits != nullptr && Edits->ReadBodyProperty)
					{
						Components->BodySettings.*Property.Member = Edits->ReadBodyProperty(Property.Member);
					}

					Candidate = Components->BodySettings.*Property.Member;
				},
				    .Flush = [&](const bool bCanceled)
				{
					FlushDetailsEdit(Edits, MeshResult, bCanceled);
					if (Edits != nullptr && Edits->ReadBodyProperty)
					{
						Components->BodySettings.*Property.Member = Edits->ReadBodyProperty(Property.Member);
					}

					Candidate = Components->BodySettings.*Property.Member;
					if (bCanceled)
					{
						Components->MixedBodySettings[Index] = State.BodyPropertyWasMixed[Index];
					}
				},
				};
				const char* const Format = bMixed ? "Multiple values" : Property.Format;
				ImGui::BeginDisabled(bDragging || (Property.bDynamicOnly && !Components->bAnyDynamicBody));
				if (DrawNumericDragFloat("##BodyValue", &Candidate, Property.Speed, static_cast<float>(Descriptor.Range->Minimum), static_cast<float>(Descriptor.Range->Maximum), Format, ImGuiSliderFlags_AlwaysClamp, &Edit, true) && !Edit.bCanceled)
				{
					if (Edits != nullptr && Edits->ApplyBodyProperty)
					{
						Edits->ApplyBodyProperty(Property.Member, Candidate);
					}

					Components->BodySettings.*Property.Member = Candidate;
					Components->MixedBodySettings[Index] = false;
				}

				MeshResult.bEditFinished |= Edit.bFinished;
				MeshResult.bEditCanceled |= Edit.bCanceled;
				ImGui::TableSetColumnIndex(2);
				const float Default = std::get<float>(Descriptor.Default);
				if (Components->MixedBodySettings[Index] || Components->BodySettings.*Property.Member != Default)
				{
					ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.f * UiScale);
					if (DrawResetButton(Descriptor.Label.data()))
					{
						BeginDetailsEdit(Edits, MeshResult);

						if (Edits != nullptr && Edits->ApplyBodyProperty)
						{
							Edits->ApplyBodyProperty(Property.Member, Default);
						}

						Components->BodySettings.*Property.Member = Default;
						Components->MixedBodySettings[Index] = false;
						MeshResult.bEditFinished = true;
					}
				}

				ImGui::EndDisabled();
				ImGui::PopID();
			}

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextDisabled("Shape");
			ImGui::TableSetColumnIndex(1);
			ImGui::TextDisabled("Box (mesh bounds)");
			ImGui::EndTable();
		}

		ImGui::PopStyleVar(2);
	}

	if (Components)
	{
		FLevelEntity Visual{.Light = Components->Light, .SkyAtmosphere = Components->SkyAtmosphere, .HeightFog = Components->HeightFog, .SoftBody = Components->SoftBody};
		constexpr std::array<std::string_view, 5> LightLabels{"Directional Light", "Sky Light", "Point Light", "Spot Light", "Rect Light"};
		if (bLight && DrawSectionHeader(LightLabels[static_cast<std::size_t>(Visual.Light->Type)].data(), !Components->bAllLight || Components->MixedLight[0], EDetailsComponentAction::RemoveLight))
		{
			DrawVisualProperties(ToolUI, ELevelComponentType::Light, Visual, Components->MixedLight, State, *Components, Query, bDragging, Edits, MeshResult);
			Components->Light = *Visual.Light;
		}

		if (bAtmosphere && DrawSectionHeader("Sky Atmosphere", !Components->bAllSkyAtmosphere, EDetailsComponentAction::RemoveSkyAtmosphere))
		{
			DrawVisualProperties(ToolUI, ELevelComponentType::SkyAtmosphere, Visual, Components->MixedSkyAtmosphere, State, *Components, Query, bDragging, Edits, MeshResult);
			Components->SkyAtmosphere = *Visual.SkyAtmosphere;
		}

		if (bFog && DrawSectionHeader("Height Fog", !Components->bAllHeightFog, EDetailsComponentAction::RemoveHeightFog))
		{
			DrawVisualProperties(ToolUI, ELevelComponentType::HeightFog, Visual, Components->MixedHeightFog, State, *Components, Query, bDragging, Edits, MeshResult);
			Components->HeightFog = *Visual.HeightFog;
		}

		if (bSoftBody && DrawSectionHeader("Soft Body", !Components->bAllSoftBody, EDetailsComponentAction::RemoveSoftBody))
		{
			DrawVisualProperties(ToolUI, ELevelComponentType::SoftBody, Visual, Components->MixedSoftBody, State, *Components, Query, bDragging, Edits, MeshResult);
			Components->SoftBody = *Visual.SoftBody;
		}
	}

	ToolUI.EndPanel();
	return MeshResult;
}
}
