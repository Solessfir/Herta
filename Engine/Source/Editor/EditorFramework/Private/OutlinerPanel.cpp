#include "OutlinerPanel.h"

#include "Herta/ToolUI/ToolUI.h"
#include "PreviewScene.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

namespace Herta
{
void BuildOutlinerVisibleRows(FOutlinerPanelState& State, const std::span<const FPreviewObject> Objects)
{
	bool bHierarchyChanged = State.CachedHierarchy.size() != Objects.size();
	for (std::size_t Index = 0; !bHierarchyChanged && Index < Objects.size(); ++Index)
	{
		bHierarchyChanged = State.CachedHierarchy[Index] != std::pair{Objects[Index].Id, Objects[Index].Parent};
	}

	if (bHierarchyChanged)
	{
		const auto HashId = [](const FObjectId Id)
		{
			return std::hash<std::uint64_t>{}(Id.GetHigh()) ^ (std::hash<std::uint64_t>{}(Id.GetLow()) << 1);
		};
		std::unordered_map<FObjectId, int, decltype(HashId)> Indices;
		Indices.reserve(Objects.size());
		State.CachedHierarchy.clear();
		State.CachedHierarchy.reserve(Objects.size());
		for (std::size_t Index = 0; Index < Objects.size(); ++Index)
		{
			const FPreviewObject& Object = Objects[Index];
			State.CachedHierarchy.emplace_back(Object.Id, Object.Parent);
			if (Object.Id.IsValid())
			{
				Indices.emplace(Object.Id, static_cast<int>(Index));
			}
		}

		std::erase_if(State.CollapsedObjects, [&](const FObjectId Id)
		{
			return !Indices.contains(Id);
		});

		std::vector<std::vector<int>> Children(Objects.size());
		std::vector<int> Roots;
		State.ParentIndices.assign(Objects.size(), -1);
		for (std::size_t Index = 0; Index < Objects.size(); ++Index)
		{
			const auto Parent = Objects[Index].Parent ? Indices.find(*Objects[Index].Parent) : Indices.end();
			if (Parent != Indices.end() && Parent->second != static_cast<int>(Index))
			{
				Children[static_cast<std::size_t>(Parent->second)].push_back(static_cast<int>(Index));
				State.ParentIndices[Index] = Parent->second;
			}
			else
			{
				Roots.push_back(static_cast<int>(Index));
			}
		}

		State.HierarchyRows.clear();
		State.HierarchyRows.reserve(Objects.size());
		std::vector<FOutlinerRow> Pending;
		Pending.reserve(Objects.size());
		std::vector<bool> Visited(Objects.size());
		const auto AppendTree = [&](const int Root)
		{
			State.ParentIndices[static_cast<std::size_t>(Root)] = -1;
			Pending.push_back({.ObjectIndex = Root});
			while (!Pending.empty())
			{
				FOutlinerRow Row = Pending.back();
				Pending.pop_back();
				const std::size_t Index = static_cast<std::size_t>(Row.ObjectIndex);
				if (Visited[Index])
				{
					continue;
				}

				Visited[Index] = true;
				Row.bHasChildren = !Children[Index].empty();
				State.HierarchyRows.push_back(Row);
				for (auto Child = Children[Index].rbegin(); Child != Children[Index].rend(); ++Child)
				{
					if (!Visited[static_cast<std::size_t>(*Child)])
					{
						Pending.push_back({.ObjectIndex = *Child, .Depth = Row.Depth + 1});
					}
				}
			}
		};
		for (const int Root : Roots)
		{
			AppendTree(Root);
		}

		// Keep malformed preview metadata bounded; the authored world validates hierarchy separately.
		for (std::size_t Index = 0; Index < Objects.size(); ++Index)
		{
			if (!Visited[Index])
			{
				AppendTree(static_cast<int>(Index));
			}
		}
	}

	const bool bSearching = State.Search.IsActive();
	std::vector<bool> Matches;
	if (bSearching)
	{
		Matches.resize(Objects.size());
		std::string SearchText;
		for (std::size_t Index = 0; Index < Objects.size(); ++Index)
		{
			SearchText.assign(Objects[Index].Label);
			SearchText.append(Objects[Index].Mesh.IsValid() ? " Static Mesh" : " Entity");
			Matches[Index] = State.Search.PassFilter(SearchText.c_str());
		}

		for (auto Row = State.HierarchyRows.rbegin(); Row != State.HierarchyRows.rend(); ++Row)
		{
			const int Parent = State.ParentIndices[static_cast<std::size_t>(Row->ObjectIndex)];
			if (Matches[static_cast<std::size_t>(Row->ObjectIndex)] && Parent >= 0)
			{
				Matches[static_cast<std::size_t>(Parent)] = true;
			}
		}
	}

	State.VisibleRows.clear();
	State.VisibleIndices.clear();
	State.VisibleRows.reserve(Objects.size());
	State.VisibleIndices.reserve(Objects.size());
	int CollapsedDepth = -1;
	for (const FOutlinerRow& Row : State.HierarchyRows)
	{
		if (CollapsedDepth >= 0 && Row.Depth > CollapsedDepth)
		{
			continue;
		}

		CollapsedDepth = -1;
		const std::size_t Index = static_cast<std::size_t>(Row.ObjectIndex);
		if (bSearching && !Matches[Index])
		{
			continue;
		}

		State.VisibleRows.push_back(Row);
		State.VisibleIndices.push_back(Row.ObjectIndex);
		if (!bSearching && State.CollapsedObjects.contains(Objects[Index].Id))
		{
			CollapsedDepth = Row.Depth;
		}
	}
}

std::optional<FOutlinerReparentRequest> MakeOutlinerReparentRequest(const std::span<const FPreviewObject> Objects, const FPreviewSelection& Selection, const int SourceIndex, const std::optional<FObjectId> Parent)
{
	if (SourceIndex < 0 || static_cast<std::size_t>(SourceIndex) >= Objects.size())
	{
		return std::nullopt;
	}

	FOutlinerReparentRequest Request{.Parent = Parent};
	std::set<FObjectId> Added;
	const auto AddObject = [&](const int Index)
	{
		if (Index >= 0 && static_cast<std::size_t>(Index) < Objects.size())
		{
			const FPreviewObject& Object = Objects[static_cast<std::size_t>(Index)];
			if (Object.Id.IsValid() && Added.insert(Object.Id).second)
			{
				Request.Objects.push_back(Object.Id);
			}
		}
	};
	if (Selection.Contains(SourceIndex))
	{
		for (const int Index : Selection.Indices)
		{
			AddObject(Index);
		}
	}
	else
	{
		AddObject(SourceIndex);
	}

	if (Request.Objects.empty() || (Parent && Added.contains(*Parent)))
	{
		return std::nullopt;
	}

	return Request;
}

namespace
{
constexpr char OutlinerPayload[] = "Herta.OutlinerObjects";

void AcceptOutlinerDrop(FOutlinerPanelState& State, const std::optional<FObjectId> Parent)
{
	if (const ImGuiPayload* const Payload = ImGui::AcceptDragDropPayload(OutlinerPayload))
	{
		if (Payload->DataSize <= 0 || static_cast<std::size_t>(Payload->DataSize) % sizeof(FObjectId) != 0)
		{
			return;
		}

		FOutlinerReparentRequest Request{.Parent = Parent};
		Request.Objects.resize(static_cast<std::size_t>(Payload->DataSize) / sizeof(FObjectId));
		std::memcpy(Request.Objects.data(), Payload->Data, static_cast<std::size_t>(Payload->DataSize));
		if (!Parent || !std::ranges::any_of(Request.Objects, [&](const FObjectId Id)
		{
			return Id == *Parent;
		}))
		{
			State.ReparentRequest = std::move(Request);
		}
	}
}
}

bool DrawOutlinerRenameField(FOutlinerPanelState& State, const ImVec2 Position, const float Width, const bool bStartRename)
{
	ImGui::SetCursorScreenPos(Position);
	ImGui::SetNextItemWidth(std::max(1.f, Width));
	if (bStartRename)
	{
		ImGui::SetKeyboardFocusHere();
	}

	const bool bCancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
	ImGui::PushStyleColor(ImGuiCol_NavCursor, {0, 0, 0, 0});
	const bool bCommit = ImGui::InputText("##ObjectLabel", State.RenameBuffer.data(), State.RenameBuffer.size(), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
	ImGui::PopStyleColor();
	const bool bHovered = ImGui::IsItemHovered();

	if (bCancel || bCommit || (!bStartRename && ImGui::IsItemDeactivated()))
	{
		State.bRenameCommitted = !bCancel;
		State.bRenaming = false;
	}

	return bHovered;
}

bool DrawPreviewOutlinerContents(FPreviewSelection& Selection, const std::span<const FPreviewObject> Objects, const bool bDragging, FOutlinerPanelState& State)
{
	State.bRenameCommitted = false;
	bool bFocusRequested = false;
	const float Scale = ImGui::GetFontSize() / 15.f;
	ImGui::BeginDisabled(bDragging);
	auto& VisibleIndices = State.VisibleIndices;
	BuildOutlinerVisibleRows(State, Objects);

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

	const bool bStartRename = !bDragging && State.bRenameRequested && Selection.Active >= 0 && static_cast<std::size_t>(Selection.Active) < Objects.size() && std::ranges::find(VisibleIndices, Selection.Active) != VisibleIndices.end();
	State.bRenameRequested = false;

	if (bDragging)
	{
		State.bRenaming = false;
	}

	if (State.bRenaming)
	{
		const auto Object = std::ranges::find(Objects, State.RenameObject, &FPreviewObject::Id);
		if (Object == Objects.end() || std::ranges::find(VisibleIndices, static_cast<int>(Object - Objects.begin())) == VisibleIndices.end())
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
					const FOutlinerRow& Row = State.VisibleRows[static_cast<std::size_t>(VisibleIndex)];

					ImGui::PushID(static_cast<int>(Index));
					ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {8.f * Scale, 6.f * Scale});
					ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {6.f * Scale, 12.f * Scale});
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					const float Indent = std::min(static_cast<float>(Row.Depth) * 18.f * Scale, std::max(0.f, Table->Columns[0].WorkMaxX - ImGui::GetCursorScreenPos().x - 84.f * Scale));
					const float ArrowX = ImGui::GetCursorScreenPos().x + Indent;
					const float IconX = ArrowX + 20.f * Scale;
					ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Indent + 42.f * Scale);
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

