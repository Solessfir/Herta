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
namespace
{
// Calls Visit with each line's keyword and value, and whether the line falls in the named level's section.
template <typename FVisitor>
void VisitOutlinerFlagLines(const std::string_view Text, const std::string_view Level, FVisitor&& Visit)
{
	bool bInLevel = false;
	for (std::size_t Start = 0; Start < Text.size();)
	{
		const std::size_t End = std::min(Text.find('\n', Start), Text.size());
		std::string_view Line = Text.substr(Start, End - Start);
		Start = End + 1;
		if (Line.ends_with('\r'))
		{
			Line.remove_suffix(1);
		}

		const std::size_t Space = Line.find(' ');
		if (Line.empty() || Space == std::string_view::npos)
		{
			continue;
		}

		const std::string_view Keyword = Line.substr(0, Space);
		const std::string_view Value = Line.substr(Space + 1);
		if (Keyword == "level")
		{
			bInLevel = Value == Level;
		}

		Visit(Keyword, Value, Line, bInLevel);
	}
}
}

FOutlinerFlags ParseOutlinerFlags(const std::string_view Text, const std::string_view Level)
{
	FOutlinerFlags Flags;
	VisitOutlinerFlagLines(Text, Level, [&Flags](const std::string_view Keyword, const std::string_view Value, std::string_view, const bool bInLevel)
	{
		const auto Id = bInLevel && (Keyword == "hidden" || Keyword == "locked") ? FObjectId::Parse(Value) : std::nullopt;
		if (Id)
		{
			(Keyword == "hidden" ? Flags.Hidden : Flags.Locked).insert(*Id);
		}
	});

	return Flags;
}

