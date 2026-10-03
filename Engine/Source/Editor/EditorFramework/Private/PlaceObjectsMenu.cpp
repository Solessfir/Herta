#include "PlaceObjectsMenu.h"

#include "Herta/Assets/AssetSearch.h"
#include "Herta/ToolUI/ToolUI.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <limits>
#include <string_view>

namespace Herta
{
namespace
{
constexpr const char* PopupName = "Add###PlaceObjectsMenu";
constexpr const char* SearchLabel = "##PlaceObjectsSearch";
}

void FPlaceObjectsMenuState::Reset() noexcept
{
	Search.fill('\0');
	bResultsFocused = false;
}

bool FPlaceObjectsMenuState::HasCubeMatch() const
{
	std::string_view Query(Search.data());
	const std::size_t First = Query.find_first_not_of(" \t\r\n");
	if (First == std::string_view::npos)
	{
		return true;
	}

	Query = Query.substr(First, Query.find_last_not_of(" \t\r\n") - First + 1);
	constexpr std::array Candidates{std::string_view("Cube")};
	const auto Matches = SearchAssets(Candidates, Query);
	return Matches && !Matches->empty();
}

void FPlaceObjectsMenuState::SetResultFocus(const bool bFocused)
{
	bResultsFocused = bFocused && HasCubeMatch();
}

FPlaceObjectsMenuNavigation UpdatePlaceObjectsMenuNavigation(FPlaceObjectsMenuState& State)
{
	const ImGuiID NavigationOwner = ImGui::GetID("##PlaceObjectsNavigation");
	for (const ImGuiKey Key : {ImGuiKey_Tab, ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Escape})
	{
		ImGui::SetKeyOwner(Key, NavigationOwner, ImGuiInputFlags_LockThisFrame);
	}

	const bool bTab = ImGui::IsKeyPressed(ImGuiKey_Tab, ImGuiInputFlags_None, NavigationOwner);
	const bool bArrow = ImGui::IsKeyPressed(ImGuiKey_UpArrow, ImGuiInputFlags_Repeat, NavigationOwner) || ImGui::IsKeyPressed(ImGuiKey_DownArrow, ImGuiInputFlags_Repeat, NavigationOwner);
	FPlaceObjectsMenuNavigation Navigation{
	    .bConfirm = ImGui::IsKeyPressed(ImGuiKey_Enter, ImGuiInputFlags_None, NavigationOwner) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, ImGuiInputFlags_None, NavigationOwner),
	    .bCancel = ImGui::IsKeyPressed(ImGuiKey_Escape, ImGuiInputFlags_None, NavigationOwner),
	};

	if (bTab || bArrow)
	{
		ImGui::NavMoveRequestCancel();
		State.SetResultFocus(bArrow || !State.bResultsFocused);
		Navigation.bFocusResult = State.bResultsFocused;
		Navigation.bFocusSearch = !State.bResultsFocused;
		ImGui::ClearActiveID();
	}
	else if (State.bResultsFocused && !ImGui::GetIO().InputQueueCharacters.empty())
	{
		State.bResultsFocused = false;
		Navigation.bFocusSearch = true;
		const ImGuiID SearchId = ImGui::GetID(SearchLabel);
		// Reuse the initialized text state so the first typed character is handled in this frame.
		if (ImGui::GetInputTextState(SearchId) != nullptr)
		{
			ImGui::SetActiveID(SearchId, ImGui::GetCurrentWindow());
			ImGui::SetFocusID(SearchId, ImGui::GetCurrentWindow());
		}
	}

	return Navigation;
}

void OpenPlaceObjectsMenu(FPlaceObjectsMenuState& State)
{
	State.Reset();
	ImGuiIO& IO = ImGui::GetIO();
	if (IO.KeyShift)
	{
		for (int Index = IO.InputQueueCharacters.Size - 1; Index >= 0; --Index)
		{
			if (IO.InputQueueCharacters[Index] == 'A' || IO.InputQueueCharacters[Index] == 'a')
			{
				IO.InputQueueCharacters.erase(IO.InputQueueCharacters.begin() + Index);
			}
		}
	}

	// Placement follows the pointer, not a previously keyboard-focused item.
	ImGui::SetNavCursorVisible(false);
	ImGui::OpenPopup(PopupName);
}

