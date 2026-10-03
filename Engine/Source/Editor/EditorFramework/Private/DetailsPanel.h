#pragma once

#include <array>
#include <cstddef>
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
	bool bScaleLocked = false;
	std::array<char, 256> RenameBuffer{};
	bool bRenameRequested = false;
	bool bRenaming = false;
};

struct FDetailsMeshField
{
	std::span<const std::string> Options;
	// Index of the current mesh in Options, or -1.
	int Selected = -1;
	std::string_view Status;
	bool bError = false;
};

struct FDetailsMeshResult
{
	// The option chosen this frame, or -1.
	int Chosen = -1;
	bool bOptionsOpened = false;
};

FDetailsMeshResult DrawPreviewDetailsPanel(FToolUIContext& ToolUI, bool& bOpen, bool bSelected, bool bDragging, Im3d::Vec3& Translation, Im3d::Mat3& Rotation, Im3d::Vec3& Scale, FDetailsPanelState& State, std::string& ObjectLabel, std::size_t SelectedCount = 1, const FDetailsMeshField* Mesh = nullptr);
}
