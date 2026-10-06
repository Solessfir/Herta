#include "OutlinerPanel.h"

#include "Herta/ToolUI/ToolUI.h"
#include "PreviewLevel.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <string>
#include <unordered_map>

namespace Herta
{
void BuildOutlinerVisibleRows(FOutlinerPanelState& State, const std::span<const FPreviewObject> Objects, const std::span<const FLevelFolder> Folders)
{
	const std::size_t NodeCount = Objects.size() + Folders.size();
	const auto NodeIndex = [&](const FOutlinerRow& Row)
	{
		return Row.ObjectIndex >= 0 ? static_cast<std::size_t>(Row.ObjectIndex) : Objects.size() + static_cast<std::size_t>(Row.FolderIndex);
	};
	bool bHierarchyChanged = State.CachedHierarchy.size() != Objects.size() || !std::ranges::equal(State.CachedFolders, Folders);
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
		Indices.reserve(NodeCount);
		State.CachedFolders.assign(Folders.begin(), Folders.end());
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

		for (std::size_t Index = 0; Index < Folders.size(); ++Index)
		{
			Indices.emplace(Folders[Index].Id, static_cast<int>(Objects.size() + Index));
		}

		std::erase_if(State.CollapsedFolders, [&](const FObjectId Id)
		{
			return !Indices.contains(Id);
		});

		if (State.SelectedFolder.IsValid() && std::ranges::find(Folders, State.SelectedFolder, &FLevelFolder::Id) == Folders.end())
		{
			State.SelectedFolder = {};
		}

		std::vector<int> FolderAssignments(Objects.size(), -1);
		for (std::size_t Index = 0; Index < Folders.size(); ++Index)
		{
			for (const FObjectId Id : Folders[Index].Entities)
			{
				const auto Object = Indices.find(Id);
				if (Object != Indices.end() && static_cast<std::size_t>(Object->second) < Objects.size())
				{
					FolderAssignments[static_cast<std::size_t>(Object->second)] = static_cast<int>(Objects.size() + Index);
				}
			}
		}

		std::vector<std::vector<int>> Children(NodeCount);
		std::vector<int> Roots;
		State.ParentIndices.assign(NodeCount, -1);
		for (std::size_t Index = 0; Index < Objects.size(); ++Index)
		{
			const auto Parent = Objects[Index].Parent ? Indices.find(*Objects[Index].Parent) : Indices.end();
			const int ParentIndex = Parent != Indices.end() && static_cast<std::size_t>(Parent->second) < Objects.size() && Parent->second != static_cast<int>(Index) ? Parent->second : FolderAssignments[Index];
			if (ParentIndex >= 0)
			{
				Children[static_cast<std::size_t>(ParentIndex)].push_back(static_cast<int>(Index));
				State.ParentIndices[Index] = ParentIndex;
			}
			else
			{
				Roots.push_back(static_cast<int>(Index));
			}
		}

		for (std::size_t Index = 0; Index < Folders.size(); ++Index)
		{
			const int FolderIndex = static_cast<int>(Objects.size() + Index);
			const auto Parent = Indices.find(Folders[Index].Parent);
			if (Parent != Indices.end() && static_cast<std::size_t>(Parent->second) >= Objects.size() && Parent->second != FolderIndex)
			{
				Children[static_cast<std::size_t>(Parent->second)].push_back(FolderIndex);
				State.ParentIndices[static_cast<std::size_t>(FolderIndex)] = Parent->second;
			}
			else
			{
				Roots.push_back(FolderIndex);
			}
		}

		const auto OrderNodes = [&](const int First, const int Second)
		{
			const bool bFirstFolder = static_cast<std::size_t>(First) >= Objects.size();
			const bool bSecondFolder = static_cast<std::size_t>(Second) >= Objects.size();
			if (bFirstFolder != bSecondFolder)
			{
				return bFirstFolder;
			}

			return bFirstFolder ? Folders[static_cast<std::size_t>(First) - Objects.size()].Id < Folders[static_cast<std::size_t>(Second) - Objects.size()].Id : First < Second;
		};
		std::ranges::sort(Roots, OrderNodes);
		for (auto& Siblings : Children)
		{
			std::ranges::sort(Siblings, OrderNodes);
		}

		State.HierarchyRows.clear();
		State.HierarchyRows.reserve(NodeCount);
		std::vector<FOutlinerRow> Pending;
		Pending.reserve(NodeCount);
		std::vector<bool> Visited(NodeCount);
		const auto MakeRow = [&](const int Index, const int Depth)
		{
			return static_cast<std::size_t>(Index) < Objects.size() ? FOutlinerRow{.ObjectIndex = Index, .Depth = Depth} : FOutlinerRow{.FolderIndex = Index - static_cast<int>(Objects.size()), .Depth = Depth};
		};
		const auto AppendTree = [&](const int Root)
		{
			State.ParentIndices[static_cast<std::size_t>(Root)] = -1;
			Pending.push_back(MakeRow(Root, 0));
			while (!Pending.empty())
			{
				FOutlinerRow Row = Pending.back();
				Pending.pop_back();
				const std::size_t Index = NodeIndex(Row);
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
						Pending.push_back(MakeRow(*Child, Row.Depth + 1));
					}
				}
			}
		};
		for (const int Root : Roots)
		{
			AppendTree(Root);
		}

		// Keep malformed preview metadata bounded; the authored world validates hierarchy separately.
		for (std::size_t Index = 0; Index < NodeCount; ++Index)
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
		Matches.resize(NodeCount);
		std::string SearchText;
		for (std::size_t Index = 0; Index < Objects.size(); ++Index)
		{
			SearchText.assign(Objects[Index].Label);
			SearchText.append(Objects[Index].Mesh.IsValid() ? " Static Mesh" : " Entity");
			Matches[Index] = State.Search.PassFilter(SearchText.c_str());
		}

		for (std::size_t Index = 0; Index < Folders.size(); ++Index)
		{
			SearchText = Folders[Index].Name + " Folder";
			Matches[Objects.size() + Index] = State.Search.PassFilter(SearchText.c_str());
		}

		for (auto Row = State.HierarchyRows.rbegin(); Row != State.HierarchyRows.rend(); ++Row)
		{
			const std::size_t Index = NodeIndex(*Row);
			const int Parent = State.ParentIndices[Index];
			if (Matches[Index] && Parent >= 0)
			{
				Matches[static_cast<std::size_t>(Parent)] = true;
			}
		}
	}

	State.VisibleRows.clear();
	State.VisibleIndices.clear();
	State.VisibleRows.reserve(NodeCount);
	State.VisibleIndices.reserve(Objects.size());
	int CollapsedDepth = -1;
	for (const FOutlinerRow& Row : State.HierarchyRows)
	{
		if (CollapsedDepth >= 0 && Row.Depth > CollapsedDepth)
		{
			continue;
		}

		CollapsedDepth = -1;
		const std::size_t Index = NodeIndex(Row);
		if (bSearching && !Matches[Index])
		{
			continue;
		}

		State.VisibleRows.push_back(Row);
		if (Row.ObjectIndex >= 0)
		{
			State.VisibleIndices.push_back(Row.ObjectIndex);
		}

		const bool bCollapsed = Row.FolderIndex >= 0 ? State.CollapsedFolders.contains(Folders[static_cast<std::size_t>(Row.FolderIndex)].Id) : State.CollapsedObjects.contains(Objects[Index].Id);
		if (!bSearching && bCollapsed)
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
constexpr char OutlinerFolderPayload[] = "Herta.OutlinerFolder";

void RequestOutlinerFolder(FOutlinerPanelState& State, const FPreviewSelection& Selection, const std::span<const FPreviewObject> Objects, const std::optional<FObjectId> Parent, const bool bIncludeSelection)
{
	FOutlinerFolderRequest Request{.Parent = Parent};
	if (bIncludeSelection)
	{
		const auto Selected = MakeOutlinerReparentRequest(Objects, Selection, Selection.Active, std::nullopt);
		if (Selected)
		{
			Request.Objects = Selected->Objects;
		}
	}

	State.FolderRequest = std::move(Request);
}

void AcceptOutlinerFolderDrop(FOutlinerPanelState& State, const std::optional<FObjectId> Parent)
{
	if (const ImGuiPayload* const Payload = ImGui::AcceptDragDropPayload(OutlinerFolderPayload))
	{
		if (Payload->DataSize == sizeof(FObjectId))
		{
			FObjectId Folder;
			std::memcpy(&Folder, Payload->Data, sizeof(Folder));
			if (!Parent || Folder != *Parent)
			{
				State.FolderRequest = FOutlinerFolderRequest{.Action = EOutlinerFolderAction::MoveFolder, .Folder = Folder, .Parent = Parent};
			}
		}
	}

	if (const ImGuiPayload* const Payload = ImGui::AcceptDragDropPayload(OutlinerPayload))
	{
		if (Payload->DataSize > 0 && static_cast<std::size_t>(Payload->DataSize) % sizeof(FObjectId) == 0)
		{
			FOutlinerFolderRequest Request{.Action = EOutlinerFolderAction::MoveEntities, .Parent = Parent};
			Request.Objects.resize(static_cast<std::size_t>(Payload->DataSize) / sizeof(FObjectId));
			std::memcpy(Request.Objects.data(), Payload->Data, static_cast<std::size_t>(Payload->DataSize));
			State.FolderRequest = std::move(Request);
		}
	}
}

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

bool DrawPreviewOutlinerContents(FPreviewSelection& Selection, const std::span<const FPreviewObject> Objects, const bool bDragging, FOutlinerPanelState& State, const std::span<const FLevelFolder> Folders)
{
	State.bRenameCommitted = false;
	bool bFocusRequested = false;
	const float Scale = ImGui::GetFontSize() / 15.f;
	ImGui::BeginDisabled(bDragging);
	auto& VisibleIndices = State.VisibleIndices;
	BuildOutlinerVisibleRows(State, Objects, Folders);

	const ImGuiIO& IO = ImGui::GetIO();
	if (!bDragging && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !IO.WantTextInput)
	{
		if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false))
		{
			State.SelectedFolder = {};
			Selection.SelectAll(VisibleIndices);
		}

		State.bRenameRequested |= (State.SelectedFolder.IsValid() || Selection.Active >= 0) && ImGui::IsKeyPressed(ImGuiKey_F2, false);
		if (State.SelectedFolder.IsValid() && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
		{
			State.FolderRequest = FOutlinerFolderRequest{.Action = EOutlinerFolderAction::Delete, .Folder = State.SelectedFolder};
		}

		if (IO.KeyCtrl && IO.KeyShift && ImGui::IsKeyPressed(ImGuiKey_N, false))
		{
			RequestOutlinerFolder(State, Selection, Objects, State.SelectedFolder.IsValid() ? std::optional{State.SelectedFolder} : std::nullopt, false);
		}
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

	const auto SelectedFolder = std::ranges::find(Folders, State.SelectedFolder, &FLevelFolder::Id);
	const bool bFolderVisible = SelectedFolder != Folders.end() && std::ranges::any_of(State.VisibleRows, [&](const FOutlinerRow& Row)
	{
		return Row.FolderIndex == static_cast<int>(SelectedFolder - Folders.begin());
	});
	const bool bStartRename = !bDragging && State.bRenameRequested && (bFolderVisible || (Selection.Active >= 0 && static_cast<std::size_t>(Selection.Active) < Objects.size() && std::ranges::find(VisibleIndices, Selection.Active) != VisibleIndices.end()));
	State.bRenameRequested = false;

	if (bDragging)
	{
		State.bRenaming = false;
	}

	if (State.bRenaming)
	{
		if (State.RenameFolder.IsValid())
		{
			const auto Folder = std::ranges::find(Folders, State.RenameFolder, &FLevelFolder::Id);
			if (Folder == Folders.end() || !std::ranges::any_of(State.VisibleRows, [&](const FOutlinerRow& Row)
			{
				return Row.FolderIndex == static_cast<int>(Folder - Folders.begin());
			}))
			{
				State.bRenameCommitted = Folder != Folders.end();
				State.bRenaming = false;
			}
		}
		else
		{
			const auto Object = std::ranges::find(Objects, State.RenameObject, &FPreviewObject::Id);
			if (Object == Objects.end() || std::ranges::find(VisibleIndices, static_cast<int>(Object - Objects.begin())) == VisibleIndices.end())
			{
				State.bRenameCommitted = Object != Objects.end();
				State.bRenaming = false;
			}
		}
	}

	if (bStartRename)
	{
		const std::string& Label = bFolderVisible ? SelectedFolder->Name : Objects[static_cast<std::size_t>(Selection.Active)].Label;
		State.RenameBuffer.fill('\0');
		std::copy_n(Label.begin(), std::min(Label.size(), State.RenameBuffer.size() - 1), State.RenameBuffer.begin());
		State.RenameObject = bFolderVisible ? FObjectId{} : Objects[static_cast<std::size_t>(Selection.Active)].Id;
		State.RenameFolder = bFolderVisible ? SelectedFolder->Id : FObjectId{};
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
			Clipper.Begin(static_cast<int>(State.VisibleRows.size()));
			const auto IncludeObject = [&](const int ObjectIndex)
			{
				const auto Visible = std::ranges::find(State.VisibleRows, ObjectIndex, &FOutlinerRow::ObjectIndex);
				if (Visible != State.VisibleRows.end())
				{
					Clipper.IncludeItemByIndex(static_cast<int>(Visible - State.VisibleRows.begin()));
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
				if (State.RenameFolder.IsValid())
				{
					const auto Folder = std::ranges::find(Folders, State.RenameFolder, &FLevelFolder::Id);
					if (Folder != Folders.end())
					{
						const auto Row = std::ranges::find(State.VisibleRows, static_cast<int>(Folder - Folders.begin()), &FOutlinerRow::FolderIndex);
						if (Row != State.VisibleRows.end())
						{
							Clipper.IncludeItemByIndex(static_cast<int>(Row - State.VisibleRows.begin()));
						}
					}
				}
				else
				{
					const auto Object = std::ranges::find(Objects, State.RenameObject, &FPreviewObject::Id);
					if (Object != Objects.end())
					{
						IncludeObject(static_cast<int>(Object - Objects.begin()));
					}
				}
			}

			while (Clipper.Step())
			{
				for (int VisibleIndex = Clipper.DisplayStart; VisibleIndex < Clipper.DisplayEnd; ++VisibleIndex)
				{
					const FOutlinerRow& Row = State.VisibleRows[static_cast<std::size_t>(VisibleIndex)];
					const bool bFolder = Row.FolderIndex >= 0;
					const FLevelFolder* const Folder = bFolder ? &Folders[static_cast<std::size_t>(Row.FolderIndex)] : nullptr;
					const FPreviewObject* const Object = bFolder ? nullptr : &Objects[static_cast<std::size_t>(Row.ObjectIndex)];
					const FObjectId Id = bFolder ? Folders[static_cast<std::size_t>(Row.FolderIndex)].Id : Objects[static_cast<std::size_t>(Row.ObjectIndex)].Id;
					const std::string& Label = bFolder ? Folders[static_cast<std::size_t>(Row.FolderIndex)].Name : Objects[static_cast<std::size_t>(Row.ObjectIndex)].Label;
					const bool bSelected = bFolder ? State.SelectedFolder == Id : SelectedMask[static_cast<std::size_t>(Row.ObjectIndex)];

					ImGui::PushID(bFolder ? -Row.FolderIndex - 1 : Row.ObjectIndex);
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
					const bool bRenamingRow = State.bRenaming && (bFolder ? State.RenameFolder == Id : !State.RenameFolder.IsValid() && State.RenameObject == Id);
					if (bRenamingRow)
					{
						ImGui::SetNextItemAllowOverlap();
					}

					const bool bArrowHit = Row.bHasChildren && IO.MousePos.x >= ArrowX && IO.MousePos.x < ArrowX + 18.f * Scale;
					auto& Collapsed = bFolder ? State.CollapsedFolders : State.CollapsedObjects;
					const bool bExpanded = State.Search.IsActive() || !Collapsed.contains(Id);
					if (ImGui::Selectable("##Object", bSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) && !bRenamingRow)
					{
						if (bArrowHit || (bFolder && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)))
						{
							if (!State.Search.IsActive())
							{
								if (bExpanded)
								{
									Collapsed.insert(Id);
								}
								else
								{
									Collapsed.erase(Id);
								}
							}
						}
						else if (bFolder)
						{
							State.SelectedFolder = Id;
							Selection.Select(-1);
						}
						else if (IO.KeyShift)
						{
							State.SelectedFolder = {};
							Selection.SelectRange(Row.ObjectIndex, VisibleIndices, IO.KeyCtrl);
						}
						else if (!IO.KeyCtrl || !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
						{
							State.SelectedFolder = {};
							Selection.Select(Row.ObjectIndex, IO.KeyCtrl);
						}
						RefreshSelectedMask();

						bFocusRequested = !bFolder && !bArrowHit && (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter));
					}

					ImGui::PopStyleColor(4);
					const bool bCurrentRowHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
					bRowHovered |= bCurrentRowHovered;
					const ImVec2 Minimum = ImGui::GetItemRectMin();
					const ImVec2 Maximum = ImGui::GetItemRectMax();
					if (bSelected || bCurrentRowHovered)
					{
						const ImGuiCol Color = bCurrentRowHovered ? (ImGui::IsItemActive() ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered) : ImGuiCol_Header;
						ImGui::TablePushBackgroundChannel();
						ImGui::GetWindowDrawList()->AddRectFilled({Table->WorkRect.Min.x, Minimum.y}, {Table->WorkRect.Max.x, Maximum.y}, ImGui::GetColorU32(Color), 4.f * Scale);
						ImGui::TablePopBackgroundChannel();
					}

					const float CenterY = (Minimum.y + Maximum.y) * 0.5f;
					const bool bArrowDrag = Row.bHasChildren && IO.MouseClickedPos[ImGuiMouseButton_Left].x >= ArrowX && IO.MouseClickedPos[ImGuiMouseButton_Left].x < ArrowX + 18.f * Scale;
					if (!bDragging && !bRenamingRow && !bArrowDrag && Id.IsValid() && ImGui::BeginDragDropSource())
					{
						const ImGuiPayload* Payload = ImGui::GetDragDropPayload();
						if (bFolder)
						{
							ImGui::SetDragDropPayload(OutlinerFolderPayload, &Id, sizeof(Id), ImGuiCond_Once);
						}
						else if (!Payload || !Payload->IsDataType(OutlinerPayload))
						{
							const auto Request = MakeOutlinerReparentRequest(Objects, Selection, Row.ObjectIndex, std::nullopt);
							if (Request)
							{
								ImGui::SetDragDropPayload(OutlinerPayload, Request->Objects.data(), Request->Objects.size() * sizeof(FObjectId), ImGuiCond_Once);
							}

							Payload = ImGui::GetDragDropPayload();
						}

						ImGui::TextUnformatted(Label.c_str());
						if (!bFolder && Payload && static_cast<std::size_t>(Payload->DataSize) > sizeof(FObjectId))
						{
							ImGui::TextDisabled("%d objects", static_cast<int>(static_cast<std::size_t>(Payload->DataSize) / sizeof(FObjectId)));
						}
						ImGui::TextDisabled("%s", bFolder ? "Drop on a folder to nest, or empty space for root" : "Drop on an entity to parent, or a folder to organize");

						ImGui::EndDragDropSource();
					}

					if (!bDragging && !bRenamingRow && Id.IsValid() && ImGui::BeginDragDropTarget())
					{
						if (bFolder)
						{
							AcceptOutlinerFolderDrop(State, Id);
						}
						else
						{
							AcceptOutlinerDrop(State, Id);
						}
						ImGui::EndDragDropTarget();
					}

					if (!bDragging && !bRenamingRow && ImGui::BeginPopupContextItem("##ObjectActions"))
					{
						if (bFolder)
						{
							State.SelectedFolder = Id;
							Selection.Select(-1);
							if (ToolUIMenuItem("New Folder", EToolUIMenuIcon::ContentBrowser, nullptr, "Ctrl+Shift+N"))
							{
								RequestOutlinerFolder(State, Selection, Objects, Id, false);
							}

							if (ToolUIMenuItem("Rename", EToolUIMenuIcon::Details, nullptr, "F2"))
							{
								State.bRenameRequested = true;
							}

							ImGui::BeginDisabled(!Folder->Parent.IsValid());
							if (ToolUIMenuItem("Move to root", EToolUIMenuIcon::Outliner))
							{
								State.FolderRequest = FOutlinerFolderRequest{.Action = EOutlinerFolderAction::MoveFolder, .Folder = Id};
							}

							ImGui::EndDisabled();
							ImGui::Separator();
							if (ToolUIMenuItem("Delete Folder", EToolUIMenuIcon::Delete, nullptr, "Delete"))
							{
								State.FolderRequest = FOutlinerFolderRequest{.Action = EOutlinerFolderAction::Delete, .Folder = Id};
							}
						}
						else
						{
							if (!Selection.Contains(Row.ObjectIndex))
							{
								Selection.Select(Row.ObjectIndex);
							}

							State.SelectedFolder = {};
							if (ToolUIMenuItem("New Folder from Selection", EToolUIMenuIcon::ContentBrowser))
							{
								RequestOutlinerFolder(State, Selection, Objects, std::nullopt, true);
							}

							if (ToolUIMenuItem("Move to root", EToolUIMenuIcon::Outliner))
							{
								const auto Request = MakeOutlinerReparentRequest(Objects, Selection, Row.ObjectIndex, std::nullopt);
								if (Request)
								{
									State.FolderRequest = FOutlinerFolderRequest{.Action = EOutlinerFolderAction::MoveEntities, .Objects = Request->Objects};
								}
							}

							ImGui::BeginDisabled(!Object->Parent.has_value() && Selection.Indices.size() <= 1);
							if (ToolUIMenuItem("Unparent", EToolUIMenuIcon::Entity))
							{
								State.ReparentRequest = MakeOutlinerReparentRequest(Objects, Selection, Row.ObjectIndex, std::nullopt);
							}

							ImGui::EndDisabled();
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
						ImGui::GetWindowDrawList()->AddText({LabelX, CenterY - ImGui::GetFontSize() * 0.5f}, ImGui::GetColorU32(ImGuiCol_Text), Label.data(), Label.data() + Label.size());
					}
					const ImVec2 Top{IconX + 6.f * Scale, CenterY - 7.f * Scale};
					const ImVec2 Left{IconX, CenterY - 4.f * Scale};
					const ImVec2 Right{IconX + 12.f * Scale, Left.y};
					const ImVec2 Center{Top.x, CenterY - Scale};
					const ImVec2 Bottom{Top.x, CenterY + 7.f * Scale};
					const ImVec2 Outline[]{Top, Right, {Right.x, CenterY + 4.f * Scale}, Bottom, {Left.x, CenterY + 4.f * Scale}, Left};
					ImDrawList* const Draw = ImGui::GetWindowDrawList();
					const ImU32 IconColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
					if (bFolder)
					{
						ToolUIIcon(EToolUIMenuIcon::ContentBrowser, Top.x, CenterY, Scale * 0.8f);
					}
					else if (Object->Mesh.IsValid())
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
					ImGui::TextDisabled("%s", bFolder ? "Folder" : Object->Mesh.IsValid() ? "Static Mesh"
					                                                                      : "Entity");
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
			State.SelectedFolder = {};
			RefreshSelectedMask();
		}

		if (!bDragging && !bRowHovered && ImGui::GetIO().MousePos.y >= RowsTop)
		{
			const ImGuiWindow* const Window = ImGui::GetCurrentWindow();
			const ImRect Background{{Window->InnerRect.Min.x, std::max(RowsTop, Window->InnerRect.Min.y)}, Window->InnerRect.Max};
			if (ImGui::BeginDragDropTargetCustom(Background, ImGui::GetID("##RootDrop")))
			{
				AcceptOutlinerFolderDrop(State, std::nullopt);
				ImGui::EndDragDropTarget();
			}
		}

		if (!bDragging && ImGui::BeginPopupContextWindow("##OutlinerActions", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
		{
			if (ToolUIMenuItem("New Folder", EToolUIMenuIcon::ContentBrowser, nullptr, "Ctrl+Shift+N"))
			{
				RequestOutlinerFolder(State, Selection, Objects, std::nullopt, false);
			}

			ImGui::BeginDisabled(Selection.Indices.empty());
			if (ToolUIMenuItem("New Folder from Selection", EToolUIMenuIcon::ContentBrowser))
			{
				RequestOutlinerFolder(State, Selection, Objects, std::nullopt, true);
			}

			ImGui::EndDisabled();
			ImGui::EndPopup();
		}
	}

	ImGui::EndChild();
	ImGui::Separator();
	std::string Footer = State.Search.IsActive() ? std::format("{} of {} objects", VisibleIndices.size(), Objects.size()) : std::format("{} objects", Objects.size());
	if (!Folders.empty())
	{
		Footer += std::format(" | {} folders", Folders.size());
	}

	const std::string SelectionCount = std::format(" ({} selected)", Selection.Indices.size());
	const bool bShowSelectionCount = ImGui::CalcTextSize((Footer + SelectionCount).c_str()).x <= ImGui::GetContentRegionAvail().x;
	if (bShowSelectionCount)
	{
		Footer += SelectionCount;
	}

	ImGui::TextDisabled("%s", Footer.c_str());
	if (!bShowSelectionCount && ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("%d objects selected", static_cast<int>(Selection.Indices.size()));
	}

	ImGui::EndDisabled();
	return bFocusRequested;
}

bool DrawPreviewOutlinerPanel(FToolUIContext& ToolUI, bool& bOpen, FPreviewSelection& Selection, const std::span<const FPreviewObject> Objects, const bool bDragging, FOutlinerPanelState& State, const std::span<const FLevelFolder> Folders)
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
	const bool bFocusRequested = DrawPreviewOutlinerContents(Selection, Objects, bDragging, State, Folders);
	ToolUI.EndPanel();
	return bFocusRequested;
}
}
