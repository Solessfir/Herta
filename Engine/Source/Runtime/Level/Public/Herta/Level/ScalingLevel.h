#pragma once

#include "Herta/Level/LevelSerialization.h"

#include <cstddef>
#include <cstdint>
#include <expected>

namespace Herta
{
enum class EScalingLevelWorkload : std::uint8_t
{
	Rendering,
	DynamicBodies,
};

struct FScalingLevelOptions
{
	std::size_t CubeCount = 1000;
	EScalingLevelWorkload Workload = EScalingLevelWorkload::Rendering;
};

[[nodiscard]] std::expected<FLevelDocument, FLevelError> GenerateScalingLevel(FScalingLevelOptions Options);
}