					const bool bArrowHit = Row.bHasChildren && IO.MousePos.x >= ArrowX && IO.MousePos.x < ArrowX + 18.f * Scale;
					const bool bExpanded = State.Search.IsActive() || !State.CollapsedObjects.contains(Object.Id);
					if (ImGui::Selectable("##Object", SelectedMask[Index], ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) && !bRenamingRow)
					{
						if (bArrowHit)
						{
							if (!State.Search.IsActive())
							{
								if (bExpanded)
								{
									State.CollapsedObjects.insert(Object.Id);
								}
								else
								{
									State.CollapsedObjects.erase(Object.Id);
								}
							}
						}
						else if (IO.KeyShift)
						{
							Selection.SelectRange(static_cast<int>(Index), VisibleIndices, IO.KeyCtrl);
						}
						else if (!IO.KeyCtrl || !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
						{
							Selection.Select(static_cast<int>(Index), IO.KeyCtrl);
						}
						RefreshSelectedMask();

						bFocusRequested = !bArrowHit && (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter));
					}

					ImGui::PopStyleColor(4);
					const bool bCurrentRowHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
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
					const bool bArrowDrag = Row.bHasChildren && IO.MouseClickedPos[ImGuiMouseButton_Left].x >= ArrowX && IO.MouseClickedPos[ImGuiMouseButton_Left].x < ArrowX + 18.f * Scale;
					if (!bDragging && !bRenamingRow && !bArrowDrag && Object.Id.IsValid() && ImGui::BeginDragDropSource())
					{
						const ImGuiPayload* Payload = ImGui::GetDragDropPayload();
						if (!Payload || !Payload->IsDataType(OutlinerPayload))
						{
							const auto Request = MakeOutlinerReparentRequest(Objects, Selection, static_cast<int>(Index), std::nullopt);
							if (Request)
							{
								ImGui::SetDragDropPayload(OutlinerPayload, Request->Objects.data(), Request->Objects.size() * sizeof(FObjectId), ImGuiCond_Once);
							}

							Payload = ImGui::GetDragDropPayload();
						}

						ImGui::TextUnformatted(Object.Label.c_str());
						if (Payload && static_cast<std::size_t>(Payload->DataSize) > sizeof(FObjectId))
						{
							ImGui::TextDisabled("%d objects", static_cast<int>(static_cast<std::size_t>(Payload->DataSize) / sizeof(FObjectId)));
						}
						ImGui::TextDisabled("Drop on an object to parent, or empty space for root");

						ImGui::EndDragDropSource();
					}

