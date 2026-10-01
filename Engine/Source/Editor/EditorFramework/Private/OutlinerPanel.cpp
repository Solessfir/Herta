#include "OutlinerPanel.h"

#include "Herta/ToolUI/ToolUI.h"

#include <imgui_internal.h>

namespace Herta
{
bool DrawPreviewOutlinerPanel(FToolUIContext& ToolUI, bool& bOpen, bool& bSelected, const bool bDragging, FOutlinerPanelState& State)
{
	if (!ToolUI.BeginPanel("Outliner", &bOpen))
	{
		ToolUI.EndPanel();
		return false;
	}

	bool bFocusRequested = false;
	const float Scale = ImGui::GetFontSize() / 15.0f;
	ImGui::BeginDisabled(bDragging);
	ImGui::SetNextItemWidth(-1.0f);
	if (ToolUI.DrawSearchField("##OutlinerSearch", "Search", State.Search.InputBuf, sizeof(State.Search.InputBuf)))
	{
		State.Search.Build();
	}
	const bool bCubeVisible = State.IsPreviewCubeVisible();
	const float FooterHeight = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + Scale;
	if (ImGui::BeginChild("##OutlinerEntries", {0.0f, -FooterHeight}))
	{
		bool bRowHovered = false;
		float RowsTop = ImGui::GetCursorScreenPos().y;
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.0f * Scale, 2.0f * Scale});
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {8.0f * Scale, 4.0f * Scale});
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {6.0f * Scale, 8.0f * Scale});
		ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, {0, 0, 0, 0});
		ImGui::PushStyleColor(ImGuiCol_TableBorderLight, {1, 1, 1, 0.08f});
		if (ImGui::BeginTable("##OutlinerObjects", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX))
		{
			ImGui::TableSetupColumn("Item Label", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 105.0f * ImGui::GetFontSize() / 15.0f);
			ImGui::PushStyleColor(ImGuiCol_Header, {0, 0, 0, 0});
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0, 0, 0, 0});
			ImGui::PushStyleColor(ImGuiCol_HeaderActive, {0, 0, 0, 0});
			ImGui::TableHeadersRow();
			ImGui::PopStyleColor(3);
			const ImGuiTable* const Table = ImGui::GetCurrentTable();
			ImGui::TablePushBackgroundChannel();
			ImGui::GetWindowDrawList()->AddRectFilled({Table->WorkRect.Min.x, Table->RowPosY1}, {Table->WorkRect.Max.x, Table->RowPosY2}, ImGui::GetColorU32(ImVec4{1, 1, 1, 0.05f}), 4.0f * Scale);
			ImGui::TablePopBackgroundChannel();
			RowsTop = ImGui::GetCursorScreenPos().y;
			if (bCubeVisible)
			{
				ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {8.0f * Scale, 6.0f * Scale});
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {6.0f * Scale, 12.0f * Scale});
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				const float IconX = ImGui::GetCursorScreenPos().x + 2.0f * Scale;
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 22.0f * Scale);
				ImGui::PushStyleColor(ImGuiCol_Header, {0, 0, 0, 0});
				ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0, 0, 0, 0});
				ImGui::PushStyleColor(ImGuiCol_HeaderActive, {0, 0, 0, 0});
				if (ImGui::Selectable("Preview Cube##PreviewCube", bSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
				{
					bSelected = true;
					bFocusRequested = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter);
				}
				ImGui::PopStyleColor(3);
				bRowHovered = ImGui::IsItemHovered();
				const ImVec2 Minimum = ImGui::GetItemRectMin();
				const ImVec2 Maximum = ImGui::GetItemRectMax();
				if (bSelected || bRowHovered)
				{
					const ImGuiCol Color = bRowHovered ? (ImGui::IsItemActive() ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered) : ImGuiCol_Header;
					ImGui::TablePushBackgroundChannel();
					ImGui::GetWindowDrawList()->AddRectFilled({Table->WorkRect.Min.x, Minimum.y}, {Table->WorkRect.Max.x, Maximum.y}, ImGui::GetColorU32(Color), 4.0f * Scale);
					ImGui::TablePopBackgroundChannel();
				}
				const float CenterY = (Minimum.y + Maximum.y) * 0.5f;
				const ImVec2 Top{IconX + 6.0f * Scale, CenterY - 7.0f * Scale};
				const ImVec2 Left{IconX, CenterY - 4.0f * Scale};
				const ImVec2 Right{IconX + 12.0f * Scale, Left.y};
				const ImVec2 Center{Top.x, CenterY - Scale};
				const ImVec2 Bottom{Top.x, CenterY + 7.0f * Scale};
				const ImVec2 Outline[]{Top, Right, {Right.x, CenterY + 4.0f * Scale}, Bottom, {Left.x, CenterY + 4.0f * Scale}, Left};
				ImDrawList* const Draw = ImGui::GetWindowDrawList();
				const ImU32 IconColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
				Draw->AddPolyline(Outline, 6, IconColor, ImDrawFlags_Closed, Scale);
				Draw->AddLine(Left, Center, IconColor, Scale);
				Draw->AddLine(Right, Center, IconColor, Scale);
				Draw->AddLine(Center, Bottom, IconColor, Scale);
				ImGui::TableSetColumnIndex(1);
				ImGui::TextDisabled("Static Mesh");
				ImGui::PopStyleVar(2);
			}
			ImGui::EndTable();
		}
		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar(3);
		if (!bDragging && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetIO().MousePos.y >= RowsTop && !bRowHovered)
		{
			bSelected = false;
		}
	}
	ImGui::EndChild();
	ImGui::Separator();
	if (State.Search.IsActive())
		ImGui::TextDisabled("%d of 1 object", bCubeVisible ? 1 : 0);
	else
		ImGui::TextDisabled(bSelected ? "1 object (1 selected)" : "1 object");
	ImGui::EndDisabled();
	ToolUI.EndPanel();
	return bFocusRequested;
}
}