bool DrawPlaceObjectsMenu(FToolUIContext& ToolUI, FPlaceObjectsMenuState& State, const bool bOpenRequested)
{
	if (bOpenRequested)
	{
		OpenPlaceObjectsMenu(State);
	}

	const float Scale = ImGui::GetFontSize() / ToolUI.GetMetrics().BaseFontSize;
	ImGui::SetNextWindowSizeConstraints({280.f * Scale, 0.f}, {280.f * Scale, std::numeric_limits<float>::max()});
	ImGui::SetNextWindowBgAlpha(0.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12.f * Scale, 10.f * Scale});
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.f * Scale, 8.f * Scale});
	if (!ImGui::BeginPopup(PopupName, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
	{
		ImGui::PopStyleVar(2);
		return false;
	}

	ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4{0.f, 0.f, 0.f, 0.f});
	const ImVec2 Position = ImGui::GetWindowPos();
	const ImVec2 Size = ImGui::GetWindowSize();
	ToolUI.DrawGlassSurface(Position.x, Position.y, Size.x, Size.y, ToolUI.GetMetrics().PopupRounding * Scale);
	bool bFocusSearch = ImGui::IsWindowAppearing();
	if (bFocusSearch)
	{
		State.Reset();
	}

	ImGui::TextUnformatted("Add");
	ImGui::SameLine();
	ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("Shift+A").x);
	ImGui::TextDisabled("Shift+A");
	ImGui::Separator();

	const FPlaceObjectsMenuNavigation Navigation = UpdatePlaceObjectsMenuNavigation(State);
	bFocusSearch |= Navigation.bFocusSearch;
	if (bFocusSearch)
	{
		ImGui::SetKeyboardFocusHere();
	}

	ImGui::SetNextItemWidth(-1.f);
	ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop, true);
	const bool bSearchChanged = ToolUI.DrawSearchField(SearchLabel, "Search objects", State.Search.data(), State.Search.size());
	if (ImGui::IsItemActive() || bSearchChanged)
	{
		State.bResultsFocused = false;
	}

	const bool bHasCube = State.HasCubeMatch();
	bool bCubeRequested = false;
	if (bHasCube)
	{
		ImGui::TextDisabled("Basic shapes");
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.f * Scale, 7.f * Scale});
		ImDrawList* const DrawList = ImGui::GetWindowDrawList();
		ImDrawListSplitter ResultLayers;
		ResultLayers.Split(DrawList, 2);
		ResultLayers.SetCurrentChannel(DrawList, 1);
		bCubeRequested = ToolUIMenuItem("Cube", EToolUIMenuIcon::Cube);
		ResultLayers.SetCurrentChannel(DrawList, 0);
		DrawList->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32(ImGuiCol_Header), ImGui::GetStyle().MenuItemRounding);
		ResultLayers.Merge(DrawList);

		if (Navigation.bFocusResult)
		{
			ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
			ImGui::SetNavCursorVisible(true);
		}

		ImGui::PopStyleVar();
		bCubeRequested |= Navigation.bConfirm;
	}
	else
	{
		State.bResultsFocused = false;
		ImGui::TextDisabled("No matching objects");
	}

	ImGui::PopItemFlag();
	ImGui::Separator();
	ImGui::TextDisabled("Tab to switch focus");
	if (Navigation.bCancel || bCubeRequested)
	{
		ImGui::ClearActiveID();
		ImGui::CloseCurrentPopup();
	}

	ImGui::PopStyleColor();
	ImGui::EndPopup();
	ImGui::PopStyleVar(2);
	return bCubeRequested && !Navigation.bCancel;
}
}
