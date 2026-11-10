#pragma once

#include "Herta/Level/LevelSerialization.h"
#include "Herta/Math/Matrix.h"
#include "Herta/Navigation/NavigationData.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
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

// A mesh's bounds in model space, or empty when it is not known; an unknown mesh collides as the cube a loading mesh shows, as in physics.
using FNavigationMeshBounds = std::function<std::optional<FNavigationBox>(const FAssetId& Mesh)>;

// The collision of the entities' static bodies as triangles, fitted to their meshes' bounds the way physics fits them and placed by World,
// which is parallel to Entities. Movers, triggers, soft bodies, and destructibles move or break, so they are left to runtime obstacles.
[[nodiscard]] FNavigationGeometry BuildLevelNavigationGeometry(std::span<const FLevelEntity> Entities, std::span<const FMatrix4> World, const FNavigationMeshBounds& Bounds);
// The world-space boxes around the entities' Navigation Bounds components.
[[nodiscard]] std::vector<FNavigationBox> GetLevelNavigationBoxes(std::span<const FLevelEntity> Entities, std::span<const FMatrix4> World);
// The patch key of a region's tiles.
[[nodiscard]] FHash128 GetRegionNavigationKey(const FObjectId& Region) noexcept;

// A level's navigation file, built in one piece from the level's static bodies and its regions' scenery at their offsets, within every
// Navigation Bounds of either. Tiles whose center lies in a region's bounds become that region's patch and stream with it; the rest stay
// with the level. Regions is parallel to Level.Regions; a region whose level could not load adds nothing. Empty without any bounds.
[[nodiscard]] std::expected<std::optional<FNavigationFile>, FNavigationError> BuildLevelNavigation(const FLevelDocument& Level, std::span<const std::optional<FLevelDocument>> Regions, const FNavigationMeshBounds& Bounds, const FNavigationSettings& Settings = {}, FTaskSystem* Tasks = nullptr);
}
