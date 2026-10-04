#include "OutlinerPanel.h"

#include "Herta/ToolUI/ToolUI.h"
#include "PreviewScene.h"

#include <imgui_internal.h>

#include <algorithm>
#include <string>

namespace Herta
{
bool DrawPreviewOutlinerPanel(FToolUIContext& ToolUI, bool& bOpen, FPreviewSelection& Selection, const std::span<const FPreviewObject> Objects, const bool bDragging, FOutlinerPanelState& State)
{
	State.bRenameCommitted = false;
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

	auto& VisibleIndices = State.VisibleIndices;
	VisibleIndices.clear();
	VisibleIndices.reserve(Objects.size());
	std::string SearchText;
	const auto IsObjectVisible = [&](const std::string_view Label)
	{
		if (!State.Search.IsActive())
		{
			return true;
		}

		SearchText.assign(Label);
		SearchText.append(" Static Mesh");
		return State.Search.PassFilter(SearchText.c_str());
	};
	for (std::size_t Index = 0; Index < Objects.size(); ++Index)
	{
		if (IsObjectVisible(Objects[Index].Label))
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

		State.bRenameRequested |= Selection.Active >= 0 && ImGui::IsKeyPressed(ImGuiKey_F2, false);
	}

	auto& SelectedMask = State.SelectedMask;
	const auto RefreshSelectedMask = [&]
	{
		SelectedMask.resize(Objects.size());
		std::ranges::fill(SelectedMask, false);
		for (const int Index : Selection.Indices)
		{
			if (Index >= 0 && static_cast<std::size_t>(Index) < SelectedMask.size())
			{
				SelectedMask[static_cast<std::size_t>(Index)] = true;
			}
		}
	};
	RefreshSelectedMask();

	const bool bStartRename = !bDragging && State.bRenameRequested && Selection.Active >= 0 && static_cast<std::size_t>(Selection.Active) < Objects.size() && IsObjectVisible(Objects[static_cast<std::size_t>(Selection.Active)].Label);
	State.bRenameRequested = false;

	if (bDragging)
	{
		State.bRenaming = false;
	}

	if (State.bRenaming)
	{
		const auto Object = std::ranges::find(Objects, State.RenameObject, &FPreviewObject::Id);
		if (Object == Objects.end() || !IsObjectVisible(Object->Label))
		{
			State.bRenameCommitted = Object != Objects.end();
			State.bRenaming = false;
		}
	}

	if (bStartRename)
	{
		const FPreviewObject& Object = Objects[static_cast<std::size_t>(Selection.Active)];
		State.RenameBuffer.fill('\0');
		std::copy_n(Object.Label.begin(), std::min(Object.Label.size(), State.RenameBuffer.size() - 1), State.RenameBuffer.begin());
		State.RenameObject = Object.Id;
		State.bRenaming = true;
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
			ImGuiListClipper Clipper;
			Clipper.Begin(static_cast<int>(VisibleIndices.size()));
			const auto IncludeObject = [&](const int ObjectIndex)
			{
				const auto Visible = std::ranges::find(VisibleIndices, ObjectIndex);
				if (Visible != VisibleIndices.end())
				{
					Clipper.IncludeItemByIndex(static_cast<int>(Visible - VisibleIndices.begin()));
				}
			};
			if (Selection.Active >= 0)
			{
				IncludeObject(Selection.Active);
			}

			if (Selection.Anchor >= 0)
			{
				IncludeObject(Selection.Anchor);
			}

			if (State.bRenaming)
			{
				const auto Object = std::ranges::find(Objects, State.RenameObject, &FPreviewObject::Id);
				if (Object != Objects.end())
				{
					IncludeObject(static_cast<int>(Object - Objects.begin()));
				}
			}

			while (Clipper.Step())
			{
				for (int VisibleIndex = Clipper.DisplayStart; VisibleIndex < Clipper.DisplayEnd; ++VisibleIndex)
				{
					const std::size_t Index = static_cast<std::size_t>(VisibleIndices[static_cast<std::size_t>(VisibleIndex)]);
					const FPreviewObject& Object = Objects[Index];

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
					ImGui::PushStyleColor(ImGuiCol_NavCursor, {0, 0, 0, 0});
					const float LabelX = ImGui::GetCursorScreenPos().x;
					const bool bRenamingRow = State.bRenaming && State.RenameObject == Object.Id;
					if (bRenamingRow)
					{
						ImGui::SetNextItemAllowOverlap();
					}

					if (ImGui::Selectable("##Object", SelectedMask[Index], ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) && !bRenamingRow)
					{
						if (IO.KeyShift)
						{
							Selection.SelectRange(static_cast<int>(Index), VisibleIndices, IO.KeyCtrl);
						}
						else if (!IO.KeyCtrl || !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
						{
							Selection.Select(static_cast<int>(Index), IO.KeyCtrl);
						}
						RefreshSelectedMask();

						bFocusRequested = !bRenamingRow && (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter));
					}

					ImGui::PopStyleColor(4);
					const bool bCurrentRowHovered = ImGui::IsItemHovered();
					bRowHovered |= bCurrentRowHovered;
					const ImVec2 Minimum = ImGui::GetItemRectMin();
					const ImVec2 Maximum = ImGui::GetItemRectMax();
					if (SelectedMask[Index] || bCurrentRowHovered)
					{
						const ImGuiCol Color = bCurrentRowHovered ? (ImGui::IsItemActive() ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered) : ImGuiCol_Header;
						ImGui::TablePushBackgroundChannel();
						ImGui::GetWindowDrawList()->AddRectFilled({Table->WorkRect.Min.x, Minimum.y}, {Table->WorkRect.Max.x, Maximum.y}, ImGui::GetColorU32(Color), 4.f * Scale);
						ImGui::TablePopBackgroundChannel();
					}

					const float CenterY = (Minimum.y + Maximum.y) * 0.5f;
					if (bRenamingRow)
					{
						const ImVec2 Cursor = ImGui::GetCursorScreenPos();
						ImGui::SetCursorScreenPos({LabelX, CenterY - ImGui::GetFrameHeight() * 0.5f});
						ImGui::SetNextItemWidth(std::max(1.f, Table->Columns[0].WorkMaxX - LabelX));

						if (bStartRename)
						{
							ImGui::SetKeyboardFocusHere();
						}

						const bool bCancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
						ImGui::PushStyleColor(ImGuiCol_NavCursor, {0, 0, 0, 0});
						const bool bCommit = ImGui::InputText("##ObjectLabel", State.RenameBuffer.data(), State.RenameBuffer.size(), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
						ImGui::PopStyleColor();
						bRowHovered |= ImGui::IsItemHovered();

						if (bCancel || bCommit || (!bStartRename && ImGui::IsItemDeactivated()))
						{
							State.bRenameCommitted = !bCancel;
							State.bRenaming = false;
						}

						ImGui::SetCursorScreenPos(Cursor);
					}
					else
					{
						ImGui::GetWindowDrawList()->AddText({LabelX, CenterY - ImGui::GetFontSize() * 0.5f}, ImGui::GetColorU32(ImGuiCol_Text), Object.Label.data(), Object.Label.data() + Object.Label.size());
					}
					const ImVec2 Top{IconX + 6.f * Scale, CenterY - 7.f * Scale};
					const ImVec2 Left{IconX, CenterY - 4.f * Scale};
					const ImVec2 Right{IconX + 12.f * Scale, Left.y};
					const ImVec2 Center{Top.x, CenterY - Scale};
					const ImVec2 Bottom{Top.x, CenterY + 7.f * Scale};
					const ImVec2 Outline[]{Top, Right, {Right.x, CenterY + 4.f * Scale}, Bottom, {Left.x, CenterY + 4.f * Scale}, Left};
					ImDrawList* const Draw = ImGui::GetWindowDrawList();
					const ImU32 IconColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
					if (Object.Mesh.IsValid())
					{
						Draw->AddPolyline(Outline, 6, IconColor, ImDrawFlags_Closed, Scale);
						Draw->AddLine(Left, Center, IconColor, Scale);
						Draw->AddLine(Right, Center, IconColor, Scale);
						Draw->AddLine(Center, Bottom, IconColor, Scale);
					}
					else
					{
						const ImVec2 EntityCenter{Top.x, CenterY};
						Draw->AddCircle(EntityCenter, 6.f * Scale, IconColor, 16, Scale);
						Draw->AddCircleFilled(EntityCenter, 1.5f * Scale, IconColor);
					}
					ImGui::TableSetColumnIndex(1);
					ImGui::TextDisabled("%s", Object.Mesh.IsValid() ? "Static Mesh" : "Entity");
					ImGui::PopStyleVar(2);
					ImGui::PopID();
				}
			}

			ImGui::EndTable();
		}

		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar(3);
		if (!bDragging && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetIO().MousePos.y >= RowsTop && !bRowHovered)
		{
			Selection.Select(-1);
			RefreshSelectedMask();
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