					if (!bDragging && !bRenamingRow && Object.Id.IsValid() && ImGui::BeginDragDropTarget())
					{
						AcceptOutlinerDrop(State, Object.Id);
						ImGui::EndDragDropTarget();
					}

					if (!bDragging && !bRenamingRow && ImGui::BeginPopupContextItem("##ObjectActions"))
					{
						if (ImGui::MenuItem("Move to root", nullptr, false, Object.Parent.has_value() || Selection.Contains(static_cast<int>(Index))))
						{
							State.ReparentRequest = MakeOutlinerReparentRequest(Objects, Selection, static_cast<int>(Index), std::nullopt);
						}

						ImGui::EndPopup();
					}

					if (Row.bHasChildren)
					{
						ImDrawList* const Draw = ImGui::GetWindowDrawList();
						const ImU32 ArrowColor = ImGui::GetColorU32(bCurrentRowHovered && bArrowHit ? ImGuiCol_Text : ImGuiCol_TextDisabled);
						if (bExpanded)
						{
							Draw->AddTriangleFilled({ArrowX + 4.f * Scale, CenterY - 2.f * Scale}, {ArrowX + 12.f * Scale, CenterY - 2.f * Scale}, {ArrowX + 8.f * Scale, CenterY + 3.f * Scale}, ArrowColor);
						}
						else
						{
							Draw->AddTriangleFilled({ArrowX + 6.f * Scale, CenterY - 4.f * Scale}, {ArrowX + 6.f * Scale, CenterY + 4.f * Scale}, {ArrowX + 11.f * Scale, CenterY}, ArrowColor);
						}
					}

