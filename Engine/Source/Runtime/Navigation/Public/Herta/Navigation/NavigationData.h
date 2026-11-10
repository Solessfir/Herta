#pragma once

#include "Herta/Core/Hash.h"
#include "Herta/Math/Vector.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
struct FNavigationError
{
	std::string Message;
};

// The one agent type navigation is built for, a humanoid the size of the player, and the voxels it is built from. Tiles are square in X and
// Z on a grid anchored at the world origin, so navigation built for different levels lines up wherever they meet.
struct FNavigationSettings
{
	constexpr bool operator==(const FNavigationSettings&) const = default;

	[[nodiscard]] constexpr float GetTileSize() const noexcept;

	float CellSize = 0.15f;
	float CellHeight = 0.1f;
	float AgentRadius = 0.3f;
	float AgentHeight = 1.8f;
	// Ledges up to this high are walked up, like the player's 0.35 m step.
	float AgentMaxClimb = 0.4f;
	float AgentMaxSlopeRadians = 0.7853982f;
	std::uint32_t TileCells = 64;
};

constexpr float FNavigationSettings::GetTileSize() const noexcept
{
	return CellSize * static_cast<float>(TileCells);
}

inline constexpr std::string_view NavigationDataExtension = ".hnav";
inline constexpr std::uint32_t NavigationDataVersion = 1;

// Where navigation is built: tiles overlapping any box.
struct FNavigationBox
{
	constexpr bool operator==(const FNavigationBox&) const = default;

	FVector3 Minimum{};
	FVector3 Maximum{};
};

// One walkable height layer of one tile, compressed. Tiles with stacked floors have a layer per floor.
struct FNavigationLayer
{
	bool operator==(const FNavigationLayer&) const = default;

	std::int32_t TileX = 0;
	std::int32_t TileZ = 0;
	std::vector<std::byte> Data{};
};

// A level's cooked navigation. Source identifies the geometry, boxes, and settings it was built from, so a host can tell stale data.
struct FNavigationData
{
	bool operator==(const FNavigationData&) const = default;

	FNavigationSettings Settings{};
	FHash128 Source{};
	std::vector<FNavigationBox> Boxes{};
	std::vector<FNavigationLayer> Layers{};
};

// Tiles that load and unload with something a level places, such as a streamed region, which Key identifies.
struct FNavigationPatch
{
	bool operator==(const FNavigationPatch&) const = default;

	FHash128 Key{};
	FNavigationData Data{};
};

// What a level's .hnav file holds. NavigationBuilder cooks it; the runtime only loads it.
struct FNavigationFile
{
	bool operator==(const FNavigationFile&) const = default;

	FNavigationData Base{};
	std::vector<FNavigationPatch> Patches{};
};

[[nodiscard]] std::expected<void, FNavigationError> ValidateNavigationSettings(const FNavigationSettings& Settings);
[[nodiscard]] std::vector<std::byte> SerializeNavigationFile(const FNavigationFile& File);
[[nodiscard]] std::expected<FNavigationFile, FNavigationError> ParseNavigationFile(std::span<const std::byte> Bytes);
}
