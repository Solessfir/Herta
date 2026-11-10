#pragma once

#include "Herta/Navigation/NavigationData.h"

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace Herta
{
class FTaskSystem;

// Triangles navigation is built on, in world meters. A triangle is walkable when its front face, counter-clockwise seen from outside, points
// up within the agent's slope.
struct FNavigationGeometry
{
	std::vector<FVector3> Vertices{};
	std::vector<std::uint32_t> Indices{};
};

[[nodiscard]] FHash128 HashNavigationSource(const FNavigationGeometry& Geometry, std::span<const FNavigationBox> Boxes, const FNavigationSettings& Settings);
// Builds every tile the boxes overlap with Recast, on Tasks' workers when given; the result is the same either way.
[[nodiscard]] std::expected<FNavigationData, FNavigationError> BuildNavigationData(const FNavigationGeometry& Geometry, std::span<const FNavigationBox> Boxes, const FNavigationSettings& Settings = {}, FTaskSystem* Tasks = nullptr);
}
