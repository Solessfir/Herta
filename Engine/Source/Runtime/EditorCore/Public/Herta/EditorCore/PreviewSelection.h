#pragma once

#include "Herta/EditorCore/ViewportCamera.h"

#include <utility>
#include <vector>

namespace Herta
{
// The preview cube occupies [-1, 1] on each local axis; its model must be an invertible orthogonal TRS transform.
[[nodiscard]] bool HitTestPreviewCube(const FViewportPickingRay& Ray, const FMatrix4& Model);
[[nodiscard]] std::vector<std::pair<FVector3, FVector3>> GetPreviewCubeSilhouette(const FVector3& CameraPosition, const FMatrix4& Model);
}