std::string ReplaceOutlinerFlags(const std::string_view Text, const std::string_view Level, const FOutlinerFlags& Flags)
{
	std::string Result;
	VisitOutlinerFlagLines(Text, Level, [&Result](std::string_view, std::string_view, const std::string_view Line, const bool bInLevel)
	{
		if (!bInLevel)
		{
			Result.append(Line).push_back('\n');
		}
	});

	if (!Flags.Hidden.empty() || !Flags.Locked.empty())
	{
		Result.append("level ").append(Level).push_back('\n');
		for (const FObjectId Id : Flags.Hidden)
		{
			Result.append("hidden ").append(Id.ToString()).push_back('\n');
		}

		for (const FObjectId Id : Flags.Locked)
		{
			Result.append("locked ").append(Id.ToString()).push_back('\n');
		}
	}

	return Result;
}

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
			const auto& ParentId = Objects[Index].Parent;
			const auto Parent = ParentId ? Indices.find(*ParentId) : Indices.end();
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

	const bool bSearching = State.IsFiltering();
	std::vector<bool> Matches;
	if (bSearching)
	{
		Matches.resize(NodeCount);
		std::string SearchText;
		for (std::size_t Index = 0; Index < Objects.size(); ++Index)
		{
			SearchText.assign(Objects[Index].Label);
			SearchText += ' ';
			SearchText.append(GetPreviewObjectTypeName(Objects[Index]));
			Matches[Index] = State.Search.PassFilter(SearchText.c_str()) && (State.CategoryFilter < 0 || static_cast<int>(Objects[Index].Category) == State.CategoryFilter);
		}

		// Under a type chip a folder shows only for the entities it holds.
		for (std::size_t Index = 0; Index < Folders.size(); ++Index)
		{
			SearchText = Folders[Index].Name + " Folder";
			Matches[Objects.size() + Index] = State.CategoryFilter < 0 && State.Search.PassFilter(SearchText.c_str());
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

bool ExpandOutlinerAncestors(FOutlinerPanelState& State, const std::span<const FPreviewObject> Objects, const std::span<const FLevelFolder> Folders, const int ObjectIndex)
{
	if (ObjectIndex < 0 || static_cast<std::size_t>(ObjectIndex) >= Objects.size() || static_cast<std::size_t>(ObjectIndex) >= State.ParentIndices.size())
	{
		return false;
	}

	bool bExpanded = false;
	int Parent = State.ParentIndices[static_cast<std::size_t>(ObjectIndex)];
	for (std::size_t Steps = 0; Parent >= 0 && Steps < State.ParentIndices.size(); ++Steps)
	{
		const auto Index = static_cast<std::size_t>(Parent);
		bExpanded |= Index < Objects.size() ? State.CollapsedObjects.erase(Objects[Index].Id) > 0 : State.CollapsedFolders.erase(Folders[Index - Objects.size()].Id) > 0;
		Parent = State.ParentIndices[Index];
	}

	return bExpanded;
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
	// A viewport pick keeps its drag interaction until release, so the request waits for the panel to accept input.
	const bool bReveal = State.bRevealSelection && !bDragging;
	State.bRevealSelection &= bDragging;
	if (bReveal && ExpandOutlinerAncestors(State, Objects, Folders, Selection.Active))
	{
		BuildOutlinerVisibleRows(State, Objects, Folders);
	}

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

	if (ImGui::BeginChild("##OutlinerEntries", {0.f, 0.f}))
	{
		bool bRowHovered = false;
		float RowsTop = ImGui::GetCursorScreenPos().y;
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.f * Scale, 2.f * Scale});
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {8.f * Scale, 4.f * Scale});
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {6.f * Scale, 8.f * Scale});
		ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, {0, 0, 0, 0});
		ImGui::PushStyleColor(ImGuiCol_TableBorderLight, {1, 1, 1, 0.08f});
		// The folders holding the top row pin under the header as frozen rows. Frozen rows push the scrolled rows down, so the scroll grows by
		// their height to cancel that: the list holds still, and a pinned folder covers the row it now sits over. The top row comes from the
		// scroll without them.
		std::vector<int> StickyRows;
		const ImGuiTable* const Previous = ImGui::TableFindByID(ImGui::GetID("##OutlinerObjects"));
		if (Previous && Previous->InnerWindow && State.RowHeight > 0.f)
		{
			const float Scroll = Previous->InnerWindow->Scroll.y;
			const float ListScroll = std::max(0.f, Scroll - static_cast<float>(State.PinnedCount) * State.RowHeight);
			const int TopRow = static_cast<int>(ListScroll / State.RowHeight);
			if (TopRow > 0 && TopRow < static_cast<int>(State.VisibleRows.size()))
			{
				int Depth = State.VisibleRows[static_cast<std::size_t>(TopRow)].Depth;
				for (int Index = TopRow - 1; Index >= 0 && Depth > 0; --Index)
				{
					const FOutlinerRow& Row = State.VisibleRows[static_cast<std::size_t>(Index)];
					if (Row.Depth < Depth)
					{
						Depth = Row.Depth;
						if (Row.FolderIndex >= 0)
						{
							StickyRows.insert(StickyRows.begin(), Index);
						}
					}
				}
			}

			if (static_cast<int>(StickyRows.size()) != State.PinnedCount)
			{
				ImGui::SetNextWindowScroll({-1.f, ListScroll + static_cast<float>(StickyRows.size()) * State.RowHeight});
			}
		}

		State.PinnedCount = static_cast<int>(StickyRows.size());
		if (ImGui::BeginTable("##OutlinerObjects", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_ScrollY))
		{
			ImGui::TableSetupColumn("Item Label", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 105.f * ImGui::GetFontSize() / 15.f);

			ImGui::TableSetupScrollFreeze(0, 1 + static_cast<int>(StickyRows.size()));
			ImGui::PushStyleColor(ImGuiCol_Header, {0, 0, 0, 0});
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {0, 0, 0, 0});
			ImGui::PushStyleColor(ImGuiCol_HeaderActive, {0, 0, 0, 0});
			ImGui::TableHeadersRow();
			ImGui::PopStyleColor(3);
			const ImGuiTable* const Table = ImGui::GetCurrentTable();
			ImGui::TablePushBackgroundChannel();
			ImGui::GetWindowDrawList()->AddRectFilled({Table->WorkRect.Min.x, Table->RowPosY1}, {Table->WorkRect.Max.x, Table->RowPosY2}, ImGui::GetColorU32(ImVec4{1, 1, 1, 0.05f}), 4.f * Scale);
			// The selection count rides at the right of the label header, which stays pinned while the list scrolls.
			if (!Selection.Indices.empty())
			{
				const std::string Selected = std::format("{} selected", Selection.Indices.size());
				const float Right = Table->Columns[0].WorkMaxX - 4.f * Scale;
				const float Left = Right - ImGui::CalcTextSize(Selected.c_str()).x;
				if (Left > Table->Columns[0].WorkMinX + ImGui::CalcTextSize("Item Label").x + 12.f * Scale)
				{
					ImGui::GetWindowDrawList()->AddText({Left, (Table->RowPosY1 + Table->RowPosY2 - ImGui::GetFontSize()) * 0.5f}, ImGui::GetColorU32(ImGuiCol_TextDisabled), Selected.c_str());
				}
			}

			ImGui::TablePopBackgroundChannel();
			RowsTop = ImGui::GetCursorScreenPos().y;
			const auto DrawRow = [&](const int VisibleIndex)
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
				// The lock and eye sit in two fixed slots at the right of the label column and, like the arrow, are hit by position inside the row's click.
				const float ButtonSize = 20.f * Scale;
				const float LockX = Table->Columns[0].WorkMaxX - ButtonSize;
				const float EyeX = LockX - ButtonSize - 2.f * Scale;
				const bool bLockHit = !bFolder && IO.MousePos.x >= LockX && IO.MousePos.x < LockX + ButtonSize;
				const bool bEyeHit = !bFolder && IO.MousePos.x >= EyeX && IO.MousePos.x < EyeX + ButtonSize;
				auto& Collapsed = bFolder ? State.CollapsedFolders : State.CollapsedObjects;
				const bool bExpanded = State.IsFiltering() || !Collapsed.contains(Id);
				if (ImGui::Selectable("##Object", bSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) && !bRenamingRow)
				{
					if (bLockHit || bEyeHit)
					{
						std::set<FObjectId>& Flags = bLockHit ? State.LockedObjects : State.HiddenObjects;
						if (!Flags.erase(Id))
						{
							Flags.insert(Id);
						}
					}
					else if (bArrowHit || (bFolder && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)))
					{
						if (!State.IsFiltering())
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
				if (bReveal && !bFolder && Row.ObjectIndex == Selection.Active)
				{
					ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleCenterY);
				}

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

				// An entity's eye and lock show while set or while the pointer is on its row.
				const bool bHiddenObject = !bFolder && State.HiddenObjects.contains(Id);
				const bool bLockedObject = !bFolder && State.LockedObjects.contains(Id);
				const bool bShowButtons = !bFolder && !bRenamingRow;
				const bool bShowEye = bShowButtons && (bCurrentRowHovered || bHiddenObject);
				const bool bShowLock = bShowButtons && (bCurrentRowHovered || bLockedObject);
				const int RowButtons = bShowEye ? 2 : bShowLock ? 1
				                                                : 0;
				if (bRenamingRow)
				{
					bRowHovered |= DrawOutlinerRenameField(State, {LabelX, CenterY - ImGui::GetFrameHeight() * 0.5f}, Table->Columns[0].WorkMaxX - LabelX, bStartRename);
				}
				else
				{
					// Drawn text does not widen the column's measured contents, so the table would drop its clip rect; the ellipsis keeps a long name out of the Type column.
					const float LabelMaxX = Table->Columns[0].WorkMaxX - static_cast<float>(RowButtons) * 22.f * Scale;
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(bHiddenObject ? ImGuiCol_TextDisabled : ImGuiCol_Text));
					ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), {LabelX, CenterY - ImGui::GetFontSize() * 0.5f}, {LabelMaxX, CenterY + ImGui::GetFontSize() * 0.5f}, LabelMaxX, Label.data(), Label.data() + Label.size(), nullptr);
					ImGui::PopStyleColor();
				}
				const auto Kind = bFolder ? EPreviewObjectKind::Entity : Object->Kind;
				const EToolUIMenuIcon Icon = bFolder                                                                                 ? EToolUIMenuIcon::Folder
				                             : Kind == EPreviewObjectKind::SkyAtmosphere                                             ? EToolUIMenuIcon::SkyAtmosphere
				                             : Kind == EPreviewObjectKind::HeightFog                                                 ? EToolUIMenuIcon::Fog
				                             : Kind == EPreviewObjectKind::SoftBody || Kind == EPreviewObjectKind::Trigger           ? EToolUIMenuIcon::Physics
				                             : Kind >= EPreviewObjectKind::DirectionalLight && Kind <= EPreviewObjectKind::RectLight ? EToolUIMenuIcon::Light
				                             : Object->Mesh.IsValid()                                                                ? EToolUIMenuIcon::Cube
				                                                                                                                     : EToolUIMenuIcon::Entity;
				const FToolUIColor IconColor = bFolder ? ToolUITheme::TypeOther : GetPreviewCategoryColor(Object->Category);
				ToolUIIcon(Icon, IconX + 6.f * Scale, CenterY, Scale * 0.8f, IM_COL32(IconColor.Red, IconColor.Green, IconColor.Blue, bHiddenObject ? 110 : 255));
				const auto DrawRowButton = [&](const float X, const EToolUIMenuIcon ButtonIcon, const bool bSet, const bool bHit, const char* const Tip)
				{
					const bool bButtonHovered = bCurrentRowHovered && bHit;
					if (bButtonHovered)
					{
						ImGui::GetWindowDrawList()->AddRectFilled({X, CenterY - ButtonSize * 0.5f}, {X + ButtonSize, CenterY + ButtonSize * 0.5f}, ImGui::GetColorU32(ImGuiCol_HeaderHovered), 4.f * Scale);
						ImGui::SetTooltip("%s", Tip);
					}

					ToolUIIcon(ButtonIcon, X + ButtonSize * 0.5f, CenterY, Scale * 0.75f, ImGui::GetColorU32(bSet || bButtonHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled));
				};

				if (bShowLock)
				{
					DrawRowButton(LockX, bLockedObject ? EToolUIMenuIcon::LockClosed : EToolUIMenuIcon::LockOpen, bLockedObject, bLockHit, bLockedObject ? "Locked: viewport clicks pass through. Click to unlock." : "Lock, so viewport clicks pass through");
				}

				if (bShowEye)
				{
					DrawRowButton(EyeX, bHiddenObject ? EToolUIMenuIcon::Hidden : EToolUIMenuIcon::Visible, bHiddenObject, bEyeHit, bHiddenObject ? "Hidden in the editor viewport. Click to show." : "Hide in the editor viewport");
				}

				ImGui::TableSetColumnIndex(1);
				const std::string_view TypeName = bFolder ? "Folder" : GetPreviewObjectTypeName(*Object);
				ImGui::TextDisabled("%.*s", static_cast<int>(TypeName.size()), TypeName.data());
				ImGui::PopStyleVar(2);
				ImGui::PopID();
			};

			// The pinned folders draw first, as the frozen rows below the header, tinted like it.
			for (const int StickyRow : StickyRows)
			{
				ImGui::PushID("##Sticky");
				DrawRow(StickyRow);
				ImGui::PopID();
				// The row's own bounds, as a selectable reaches past them into its neighbors.
				ImDrawList* const Draw = ImGui::GetWindowDrawList();
				ImGui::TablePushBackgroundChannel();
				Draw->AddRectFilled({Table->WorkRect.Min.x, Table->RowPosY1}, {Table->WorkRect.Max.x, Table->RowPosY2}, ImGui::GetColorU32(ImVec4{1, 1, 1, 0.05f}));
				if (StickyRow == StickyRows.back())
				{
					Draw->AddLine({Table->WorkRect.Min.x, Table->RowPosY2 - 1.f}, {Table->WorkRect.Max.x, Table->RowPosY2 - 1.f}, ImGui::GetColorU32(ImGuiCol_Border));
				}

				ImGui::TablePopBackgroundChannel();
			}

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
					DrawRow(VisibleIndex);
				}
			}

			// Pinned rows shift the measurement by a fraction of a pixel, which would move the top row across a boundary and flicker the
			// pin, so the height is taken only while nothing is pinned.
			if (StickyRows.empty() || State.RowHeight <= 0.f)
			{
				State.RowHeight = std::max(Clipper.ItemsHeight, 0.f);
			}

			ImGui::EndTable();
		}

		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar(3);
		// The rows scroll in the table's own child window.
		const bool bEntriesHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
		if (!bDragging && bEntriesHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetIO().MousePos.y >= RowsTop && !bRowHovered)
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

		if (!bDragging && bEntriesHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !ImGui::IsAnyItemHovered())
		{
			ImGui::OpenPopup("##OutlinerActions");
		}

		if (!bDragging && ImGui::BeginPopup("##OutlinerActions"))
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
	ImGui::EndDisabled();
	return bFocusRequested;
}

