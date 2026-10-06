#pragma once

#include "Herta/Level/World.h"

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Im3d
{
struct Mat3;
struct Vec3;
}

namespace Herta
{
class FToolUIContext;

enum class EDetailsTransformSpace
{
	Local,
	World
};

struct FDetailsPanelState
{
	std::array<EDetailsTransformSpace, 3> Spaces{};
	std::array<char, 96> Search{};
	std::array<char, 96> MeshSearch{};
	std::array<char, 96> ComponentSearch{};
	std::array<bool, 6> BodyPropertyWasMixed{};
	int ComponentResult = 0;
	bool bAddComponentRequested = false;
	bool bScaleLocked = false;
};

struct FDetailsMeshField
{
	std::span<const std::string> Options;
	// Index of the current mesh in Options, or -1.
	int Selected = -1;
	std::string_view Status;
	bool bError = false;
};

enum class EDetailsComponentAction
{
	None,
	AddStaticMesh,
	RemoveStaticMesh,
	AddRigidBody,
	RemoveRigidBody,
};

struct FDetailsComponentField
{
	bool bAnyMesh = false;
	bool bAllMesh = false;
	bool bMixedMeshAsset = false;
	bool bAnyBody = false;
	bool bAllBody = false;
	FLevelRigidBodySettings BodySettings{};
	std::array<bool, 6> MixedBodySettings{};
	bool bAnyDynamicBody = false;
	std::optional<ELevelBodyType> BodyType{};
};

struct FDetailsMeshResult
{
	// The option chosen this frame, or -1.
	int Chosen = -1;
	bool bOptionsOpened = false;
	// Complete the transaction after propagating this frame's transform to the selection.
	bool bEditFinished = false;
	bool bEditCanceled = false;
	EDetailsComponentAction ComponentAction = EDetailsComponentAction::None;
	std::optional<ELevelBodyType> BodyTypeChosen{};
};

struct FDetailsEditCallbacks
{
	std::function<void()> Begin;
	// Flush a previous gesture before another edit starts in the same frame.
	std::function<void(bool)> Flush;
	std::function<void(float FLevelRigidBodySettings::*, float)> ApplyBodyProperty;
	std::function<float(float FLevelRigidBodySettings::*)> ReadBodyProperty;
};

FDetailsMeshResult DrawPreviewDetailsPanel(FToolUIContext& ToolUI, bool& bOpen, bool bSelected, bool bDragging, Im3d::Vec3& Translation, Im3d::Mat3& Rotation, Im3d::Vec3& Scale, FDetailsPanelState& State, const std::string& ObjectLabel, std::size_t SelectedCount = 1, const FDetailsMeshField* Mesh = nullptr, const FDetailsEditCallbacks* Edits = nullptr, FDetailsComponentField* Components = nullptr);
}
