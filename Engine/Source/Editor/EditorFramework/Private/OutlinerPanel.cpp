#include "OutlinerPanel.h"

#include "Herta/ToolUI/ToolUI.h"
#include "PreviewScene.h"

#include <imgui_internal.h>

namespace Herta
{
bool DrawPreviewOutlinerPanel(FToolUIContext& ToolUI, bool& bOpen, FPreviewSelection& Selection, const std::span<const FPreviewObject> Objects, const bool bDragging, FOutlinerPanelState& State)
{
	State.bRenameRequested = false;
	if (!ToolUI.BeginPanel("Outliner", &bOpen))
	{
		ToolUI.EndPanel();
		return false;
	}

	bool bFocusRequested = false;
	const float Scale = ImGui::GetFontSize() / 15.f;
	ImGui::BeginDisabled(bDragging);
	ImGui::SetNextItemWidth(-1.f);
	if (ToolUI.DrawSearchField("##OutlinerSearch", "Search", State.Search.InputBuf, sizeof(State.Search.InputBuf)))
	{
		State.Search.Build();
	}

	std::vector<int> VisibleIndices;
	for (std::size_t Index = 0; Index < Objects.size(); ++Index)
	{
		if (State.IsObjectVisible(Objects[Index].Label))
		{
			VisibleIndices.push_back(static_cast<int>(Index));
		}
	}

	const ImGuiIO& IO = ImGui::GetIO();
	if (!bDragging && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !IO.WantTextInput)
	{
		if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false))
		{
			Selection.SelectAll(VisibleIndices);
		}

		State.bRenameRequested = !Selection.Indices.empty() && ImGui::IsKeyPressed(ImGuiKey_F2, false);
	}

	const float FooterHeight = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + Scale;
	if (ImGui::BeginChild("##OutlinerEntries", {0.f, -FooterHeight}))
	{
		bool bRowHovered = false;
		float RowsTop = ImGui::GetCursorScreenPos().y;
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.f * Scale, 2.f * Scale});
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {8.f * Scale, 4.f * Scale});
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {6.f * Scale, 8.f * Scale});
		ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, {0, 0, 0, 0});
		ImGui::PushStyleColor(ImGuiCol_TableBorderLight, {1, 1, 1, 0.08f});
		if (ImGui::BeginTable("##OutlinerObjects", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX))
		{
			ImGui::TableSetupColumn("Item Label", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 105.f * ImGui::GetFontSize() / 15.f);
			ImGui::PushStyleColor(ImGuiCol_Header, {0, 0, 0, 0});
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0, 0, 0, 0});
			ImGui::PushStyleColor(ImGuiCol_HeaderActive, {0, 0, 0, 0});
			ImGui::TableHeadersRow();
			ImGui::PopStyleColor(3);
			const ImGuiTable* const Table = ImGui::GetCurrentTable();
			ImGui::TablePushBackgroundChannel();
			ImGui::GetWindowDrawList()->AddRectFilled({Table->WorkRect.Min.x, Table->RowPosY1}, {Table->WorkRect.Max.x, Table->RowPosY2}, ImGui::GetColorU32(ImVec4{1, 1, 1, 0.05f}), 4.f * Scale);
			ImGui::TablePopBackgroundChannel();
			RowsTop = ImGui::GetCursorScreenPos().y;
			for (std::size_t Index = 0; Index < Objects.size(); ++Index)
			{
				const FPreviewObject& Object = Objects[Index];
				if (!State.IsObjectVisible(Object.Label))
				{
					continue;
				}

				ImGui::PushID(static_cast<int>(Index));
				ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {8.f * Scale, 6.f * Scale});
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {6.f * Scale, 12.f * Scale});
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				const float IconX = ImGui::GetCursorScreenPos().x + 2.f * Scale;
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 22.f * Scale);
				ImGui::PushStyleColor(ImGuiCol_Header, {0, 0, 0, 0});
				ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0, 0, 0, 0});
				ImGui::PushStyleColor(ImGuiCol_HeaderActive, {0, 0, 0, 0});
				const float LabelX = ImGui::GetCursorScreenPos().x;
				if (ImGui::Selectable("##Object", Selection.Contains(static_cast<int>(Index)), ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
				{
					if (IO.KeyShift)
					{
						Selection.SelectRange(static_cast<int>(Index), VisibleIndices, IO.KeyCtrl);
					}
					else if (!IO.KeyCtrl || !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					{
						Selection.Select(static_cast<int>(Index), IO.KeyCtrl);
					}

					bFocusRequested = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter);
				}

				ImGui::PopStyleColor(3);
				const bool bCurrentRowHovered = ImGui::IsItemHovered();
				bRowHovered |= bCurrentRowHovered;
				const ImVec2 Minimum = ImGui::GetItemRectMin();
				const ImVec2 Maximum = ImGui::GetItemRectMax();
				if (Selection.Contains(static_cast<int>(Index)) || bCurrentRowHovered)
				{
					const ImGuiCol Color = bCurrentRowHovered ? (ImGui::IsItemActive() ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered) : ImGuiCol_Header;
					ImGui::TablePushBackgroundChannel();
					ImGui::GetWindowDrawList()->AddRectFilled({Table->WorkRect.Min.x, Minimum.y}, {Table->WorkRect.Max.x, Maximum.y}, ImGui::GetColorU32(Color), 4.f * Scale);
					ImGui::TablePopBackgroundChannel();
				}

				const float CenterY = (Minimum.y + Maximum.y) * 0.5f;
				ImGui::GetWindowDrawList()->AddText({LabelX, CenterY - ImGui::GetFontSize() * 0.5f}, ImGui::GetColorU32(ImGuiCol_Text), Object.Label.data(), Object.Label.data() + Object.Label.size());
				const ImVec2 Top{IconX + 6.f * Scale, CenterY - 7.f * Scale};
				const ImVec2 Left{IconX, CenterY - 4.f * Scale};
				const ImVec2 Right{IconX + 12.f * Scale, Left.y};
				const ImVec2 Center{Top.x, CenterY - Scale};
				const ImVec2 Bottom{Top.x, CenterY + 7.f * Scale};
				const ImVec2 Outline[]{Top, Right, {Right.x, CenterY + 4.f * Scale}, Bottom, {Left.x, CenterY + 4.f * Scale}, Left};
				ImDrawList* const Draw = ImGui::GetWindowDrawList();
				const ImU32 IconColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
				Draw->AddPolyline(Outline, 6, IconColor, ImDrawFlags_Closed, Scale);
				Draw->AddLine(Left, Center, IconColor, Scale);
				Draw->AddLine(Right, Center, IconColor, Scale);
				Draw->AddLine(Center, Bottom, IconColor, Scale);
				ImGui::TableSetColumnIndex(1);
				ImGui::TextDisabled("Static Mesh");
				ImGui::PopStyleVar(2);
				ImGui::PopID();
			}

			ImGui::EndTable();
		}

		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar(3);
		if (!bDragging && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetIO().MousePos.y >= RowsTop && !bRowHovered)
		{
			Selection.Select(-1);
		}
	}

	ImGui::EndChild();
	ImGui::Separator();
	if (State.Search.IsActive())
	{
		ImGui::TextDisabled("%d of %d objects (%d selected)", static_cast<int>(VisibleIndices.size()), static_cast<int>(Objects.size()), static_cast<int>(Selection.Indices.size()));
	}
	else
	{
		ImGui::TextDisabled("%d objects (%d selected)", static_cast<int>(Objects.size()), static_cast<int>(Selection.Indices.size()));
	}

	ImGui::EndDisabled();
	ToolUI.EndPanel();
	return bFocusRequested;
}
}
