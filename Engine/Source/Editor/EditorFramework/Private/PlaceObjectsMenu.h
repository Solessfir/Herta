#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace Herta
{
class FToolUIContext;

enum class EPlaceObjectType : std::uint8_t
{
	EmptyEntity,
	Cube,
	DirectionalLight,
	SkyLight,
	PointLight,
	SpotLight,
	RectLight,
	SkyAtmosphere,
	HeightFog,
};

struct FPlaceObjectsMenuState
{
	void Reset() noexcept;
	bool HasCubeMatch() const;
	bool HasEmptyEntityMatch() const;
	bool HasMatch(EPlaceObjectType Type) const;
	bool HasAnyMatch() const;
	void SetResultFocus(bool bFocused);
	void SelectFirstResult();

	std::array<char, 96> Search{};
	EPlaceObjectType SelectedResult = EPlaceObjectType::EmptyEntity;
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
std::optional<EPlaceObjectType> DrawPlaceObjectsMenu(FToolUIContext& ToolUI, FPlaceObjectsMenuState& State, bool bOpenRequested);
}
