#include "PlaceObjectsMenu.h"

#include "Herta/Assets/AssetSearch.h"
#include "Herta/ToolUI/ToolUI.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <limits>
#include <string_view>

namespace Herta
{
namespace
{
constexpr const char* PopupName = "Add###PlaceObjectsMenu";
constexpr const char* SearchLabel = "##PlaceObjectsSearch";
constexpr std::array Candidates{std::string_view("Empty Entity"), std::string_view("Cube")};

std::optional<std::vector<FAssetSearchMatch>> GetMatches(const FPlaceObjectsMenuState& State)
{
	std::string_view Query(State.Search.data());
	const std::size_t First = Query.find_first_not_of(" \t\r\n");
	if (First != std::string_view::npos)
	{
		Query = Query.substr(First, Query.find_last_not_of(" \t\r\n") - First + 1);
	}
	else
	{
		Query = {};
	}

	return SearchAssets(Candidates, Query);
}

bool ContainsResult(const std::optional<std::vector<FAssetSearchMatch>>& Matches, const EPlaceObjectType Type)
{
	const std::size_t Index = Type == EPlaceObjectType::EmptyEntity ? 0 : 1;
	return Matches && std::ranges::any_of(*Matches, [Index](const FAssetSearchMatch& Match)
	{
		return Match.Index == Index;
	});
}

EPlaceObjectType GetResultType(const std::size_t Index)
{
	return Index == 0 ? EPlaceObjectType::EmptyEntity : EPlaceObjectType::Cube;
}

bool DrawPlaceObjectResult(const std::string_view Label, const EToolUIMenuIcon Icon, const bool bSelected)
{
	ImDrawList* const DrawList = ImGui::GetWindowDrawList();
	ImDrawListSplitter ResultLayers;
	ResultLayers.Split(DrawList, 2);
	ResultLayers.SetCurrentChannel(DrawList, 1);
	const bool bPressed = ToolUIMenuItem(Label, Icon);
	const ImVec2 Minimum = ImGui::GetItemRectMin();
	const ImVec2 Maximum = ImGui::GetItemRectMax();
	ResultLayers.SetCurrentChannel(DrawList, 0);
	if (bSelected)
	{
		DrawList->AddRectFilled(Minimum, Maximum, ImGui::GetColorU32(ImGuiCol_Header), ImGui::GetStyle().MenuItemRounding);
	}
	ResultLayers.Merge(DrawList);
	return bPressed;
}
}

void FPlaceObjectsMenuState::Reset() noexcept
{
	Search.fill('\0');
	SelectedResult = EPlaceObjectType::EmptyEntity;
	bResultsFocused = false;
}

bool FPlaceObjectsMenuState::HasCubeMatch() const
{
	return ContainsResult(GetMatches(*this), EPlaceObjectType::Cube);
}

bool FPlaceObjectsMenuState::HasEmptyEntityMatch() const
{
	return ContainsResult(GetMatches(*this), EPlaceObjectType::EmptyEntity);
}

bool FPlaceObjectsMenuState::HasAnyMatch() const
{
	const auto Matches = GetMatches(*this);
	return Matches && !Matches->empty();
}

void FPlaceObjectsMenuState::SetResultFocus(const bool bFocused)
{
	const auto Matches = GetMatches(*this);
	bResultsFocused = bFocused && Matches && !Matches->empty();
	if (bResultsFocused && !ContainsResult(Matches, SelectedResult))
	{
		SelectFirstResult();
	}
}

void FPlaceObjectsMenuState::SelectFirstResult()
{
	if (const auto Matches = GetMatches(*this); Matches && !Matches->empty())
	{
		SelectedResult = GetResultType(Matches->front().Index);
	}
	else
	{
		bResultsFocused = false;
	}
}

FPlaceObjectsMenuNavigation UpdatePlaceObjectsMenuNavigation(FPlaceObjectsMenuState& State)
{
	const ImGuiID NavigationOwner = ImGui::GetID("##PlaceObjectsNavigation");
	for (const ImGuiKey Key : {ImGuiKey_Tab, ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Escape})
	{
		ImGui::SetKeyOwner(Key, NavigationOwner, ImGuiInputFlags_LockThisFrame);
	}

	const bool bTab = ImGui::IsKeyPressed(ImGuiKey_Tab, ImGuiInputFlags_None, NavigationOwner);
	const bool bUp = ImGui::IsKeyPressed(ImGuiKey_UpArrow, ImGuiInputFlags_Repeat, NavigationOwner);
	const bool bDown = ImGui::IsKeyPressed(ImGuiKey_DownArrow, ImGuiInputFlags_Repeat, NavigationOwner);
	const bool bArrow = bUp || bDown;
	FPlaceObjectsMenuNavigation Navigation{
	    .bConfirm = ImGui::IsKeyPressed(ImGuiKey_Enter, ImGuiInputFlags_None, NavigationOwner) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, ImGuiInputFlags_None, NavigationOwner),
	    .bCancel = ImGui::IsKeyPressed(ImGuiKey_Escape, ImGuiInputFlags_None, NavigationOwner),
	};