					if (bRenamingRow)
					{
						bRowHovered |= DrawOutlinerRenameField(State, {LabelX, CenterY - ImGui::GetFrameHeight() * 0.5f}, Table->Columns[0].WorkMaxX - LabelX, bStartRename);
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

		if (!bDragging && !bRowHovered && ImGui::GetIO().MousePos.y >= RowsTop)
		{
			const ImGuiWindow* const Window = ImGui::GetCurrentWindow();
			const ImRect Background{{Window->InnerRect.Min.x, std::max(RowsTop, Window->InnerRect.Min.y)}, Window->InnerRect.Max};
			if (ImGui::BeginDragDropTargetCustom(Background, ImGui::GetID("##RootDrop")))
			{
				AcceptOutlinerDrop(State, std::nullopt);
				ImGui::EndDragDropTarget();
			}
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
	return bFocusRequested;
}

bool DrawPreviewOutlinerPanel(FToolUIContext& ToolUI, bool& bOpen, FPreviewSelection& Selection, const std::span<const FPreviewObject> Objects, const bool bDragging, FOutlinerPanelState& State)
{
	State.bRenameCommitted = false;
	if (!ToolUI.BeginPanel("Outliner", &bOpen))
	{
		ToolUI.EndPanel();
		return false;
	}

	ImGui::BeginDisabled(bDragging);
	ImGui::SetNextItemWidth(-1.f);
	if (ToolUI.DrawSearchField("##OutlinerSearch", "Search", State.Search.InputBuf, sizeof(State.Search.InputBuf)))
	{
		State.Search.Build();
	}

	ImGui::EndDisabled();
	const bool bFocusRequested = DrawPreviewOutlinerContents(Selection, Objects, bDragging, State);
	ToolUI.EndPanel();
	return bFocusRequested;
}
}
