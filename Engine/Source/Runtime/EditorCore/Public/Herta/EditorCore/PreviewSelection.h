#pragma once

#include "Herta/EditorCore/ViewportCamera.h"

#include <optional>

namespace Herta
{
// The preview cube occupies [-1, 1] on each local axis; its model must be an invertible orthogonal TRS transform.
[[nodiscard]] std::optional<double> HitTestPreviewCube(const FViewportPickingRay& Ray, const FMatrix4& Model);
// Conservative projected bounds, clipped to depth 0..1. The rectangle uses normalized top-left screen coordinates.
[[nodiscard]] bool IntersectsPreviewCubeSelectionRect(const FMatrix4& ViewProjection, const FMatrix4& Model, FVector2 First, FVector2 Second);
}