	if (bTab || bArrow)
	{
		ImGui::NavMoveRequestCancel();
		if (bArrow && !State.bResultsFocused)
		{
			State.SelectFirstResult();
		}
		else if (bArrow)
		{
			const auto Matches = GetMatches(State);
			if (Matches && !Matches->empty())
			{
				const auto Current = std::ranges::find_if(*Matches, [&State](const FAssetSearchMatch& Match)
				{
					return GetResultType(Match.Index) == State.SelectedResult;
				});
				const std::size_t CurrentIndex = Current == Matches->end() ? 0 : static_cast<std::size_t>(Current - Matches->begin());
				const std::size_t NextIndex = bDown ? (CurrentIndex + 1) % Matches->size() : (CurrentIndex + Matches->size() - 1) % Matches->size();
				State.SelectedResult = GetResultType((*Matches)[NextIndex].Index);
			}
		}

		State.SetResultFocus(bArrow || !State.bResultsFocused);
		Navigation.bFocusResult = State.bResultsFocused;
		Navigation.bFocusSearch = !State.bResultsFocused;
		ImGui::ClearActiveID();
	}
	else if (State.bResultsFocused && !ImGui::GetIO().InputQueueCharacters.empty())
	{
		State.bResultsFocused = false;
		State.SelectFirstResult();
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

std::optional<EPlaceObjectType> DrawPlaceObjectsMenu(FToolUIContext& ToolUI, FPlaceObjectsMenuState& State, const bool bOpenRequested)
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
		return std::nullopt;
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
		if (bSearchChanged)
		{
			State.SelectFirstResult();
		}
	}

	const auto Matches = GetMatches(State);
	std::optional<EPlaceObjectType> Chosen;
	if (Matches && !Matches->empty())
	{
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.f * Scale, 7.f * Scale});
		if (ContainsResult(Matches, EPlaceObjectType::EmptyEntity))
		{
			ImGui::TextDisabled("Entity");
			const bool bPressed = DrawPlaceObjectResult("Empty Entity", EToolUIMenuIcon::Entity, State.SelectedResult == EPlaceObjectType::EmptyEntity);
			if (Navigation.bFocusResult && State.SelectedResult == EPlaceObjectType::EmptyEntity)
			{
				ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
			}

			if (bPressed)
			{
				Chosen = EPlaceObjectType::EmptyEntity;
			}
		}

		if (ContainsResult(Matches, EPlaceObjectType::Cube))
		{
			ImGui::TextDisabled("Basic shapes");
			const bool bPressed = DrawPlaceObjectResult("Cube", EToolUIMenuIcon::Cube, State.SelectedResult == EPlaceObjectType::Cube);
			if (Navigation.bFocusResult && State.SelectedResult == EPlaceObjectType::Cube)
			{
				ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
			}

			if (bPressed)
			{
				Chosen = EPlaceObjectType::Cube;
			}
		}

		ImGui::PopStyleVar();
		if (Navigation.bConfirm)
		{
			Chosen = State.bResultsFocused && ContainsResult(Matches, State.SelectedResult) ? State.SelectedResult : GetResultType(Matches->front().Index);
		}
	}
	else
	{
		State.bResultsFocused = false;
		ImGui::TextDisabled("No matching items");
	}

	ImGui::PopItemFlag();
	ImGui::Separator();
	ImGui::TextDisabled("Tab to switch focus");
	if (Navigation.bCancel || Chosen)
	{
		ImGui::ClearActiveID();
		ImGui::CloseCurrentPopup();
	}

	ImGui::PopStyleColor();
	ImGui::EndPopup();
	ImGui::PopStyleVar(2);
	return Navigation.bCancel ? std::nullopt : Chosen;
}
}
