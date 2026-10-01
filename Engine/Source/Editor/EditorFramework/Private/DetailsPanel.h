#pragma once

#include <array>
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
	bool bScaleLocked = false;
};

void DrawPreviewDetailsPanel(FToolUIContext& ToolUI, bool& bOpen, bool bSelected, bool bDragging, Im3d::Vec3& Translation, Im3d::Mat3& Rotation, Im3d::Vec3& Scale, FDetailsPanelState& State, std::string_view ObjectLabel);
}
