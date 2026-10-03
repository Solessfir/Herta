#pragma once

#include <array>

namespace Herta
{
class FToolUIContext;

struct FPlaceObjectsMenuState
{
	void Reset() noexcept;
	bool HasCubeMatch() const;
	void SetResultFocus(bool bFocused);

	std::array<char, 96> Search{};
	bool bResultsFocused = false;
};

struct FPlaceObjectsMenuNavigation
{
	bool bFocusSearch = false;
	bool bFocusResult = false;
	bool bConfirm = false;
	bool bCancel = false;
};

FPlaceObjectsMenuNavigation UpdatePlaceObjectsMenuNavigation(FPlaceObjectsMenuState& State);
void OpenPlaceObjectsMenu(FPlaceObjectsMenuState& State);
bool DrawPlaceObjectsMenu(FToolUIContext& ToolUI, FPlaceObjectsMenuState& State, bool bOpenRequested);
}