bool DrawPreviewOutlinerPanel(FToolUIContext& ToolUI, bool& bOpen, FPreviewSelection& Selection, const std::span<const FPreviewObject> Objects, const bool bDragging, FOutlinerPanelState& State, const std::span<const FLevelFolder> Folders)
{
	State.bRenameCommitted = false;
	ToolUI.SetPanelBadge("Outliner", std::to_string(Objects.size()));
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

	// Type chips narrow the list; the selection count sits at the row's right end when it fits.
	const float Scale = ImGui::GetFontSize() / 15.f;
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4.f * Scale, ImGui::GetStyle().ItemSpacing.y});
	if (ToolUIChip("All##OutlinerAll", State.CategoryFilter < 0))
	{
		State.CategoryFilter = -1;
	}

	constexpr std::array<std::pair<EPreviewObjectCategory, const char*>, 4> Chips{{
	    {EPreviewObjectCategory::Mesh, "Mesh"},
	    {EPreviewObjectCategory::Light, "Light"},
	    {EPreviewObjectCategory::Character, "Character"},
	    {EPreviewObjectCategory::Physics, "Physics"},
	}};

	for (const auto& [Category, Label] : Chips)
	{
		const FToolUIColor Color = GetPreviewCategoryColor(Category);
		const float ChipWidth = ImGui::CalcTextSize(Label).x + 32.f * Scale;
		if (ImGui::GetItemRectMax().x + 4.f * Scale + ChipWidth <= ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x)
		{
			ImGui::SameLine();
		}

		if (ToolUIChip(Label, State.CategoryFilter == static_cast<int>(Category), &Color))
		{
			State.CategoryFilter = State.CategoryFilter == static_cast<int>(Category) ? -1 : static_cast<int>(Category);
		}
	}

	ImGui::PopStyleVar();
	ImGui::EndDisabled();
	const bool bFocusRequested = DrawPreviewOutlinerContents(Selection, Objects, bDragging, State, Folders);
	ToolUI.EndPanel();
	return bFocusRequested;
}
}
